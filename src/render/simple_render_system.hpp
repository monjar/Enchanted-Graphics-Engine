#pragma once

#include "render/ege_camera.hpp"
#include "render/ege_frame_info.hpp"
#include "rhi/ege_pipeline.hpp"
#include "scene/ege_game_object.hpp"

#include <memory>
#include <vector>

namespace ege {

    class SimpleRenderSystem {
    public:
        SimpleRenderSystem(
            Device& device, VkRenderPass renderPass, VkDescriptorSetLayout globalSetLayout);
        ~SimpleRenderSystem();

        // Delete copy constructor and operator1
        SimpleRenderSystem(const SimpleRenderSystem& other) = delete;
        SimpleRenderSystem& operator=(const SimpleRenderSystem&) = delete;

        void renderGameObjects(FrameInfo& frameInfo);

    private:
        void createPipelineLayout(VkDescriptorSetLayout globalSetLayout);
        void createPipeline(VkRenderPass renderPass);

        Device& device;

        std::unique_ptr<Pipeline> pipeline;
        VkPipelineLayout pipelineLayout;
    };
}  // namespace ege