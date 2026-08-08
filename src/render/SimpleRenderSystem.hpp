#pragma once

#include "render/Camera.hpp"
#include "render/FrameInfo.hpp"
#include "rhi/Pipeline.hpp"
#include "scene/GameObject.hpp"

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