#pragma once

#include "render/OcclusionCulling.hpp"
#include "rhi/Buffer.hpp"
#include "rhi/Descriptors.hpp"
#include "rhi/Pipeline.hpp"

#include <memory>
#include <vector>

namespace ege {

    // Builds the depth pyramid on the GPU and brings its smallest level back.
    //
    // The frame's depth is halved, and halved again, each step taking the
    // farthest of the texels it covers, until what is left is a few thousand
    // floats. That last level is copied into a mapped buffer, and the frame
    // that reuses this frame's index - two frames later, once its fence has
    // been waited on - reads it and finishes the pyramid on the CPU, where the
    // draws are decided. See render/OcclusionCulling.hpp for what is done with
    // it and what the latency costs.
    //
    // The last level is an image this system owns rather than a frame graph
    // transient, because it has to outlive the frame that produced it. It is
    // imported into the graph each frame like the swapchain image is, and left
    // in TRANSFER_SRC_OPTIMAL for the copy that follows the graph - the same
    // arrangement the frame recorder uses.
    class OcclusionSystem {
    public:
        OcclusionSystem(Device& device, uint32_t framesInFlight);
        ~OcclusionSystem();

        OcclusionSystem(const OcclusionSystem&) = delete;
        OcclusionSystem& operator=(const OcclusionSystem&) = delete;

        // How many halvings it takes to bring `extent` down to something worth
        // copying back, and how large a given step's result is. Both are pure
        // arithmetic on the extent, so the frame can ask how many passes to
        // declare before anything is allocated.
        static uint32_t reductionSteps(VkExtent2D extent);
        static VkExtent2D stepExtent(VkExtent2D extent, uint32_t step);

        // Matches the readback resources to this frame size, rebuilding them
        // if the window changed. Discards whatever had been read back: a
        // pyramid of the old size describes a frame that no longer exists.
        void resize(VkExtent2D renderExtent);

        // Reads whatever the GPU left at this index, which is the frame that
        // used the index last. Does nothing until one has.
        void collect(uint32_t frameIndex, const glm::mat4& viewProjection);

        // What the last collect found, for the renderer to test against.
        const OcclusionSnapshot& snapshot() const { return current; }

        // One halving. `source` is the previous step's result, or the frame's
        // depth for step zero.
        void reduce(
            VkCommandBuffer commandBuffer,
            uint32_t frameIndex,
            uint32_t step,
            VkImageView sourceView,
            VkExtent2D sourceExtent);

        // The copy out of the last level, recorded after the graph has run and
        // left the image in TRANSFER_SRC_OPTIMAL.
        void recordCopy(VkCommandBuffer commandBuffer, uint32_t frameIndex);

        VkImage readbackImage(uint32_t frameIndex) const { return targets[frameIndex].image; }

        VkImageView readbackView(uint32_t frameIndex) const { return targets[frameIndex].view; }

        // The size of the level that comes back, which is the render extent
        // halved as many times as it took to get under the cap - not the
        // render extent itself.
        VkExtent2D readbackExtent() const { return pyramidExtent; }

        // The format every level is built in. Single channel and full float:
        // depth compares are the whole point, and half a float's mantissa is
        // not enough to tell two distant surfaces apart.
        static constexpr VkFormat levelFormat = VK_FORMAT_R32_SFLOAT;

        // Turned off, nothing is culled and nothing is read back - the frame
        // is what it was before any of this existed. The first thing to try
        // when geometry goes missing.
        bool enabled = true;

    private:
        // A cap on how many halvings can be declared, so the descriptor sets
        // for them can be allocated once. Eight covers a 32768-pixel frame,
        // which is a long way beyond anything this will run on.
        static constexpr uint32_t maxReductionSteps = 8;

        struct Target {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            std::unique_ptr<Buffer> readback;
            // The camera the frame that filled this was seen through. The test
            // has to use it rather than the camera of the frame doing the
            // testing, which is two frames younger.
            glm::mat4 viewProjection{1.f};
            // Whether a frame has actually copied into it yet. The first
            // frames at each index read a buffer nothing has written.
            bool written = false;
        };

        void destroyTargets();

        Device& device;
        uint32_t frames = 0;
        VkExtent2D lastExtent{0, 0};
        VkExtent2D pyramidExtent{0, 0};

        std::vector<Target> targets;

        VkSampler sampler = VK_NULL_HANDLE;
        std::unique_ptr<DescriptorSetLayout> setLayout;
        std::unique_ptr<DescriptorPool> pool;
        // Indexed frameIndex * maxReductionSteps + step: each step reads a
        // different image within the same frame, so each needs its own set.
        std::vector<VkDescriptorSet> descriptorSets;

        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        std::unique_ptr<Pipeline> pipeline;

        OcclusionSnapshot current{};
        std::vector<float> scratch;
    };

}  // namespace ege
