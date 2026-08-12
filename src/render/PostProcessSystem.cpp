#include "render/PostProcessSystem.hpp"

#include "core/Assert.hpp"

#include <stdexcept>

namespace ege {

    PostProcessSystem::PostProcessSystem(
        Device& deviceRef, VkFormat outputFormat, uint32_t framesInFlight)
        : device{deviceRef} {
        createSampler();
        createDescriptors(framesInFlight);
        createPipelineLayout();
        createPipeline(outputFormat);
    }

    PostProcessSystem::~PostProcessSystem() {
        vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
        vkDestroySampler(device.device(), sampler, nullptr);
    }

    void PostProcessSystem::createSampler() {
        // The scene target is sampled exactly at pixel centres at the same
        // resolution, so filtering never actually blends - but linear keeps
        // this correct if a scaled or half-resolution source ever appears.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the post-process sampler"};
        }
    }

    void PostProcessSystem::createDescriptors(uint32_t framesInFlight) {
        setLayout =
            DescriptorSetLayout::Builder(device)
                .addBinding(
                    0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .build();

        pool = DescriptorPool::Builder(device)
                   .setMaxSets(framesInFlight)
                   .addPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, framesInFlight)
                   .build();

        descriptorSets.resize(framesInFlight, VK_NULL_HANDLE);
        for (VkDescriptorSet& set : descriptorSets) {
            if (!pool->allocateDescriptor(setLayout->getDescriptorSetLayout(), set)) {
                throw std::runtime_error{"failed to allocate a post-process descriptor set"};
            }
        }
    }

    void PostProcessSystem::createPipelineLayout() {
        const VkDescriptorSetLayout layoutHandle = setLayout->getDescriptorSetLayout();

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &layoutHandle;

        if (vkCreatePipelineLayout(
                device.device(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the post-process pipeline layout"};
        }
    }

    void PostProcessSystem::createPipeline(VkFormat outputFormat) {
        EGE_ASSERT(pipelineLayout != VK_NULL_HANDLE, "pipeline layout must exist first");

        PipelineConfigInfo pipelineConfig{};
        Pipeline::defaultPipelineConfigInfo(pipelineConfig);
        // No vertex buffer - the triangle comes from gl_VertexIndex - and no
        // depth attachment, so both test and write are off. Culling is off
        // too: the triangle's winding is an implementation detail of the
        // vertex shader, not something worth keeping in sync with cull state.
        pipelineConfig.bindingDescriptions.clear();
        pipelineConfig.attributeDescriptions.clear();
        pipelineConfig.depthStencilInfo.depthTestEnable = VK_FALSE;
        pipelineConfig.depthStencilInfo.depthWriteEnable = VK_FALSE;
        pipelineConfig.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
        pipelineConfig.colorAttachmentFormats = {outputFormat};
        pipelineConfig.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        pipelineConfig.pipelineLayout = pipelineLayout;

        pipeline = std::make_unique<Pipeline>(
            device, "fullscreen.vert.spv", "tonemap.frag.spv", pipelineConfig);
    }

    void PostProcessSystem::render(
        VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImageView sceneColorView) {
        EGE_ASSERT(frameIndex < descriptorSets.size(), "frame index out of range");

        // Rewritten every frame, unconditionally. The graph may recycle, evict
        // and recreate physical images, and a recreated view can legally get a
        // previously seen handle value - so caching "which view is bound" can
        // false-negative into a descriptor that references a destroyed view.
        // Updating a set that only retired command buffers reference is legal;
        // that is exactly what per-frame sets guarantee.
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = sampler;
        imageInfo.imageView = sceneColorView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        DescriptorWriter(*setLayout, *pool)
            .writeImage(0, &imageInfo)
            .overwrite(descriptorSets[frameIndex]);

        pipeline->bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            0,
            1,
            &descriptorSets[frameIndex],
            0,
            nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

}  // namespace ege
