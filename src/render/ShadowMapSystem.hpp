#pragma once

#include "rhi/Pipeline.hpp"
#include "scene/World.hpp"

#include <glm/glm.hpp>

#include <memory>

namespace ege {

    // Renders the scene's depth from the sun's point of view.
    //
    // Depth-only: no color attachment, no descriptor sets - each draw pushes
    // the light view-projection premultiplied with its model matrix. The
    // frame graph owns the shadow map image itself; this system only records
    // draws into whatever depth attachment the graph has bound.
    class ShadowMapSystem {
    public:
        // The shadow map's side length. A frame graph transient with an
        // absolute extent, so it is independent of window size.
        static constexpr uint32_t resolution = 2048;

        ShadowMapSystem(Device& device, VkFormat depthFormat);
        ~ShadowMapSystem();

        ShadowMapSystem(const ShadowMapSystem&) = delete;
        ShadowMapSystem& operator=(const ShadowMapSystem&) = delete;

        // Records every mesh into the current depth attachment. No culling:
        // the sun's frustum is sized to the scene, so everything casts.
        void render(VkCommandBuffer commandBuffer, World& world, const glm::mat4& lightViewProj);

        // The comparison sampler the lighting shader reads the map through.
        VkSampler comparisonSampler() const { return shadowSampler; }

    private:
        Device& device;

        std::unique_ptr<Pipeline> pipeline;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkSampler shadowSampler = VK_NULL_HANDLE;
    };

}  // namespace ege
