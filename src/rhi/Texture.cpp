#include "rhi/Texture.hpp"

#include "core/Assert.hpp"
#include "core/Log.hpp"
#include "rhi/Buffer.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ege {

    namespace {

        uint32_t mipLevelsFor(uint32_t width, uint32_t height) {
            const auto largest = static_cast<float>(std::max(width, height));
            return static_cast<uint32_t>(std::floor(std::log2(largest))) + 1;
        }

    }  // namespace

    std::unique_ptr<Texture> Texture::fromFile(
        Device& device, const std::string& path, const TextureConfig& config) {
        int width = 0;
        int height = 0;
        int channels = 0;

        // Forced to four channels: three-channel formats are not universally
        // supported as sampled images, and the padding costs less than the
        // format capability checks and fallbacks would.
        stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr) {
            throw std::runtime_error{"failed to load image " + path + ": " + stbi_failure_reason()};
        }

        auto texture = std::unique_ptr<Texture>{new Texture{
            device, pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), config}};
        stbi_image_free(pixels);

        EGE_DEBUG(
            "Loaded texture {} ({}x{}, {} source channels, {} mips)",
            path,
            width,
            height,
            channels,
            texture->mipLevels());
        return texture;
    }

    std::unique_ptr<Texture> Texture::fromPixels(
        Device& device,
        const void* pixels,
        uint32_t width,
        uint32_t height,
        const TextureConfig& config) {
        return std::unique_ptr<Texture>{new Texture{device, pixels, width, height, config}};
    }

    std::unique_ptr<Texture> Texture::solidColor(
        Device& device, glm::vec4 color, const TextureConfig& config) {
        const auto toByte = [](float channel) {
            return static_cast<uint8_t>(std::clamp(channel, 0.f, 1.f) * 255.f + 0.5f);
        };
        const uint8_t pixel[4] = {
            toByte(color.r), toByte(color.g), toByte(color.b), toByte(color.a)};

        TextureConfig single = config;
        // A single texel has no mip chain to build.
        single.generateMipmaps = false;
        return fromPixels(device, pixel, 1, 1, single);
    }

    Texture::Texture(
        Device& deviceRef, const void* pixels, uint32_t w, uint32_t h, const TextureConfig& config)
        : device{deviceRef}, imageWidth{w}, imageHeight{h} {
        EGE_VERIFY(w > 0 && h > 0, "texture dimensions must be non-zero");

        imageFormat = config.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        imageMipLevels = config.generateMipmaps ? mipLevelsFor(w, h) : 1;

        createImage(pixels, config);
        createView(config);
        createSampler(config);
    }

    Texture::~Texture() {
        vkDestroySampler(device.device(), imageSampler, nullptr);
        vkDestroyImageView(device.device(), imageView, nullptr);
        device.destroyImage(image, allocation);
    }

    void Texture::createImage(const void* pixels, const TextureConfig& config) {
        const VkDeviceSize imageSize = static_cast<VkDeviceSize>(imageWidth) * imageHeight * 4;

        Buffer staging{
            device,
            imageSize,
            1,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        staging.map();
        staging.writeToBuffer(const_cast<void*>(pixels));

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {imageWidth, imageHeight, 1};
        imageInfo.mipLevels = imageMipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = imageFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // TRANSFER_SRC as well as DST because mip generation blits from level
        // n to level n+1, so the image is both source and destination.
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        device.createImageWithInfo(
            imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, allocation);

        VkCommandBuffer commandBuffer = device.beginSingleTimeCommands();
        transitionLayout(
            commandBuffer,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0,
            imageMipLevels);
        device.endSingleTimeCommands(commandBuffer);

        device.copyBufferToImage(staging.getBuffer(), image, imageWidth, imageHeight, 1);

        if (imageMipLevels > 1) {
            generateMipmaps();
        } else {
            commandBuffer = device.beginSingleTimeCommands();
            transitionLayout(
                commandBuffer,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                0,
                1);
            device.endSingleTimeCommands(commandBuffer);
        }

        (void)config;
    }

    void Texture::generateMipmaps() {
        // Blitting requires the format to support linear filtering. Every
        // driver supports it for RGBA8, but checking is cheap and the failure
        // mode otherwise is a validation error rather than an exception.
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(
            device.physicalDeviceHandle(), imageFormat, &formatProperties);
        if ((formatProperties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
            throw std::runtime_error{"texture format does not support linear blitting"};
        }

        VkCommandBuffer commandBuffer = device.beginSingleTimeCommands();

        auto mipWidth = static_cast<int32_t>(imageWidth);
        auto mipHeight = static_cast<int32_t>(imageHeight);

        for (uint32_t level = 1; level < imageMipLevels; level++) {
            // The previous level becomes the blit source.
            transitionLayout(
                commandBuffer,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                level - 1,
                1);

            VkImageBlit blit{};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = level - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = 1;
            blit.dstOffsets[0] = {0, 0, 0};
            // Halve, but never below one: a 1024x1 texture still has levels
            // after the height has bottomed out.
            blit.dstOffsets[1] = {
                mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = level;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = 1;

            vkCmdBlitImage(
                commandBuffer,
                image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &blit,
                VK_FILTER_LINEAR);

            // Source level is finished with; move it to its final layout.
            transitionLayout(
                commandBuffer,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                level - 1,
                1);

            if (mipWidth > 1) {
                mipWidth /= 2;
            }
            if (mipHeight > 1) {
                mipHeight /= 2;
            }
        }

        // The last level was never a blit source, so it is still TRANSFER_DST.
        transitionLayout(
            commandBuffer,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            imageMipLevels - 1,
            1);

        device.endSingleTimeCommands(commandBuffer);
    }

    void Texture::transitionLayout(
        VkCommandBuffer commandBuffer,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        uint32_t baseMipLevel,
        uint32_t levelCount) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = baseMipLevel;
        barrier.subresourceRange.levelCount = levelCount;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (
            oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (
            oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (
            oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else {
            EGE_VERIFY(false, "unsupported image layout transition");
        }

        vkCmdPipelineBarrier(
            commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void Texture::createView(const TextureConfig& config) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = imageMipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device.device(), &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create texture image view"};
        }

        (void)config;
    }

    void Texture::createSampler(const TextureConfig& config) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = config.magFilter;
        samplerInfo.minFilter = config.minFilter;
        samplerInfo.addressModeU = config.addressMode;
        samplerInfo.addressModeV = config.addressMode;
        samplerInfo.addressModeW = config.addressMode;

        // The device already requests samplerAnisotropy; this is the first
        // thing to actually use it. Clamped to what the device supports, since
        // asking for more is a validation error.
        const float deviceMaximum = device.properties.limits.maxSamplerAnisotropy;
        const float requested = std::min(config.maxAnisotropy, deviceMaximum);
        samplerInfo.anisotropyEnable = requested > 1.f ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy = requested > 1.f ? requested : 1.f;

        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.f;
        samplerInfo.minLod = 0.f;
        samplerInfo.maxLod = static_cast<float>(imageMipLevels);

        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &imageSampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create texture sampler"};
        }
    }

    VkDescriptorImageInfo Texture::descriptorInfo() const {
        VkDescriptorImageInfo info{};
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info.imageView = imageView;
        info.sampler = imageSampler;
        return info;
    }

}  // namespace ege
