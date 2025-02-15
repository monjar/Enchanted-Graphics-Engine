#include "ege_engine.hpp"
#include <array>
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
			drawFrame();
		}
		
		vkDeviceWaitIdle(egeDevice.device());
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


	void EnchantedEngine::createCommandBuffers() {

		commandBuffers.resize(egeSwapChain.imageCount());

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = egeDevice.getCommandPool();
		allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

		if (vkAllocateCommandBuffers(egeDevice.device(), &allocInfo, commandBuffers.data()) != VK_SUCCESS){
			throw std::runtime_error("Failed to allocate command buffer!");
		}

		for (size_t i = 0; i < commandBuffers.size(); i++)
		{
			VkCommandBufferBeginInfo beinInfo{};
			beinInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

			if(vkBeginCommandBuffer(commandBuffers[i], &beinInfo) != VK_SUCCESS) {
				throw std::runtime_error("Command buffer Failed to begin recording!");
			}

			VkRenderPassBeginInfo renderPassInfo{};

			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassInfo.renderPass = egeSwapChain.getRenderPass();
			renderPassInfo.framebuffer = egeSwapChain.getFrameBuffer(i);

			renderPassInfo.renderArea.offset = { 0,0 };
			renderPassInfo.renderArea.extent = egeSwapChain.getSwapChainExtent();

			std::array<VkClearValue, 2> clearValues{};
			clearValues[0].color = { 0.1f, 0.1f, 0.1f, 1.0f };
			clearValues[1].depthStencil = { 1.0f, 0 };
			renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
			renderPassInfo.pClearValues = clearValues.data();

			vkCmdBeginRenderPass(commandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

			egePipeline->bind(commandBuffers[i]);
			vkCmdDraw(commandBuffers[i], 3, 1, 0, 0);

			vkCmdEndRenderPass(commandBuffers[i]);
			if (vkEndCommandBuffer(commandBuffers[i]) != VK_SUCCESS) {
				throw std::runtime_error("failed to record command buffer!");
			}
		}
	}

	void EnchantedEngine::drawFrame() {
		uint32_t imageIndex;
		auto result = egeSwapChain.acquireNextImage(&imageIndex);
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			throw std::runtime_error("failed to acquire swap chain image!");
		}
		result = egeSwapChain.submitCommandBuffers(&commandBuffers[imageIndex], &imageIndex);
		if (result != VK_SUCCESS) {
			throw std::runtime_error("failed to present swap chain image!");
		}
	}


}