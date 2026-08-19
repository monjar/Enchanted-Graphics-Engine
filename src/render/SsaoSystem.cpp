#include "render/SsaoSystem.hpp"

#include "core/Assert.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace ege {

    namespace {

        // Mirrors the SsaoKernel block in shaders/ssao.frag. std140, so every
        // entry is a vec4 whether or not its fourth component means anything.
        struct SsaoKernelBlock {
            glm::vec4 samples[ssaoMaxSamples]{};
            // x: radius in world units, y: depth bias, z: strength exponent,
            // w: how many samples are in use.
            glm::vec4 params{};
        };

    }  // namespace

    SsaoSystem::SsaoSystem(Device& deviceRef, VkFormat occlusionFormat, uint32_t framesInFlight)
        : device{deviceRef}, frames{framesInFlight} {
        static_assert(
            ssaoSampleCount <= ssaoMaxSamples, "more samples in use than the block has room for");

        createSamplers();
        createKernel();
        createDescriptors(framesInFlight);
        createPipelines(occlusionFormat);
    }

    SsaoSystem::~SsaoSystem() {
        vkDestroyPipelineLayout(device.device(), blurPipelineLayout, nullptr);
        vkDestroyPipelineLayout(device.device(), occlusionPipelineLayout, nullptr);
        vkDestroySampler(device.device(), linearSampler, nullptr);
        vkDestroySampler(device.device(), depthSampler, nullptr);
    }

    void SsaoSystem::createSamplers() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        // Nearest, and not only because averaging two depths gives a surface
        // neither of them was: linear filtering of a depth format is not a
        // feature every device has to support, so asking for it is asking to
        // be refused somewhere.
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        // Clamped, so a sample that lands off screen reads the edge rather
        // than wrapping round to the far side of the frame.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &depthSampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the occlusion depth sampler"};
        }

        VkSamplerCreateInfo linearInfo = samplerInfo;
        linearInfo.magFilter = VK_FILTER_LINEAR;
        linearInfo.minFilter = VK_FILTER_LINEAR;

        if (vkCreateSampler(device.device(), &linearInfo, nullptr, &linearSampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the occlusion sampler"};
        }
    }

    void SsaoSystem::createKernel() {
        SsaoKernelBlock block{};
        const std::vector<glm::vec4> samples = ssaoKernel(ssaoSampleCount);
        for (std::size_t i = 0; i < samples.size(); i++) {
            block.samples[i] = samples[i];
        }
        block.params = glm::vec4{
            ssaoRadius, ssaoBias, ssaoPower, static_cast<float>(ssaoSampleCount)};

        // Written once and never again, unlike the per-frame uniform buffers:
        // nothing in it depends on the camera or on the scene.
        kernelBuffer = std::make_unique<Buffer>(
            device,
            sizeof(SsaoKernelBlock),
            1,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        kernelBuffer->map();
        kernelBuffer->writeToBuffer(&block);
        kernelBuffer->flush();

        // The rotation tile, as eight-bit pixels: the vectors are unit length,
        // so half-and-offset uses the encoding's whole range and the shader
        // undoes it with one multiply-add.
        const std::vector<glm::vec4> rotations = ssaoNoise(ssaoNoiseSize * ssaoNoiseSize);
        std::vector<uint8_t> pixels(rotations.size() * 4, 0);
        for (std::size_t i = 0; i < rotations.size(); i++) {
            pixels[i * 4 + 0] = static_cast<uint8_t>((rotations[i].x * 0.5f + 0.5f) * 255.f);
            pixels[i * 4 + 1] = static_cast<uint8_t>((rotations[i].y * 0.5f + 0.5f) * 255.f);
            pixels[i * 4 + 2] = 0;
            pixels[i * 4 + 3] = 255;
        }

        TextureConfig noiseConfig{};
        // Not a colour: applying the sRGB transfer curve to a direction would
        // bend every rotation towards the axes.
        noiseConfig.srgb = false;
        // Neither filtered nor mipped. The pattern is the point; smoothing it
        // would make neighbouring pixels sample the same directions again,
        // which is the banding it exists to break up.
        noiseConfig.generateMipmaps = false;
        noiseConfig.minFilter = VK_FILTER_NEAREST;
        noiseConfig.magFilter = VK_FILTER_NEAREST;
        noiseConfig.addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        noiseConfig.maxAnisotropy = 0.f;

        noiseTexture =
            Texture::fromPixels(device, pixels.data(), ssaoNoiseSize, ssaoNoiseSize, noiseConfig);
    }

    void SsaoSystem::createDescriptors(uint32_t framesInFlight) {
        // Binding 0 is the uniform block, the same number the global set uses,
        // so this pass includes the same declaration of it as everything else.
        // The rest are this pass's own.
        occlusionSetLayout =
            DescriptorSetLayout::Builder(device)
                .addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .addBinding(
                    1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .addBinding(
                    2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .addBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .build();

        blurSetLayout =
            DescriptorSetLayout::Builder(device)
                .addBinding(
                    0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .build();

        pool = DescriptorPool::Builder(device)
                   .setMaxSets(framesInFlight * 2)
                   .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight * 2)
                   .addPoolSize(
                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, framesInFlight * 3)
                   .build();

        occlusionSets.resize(framesInFlight, VK_NULL_HANDLE);
        for (VkDescriptorSet& set : occlusionSets) {
            if (!pool->allocateDescriptor(occlusionSetLayout->getDescriptorSetLayout(), set)) {
                throw std::runtime_error{"failed to allocate an occlusion descriptor set"};
            }
        }
        blurSets.resize(framesInFlight, VK_NULL_HANDLE);
        for (VkDescriptorSet& set : blurSets) {
            if (!pool->allocateDescriptor(blurSetLayout->getDescriptorSetLayout(), set)) {
                throw std::runtime_error{"failed to allocate an occlusion blur descriptor set"};
            }
        }
    }

    void SsaoSystem::createPipelines(VkFormat occlusionFormat) {
        auto makeLayout = [&](VkDescriptorSetLayout setLayout, VkPipelineLayout& layout) {
            VkPipelineLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &setLayout;
            if (vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &layout) !=
                VK_SUCCESS) {
                throw std::runtime_error{"failed to create an occlusion pipeline layout"};
            }
        };

        makeLayout(occlusionSetLayout->getDescriptorSetLayout(), occlusionPipelineLayout);
        makeLayout(blurSetLayout->getDescriptorSetLayout(), blurPipelineLayout);

        PipelineConfigInfo config{};
        Pipeline::defaultPipelineConfigInfo(config);
        // Fullscreen triangles from gl_VertexIndex, so no vertex buffers and
        // no depth of their own - both passes read depth rather than test it.
        config.bindingDescriptions.clear();
        config.attributeDescriptions.clear();
        config.depthStencilInfo.depthTestEnable = VK_FALSE;
        config.depthStencilInfo.depthWriteEnable = VK_FALSE;
        config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
        config.colorAttachmentFormats = {occlusionFormat};
        config.depthAttachmentFormat = VK_FORMAT_UNDEFINED;

        config.pipelineLayout = occlusionPipelineLayout;
        occlusionPipeline =
            std::make_unique<Pipeline>(device, "fullscreen.vert.spv", "ssao.frag.spv", config);

        config.pipelineLayout = blurPipelineLayout;
        blurPipeline =
            std::make_unique<Pipeline>(device, "fullscreen.vert.spv", "ssao_blur.frag.spv", config);
    }

    void SsaoSystem::renderOcclusion(
        VkCommandBuffer commandBuffer,
        uint32_t frameIndex,
        const VkDescriptorBufferInfo& globalUbo,
        VkImageView depthView) {
        EGE_ASSERT(frameIndex < frames, "frame index out of range");

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = depthSampler;
        depthInfo.imageView = depthView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo noiseInfo = noiseTexture->descriptorInfo();
        VkDescriptorBufferInfo kernelInfo = kernelBuffer->descriptorInfo();

        // Written before it is bound and not touched again this frame, on the
        // same terms as every other per-frame set in the engine.
        VkDescriptorSet& set = occlusionSets[frameIndex];
        DescriptorWriter(*occlusionSetLayout, *pool)
            .writeBuffer(0, const_cast<VkDescriptorBufferInfo*>(&globalUbo))
            .writeImage(1, &depthInfo)
            .writeImage(2, &noiseInfo)
            .writeBuffer(3, &kernelInfo)
            .overwrite(set);

        occlusionPipeline->bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            occlusionPipelineLayout,
            0,
            1,
            &set,
            0,
            nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    void SsaoSystem::renderBlur(
        VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImageView occlusionView) {
        EGE_ASSERT(frameIndex < frames, "frame index out of range");

        VkDescriptorImageInfo occlusionInfo{};
        occlusionInfo.sampler = linearSampler;
        occlusionInfo.imageView = occlusionView;
        occlusionInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorSet& set = blurSets[frameIndex];
        DescriptorWriter(*blurSetLayout, *pool).writeImage(0, &occlusionInfo).overwrite(set);

        blurPipeline->bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            blurPipelineLayout,
            0,
            1,
            &set,
            0,
            nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

}  // namespace ege
