#pragma once

#include "rhi/Device.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace ege {

    class Buffer;

    // Writes rendered frames to disk, one PNG per frame.
    //
    // The engine can already be looked at; this is what lets it be shown, and
    // what a golden-image regression test will eventually read. Both want the
    // same thing: the exact pixels the GPU produced, at a frame rate decided
    // by the recording rather than by how fast the machine happens to render.
    //
    // The copy is recorded into the frame's own command buffer, before the
    // image is handed to the presentation engine - an image that has been
    // presented belongs to the presentation engine until it is acquired
    // again, and reading it there is the kind of thing that works on one
    // driver and corrupts on another.
    class FrameRecorder {
    public:
        FrameRecorder(Device& device, std::filesystem::path directory, VkExtent2D extent);
        ~FrameRecorder();

        FrameRecorder(const FrameRecorder&) = delete;
        FrameRecorder& operator=(const FrameRecorder&) = delete;

        // Records the copy out of `image`, which must be in
        // TRANSFER_SRC_OPTIMAL, and leaves it in `finalLayout`. Call after the
        // frame graph has run and before the frame is submitted.
        void recordCopy(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout finalLayout);

        // Writes the frame the last recordCopy captured. Call after the
        // submission has completed - the device is waited on here, because a
        // recording is not something anyone is trying to run fast.
        void writeFrame(VkFormat format);

        std::size_t frameCount() const { return frames; }

        const std::filesystem::path& directory() const { return outputDirectory; }

    private:
        Device& device;
        std::filesystem::path outputDirectory;
        VkExtent2D imageExtent{0, 0};
        std::unique_ptr<Buffer> staging;
        std::size_t frames = 0;
        bool pending = false;
    };

}  // namespace ege
