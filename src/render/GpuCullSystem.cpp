#include "render/GpuCullSystem.hpp"

#include "core/Assert.hpp"
#include "render/PbrRenderSystem.hpp"

#include <cstring>
#include <stdexcept>

namespace ege {

    namespace {

        constexpr uint32_t workgroupSize = 64;

        uint32_t dispatchGroups(uint32_t invocations) {
            return (invocations + workgroupSize - 1) / workgroupSize;
        }

    }  // namespace

    GpuCullSystem::GpuCullSystem(Device& deviceRef, uint32_t framesInFlight) : device{deviceRef} {
        createBuffers(framesInFlight);
        createDescriptors(framesInFlight);
        createPipelines();

        counts.assign(framesInFlight, 0);
        batchCounts.assign(framesInFlight, 0);
    }

    GpuCullSystem::~GpuCullSystem() {
        vkDestroySampler(device.device(), levelSampler, nullptr);
        vkDestroyPipelineLayout(device.device(), earlyPipelineLayout, nullptr);
        vkDestroyPipelineLayout(device.device(), latePipelineLayout, nullptr);
    }

    void GpuCullSystem::createBuffers(uint32_t framesInFlight) {
        uniforms.reserve(framesInFlight);
        cullInputs.reserve(framesInFlight);
        drawCommands.reserve(framesInFlight);
        stats.reserve(framesInFlight);
        compacted.reserve(framesInFlight);

        for (uint32_t i = 0; i < framesInFlight; i++) {
            uniforms.push_back(std::make_unique<Buffer>(
                device,
                sizeof(CullUniform),
                1,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
            uniforms.back()->map();

            cullInputs.push_back(std::make_unique<Buffer>(
                device,
                sizeof(GpuCullInput),
                maxDrawInstances,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
            cullInputs.back()->map();

            // Storage for the compute passes' atomics, indirect for the draws
            // that consume the result - the whole point of the buffer is that
            // it is both.
            drawCommands.push_back(std::make_unique<Buffer>(
                device,
                sizeof(uint32_t),
                maxDrawInstances * 2 * drawCommandWords,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
            drawCommands.back()->map();

            // Written by the late pass, read back here two frames later, when
            // the fence that recycled the index has proven the writes done.
            stats.push_back(std::make_unique<Buffer>(
                device,
                sizeof(GpuCullStatsData),
                1,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
            stats.back()->map();

            // Two whole windows: the early pass compacts into the first, the
            // late pass into the second, and neither needs the other's count
            // to know where its own begins.
            compacted.push_back(std::make_unique<Buffer>(
                device,
                sizeof(GpuInstance),
                maxDrawInstances * 2,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        }

        visibility = std::make_unique<Buffer>(
            device,
            sizeof(uint32_t),
            maxDrawInstances,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        // "Nothing was visible last frame" - so the first frame's early pass
        // draws nothing, its pyramid stays empty, and the late pass admits
        // everything against it. Correct from the first pixel, and settled by
        // the second frame. Also the only reason the buffer needs a transfer
        // usage bit.
        VkCommandBuffer commandBuffer = device.beginSingleTimeCommands();
        vkCmdFillBuffer(commandBuffer, visibility->getBuffer(), 0, VK_WHOLE_SIZE, 0);
        device.endSingleTimeCommands(commandBuffer);
    }

    void GpuCullSystem::createDescriptors(uint32_t framesInFlight) {
        // Nearest and clamped, exactly as the reduce chain samples: a
        // filtered depth is an average of two surfaces, which is a surface
        // that was never there.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &levelSampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the culling level sampler"};
        }

        DescriptorSetLayout::Builder earlyBuilder{device};
        earlyBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
        earlyLayout = earlyBuilder.build();

        // The late layout is the early one plus what only the late pass
        // touches: the stats it counts into and the pyramid it rules against.
        DescriptorSetLayout::Builder lateBuilder{device};
        lateBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
            .addBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
        for (uint32_t level = 0; level < gpuCullBoundLevels; level++) {
            lateBuilder.addBinding(
                7 + level, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
        }
        lateLayout = lateBuilder.build();

        pool =
            DescriptorPool::Builder(device)
                .setMaxSets(framesInFlight * 2)
                .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight * 2)
                .addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, framesInFlight * 12)
                .addPoolSize(
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, framesInFlight * gpuCullBoundLevels)
                .build();

        earlySets.resize(framesInFlight);
        lateSets.resize(framesInFlight);
        for (uint32_t i = 0; i < framesInFlight; i++) {
            if (!pool->allocateDescriptor(earlyLayout->getDescriptorSetLayout(), earlySets[i]) ||
                !pool->allocateDescriptor(lateLayout->getDescriptorSetLayout(), lateSets[i])) {
                throw std::runtime_error{"failed to allocate a culling descriptor set"};
            }
        }
    }

    void GpuCullSystem::createPipelines() {
        VkDescriptorSetLayout earlyRaw = earlyLayout->getDescriptorSetLayout();
        VkPipelineLayoutCreateInfo earlyInfo{};
        earlyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        earlyInfo.setLayoutCount = 1;
        earlyInfo.pSetLayouts = &earlyRaw;
        if (vkCreatePipelineLayout(device.device(), &earlyInfo, nullptr, &earlyPipelineLayout) !=
            VK_SUCCESS) {
            throw std::runtime_error{"failed to create the early culling pipeline layout"};
        }

        VkDescriptorSetLayout lateRaw = lateLayout->getDescriptorSetLayout();
        VkPipelineLayoutCreateInfo lateInfo{};
        lateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lateInfo.setLayoutCount = 1;
        lateInfo.pSetLayouts = &lateRaw;
        if (vkCreatePipelineLayout(device.device(), &lateInfo, nullptr, &latePipelineLayout) !=
            VK_SUCCESS) {
            throw std::runtime_error{"failed to create the late culling pipeline layout"};
        }

        earlyPipeline = std::make_unique<ComputePipeline>(
            device, "gpu_cull_early.comp.spv", earlyPipelineLayout);
        latePipeline =
            std::make_unique<ComputePipeline>(device, "gpu_cull_late.comp.spv", latePipelineLayout);
    }

    void GpuCullSystem::setLevelExtents(const std::array<glm::uvec2, gpuCullBoundLevels>& extents) {
        boundExtents = extents;
    }

    void GpuCullSystem::feed(
        uint32_t frameIndex,
        const std::vector<GpuCullInput>& candidates,
        const std::vector<SeedBatch>& batches,
        const glm::mat4& view,
        const glm::mat4& projection,
        bool occlusionEnabled) {
        EGE_ASSERT(frameIndex < counts.size(), "frame index out of range");
        EGE_ASSERT(candidates.size() <= maxDrawInstances, "prepare() bounds the draw list");

        // What the GPU counted the last time this index ran, before this run
        // resets the counters underneath it.
        std::memcpy(&collected, stats[frameIndex]->getMappedMemory(), sizeof(collected));
        const GpuCullStatsData zeroed{};
        stats[frameIndex]->writeToBuffer(
            const_cast<GpuCullStatsData*>(&zeroed), sizeof(GpuCullStatsData));
        stats[frameIndex]->flush();

        counts[frameIndex] = static_cast<uint32_t>(candidates.size());
        batchCounts[frameIndex] = static_cast<uint32_t>(batches.size());
        if (candidates.empty()) {
            return;
        }

        cullInputs[frameIndex]->writeToBuffer(
            const_cast<GpuCullInput*>(candidates.data()), candidates.size() * sizeof(GpuCullInput));
        cullInputs[frameIndex]->flush();

        const std::vector<uint32_t> seeded = seedDrawCommands(batches, maxDrawInstances);
        drawCommands[frameIndex]->writeToBuffer(
            const_cast<uint32_t*>(seeded.data()), seeded.size() * sizeof(uint32_t));
        drawCommands[frameIndex]->flush();

        CullUniform uniform{};
        uniform.view = view;
        uniform.projection = projection;
        // The projection's own depth terms, and the near plane recovered from
        // them: depth reaches zero exactly where z = -offset / scale.
        const float depthScale = projection[2][2];
        const float depthOffset = projection[3][2];
        uniform.depthTerms = glm::vec4{
            depthScale,
            depthOffset,
            depthScale != 0.f ? -depthOffset / depthScale : 0.f,
            occlusionEnabled ? 1.f : 0.f};
        uniform.counts =
            glm::uvec4{counts[frameIndex], batchCounts[frameIndex], maxDrawInstances, 0};
        for (uint32_t level = 0; level < gpuCullBoundLevels; level++) {
            uniform.levelExtents[level] = glm::ivec4{
                static_cast<int>(boundExtents[level].x),
                static_cast<int>(boundExtents[level].y),
                0,
                0};
        }
        uniforms[frameIndex]->writeToBuffer(&uniform, sizeof(uniform));
        uniforms[frameIndex]->flush();
    }

    void GpuCullSystem::recordEarly(
        VkCommandBuffer commandBuffer,
        uint32_t frameIndex,
        const VkDescriptorBufferInfo& candidates) {
        if (counts[frameIndex] == 0) {
            return;
        }

        VkDescriptorBufferInfo uniformInfo = uniforms[frameIndex]->descriptorInfo();
        VkDescriptorBufferInfo inputsInfo = cullInputs[frameIndex]->descriptorInfo();
        VkDescriptorBufferInfo compactedInfo = compacted[frameIndex]->descriptorInfo();
        VkDescriptorBufferInfo commandsInfo = drawCommands[frameIndex]->descriptorInfo();
        VkDescriptorBufferInfo visibilityInfo = visibility->descriptorInfo();

        // Written before it is bound and untouched after, on the same terms
        // every per-frame set in this engine is.
        DescriptorWriter(*earlyLayout, *pool)
            .writeBuffer(0, &uniformInfo)
            .writeBuffer(1, &inputsInfo)
            .writeBuffer(2, const_cast<VkDescriptorBufferInfo*>(&candidates))
            .writeBuffer(3, &compactedInfo)
            .writeBuffer(4, &commandsInfo)
            .writeBuffer(5, &visibilityInfo)
            .overwrite(earlySets[frameIndex]);

        earlyPipeline->bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            earlyPipelineLayout,
            0,
            1,
            &earlySets[frameIndex],
            0,
            nullptr);
        vkCmdDispatch(commandBuffer, dispatchGroups(counts[frameIndex]), 1, 1);
    }

    void GpuCullSystem::recordLate(
        VkCommandBuffer commandBuffer,
        uint32_t frameIndex,
        const VkDescriptorBufferInfo& candidates,
        const std::array<VkImageView, gpuCullBoundLevels>& levels) {
        if (counts[frameIndex] == 0) {
            return;
        }

        VkDescriptorBufferInfo uniformInfo = uniforms[frameIndex]->descriptorInfo();
        VkDescriptorBufferInfo inputsInfo = cullInputs[frameIndex]->descriptorInfo();
        VkDescriptorBufferInfo compactedInfo = compacted[frameIndex]->descriptorInfo();
        VkDescriptorBufferInfo commandsInfo = drawCommands[frameIndex]->descriptorInfo();
        VkDescriptorBufferInfo visibilityInfo = visibility->descriptorInfo();
        VkDescriptorBufferInfo statsInfo = stats[frameIndex]->descriptorInfo();

        std::array<VkDescriptorImageInfo, gpuCullBoundLevels> levelInfos{};
        for (uint32_t level = 0; level < gpuCullBoundLevels; level++) {
            levelInfos[level].sampler = levelSampler;
            levelInfos[level].imageView = levels[level];
            levelInfos[level].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        DescriptorWriter writer{*lateLayout, *pool};
        writer.writeBuffer(0, &uniformInfo)
            .writeBuffer(1, &inputsInfo)
            .writeBuffer(2, const_cast<VkDescriptorBufferInfo*>(&candidates))
            .writeBuffer(3, &compactedInfo)
            .writeBuffer(4, &commandsInfo)
            .writeBuffer(5, &visibilityInfo)
            .writeBuffer(6, &statsInfo);
        for (uint32_t level = 0; level < gpuCullBoundLevels; level++) {
            writer.writeImage(7 + level, &levelInfos[level]);
        }
        writer.overwrite(lateSets[frameIndex]);

        latePipeline->bind(commandBuffer);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            latePipelineLayout,
            0,
            1,
            &lateSets[frameIndex],
            0,
            nullptr);
        vkCmdDispatch(commandBuffer, dispatchGroups(counts[frameIndex]), 1, 1);
    }

}  // namespace ege
