#include "render/BloomSystem.hpp"

#include "core/Assert.hpp"

#include <stdexcept>

namespace ege {

    namespace {

        struct BlurPush {
            glm::vec2 direction{1.f, 0.f};
        };

    }  // namespace

    BloomSystem::BloomSystem(Device& deviceRef, VkFormat format, uint32_t framesInFlight)
        : device{deviceRef}, frames{framesInFlight} {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the bloom sampler"};
        }

        setLayout =
            DescriptorSetLayout::Builder(device)
                .addBinding(
                    0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .build();

        const uint32_t setCount = stageCount * framesInFlight;
        pool = DescriptorPool::Builder(device)
                   .setMaxSets(setCount)
                   .addPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount)
                   .build();

        descriptorSets.resize(setCount, VK_NULL_HANDLE);
        for (VkDescriptorSet& set : descriptorSets) {
            if (!pool->allocateDescriptor(setLayout->getDescriptorSetLayout(), set)) {
                throw std::runtime_error{"failed to allocate a bloom descriptor set"};
            }
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(BlurPush);

        const VkDescriptorSetLayout layoutHandle = setLayout->getDescriptorSetLayout();

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &layoutHandle;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(
                device.device(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the bloom pipeline layout"};
        }

        PipelineConfigInfo config{};
        Pipeline::defaultPipelineConfigInfo(config);
        config.bindingDescriptions.clear();
        config.attributeDescriptions.clear();
        config.depthStencilInfo.depthTestEnable = VK_FALSE;
        config.depthStencilInfo.depthWriteEnable = VK_FALSE;
        config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
        config.colorAttachmentFormats = {format};
        config.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        config.pipelineLayout = pipelineLayout;

        brightPipeline = std::make_unique<Pipeline>(
            device, "fullscreen.vert.spv", "bloom_bright.frag.spv", config);
        blurPipeline = std::make_unique<Pipeline>(
            device, "fullscreen.vert.spv", "bloom_blur.frag.spv", config);
    }

    BloomSystem::~BloomSystem() {
        vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
        vkDestroySampler(device.device(), sampler, nullptr);
    }

    void BloomSystem::draw(
        VkCommandBuffer commandBuffer,
        Pipeline& pipeline,
        uint32_t setIndex,
        VkImageView sourceView,
        const void* pushData,
        uint32_t pushSize) {
        // Rewritten every frame; the set's previous use retired with its
        // frame's fence, and the graph may back the transient with a
        // different physical image at any time.
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = sampler;
        imageInfo.imageView = sourceView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        DescriptorWriter(*setLayout, *pool)
            .writeImage(0, &imageInfo)
            .overwrite(descriptorSets[setIndex]);

        pipeline.bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            0,
            1,
            &descriptorSets[setIndex],
            0,
            nullptr);
        if (pushSize > 0) {
            vkCmdPushConstants(
                commandBuffer, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, pushData);
        }
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    void BloomSystem::renderBrightPass(
        VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImageView sourceView) {
        EGE_ASSERT(frameIndex < frames, "frame index out of range");
        draw(
            commandBuffer,
            *brightPipeline,
            stageBright * frames + frameIndex,
            sourceView,
            nullptr,
            0);
    }

    void BloomSystem::renderBlur(
        VkCommandBuffer commandBuffer,
        uint32_t frameIndex,
        VkImageView sourceView,
        glm::vec2 direction) {
        EGE_ASSERT(frameIndex < frames, "frame index out of range");
        const uint32_t stage = direction.x != 0.f ? stageBlurH : stageBlurV;

        BlurPush push{};
        push.direction = direction;
        draw(
            commandBuffer,
            *blurPipeline,
            stage * frames + frameIndex,
            sourceView,
            &push,
            sizeof(push));
    }

}  // namespace ege
