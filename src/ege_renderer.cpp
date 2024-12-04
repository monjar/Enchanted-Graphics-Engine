#include "ege_renderer.hpp"


#include <array>
#include <stdexcept>

namespace ege {


	EgeRenderer::EgeRenderer(EgeWindow& window, EgeDevice& device) : egeWindow{ window }, egeDevice{ device } {
		recreateSwapChain();
		createCommandBuffers();
	}


	EgeRenderer::~EgeRenderer() {
		freeCommandBuffers();
	}



	void EgeRenderer::recreateSwapChain() {
		auto extent = egeWindow.getExtent();
		while (extent.width == 0 || extent.height == 0) {
			extent = egeWindow.getExtent();
			glfwWaitEvents();
		}
		vkDeviceWaitIdle(egeDevice.device());
		if (egeSwapChain == nullptr) {
			egeSwapChain = std::make_unique<EgeSwapChain>(egeDevice, extent);
		}
		else {
			std::shared_ptr<EgeSwapChain> oldSwapChain = std::move(egeSwapChain);
			egeSwapChain = std::make_unique<EgeSwapChain>(egeDevice, extent, oldSwapChain);

			if (!oldSwapChain->compareSwapFormats(*egeSwapChain.get())) {
				throw std::runtime_error("Swap chain image(or depth) format has changed!");
			}
		}
	}
	void EgeRenderer::freeCommandBuffers() {
		vkFreeCommandBuffers(
			egeDevice.device(),
			egeDevice.getCommandPool(),
			static_cast<uint32_t>(commandBuffers.size()),
			commandBuffers.data());
		commandBuffers.clear();
	}

	void EgeRenderer::createCommandBuffers() {

		commandBuffers.resize(EgeSwapChain::MAX_FRAMES_IN_FLIGHT);

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = egeDevice.getCommandPool();
		allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

		if (vkAllocateCommandBuffers(egeDevice.device(), &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
			throw std::runtime_error("Failed to allocate command buffer!");
		}

	}



	VkCommandBuffer EgeRenderer::beginFrame() {
		assert(!isFrameStarted && "Cant begin frame while it's already started");

		auto result = egeSwapChain->acquireNextImage(&currentImageIndex);

		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			recreateSwapChain();
			return nullptr;
		}

		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			throw std::runtime_error("failed to acquire swap chain image!");
		}
		isFrameStarted = true;

		auto commandBuffer = getCurrentCommandBuffer();


		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
			throw std::runtime_error("failed to begin recording command buffer!");
		}

		return commandBuffer;

	}

	void EgeRenderer::endFrame() {
		assert(isFrameStarted && "Can't call endFrame while frame is not in progress");
		auto commandBuffer = getCurrentCommandBuffer();
		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
			throw std::runtime_error("failed to record command buffer!");
		}

		auto result = egeSwapChain->submitCommandBuffers(&commandBuffer, &currentImageIndex);
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
			egeWindow.wasWindowResized()) {
			egeWindow.resetWindowResizedFlag();
			recreateSwapChain();
		}
		else if (result != VK_SUCCESS) {
			throw std::runtime_error("failed to present swap chain image!");
		}

		isFrameStarted = false;
		currentFrameIndex = (currentFrameIndex + 1) % EgeSwapChain::MAX_FRAMES_IN_FLIGHT;
	}

	void EgeRenderer::beginSwapChainRenderPass(VkCommandBuffer commandBuffer) {
		assert(isFrameStarted && "Cant beginSwap chain while frame is not in progress");

		assert(commandBuffer == getCurrentCommandBuffer() && "Can't begin render pass on cmd buffer from a different frame.");

		VkRenderPassBeginInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassInfo.renderPass = egeSwapChain->getRenderPass();
		renderPassInfo.framebuffer = egeSwapChain->getFrameBuffer(currentImageIndex);

		renderPassInfo.renderArea.offset = { 0, 0 };
		renderPassInfo.renderArea.extent = egeSwapChain->getSwapChainExtent();

		std::array<VkClearValue, 2> clearValues{};
		clearValues[0].color = { 0.01f, 0.01f, 0.01f, 1.0f };
		clearValues[1].depthStencil = { 1.0f, 0 };
		renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
		renderPassInfo.pClearValues = clearValues.data();

		vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport{};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = static_cast<float>(egeSwapChain->getSwapChainExtent().width);
		viewport.height = static_cast<float>(egeSwapChain->getSwapChainExtent().height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		VkRect2D scissor{ {0, 0}, egeSwapChain->getSwapChainExtent() };
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
	}
	void EgeRenderer::endSwapChainRenderPass(VkCommandBuffer commandBuffer) {

		assert(isFrameStarted && "Cant end Swap chain while frame is not in progress");

		assert(commandBuffer == getCurrentCommandBuffer() && "Can't end render pass on cmd buffer from a different frame.");

		vkCmdEndRenderPass(commandBuffer);

	}



}