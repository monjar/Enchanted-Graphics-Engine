#pragma once

#include "rhi/Descriptors.hpp"
#include "rhi/Pipeline.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace ege {

    // Extracts what glows and blurs it: a soft-thresholded bright-pass at
    // half resolution, then a separable Gaussian, each stage a frame graph
    // pass reading the previous stage's transient. The composite back onto
    // the frame happens in the tonemap shader, in linear light.
    class BloomSystem {
    public:
        BloomSystem(Device& device, VkFormat format, uint32_t framesInFlight);
        ~BloomSystem();

        BloomSystem(const BloomSystem&) = delete;
        BloomSystem& operator=(const BloomSystem&) = delete;

        // Each records one fullscreen draw reading sourceView. The blur
        // passes share a pipeline; the direction picks the axis.
        void renderBrightPass(
            VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImageView sourceView);
        void renderBlur(
            VkCommandBuffer commandBuffer,
            uint32_t frameIndex,
            VkImageView sourceView,
            glm::vec2 direction);

    private:
        // Stages that need their own per-frame descriptor set, because each
        // reads a different image within the same frame.
        enum Stage { stageBright = 0, stageBlurH = 1, stageBlurV = 2, stageCount = 3 };

        void draw(
            VkCommandBuffer commandBuffer,
            Pipeline& pipeline,
            uint32_t setIndex,
            VkImageView sourceView,
            const void* pushData,
            uint32_t pushSize);

        Device& device;

        std::unique_ptr<DescriptorSetLayout> setLayout;
        std::unique_ptr<DescriptorPool> pool;
        // Indexed stage * framesInFlight + frameIndex.
        std::vector<VkDescriptorSet> descriptorSets;
        uint32_t frames = 0;

        VkSampler sampler = VK_NULL_HANDLE;
        std::unique_ptr<Pipeline> brightPipeline;
        std::unique_ptr<Pipeline> blurPipeline;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    };

}  // namespace ege
