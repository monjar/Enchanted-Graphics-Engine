#include "render/ClusterLightSystem.hpp"

#include <stdexcept>

namespace ege {

    ClusterLightSystem::ClusterLightSystem(Device& deviceRef, uint32_t framesInFlight)
        : device{deviceRef} {
        // Binding numbers match the graphics global set - 0 for the uniform
        // block, 6 and 7 for the two storage buffers - so both pipelines can
        // include the same declaration of the block. A set layout is a map
        // from binding number to descriptor, not a dense array, so the gap
        // between 0 and 6 costs nothing.
        setLayout =
            DescriptorSetLayout::Builder(device)
                .addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
                .addBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
                .addBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
                .build();

        pool = DescriptorPool::Builder(device)
                   .setMaxSets(framesInFlight)
                   .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight)
                   .addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, framesInFlight * 2)
                   .build();

        descriptorSets.resize(framesInFlight);
        for (VkDescriptorSet& set : descriptorSets) {
            if (!pool->allocateDescriptor(setLayout->getDescriptorSetLayout(), set)) {
                throw std::runtime_error{"failed to allocate a light culling descriptor set"};
            }
        }

        VkDescriptorSetLayout rawLayout = setLayout->getDescriptorSetLayout();
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &rawLayout;

        if (vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &pipelineLayout) !=
            VK_SUCCESS) {
            throw std::runtime_error{"failed to create the light culling pipeline layout"};
        }

        pipeline = std::make_unique<ComputePipeline>(device, "light_cull.comp.spv", pipelineLayout);
    }

    ClusterLightSystem::~ClusterLightSystem() {
        vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
    }

    void ClusterLightSystem::cull(
        VkCommandBuffer commandBuffer,
        uint32_t frameIndex,
        const VkDescriptorBufferInfo& globalUbo,
        const VkDescriptorBufferInfo& lights,
        const VkDescriptorBufferInfo& clusterList) {
        VkDescriptorSet& set = descriptorSets[frameIndex];

        // Written before it is bound, and not touched again this frame. The
        // set is per-frame, so the copy being rewritten here is one no frame
        // still in flight is reading.
        DescriptorWriter(*setLayout, *pool)
            .writeBuffer(0, const_cast<VkDescriptorBufferInfo*>(&globalUbo))
            .writeBuffer(6, const_cast<VkDescriptorBufferInfo*>(&lights))
            .writeBuffer(7, const_cast<VkDescriptorBufferInfo*>(&clusterList))
            .overwrite(set);

        pipeline->bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        // One invocation per cluster. The grid is fixed, so this dispatch is
        // the same size every frame regardless of how many lights there are -
        // the light count only changes how long each invocation's loop is.
        vkCmdDispatch(commandBuffer, dispatchGroupCount(clusterCount, workgroupSize), 1, 1);
    }

}  // namespace ege
