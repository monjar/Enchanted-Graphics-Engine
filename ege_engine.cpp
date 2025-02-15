#include "ege_engine.hpp"
#include <stdexcept>
namespace ege {



	EnchantedEngine::EnchantedEngine() {
		createPipelineLayout();
		createPipeline();
		createCommandBuffers();
	}


	EnchantedEngine::~EnchantedEngine() {
		vkDestroyPipelineLayout(egeDevice.device(), pipelineLayout, nullptr);
	}


	void EnchantedEngine::run() {
		while (!egeWindow.shouldClose()) {
			glfwPollEvents();
		}

	}


	void EnchantedEngine::createPipelineLayout() {
		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 0;
		pipelineLayoutInfo.pSetLayouts = nullptr;

		pipelineLayoutInfo.pushConstantRangeCount = 0;
		pipelineLayoutInfo.pPushConstantRanges = nullptr;

		if (vkCreatePipelineLayout(egeDevice.device(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
			throw std::runtime_error("Failed to Create Pipeline Layout!");
		}
	}


	void EnchantedEngine::createPipeline() {
		// getting width from swap chain as it doesnt necessarily match the screen size. eg in mac Retina display
		auto pipelineConfig = EgePipeline::defaultPipelineConfigInfo(egeSwapChain.width(), egeSwapChain.height());

		pipelineConfig.renderPass = egeSwapChain.getRenderPass();
		pipelineConfig.pipelineLayout = pipelineLayout;
		egePipeline = std::make_unique<EgePipeline>(
			egeDevice, "CompiledShaders/simple_shader.vert.spv", "CompiledShaders/simple_shader.frag.spv", pipelineConfig
		);

	}


	void EnchantedEngine::createCommandBuffers() {};
	void EnchantedEngine::drawFrame() {};


}