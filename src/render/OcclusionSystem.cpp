#include "render/OcclusionSystem.hpp"

#include "core/Assert.hpp"

#include <algorithm>
#include <stdexcept>

namespace ege {

    namespace {

        struct ReducePush {
            int32_t sourceWidth = 0;
            int32_t sourceHeight = 0;
        };

        VkExtent2D halved(VkExtent2D extent) {
            return {
                std::max(1u, (extent.width + 1u) / 2u), std::max(1u, (extent.height + 1u) / 2u)};
        }

    }  // namespace

    uint32_t OcclusionSystem::reductionSteps(VkExtent2D extent) {
        uint32_t steps = 0;
        VkExtent2D current = extent;
        do {
            current = halved(current);
            steps++;
        } while (std::max(current.width, current.height) > coarsestLevelSize &&
                 steps < maxReductionSteps);
        return steps;
    }

    VkExtent2D OcclusionSystem::stepExtent(VkExtent2D extent, uint32_t step) {
        VkExtent2D current = extent;
        for (uint32_t i = 0; i <= step; i++) {
            current = halved(current);
        }
        return current;
    }

    OcclusionSystem::OcclusionSystem(Device& deviceRef, uint32_t framesInFlight)
        : device{deviceRef}, frames{framesInFlight} {
        // Point sampling throughout: every lookup is a texelFetch at an exact
        // integer texel, and a filtered depth is an average of two surfaces,
        // which is a surface that was never there.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the depth pyramid sampler"};
        }

        setLayout =
            DescriptorSetLayout::Builder(device)
                .addBinding(
                    0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .build();

        const uint32_t setCount = framesInFlight * maxReductionSteps;
        pool = DescriptorPool::Builder(device)
                   .setMaxSets(setCount)
                   .addPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount)
                   .build();

        descriptorSets.resize(setCount, VK_NULL_HANDLE);
        for (VkDescriptorSet& set : descriptorSets) {
            if (!pool->allocateDescriptor(setLayout->getDescriptorSetLayout(), set)) {
                throw std::runtime_error{"failed to allocate a depth pyramid descriptor set"};
            }
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(ReducePush);

        const VkDescriptorSetLayout rawLayout = setLayout->getDescriptorSetLayout();
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &rawLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &pipelineLayout) !=
            VK_SUCCESS) {
            throw std::runtime_error{"failed to create the depth pyramid pipeline layout"};
        }

        PipelineConfigInfo config{};
        Pipeline::defaultPipelineConfigInfo(config);
        config.bindingDescriptions.clear();
        config.attributeDescriptions.clear();
        config.depthStencilInfo.depthTestEnable = VK_FALSE;
        config.depthStencilInfo.depthWriteEnable = VK_FALSE;
        config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
        config.colorAttachmentFormats = {levelFormat};
        config.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        config.pipelineLayout = pipelineLayout;

        pipeline = std::make_unique<Pipeline>(
            device, "fullscreen.vert.spv", "hzb_reduce.frag.spv", config);
    }

    OcclusionSystem::~OcclusionSystem() {
        vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
        vkDestroySampler(device.device(), sampler, nullptr);
    }

    void OcclusionSystem::reduce(
        VkCommandBuffer commandBuffer,
        uint32_t frameIndex,
        uint32_t step,
        VkImageView sourceView,
        VkExtent2D sourceExtent) {
        EGE_ASSERT(frameIndex < frames, "frame index out of range");
        EGE_ASSERT(step < maxReductionSteps, "more reduction steps than sets were allocated");

        VkDescriptorImageInfo sourceInfo{};
        sourceInfo.sampler = sampler;
        sourceInfo.imageView = sourceView;
        sourceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorSet& set = descriptorSets[frameIndex * maxReductionSteps + step];
        DescriptorWriter(*setLayout, *pool).writeImage(0, &sourceInfo).overwrite(set);

        ReducePush push{};
        push.sourceWidth = static_cast<int32_t>(sourceExtent.width);
        push.sourceHeight = static_cast<int32_t>(sourceExtent.height);

        pipeline->bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(
            commandBuffer,
            pipelineLayout,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(ReducePush),
            &push);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

}  // namespace ege
