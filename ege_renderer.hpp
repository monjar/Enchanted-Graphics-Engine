#pragma once

#include "ege_window.hpp"
#include "ege_swap_chain.hpp"
#include "ege_model.hpp"

#include <memory>

#include <vector>

namespace ege {

	class EgeRenderer {
	public:

		EgeRenderer(EgeWindow &window, EgeDevice &device);
		~EgeRenderer();



		//Delete copy constructor and operator1
		EgeRenderer(const EgeRenderer& other) = delete;
		EgeRenderer& operator = (const EgeRenderer&) = delete;

		VkRenderPass getSwapChainRenderPass() const {
			return egeSwapChain->getRenderPass();
		}
		bool isFrameInProgress() const { return isFrameStarted; };

		VkCommandBuffer getCurrentCommandBuffer() const {
			assert(isFrameStarted && "Cannot get command buffer when frame not in progress");
			return commandBuffers[currentFrameIndex];
		}
		int getFrameIndex() const {
			assert(isFrameStarted && "Cannot get frame Index When not in progress");
			return currentFrameIndex;
		}
		VkCommandBuffer beginFrame();
		void endFrame();
		void beginSwapChainRenderPass(VkCommandBuffer commandBuffer);
		void endSwapChainRenderPass(VkCommandBuffer commandBuffer);

	private:

		void createCommandBuffers();
		void freeCommandBuffers();
		void recreateSwapChain();


		EgeWindow& egeWindow;
		EgeDevice& egeDevice;
		std::unique_ptr<EgeSwapChain> egeSwapChain;
		std::vector<VkCommandBuffer> commandBuffers;
		uint32_t currentImageIndex;
		int currentFrameIndex{ 0 };
		bool isFrameStarted{ false };


	};
}