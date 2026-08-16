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

        // Per face of a point light's cube. Far smaller than the sun's,
        // because there are six of them per light and a point light lights a
        // small neighbourhood rather than the whole scene - so each face
        // covers a fraction of the world the sun's map has to.
        static constexpr uint32_t pointResolution = 512;

        ShadowMapSystem(Device& device, VkFormat depthFormat);
        ~ShadowMapSystem();

        ShadowMapSystem(const ShadowMapSystem&) = delete;
        ShadowMapSystem& operator=(const ShadowMapSystem&) = delete;

        // Records every mesh into the current depth attachment. No culling:
        // the sun's frustum is sized to the scene, so everything casts.
        void render(VkCommandBuffer commandBuffer, World& world, const glm::mat4& lightViewProj);

        // The comparison sampler the lighting shader reads the sun's cascades
        // through.
        VkSampler comparisonSampler() const { return shadowSampler; }

        // The one for point lights' cubes. Separate because it clamps to edge
        // rather than to a border: a cube has no outside to fall off into, and
        // a border colour at a face edge would draw a seam along it.
        VkSampler cubeComparisonSampler() const { return cubeSampler; }

    private:
        Device& device;

        std::unique_ptr<Pipeline> pipeline;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkSampler shadowSampler = VK_NULL_HANDLE;
        VkSampler cubeSampler = VK_NULL_HANDLE;
    };

}  // namespace ege
