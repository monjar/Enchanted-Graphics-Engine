#pragma once

#include "rhi/Descriptors.hpp"
#include "rhi/Pipeline.hpp"

#include <memory>
#include <vector>

namespace ege {

    // The display transform: samples the HDR scene target and writes the
    // tonemapped result to the backbuffer with a fullscreen triangle.
    //
    // This is the seed of the post-processing stack. Bloom, exposure and
    // anti-aliasing all slot in as further frame graph passes between the
    // scene and this one; what stays fixed is that this pass is where linear
    // radiance becomes display color, and nothing downstream of it should do
    // lighting math.
    class PostProcessSystem {
    public:
        PostProcessSystem(Device& device, VkFormat outputFormat, uint32_t framesInFlight);
        ~PostProcessSystem();

        PostProcessSystem(const PostProcessSystem&) = delete;
        PostProcessSystem& operator=(const PostProcessSystem&) = delete;

        // Records the fullscreen pass. The scene view is bound fresh each
        // frame because the frame graph may hand back a different physical
        // image after a resize; the per-frame descriptor set makes that safe
        // while earlier frames are still in flight.
        void render(VkCommandBuffer commandBuffer, uint32_t frameIndex, VkImageView sceneColorView);

    private:
        void createSampler();
        void createDescriptors(uint32_t framesInFlight);
        void createPipelineLayout();
        void createPipeline(VkFormat outputFormat);

        Device& device;

        std::unique_ptr<DescriptorSetLayout> setLayout;
        std::unique_ptr<DescriptorPool> pool;
        std::vector<VkDescriptorSet> descriptorSets;

        VkSampler sampler = VK_NULL_HANDLE;
        std::unique_ptr<Pipeline> pipeline;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    };

}  // namespace ege
