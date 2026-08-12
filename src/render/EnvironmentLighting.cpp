#include "render/EnvironmentLighting.hpp"

#include "core/Assert.hpp"
#include "core/Log.hpp"
#include "rhi/Pipeline.hpp"

#include <stdexcept>

namespace ege {

    namespace {

        // Radiance wants float precision; 16 bits per channel is the float
        // format with guaranteed color-attachment support.
        constexpr VkFormat mapFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

        // Sizes chosen for what each map stores. The environment carries the
        // sun so it gets resolution; irradiance is smooth by construction and
        // 16 pixels per face genuinely suffices; the prefiltered chain runs
        // 64 down to 4 across five roughness levels. Small enough that a CPU
        // rasterizer in CI finishes the whole precompute in about a second.
        constexpr uint32_t environmentSize = 256;
        constexpr uint32_t irradianceSize = 16;
        constexpr uint32_t prefilteredSize = 64;
        constexpr uint32_t prefilteredMips = 5;
        constexpr uint32_t brdfLutSize = 256;

        // Shared by all four generation shaders, whether they use both fields
        // or neither, so one pipeline layout serves every pass.
        struct GenerationPush {
            int32_t faceIndex = 0;
            float roughness = 0.f;
        };

        uint32_t fullMipCount(uint32_t size) {
            uint32_t mips = 1;
            while (size > 1) {
                size /= 2;
                mips++;
            }
            return mips;
        }

        VkImageMemoryBarrier2 imageBarrier(
            VkImage image,
            VkImageLayout oldLayout,
            VkImageLayout newLayout,
            VkPipelineStageFlags2 srcStage,
            VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dstStage,
            VkAccessFlags2 dstAccess,
            uint32_t baseMip,
            uint32_t mipCount,
            uint32_t layerCount) {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.image = image;
            barrier.subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, layerCount};
            return barrier;
        }

