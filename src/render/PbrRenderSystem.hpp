#pragma once

#include "render/Bounds.hpp"
#include "render/FrameInfo.hpp"
#include "render/Material.hpp"
#include "render/Model.hpp"
#include "rhi/Pipeline.hpp"

#include <memory>
#include <vector>

namespace ege {

    // Draws the scene with the metallic-roughness PBR shader.
    //
    // Replaces SimpleRenderSystem, whose shader had a single hardcoded point
    // light, no textures, Lambert diffuse and no specular term at all.
    class PbrRenderSystem {
    public:
        PbrRenderSystem(
            Device& device,
            VkFormat colorFormat,
            VkFormat depthFormat,
            VkDescriptorSetLayout globalSetLayout,
            VkDescriptorSetLayout materialSetLayout,
            VkSampleCountFlagBits samples);
        ~PbrRenderSystem();

        PbrRenderSystem(const PbrRenderSystem&) = delete;
        PbrRenderSystem& operator=(const PbrRenderSystem&) = delete;

        void render(FrameInfo& frameInfo);

        // Draw statistics for the frame just submitted.
        struct Stats {
            std::size_t candidates = 0;
            std::size_t culled = 0;
            std::size_t drawn = 0;
            std::size_t materialBinds = 0;
        };

        const Stats& stats() const { return frameStats; }

        // Culling can be turned off to check that it is not wrongly discarding
        // something - the first thing to try when geometry goes missing.
        bool cullingEnabled = true;

    private:
        void createPipelineLayout(
            VkDescriptorSetLayout globalSetLayout, VkDescriptorSetLayout materialSetLayout);
        void createPipeline(
            VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples);

        Device& device;

        // One entry per visible object, sorted before submission.
        struct DrawItem {
            VkDescriptorSet materialSet = VK_NULL_HANDLE;
            const Material* material = nullptr;
            const Model* model = nullptr;
            glm::mat4 modelMatrix{1.f};
            glm::mat4 normalMatrix{1.f};
        };

        std::vector<DrawItem> drawList;
        Stats frameStats{};

        std::unique_ptr<Pipeline> pipeline;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    };

}  // namespace ege
