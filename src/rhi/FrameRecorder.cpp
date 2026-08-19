#include "rhi/FrameRecorder.hpp"

#include "core/Log.hpp"
#include "rhi/Buffer.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstring>
#include <system_error>
#include <vector>

namespace ege {

    namespace {

        // Whether the channels arrive as blue-green-red rather than
        // red-green-blue. Swapchains commonly hand out BGRA, and PNG is RGBA.
        bool isBlueFirst(VkFormat format) {
            switch (format) {
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB:
                case VK_FORMAT_B8G8R8A8_SNORM:
                    return true;
                default:
                    return false;
            }
        }

    }  // namespace

    FrameRecorder::FrameRecorder(
        Device& deviceRef, std::filesystem::path directory, VkExtent2D extent)
        : device{deviceRef}, outputDirectory{std::move(directory)}, imageExtent{extent} {
        std::error_code errorCode;
        std::filesystem::create_directories(outputDirectory, errorCode);
        if (errorCode) {
            EGE_ERROR(
                "cannot create the recording directory {}: {}",
                outputDirectory.string(),
                errorCode.message());
        }

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4u;
        staging = std::make_unique<Buffer>(
            device,
            bytes,
            1,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging->map();

        EGE_INFO(
            "Recording {}x{} frames to {}", extent.width, extent.height, outputDirectory.string());
    }

    FrameRecorder::~FrameRecorder() = default;

    void FrameRecorder::recordCopy(
        VkCommandBuffer commandBuffer, VkImage image, VkImageLayout finalLayout) {
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {imageExtent.width, imageExtent.height, 1};

        vkCmdCopyImageToBuffer(
            commandBuffer,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            staging->getBuffer(),
            1,
            &region);

        // The image was handed over in TRANSFER_SRC_OPTIMAL and has to leave
        // in whatever its owner expects - for a swapchain image, PRESENT_SRC.
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_NONE;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = finalLayout;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);

        pending = true;
    }

    void FrameRecorder::writeFrame(VkFormat format) {
        if (!pending) {
            return;
        }
        pending = false;

        // The copy is somewhere in the queue; the pixels are not readable
        // until it has run. Waiting on the whole device is heavy-handed and
        // exactly right here - a recording is not trying to be fast.
        device.waitIdle();

        const std::size_t pixelCount =
            static_cast<std::size_t>(imageExtent.width) * imageExtent.height;
        std::vector<unsigned char> pixels(pixelCount * 4u);
        std::memcpy(pixels.data(), staging->getMappedMemory(), pixels.size());

        if (isBlueFirst(format)) {
            for (std::size_t index = 0; index < pixelCount; index++) {
                std::swap(pixels[index * 4u], pixels[index * 4u + 2u]);
            }
        }

        // Opaque regardless of what the frame left in alpha: a window's alpha
        // channel is about compositing, and a recording of it should not come
        // out see-through.
        for (std::size_t index = 0; index < pixelCount; index++) {
            pixels[index * 4u + 3u] = 255;
        }

        char name[32];
        std::snprintf(name, sizeof(name), "frame_%05zu.png", frames);
        const std::filesystem::path path = outputDirectory / name;

        const int written = stbi_write_png(
            path.string().c_str(),
            static_cast<int>(imageExtent.width),
            static_cast<int>(imageExtent.height),
            4,
            pixels.data(),
            static_cast<int>(imageExtent.width) * 4);
        if (written == 0) {
            EGE_ERROR("failed to write {}", path.string());
            return;
        }

        frames++;
    }

}  // namespace ege