        void recordBarrier(VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2& barrier) {
            VkDependencyInfo dependencyInfo{};
            dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependencyInfo.imageMemoryBarrierCount = 1;
            dependencyInfo.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
        }

    }  // namespace

    EnvironmentLighting::EnvironmentLighting(Device& deviceRef) : device{deviceRef} {
        createSampler();
        createDescriptors();

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(GenerationPush);

        const VkDescriptorSetLayout setLayoutHandle = samplerSetLayout->getDescriptorSetLayout();

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayoutHandle;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &pipelineLayout) !=
            VK_SUCCESS) {
            throw std::runtime_error{"failed to create the IBL generation pipeline layout"};
        }

        environment = createCubeMap(
            environmentSize,
            fullMipCount(environmentSize),
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        irradiance = createCubeMap(
            irradianceSize, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        prefiltered = createCubeMap(
            prefilteredSize,
            prefilteredMips,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

        generateEnvironment();

        // The convolution passes read the finished environment.
        VkDescriptorImageInfo environmentImageInfo = environmentInfo();
        DescriptorWriter(*samplerSetLayout, *descriptorPool)
            .writeImage(0, &environmentImageInfo)
            .build(environmentSet);

        generateIrradiance();
        generatePrefiltered();
        generateBrdfLut();

        EGE_INFO(
            "Image-based lighting ready: environment {}px, irradiance {}px, "
            "prefiltered {}px x{} mips, BRDF LUT {}px",
            environmentSize,
            irradianceSize,
            prefilteredSize,
            prefilteredMips,
            brdfLutSize);
    }

    EnvironmentLighting::~EnvironmentLighting() {
        destroyCubeMap(environment);
        destroyCubeMap(irradiance);
        destroyCubeMap(prefiltered);

        if (brdfLutView != VK_NULL_HANDLE) {
            vkDestroyImageView(device.device(), brdfLutView, nullptr);
            device.destroyImage(brdfLut, brdfLutAllocation);
        }

        vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
        vkDestroySampler(device.device(), sampler, nullptr);
    }

    VkDescriptorImageInfo EnvironmentLighting::environmentInfo() const {
        return {sampler, environment.cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    VkDescriptorImageInfo EnvironmentLighting::irradianceInfo() const {
        return {sampler, irradiance.cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    VkDescriptorImageInfo EnvironmentLighting::prefilteredInfo() const {
        return {sampler, prefiltered.cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    VkDescriptorImageInfo EnvironmentLighting::brdfLutInfo() const {
        return {sampler, brdfLutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    EnvironmentLighting::CubeMap EnvironmentLighting::createCubeMap(
        uint32_t size, uint32_t mipLevels, VkImageUsageFlags usage) {
        CubeMap cube{};
        cube.size = size;
        cube.mipLevels = mipLevels;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {size, size, 1};
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 6;
        imageInfo.format = mapFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        device.createImageWithInfo(
            imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cube.image, cube.allocation);

        VkImageViewCreateInfo cubeViewInfo{};
        cubeViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        cubeViewInfo.image = cube.image;
        cubeViewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        cubeViewInfo.format = mapFormat;
        cubeViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 6};

        if (vkCreateImageView(device.device(), &cubeViewInfo, nullptr, &cube.cubeView) !=
            VK_SUCCESS) {
            throw std::runtime_error{"failed to create a cubemap view"};
        }

        cube.faceViews.resize(static_cast<size_t>(mipLevels) * 6);
        for (uint32_t mip = 0; mip < mipLevels; mip++) {
            for (uint32_t face = 0; face < 6; face++) {
                VkImageViewCreateInfo faceViewInfo{};
                faceViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                faceViewInfo.image = cube.image;
                faceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                faceViewInfo.format = mapFormat;
                faceViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, face, 1};

                if (vkCreateImageView(
                        device.device(), &faceViewInfo, nullptr, &cube.faceViews[mip * 6 + face]) !=
                    VK_SUCCESS) {
                    throw std::runtime_error{"failed to create a cubemap face view"};
                }
            }
        }

        return cube;
    }

    void EnvironmentLighting::destroyCubeMap(CubeMap& cube) {
        if (cube.image == VK_NULL_HANDLE) {
            return;
        }
        for (VkImageView view : cube.faceViews) {
            vkDestroyImageView(device.device(), view, nullptr);
        }
        vkDestroyImageView(device.device(), cube.cubeView, nullptr);
        device.destroyImage(cube.image, cube.allocation);
        cube = CubeMap{};
    }

    void EnvironmentLighting::createSampler() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the IBL sampler"};
        }
    }

    void EnvironmentLighting::createDescriptors() {
        samplerSetLayout =
            DescriptorSetLayout::Builder(device)
                .addBinding(
                    0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .build();
        descriptorPool = DescriptorPool::Builder(device)
                             .setMaxSets(1)
                             .addPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1)
                             .build();
    }

    namespace {

        // One fullscreen draw into one cube face (or a 2D target).
        void drawIntoView(
            VkCommandBuffer commandBuffer,
            VkImageView target,
            uint32_t size,
            const GenerationPush& push,
            VkPipelineLayout pipelineLayout) {
            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = target;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            // Every pixel is written, so nothing needs loading or clearing.
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = {{0, 0}, {size, size}};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;

            vkCmdBeginRendering(commandBuffer, &renderingInfo);

            VkViewport viewport{};
            viewport.width = static_cast<float>(size);
            viewport.height = static_cast<float>(size);
            viewport.maxDepth = 1.0f;
            VkRect2D scissor{{0, 0}, {size, size}};
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

            vkCmdPushConstants(
                commandBuffer,
                pipelineLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                0,
                sizeof(GenerationPush),
                &push);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            vkCmdEndRendering(commandBuffer);
        }

        void makeGenerationPipelineConfig(PipelineConfigInfo& config, VkPipelineLayout layout) {
            Pipeline::defaultPipelineConfigInfo(config);
            config.bindingDescriptions.clear();
            config.attributeDescriptions.clear();
            config.depthStencilInfo.depthTestEnable = VK_FALSE;
            config.depthStencilInfo.depthWriteEnable = VK_FALSE;
            config.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
            config.colorAttachmentFormats = {mapFormat};
            config.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
            config.pipelineLayout = layout;
        }

    }  // namespace

    void EnvironmentLighting::generateEnvironment() {
        PipelineConfigInfo config{};
        makeGenerationPipelineConfig(config, pipelineLayout);
        Pipeline skyPipeline{device, "fullscreen.vert.spv", "env_sky.frag.spv", config};

        VkCommandBuffer commandBuffer = device.beginSingleTimeCommands();

        recordBarrier(
            commandBuffer,
            imageBarrier(
                environment.image,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                0,
                1,
                6));

        skyPipeline.bind(commandBuffer);
        for (int face = 0; face < 6; face++) {
            GenerationPush push{};
            push.faceIndex = face;
            drawIntoView(
                commandBuffer,
                environment.faceViews[static_cast<size_t>(face)],
                environment.size,
                push,
                pipelineLayout);
        }

        // Mip chain by successive blits, six layers at a time. The prefilter
        // pass reads these mips to keep sparse sun samples from speckling.
        recordBarrier(
            commandBuffer,
            imageBarrier(
                environment.image,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                0,
                1,
                6));

        int32_t mipSize = static_cast<int32_t>(environment.size);
        for (uint32_t mip = 1; mip < environment.mipLevels; mip++) {
            recordBarrier(
                commandBuffer,
                imageBarrier(
                    environment.image,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE,
                    VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    mip,
                    1,
                    6));

            VkImageBlit2 blitRegion{};
            blitRegion.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
            blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 6};
            blitRegion.srcOffsets[1] = {mipSize, mipSize, 1};
            blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 6};
            blitRegion.dstOffsets[1] = {
                mipSize > 1 ? mipSize / 2 : 1, mipSize > 1 ? mipSize / 2 : 1, 1};

            VkBlitImageInfo2 blitInfo{};
            blitInfo.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
            blitInfo.srcImage = environment.image;
            blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blitInfo.dstImage = environment.image;
            blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blitInfo.regionCount = 1;
            blitInfo.pRegions = &blitRegion;
            blitInfo.filter = VK_FILTER_LINEAR;
            vkCmdBlitImage2(commandBuffer, &blitInfo);

            recordBarrier(
                commandBuffer,
                imageBarrier(
                    environment.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT,
                    mip,
                    1,
                    6));

            mipSize = mipSize > 1 ? mipSize / 2 : 1;
        }

        recordBarrier(
            commandBuffer,
            imageBarrier(
                environment.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                0,
                environment.mipLevels,
                6));

        device.endSingleTimeCommands(commandBuffer);
    }

    void EnvironmentLighting::generateIrradiance() {
        PipelineConfigInfo config{};
        makeGenerationPipelineConfig(config, pipelineLayout);
        Pipeline irradiancePipeline{
            device, "fullscreen.vert.spv", "env_irradiance.frag.spv", config};

        VkCommandBuffer commandBuffer = device.beginSingleTimeCommands();

        recordBarrier(
            commandBuffer,
            imageBarrier(
                irradiance.image,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                0,
                1,
                6));

        irradiancePipeline.bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            0,
            1,
            &environmentSet,
            0,
            nullptr);

        for (int face = 0; face < 6; face++) {
            GenerationPush push{};
            push.faceIndex = face;
            drawIntoView(
                commandBuffer,
                irradiance.faceViews[static_cast<size_t>(face)],
                irradiance.size,
                push,
                pipelineLayout);
        }

        recordBarrier(
            commandBuffer,
            imageBarrier(
                irradiance.image,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                0,
                1,
                6));

        device.endSingleTimeCommands(commandBuffer);
    }

    void EnvironmentLighting::generatePrefiltered() {
        PipelineConfigInfo config{};
        makeGenerationPipelineConfig(config, pipelineLayout);
        Pipeline prefilterPipeline{device, "fullscreen.vert.spv", "env_prefilter.frag.spv", config};

        VkCommandBuffer commandBuffer = device.beginSingleTimeCommands();

        recordBarrier(
            commandBuffer,
            imageBarrier(
                prefiltered.image,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                0,
                prefiltered.mipLevels,
                6));

        prefilterPipeline.bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            0,
            1,
            &environmentSet,
            0,
            nullptr);

        for (uint32_t mip = 0; mip < prefiltered.mipLevels; mip++) {
            const uint32_t mipSize = prefiltered.size >> mip;
            for (int face = 0; face < 6; face++) {
                GenerationPush push{};
                push.faceIndex = face;
                push.roughness =
                    static_cast<float>(mip) / static_cast<float>(prefiltered.mipLevels - 1);
                drawIntoView(
                    commandBuffer,
                    prefiltered.faceViews[mip * 6 + static_cast<uint32_t>(face)],
                    mipSize,
                    push,
                    pipelineLayout);
            }
        }

        recordBarrier(
            commandBuffer,
            imageBarrier(
                prefiltered.image,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                0,
                prefiltered.mipLevels,
                6));

        device.endSingleTimeCommands(commandBuffer);
    }

    void EnvironmentLighting::generateBrdfLut() {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {brdfLutSize, brdfLutSize, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = mapFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        device.createImageWithInfo(
            imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, brdfLut, brdfLutAllocation);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = brdfLut;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = mapFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(device.device(), &viewInfo, nullptr, &brdfLutView) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the BRDF LUT view"};
        }

        PipelineConfigInfo config{};
        makeGenerationPipelineConfig(config, pipelineLayout);
        Pipeline brdfPipeline{device, "fullscreen.vert.spv", "env_brdf_lut.frag.spv", config};

        VkCommandBuffer commandBuffer = device.beginSingleTimeCommands();

        recordBarrier(
            commandBuffer,
            imageBarrier(
                brdfLut,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                0,
                1,
                1));

        brdfPipeline.bind(commandBuffer);
        drawIntoView(commandBuffer, brdfLutView, brdfLutSize, GenerationPush{}, pipelineLayout);

        recordBarrier(
            commandBuffer,
            imageBarrier(
                brdfLut,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                0,
                1,
                1));

        device.endSingleTimeCommands(commandBuffer);
    }

}  // namespace ege
