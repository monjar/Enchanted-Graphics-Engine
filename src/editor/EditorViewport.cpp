#include "editor/EditorViewport.hpp"

#include "core/Log.hpp"

#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <stdexcept>

namespace ege {

    EditorViewport::EditorViewport(Device& deviceRef, VkFormat format, uint32_t framesInFlight)
        : device{deviceRef},
          imageFormat{format},
          // One frame of margin past the deepest frame that could still be
          // reading a replaced target.
          retireDelay{framesInFlight + 1} {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        // Clamped rather than repeated: the panel may be laid out at any size,
        // and a filter tap that wrapped would pull the opposite edge in.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        samplerInfo.maxLod = 1.f;

        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the editor viewport sampler"};
        }
    }

    EditorViewport::~EditorViewport() {
        // The device is idle by the time the editor tears down, so nothing here
        // has to wait out a frame.
        for (Retired& target : retired) {
            destroyRetired(target);
        }
        Retired current{image, allocation, view, textureDescriptor, 0};
        destroyRetired(current);
        vkDestroySampler(device.device(), sampler, nullptr);
    }

    void EditorViewport::resize(VkExtent2D extent) {
        ageRetired();

        // A panel can be dragged to nothing; a zero-extent image cannot be
        // created, and one pixel renders harmlessly until it is dragged back.
        const VkExtent2D wanted{std::max(extent.width, 1u), std::max(extent.height, 1u)};
        if (image != VK_NULL_HANDLE && wanted.width == targetExtent.width &&
            wanted.height == targetExtent.height) {
            return;
        }

        if (image != VK_NULL_HANDLE) {
            retired.push_back(Retired{image, allocation, view, textureDescriptor, retireDelay});
            image = VK_NULL_HANDLE;
            allocation = VK_NULL_HANDLE;
            view = VK_NULL_HANDLE;
            textureDescriptor = VK_NULL_HANDLE;
        }

        createTarget(wanted);
    }

    void EditorViewport::createTarget(VkExtent2D extent) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = imageFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Written by the tonemap pass, read by the UI pass - the two halves of
        // what makes this a viewport rather than a render target.
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        device.createImageWithInfo(
            imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, allocation);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(device.device(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
            device.destroyImage(image, allocation);
            image = VK_NULL_HANDLE;
            allocation = VK_NULL_HANDLE;
            throw std::runtime_error{"failed to create the editor viewport image view"};
        }

        // The layout named here is what the image is in whenever ImGui samples
        // it, which the UI pass's declared read is what guarantees.
        textureDescriptor =
            ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        targetExtent = extent;
        EGE_DEBUG("editor viewport target is now {}x{}", extent.width, extent.height);
    }

    void EditorViewport::ageRetired() {
        for (auto it = retired.begin(); it != retired.end();) {
            if (it->framesLeft == 0) {
                destroyRetired(*it);
                it = retired.erase(it);
            } else {
                it->framesLeft--;
                ++it;
            }
        }
    }

    void EditorViewport::destroyRetired(Retired& target) {
        if (target.textureDescriptor != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(target.textureDescriptor);
            target.textureDescriptor = VK_NULL_HANDLE;
        }
        if (target.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device.device(), target.view, nullptr);
            target.view = VK_NULL_HANDLE;
        }
        if (target.image != VK_NULL_HANDLE) {
            device.destroyImage(target.image, target.allocation);
            target.image = VK_NULL_HANDLE;
            target.allocation = VK_NULL_HANDLE;
        }
    }

}  // namespace ege
