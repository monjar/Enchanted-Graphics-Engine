#pragma once

#include "ege_window.hpp"
#include "ege_pipeline.hpp"
#include "ege_swap_chain.hpp"
#include "ege_model.hpp"
#include "ege_game_object.hpp"

#include <memory>

#include <vector>

namespace ege {

	class EnchantedEngine {
	public:
		static constexpr int WIDTH = 800, HEIGHT = 600;

		EnchantedEngine();
		~EnchantedEngine();



		//Delete copy constructor and operator1
		EnchantedEngine(const EnchantedEngine& other) = delete;
		EnchantedEngine& operator = (const EnchantedEngine&) = delete;


		void run();
	private:

		void loadGameObjects();
		void createPipelineLayout();
		void createPipeline();
		void createCommandBuffers();
		void freeCommandBuffers();
		void drawFrame();
		void recreateSwapChain();
		void recordCommandBuffer(int imageIndex);
		void renderGameObjects(VkCommandBuffer commandBuffer);


		EgeWindow egeWindow{ WIDTH, HEIGHT, "Hello World!" };
		EgeDevice egeDevice{ egeWindow };
		std::unique_ptr<EgeSwapChain> egeSwapChain;
		std::unique_ptr<EgePipeline> egePipeline;
		VkPipelineLayout pipelineLayout;
		std::vector<VkCommandBuffer> commandBuffers;
		std::vector<EgeGameObject> gameObjects;


	};
}