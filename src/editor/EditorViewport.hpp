#pragma once

#include "rhi/Device.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace ege {

    // The image the editor renders the scene into instead of the backbuffer,
    // so the UI can sample it as an ordinary texture and lay it out as a panel
    // like any other.
    //
    // This is what turns the overlay into an editor: with the scene confined to
    // a panel, the hierarchy and inspector stop floating over the picture they
    // are describing, the view gets its own aspect ratio independent of the
    // window, and everything the editor eventually draws around the scene -
    // gizmos, selection outlines, a second camera preview - has somewhere to
    // live. The frame graph makes the swap cheap: the tonemap pass writes here
    // rather than to the backbuffer, and every barrier that implies is derived.
    //
    // Format deliberately matches the swapchain's. The tonemap pass writes
    // linear values that the sRGB attachment encodes, ImGui's sampler decodes
    // them back to linear, and the sRGB backbuffer encodes them again - so the
    // detour through a texture is an exact identity rather than a colour shift.
    class EditorViewport {
    public:
        // Requires the ImGui Vulkan backend to be initialised: the descriptor
        // set the panel draws with is allocated from its pool.
        EditorViewport(Device& device, VkFormat format, uint32_t framesInFlight);
        ~EditorViewport();

        EditorViewport(const EditorViewport&) = delete;
        EditorViewport& operator=(const EditorViewport&) = delete;

        // Sizes the target, reallocating only when the extent actually changed.
        // Call once per frame and before any UI is declared: a resize replaces
        // the descriptor set, and a draw list that already referenced the old
        // one would be pointing at freed memory.
        void resize(VkExtent2D extent);

        bool valid() const { return image != VK_NULL_HANDLE; }

        VkImage imageHandle() const { return image; }

        VkImageView viewHandle() const { return view; }

        VkExtent2D extent() const { return targetExtent; }

        // The ImGui texture handle for the current target.
        VkDescriptorSet textureSet() const { return textureDescriptor; }

    private:
        // A target replaced by a resize. Frames already submitted may still be
        // sampling it, so destruction waits out the frames in flight rather
        // than stalling the device on every drag of a panel edge.
        struct Retired {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkDescriptorSet textureDescriptor = VK_NULL_HANDLE;
            uint32_t framesLeft = 0;
        };

        void createTarget(VkExtent2D extent);
        void destroyRetired(Retired& target);
        void ageRetired();

        Device& device;
        VkFormat imageFormat = VK_FORMAT_UNDEFINED;
        uint32_t retireDelay = 0;

        VkSampler sampler = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet textureDescriptor = VK_NULL_HANDLE;
        VkExtent2D targetExtent{0, 0};

        std::vector<Retired> retired;
    };

}  // namespace ege
