#pragma once

#include "render/FrameInfo.hpp"
#include "render/Material.hpp"
#include "rhi/Pipeline.hpp"

#include <memory>

namespace ege {

    // Draws the scene with the metallic-roughness PBR shader.
    //
    // Replaces SimpleRenderSystem, whose shader had a single hardcoded point
    // light, no textures, Lambert diffuse and no specular term at all.
    class PbrRenderSystem {
    public:
        PbrRenderSystem(
            Device& device,
            VkRenderPass renderPass,
            VkDescriptorSetLayout globalSetLayout,
            VkDescriptorSetLayout materialSetLayout);
        ~PbrRenderSystem();

        PbrRenderSystem(const PbrRenderSystem&) = delete;
        PbrRenderSystem& operator=(const PbrRenderSystem&) = delete;

        void render(FrameInfo& frameInfo);

    private:
        void createPipelineLayout(
            VkDescriptorSetLayout globalSetLayout, VkDescriptorSetLayout materialSetLayout);
        void createPipeline(VkRenderPass renderPass);

        Device& device;

        std::unique_ptr<Pipeline> pipeline;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    };

}  // namespace ege
