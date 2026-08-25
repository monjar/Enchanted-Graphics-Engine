#pragma once

#include "rhi/Descriptors.hpp"
#include "rhi/Pipeline.hpp"

#include <memory>
#include <vector>

namespace ege {

    // Builds the depth pyramid: the frame's depth halved, and halved again,
    // each step taking the farthest of the texels it covers, until the whole
    // screen is a handful of texels.
    //
    // This used to end in a copy back to the CPU, where the culling test ran
    // against a pyramid a couple of frames old. The test lives on the GPU
    // now - see GpuCullSystem - and reads the chain's coarsest levels in the
    // same frame that builds them, so what is left here is exactly the
    // building: one fragment pipeline, run once per level, and the arithmetic
    // that says how many levels a frame has.
    class OcclusionSystem {
    public:
        OcclusionSystem(Device& device, uint32_t framesInFlight);
        ~OcclusionSystem();

        OcclusionSystem(const OcclusionSystem&) = delete;
        OcclusionSystem& operator=(const OcclusionSystem&) = delete;

        // How far down the chain runs: to a level this small on its longer
        // axis, so the coarsest levels the culling shader binds genuinely
        // bound large pieces of the screen in a few texels.
        static constexpr uint32_t coarsestLevelSize = 8;

        // A cap on how many halvings can be declared, so the descriptor sets
        // for them can be allocated once. Eight halvings shrink the longer
        // axis by 256x, which leaves a 4K frame at fifteen texels - coarse
        // enough for the coarsest bound level to mean what it claims.
        static constexpr uint32_t maxReductionSteps = 8;

        // How many halvings it takes to bring `extent` down to the coarsest
        // level, and how large a given step's result is. Both are pure
        // arithmetic on the extent, so the frame can ask how many passes to
        // declare before anything is allocated.
        static uint32_t reductionSteps(VkExtent2D extent);
        static VkExtent2D stepExtent(VkExtent2D extent, uint32_t step);

        // One halving. `source` is the previous step's result, or the frame's
        // depth for step zero.
        void reduce(
            VkCommandBuffer commandBuffer,
            uint32_t frameIndex,
            uint32_t step,
            VkImageView sourceView,
            VkExtent2D sourceExtent);

        // The format every level is built in. Single channel and full float:
        // depth compares are the whole point, and half a float's mantissa is
        // not enough to tell two distant surfaces apart.
        static constexpr VkFormat levelFormat = VK_FORMAT_R32_SFLOAT;

    private:
        Device& device;
        // Read only by the bounds assertion in the frame-indexed calls, which
        // Release compiles out - so in a Release build nothing reads it, and
        // clang says so. Kept rather than deleted because the check is worth
        // having; marked rather than silenced because that is what it is.
        [[maybe_unused]] uint32_t frames = 0;

        VkSampler sampler = VK_NULL_HANDLE;
        std::unique_ptr<DescriptorSetLayout> setLayout;
        std::unique_ptr<DescriptorPool> pool;
        // Indexed frameIndex * maxReductionSteps + step: each step reads a
        // different image within the same frame, so each needs its own set.
        std::vector<VkDescriptorSet> descriptorSets;

        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        std::unique_ptr<Pipeline> pipeline;
    };

}  // namespace ege
