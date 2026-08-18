#pragma once

#include "rhi/Pipeline.hpp"

#include <memory>

namespace ege {

    // Draws the environment map behind everything else.
    //
    // Not a cube mesh: a fullscreen triangle pinned to the far plane, with
    // the view ray reconstructed per pixel from the inverse matrices already
    // in the global UBO. The LESS_OR_EQUAL depth test against the scene's
    // depth buffer means only sky pixels ever run the shader, which is why
    // this draws after the opaque geometry rather than before it.
    class SkyboxSystem {
    public:
        SkyboxSystem(
            Device& device,
            VkFormat colorFormat,
            VkFormat depthFormat,
            VkDescriptorSetLayout globalSetLayout,
            VkSampleCountFlagBits samples);
        ~SkyboxSystem();

        SkyboxSystem(const SkyboxSystem&) = delete;
        SkyboxSystem& operator=(const SkyboxSystem&) = delete;

        void render(VkCommandBuffer commandBuffer, VkDescriptorSet globalDescriptorSet);

    private:
        Device& device;

        std::unique_ptr<Pipeline> pipeline;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    };

}  // namespace ege
