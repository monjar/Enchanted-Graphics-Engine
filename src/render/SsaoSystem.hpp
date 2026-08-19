#pragma once

#include "render/Ssao.hpp"
#include "rhi/Buffer.hpp"
#include "rhi/Descriptors.hpp"
#include "rhi/Pipeline.hpp"
#include "rhi/Texture.hpp"

#include <memory>
#include <vector>

namespace ege {

    // The two passes that turn the depth buffer into an occlusion map.
    //
    // The first estimates, per pixel, how much of its surroundings the surface
    // there can see; the second blurs that over one period of the rotation
    // pattern the first used. What comes out is sampled by the shading pass and
    // multiplied into the image-based ambient term - only the ambient term,
    // because a direct light either reaches a point or is blocked by a shadow
    // map that already knows.
    //
    // It owns its descriptor sets rather than borrowing the renderer's global
    // one, for the reason the light culling pass does: the scene pass writes
    // this frame's shadow maps into the global set, and updating a set an
    // earlier pass has already bound invalidates the whole command buffer.
    class SsaoSystem {
    public:
        SsaoSystem(Device& device, VkFormat occlusionFormat, uint32_t framesInFlight);
        ~SsaoSystem();

        SsaoSystem(const SsaoSystem&) = delete;
        SsaoSystem& operator=(const SsaoSystem&) = delete;

        // The estimate itself, reading the depth the pre-pass produced. That
        // depth is a frame graph transient, so which physical image backs it
        // is only known while the pass is recording.
        void renderOcclusion(
            VkCommandBuffer commandBuffer,
            uint32_t frameIndex,
            const VkDescriptorBufferInfo& globalUbo,
            VkImageView depthView);

        void renderBlur(
            VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImageView occlusionView);

        // How the shading pass samples the blurred result: linear, because it
        // is a smooth quantity read at pixel centres it was not necessarily
        // computed at.
        VkSampler resultSampler() const { return linearSampler; }

    private:
        void createSamplers();
        void createKernel();
        void createDescriptors(uint32_t framesInFlight);
        void createPipelines(VkFormat occlusionFormat);

        Device& device;
        uint32_t frames = 0;

        // Nearest for depth: a filtered depth is an average of two surfaces,
        // which is a surface that was never there. Linear for the occlusion
        // map, which really is smooth.
        VkSampler depthSampler = VK_NULL_HANDLE;
        VkSampler linearSampler = VK_NULL_HANDLE;

        // The sample offsets, generated once and uploaded once - they do not
        // depend on the camera or on anything in the scene.
        std::unique_ptr<Buffer> kernelBuffer;
        std::unique_ptr<Texture> noiseTexture;

        std::unique_ptr<DescriptorSetLayout> occlusionSetLayout;
        std::unique_ptr<DescriptorSetLayout> blurSetLayout;
        std::unique_ptr<DescriptorPool> pool;
        std::vector<VkDescriptorSet> occlusionSets;
        std::vector<VkDescriptorSet> blurSets;

        VkPipelineLayout occlusionPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout blurPipelineLayout = VK_NULL_HANDLE;
        std::unique_ptr<Pipeline> occlusionPipeline;
        std::unique_ptr<Pipeline> blurPipeline;
    };

}  // namespace ege
