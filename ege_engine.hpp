#pragma once

#include "ege_window.hpp"
#include "ege_pipeline.hpp"
#include "ege_swap_chain.hpp"
#include "ege_model.hpp"


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

		void loadModels();
		void createPipelineLayout();
		void createPipeline();
		void createCommandBuffers();
		void drawFrame();


		EgeWindow egeWindow{ WIDTH, HEIGHT, "Hello World!" };
		EgeDevice egeDevice{ egeWindow };
		EgeSwapChain egeSwapChain{ egeDevice, egeWindow.getExtent() };
		std::unique_ptr<EgePipeline> egePipeline;
		VkPipelineLayout pipelineLayout;
		std::vector<VkCommandBuffer> commandBuffers;
		std::unique_ptr<EgeModel> egeModel;


	};
}