#pragma once

#include "ege_pipeline.hpp"
#include "ege_model.hpp"
#include "ege_game_object.hpp"


#include <memory>
#include <vector>

namespace ege {

	class SimpleRenderSystem {
	public:

		SimpleRenderSystem(EgeDevice& device, VkRenderPass renderPass);
		~SimpleRenderSystem();



		//Delete copy constructor and operator1
		SimpleRenderSystem(const SimpleRenderSystem& other) = delete;
		SimpleRenderSystem& operator = (const SimpleRenderSystem&) = delete;

		void renderGameObjects(VkCommandBuffer commandBuffer, std::vector<EgeGameObject> &gameObjects);

	private:

		void createPipelineLayout();
		void createPipeline(VkRenderPass renderPass);


		EgeDevice& egeDevice;

		std::unique_ptr<EgePipeline> egePipeline;
		VkPipelineLayout pipelineLayout;


	};
}