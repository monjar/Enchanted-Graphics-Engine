#pragma once

#include "render/ClusterGrid.hpp"
#include "rhi/Descriptors.hpp"
#include "rhi/Pipeline.hpp"

#include <memory>
#include <vector>

namespace ege {

    // The compute pass that assigns lights to clusters.
    //
    // One dispatch per frame, before anything shades: each invocation takes a
    // cluster, works out which lights reach it, and writes their indices for
    // the fragment shader to loop.
    //
    // It owns its descriptor set rather than borrowing the renderer's global
    // one, and that is not tidiness. Updating a descriptor set after a command
    // buffer has bound it invalidates the whole command buffer - so a compute
    // pass binding the global set, followed by the scene pass writing the
    // shadow map into that same set, would destroy the frame. Two sets, each
    // written and then bound, cannot collide. The binding numbers deliberately
    // match the graphics set so both can share one declaration of the uniform
    // block.
    class ClusterLightSystem {
    public:
        ClusterLightSystem(Device& device, uint32_t framesInFlight);
        ~ClusterLightSystem();

        ClusterLightSystem(const ClusterLightSystem&) = delete;
        ClusterLightSystem& operator=(const ClusterLightSystem&) = delete;

        // Points the pass at this frame's buffers and dispatches it. The
        // cluster list is a frame graph transient, so which physical buffer
        // backs it is only known while the pass is recording.
        void cull(
            VkCommandBuffer commandBuffer,
            uint32_t frameIndex,
            const VkDescriptorBufferInfo& globalUbo,
            const VkDescriptorBufferInfo& lights,
            const VkDescriptorBufferInfo& clusterList);

        // A count followed by that many indices, per cluster, at a fixed
        // stride. The stride is what makes a cluster's list addressable
        // without a table of offsets computed in a pass of its own.
        static constexpr uint32_t clusterStride = maxLightsPerCluster + 1;

        static constexpr VkDeviceSize clusterBufferSize() {
            return static_cast<VkDeviceSize>(clusterCount) * clusterStride * sizeof(uint32_t);
        }

    private:
        // Chosen to match the subgroup width common to current hardware; the
        // work is one independent cluster per invocation, so nothing here
        // depends on the number beyond the dispatch arithmetic.
        static constexpr uint32_t workgroupSize = 64;

        Device& device;

        std::unique_ptr<DescriptorSetLayout> setLayout;
        std::unique_ptr<DescriptorPool> pool;
        std::vector<VkDescriptorSet> descriptorSets;

        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        std::unique_ptr<ComputePipeline> pipeline;
    };

}  // namespace ege
