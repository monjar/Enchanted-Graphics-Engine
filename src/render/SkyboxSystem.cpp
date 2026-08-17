#include "render/SkyboxSystem.hpp"

#include <stdexcept>

namespace ege {

    SkyboxSystem::SkyboxSystem(
        Device& deviceRef,
        VkFormat colorFormat,
        VkFormat depthFormat,
        VkDescriptorSetLayout globalSetLayout,
        VkSampleCountFlagBits samples)
        : device{deviceRef} {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &globalSetLayout;

        if (vkCreatePipelineLayout(
                device.device(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the skybox pipeline layout"};
        }

        PipelineConfigInfo config{};
        Pipeline::defaultPipelineConfigInfo(config);
        // The sky renders into the same attachments the scene does, so it
        // carries the same sample count.
        config.multisampleInfo.rasterizationSamples = samples;
        config.bindingDescriptions.clear();
        config.attributeDescriptions.clear();
        // Test against the scene's depth at the far plane, write nothing:
        // the sky is behind everything and occludes nothing.
        config.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        config.depthStencilInfo.depthWriteEnable = VK_FALSE;
        config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
        config.colorAttachmentFormats = {colorFormat};
        config.depthAttachmentFormat = depthFormat;
        config.pipelineLayout = pipelineLayout;

        pipeline = std::make_unique<Pipeline>(device, "skybox.vert.spv", "skybox.frag.spv", config);
    }

    SkyboxSystem::~SkyboxSystem() {
        vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
    }

    void SkyboxSystem::render(VkCommandBuffer commandBuffer, VkDescriptorSet globalDescriptorSet) {
        pipeline->bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            0,
            1,
            &globalDescriptorSet,
            0,
            nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

}  // namespace ege
