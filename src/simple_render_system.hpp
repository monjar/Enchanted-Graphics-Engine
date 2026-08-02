#pragma once

#include "ege_camera.hpp"
#include "ege_frame_info.hpp"
#include "ege_game_object.hpp"
#include "ege_pipeline.hpp"

#include <memory>
#include <vector>

namespace ege {

    class SimpleRenderSystem {
    public:
        SimpleRenderSystem(
            EgeDevice& device, VkRenderPass renderPass, VkDescriptorSetLayout globalSetLayout);
        ~SimpleRenderSystem();

        // Delete copy constructor and operator1
        SimpleRenderSystem(const SimpleRenderSystem& other) = delete;
        SimpleRenderSystem& operator=(const SimpleRenderSystem&) = delete;

        void renderGameObjects(FrameInfo& frameInfo);

    private:
        void createPipelineLayout(VkDescriptorSetLayout globalSetLayout);
        void createPipeline(VkRenderPass renderPass);

        EgeDevice& egeDevice;

        std::unique_ptr<EgePipeline> egePipeline;
        VkPipelineLayout pipelineLayout;
    };
}  // namespace ege