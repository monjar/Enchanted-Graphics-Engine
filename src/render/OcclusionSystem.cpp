#include "render/OcclusionSystem.hpp"

#include "core/Assert.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <cstring>
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
        } while (std::max(current.width, current.height) > occlusionPyramidMaxSize &&
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

        targets.resize(framesInFlight);
    }

    OcclusionSystem::~OcclusionSystem() {
        destroyTargets();
        vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
        vkDestroySampler(device.device(), sampler, nullptr);
    }

    void OcclusionSystem::destroyTargets() {
        for (Target& target : targets) {
            target.readback.reset();
            if (target.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device.device(), target.view, nullptr);
                target.view = VK_NULL_HANDLE;
            }
            if (target.image != VK_NULL_HANDLE) {
                device.destroyImage(target.image, target.allocation);
                target.image = VK_NULL_HANDLE;
                target.allocation = VK_NULL_HANDLE;
            }
            target.written = false;
        }
    }

    void OcclusionSystem::resize(VkExtent2D renderExtent) {
        if (renderExtent.width == 0 || renderExtent.height == 0) {
            return;
        }

        // What matters is the size of the level that comes back, not the size
        // of the frame. Halving four or five times means a whole range of
        // frame sizes shares one pyramid size, so dragging an editor panel
        // crosses far fewer of these boundaries than it does frame sizes.
        const VkExtent2D wanted = stepExtent(renderExtent, reductionSteps(renderExtent) - 1u);
        if (wanted.width == pyramidExtent.width && wanted.height == pyramidExtent.height) {
            return;
        }

        // The images may still be referenced by a command buffer that has not
        // retired, so this waits rather than tracking which. That is a stall,
        // and the reason it is acceptable is the paragraph above: it happens
        // when the pyramid's own size changes, not on every frame of a drag.
        // The editor's viewport image retires its old copies on a delay
        // instead, which is what to copy if this ever becomes noticeable.
        device.waitIdle();
        destroyTargets();
        // Everything read back so far described a frame at the old size.
        current = OcclusionSnapshot{};

        pyramidExtent = wanted;

        for (Target& target : targets) {
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent = {pyramidExtent.width, pyramidExtent.height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = levelFormat;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            device.createImageWithInfo(
                imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, target.image, target.allocation);

            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = target.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = levelFormat;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            if (vkCreateImageView(device.device(), &viewInfo, nullptr, &target.view) !=
                VK_SUCCESS) {
                throw std::runtime_error{"failed to create a depth pyramid view"};
            }

            // Coherent as well as visible, so that waiting on the frame's
            // fence is all it takes for the floats to be readable.
            target.readback = std::make_unique<Buffer>(
                device,
                static_cast<VkDeviceSize>(pyramidExtent.width) * pyramidExtent.height *
                    sizeof(float),
                1,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            target.readback->map();
        }

        EGE_DEBUG(
            "Occlusion pyramid: {} reduction(s) from {}x{} down to {}x{}",
            reductionSteps(renderExtent),
            renderExtent.width,
            renderExtent.height,
            pyramidExtent.width,
            pyramidExtent.height);
    }

    void OcclusionSystem::collect(uint32_t frameIndex, const glm::mat4& viewProjection) {
        EGE_ASSERT(frameIndex < targets.size(), "frame index out of range");
        Target& target = targets[frameIndex];

        current.valid = false;

        if (!enabled || target.readback == nullptr) {
            current.pyramid.clear();
            target.written = false;
            return;
        }

        // Whatever is in this buffer was written by the frame that used this
        // index last, and its fence has been waited on - so the floats are
        // there and complete. Two frames old, which is what the test has to
        // account for and why the camera that captured it travels with it.
        if (target.written) {
            const std::size_t count =
                static_cast<std::size_t>(pyramidExtent.width) * pyramidExtent.height;
            scratch.resize(count);
            std::memcpy(scratch.data(), target.readback->getMappedMemory(), count * sizeof(float));

            current.pyramid.build(scratch.data(), pyramidExtent.width, pyramidExtent.height);
            current.viewProjection = target.viewProjection;
            current.valid = true;
        }

        // This frame's camera, kept until the frame that reads this frame's
        // pyramid comes round. Cleared rather than set: the copy at the end of
        // the frame is what says a result is really there.
        target.viewProjection = viewProjection;
        target.written = false;
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

    void OcclusionSystem::recordCopy(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
        EGE_ASSERT(frameIndex < targets.size(), "frame index out of range");
        Target& target = targets[frameIndex];
        if (!enabled || target.readback == nullptr) {
            return;
        }

        // The graph left the image in TRANSFER_SRC_OPTIMAL, which is what it
        // was imported asking for - the same handover the frame recorder gets
        // for the swapchain image.
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {pyramidExtent.width, pyramidExtent.height, 1};

        vkCmdCopyImageToBuffer(
            commandBuffer,
            target.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            target.readback->getBuffer(),
            1,
            &region);

        // Makes the copy available to the host. Signalling the frame's fence
        // would be enough on its own for coherent memory, but saying so costs
        // nothing and is what the next frame at this index relies on.
        VkBufferMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = target.readback->getBuffer();
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);

        target.written = true;
    }

}  // namespace ege
