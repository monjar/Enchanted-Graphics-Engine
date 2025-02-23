#include "ege_engine.hpp"


#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <stdexcept>

namespace ege {

	struct SimplePushConstantData {
		// by defult the float constructor fills out the diagonal
		glm::mat2 transform{ 1.f };
		glm::vec2 offset;
		alignas(16) glm::vec3 color;
	};

	EnchantedEngine::EnchantedEngine() {
		loadGameObjects();
		createPipelineLayout();
		recreateSwapChain();
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



	std::vector<EgeModel::Vertex> generateSierpinski(int level, EgeModel::Vertex a, EgeModel::Vertex b, EgeModel::Vertex c) {
		if (level == 0) {
			return { a, b, c };
		}

		// Calculate midpoints
		EgeModel::Vertex ab = { {(a.position[0] + b.position[0]) / 2.0f, (a.position[1] + b.position[1]) / 2.0f} , {(a.color[0] + b.color[0]) / 2.0f, (a.color[1] + b.color[1]) / 2.0f, (a.color[2] + b.color[2]) / 2.0f} };
		EgeModel::Vertex bc = { {(b.position[0] + c.position[0]) / 2.0f, (b.position[1] + c.position[1]) / 2.0f}, {(b.color[0] + c.color[0]) / 2.0f, (b.color[1] + c.color[1]) / 2.0f, (b.color[2] + c.color[2]) / 2.0f} };
		EgeModel::Vertex ca = { {(c.position[0] + a.position[0]) / 2.0f, (c.position[1] + a.position[1]) / 2.0f}, {(c.color[0] + a.color[0]) / 2.0f, (c.color[1] + a.color[1]) / 2.0f, (c.color[2] + a.color[2]) / 2.0f} };

		// Recursively generate subtriangles
		auto v1 = generateSierpinski(level - 1, a, ab, ca);
		auto v2 = generateSierpinski(level - 1, ab, b, bc);
		auto v3 = generateSierpinski(level - 1, ca, bc, c);

		// Merge results
		std::vector<EgeModel::Vertex> vertices;
		vertices.insert(vertices.end(), v1.begin(), v1.end());
		vertices.insert(vertices.end(), v2.begin(), v2.end());
		vertices.insert(vertices.end(), v3.begin(), v3.end());

		return vertices;
	}

	std::vector<EgeModel::Vertex> getSierpinskiVertices(int level) {
		EgeModel::Vertex a = { {0.0f, -0.5f}, {1.0f, 0.0f, 0.0f} };
		EgeModel::Vertex b = { {0.5f, 0.5f}, {0.0f, 1.0f, 0.0f} };
		EgeModel::Vertex c = { {-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f} };

		return generateSierpinski(level, a, b, c);
	}


	void EnchantedEngine::loadGameObjects() {
		std::vector<EgeModel::Vertex> vertices{{{0.0f, -0.5f}}, {{0.5f, 0.5f}}, {{-0.5f, 0.5f}}};

		//vertices = getSierpinskiVertices(4);
		auto egeModel = std::make_shared<EgeModel>(egeDevice, vertices);

		auto triangle = EgeGameObject::createGameObject();
		triangle.model = egeModel;
		triangle.color = { .1f, .8f, .1f };
		triangle.transform2d.translation.x = .2f;
		triangle.transform2d.scale = { 2.f, .5f };
		// be wary that the y axis in vulkan points down. meaning -1 is actually top
		triangle.transform2d.rotation = 0.25 * glm::two_pi<float>();

		gameObjects.push_back(std::move(triangle));
	}

	void EnchantedEngine::createPipelineLayout() {

		VkPushConstantRange pushConstantRange{};
		pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		pushConstantRange.offset = 0;
		pushConstantRange.size = sizeof(SimplePushConstantData);


		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 0;
		pipelineLayoutInfo.pSetLayouts = nullptr;

		pipelineLayoutInfo.pushConstantRangeCount = 0;
		pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

		if (vkCreatePipelineLayout(egeDevice.device(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
			throw std::runtime_error("Failed to Create Pipeline Layout!");
		}
	}


	void EnchantedEngine::createPipeline() {
		assert(egeSwapChain != nullptr && "Cannot create pipeline before swap chain");
		assert(pipelineLayout != nullptr && "Cannot create pipeline before pipeline layout");
		PipelineConfigInfo pipelineConfig{};
		EgePipeline::defaultPipelineConfigInfo(pipelineConfig);
		pipelineConfig.renderPass = egeSwapChain->getRenderPass();
		pipelineConfig.pipelineLayout = pipelineLayout;
		egePipeline = std::make_unique<EgePipeline>(
			egeDevice, "CompiledShaders/simple_shader.vert.spv", "CompiledShaders/simple_shader.frag.spv", pipelineConfig
		);

	}

	void EnchantedEngine::recreateSwapChain() {
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
			egeSwapChain = std::make_unique<EgeSwapChain>(egeDevice, extent, std::move(egeSwapChain));
			if (egeSwapChain->imageCount() != commandBuffers.size()) {
				freeCommandBuffers();
				createCommandBuffers();
			}
		}
		createPipeline();
	}

	void EnchantedEngine::freeCommandBuffers() {
		vkFreeCommandBuffers(
			egeDevice.device(),
			egeDevice.getCommandPool(),
			static_cast<uint32_t>(commandBuffers.size()),
			commandBuffers.data());
		commandBuffers.clear();
	}

	void EnchantedEngine::createCommandBuffers() {

		commandBuffers.resize(egeSwapChain->imageCount());

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = egeDevice.getCommandPool();
		allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

		if (vkAllocateCommandBuffers(egeDevice.device(), &allocInfo, commandBuffers.data()) != VK_SUCCESS){
			throw std::runtime_error("Failed to allocate command buffer!");
		}

	}

	void EnchantedEngine::recordCommandBuffer(int imageIndex) {


		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		if (vkBeginCommandBuffer(commandBuffers[imageIndex], &beginInfo) != VK_SUCCESS) {
			throw std::runtime_error("failed to begin recording command buffer!");
		}

		VkRenderPassBeginInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassInfo.renderPass = egeSwapChain->getRenderPass();
		renderPassInfo.framebuffer = egeSwapChain->getFrameBuffer(imageIndex);
		renderPassInfo.renderArea.offset = { 0, 0 };
		renderPassInfo.renderArea.extent = egeSwapChain->getSwapChainExtent();
		std::array<VkClearValue, 2> clearValues{};
		clearValues[0].color = { 0.0f, 0.0f, 0.0f, 1.0f };
		clearValues[1].depthStencil = { 1.0f, 0 };
		renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
		renderPassInfo.pClearValues = clearValues.data();

		vkCmdBeginRenderPass(commandBuffers[imageIndex], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport{};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = static_cast<float>(egeSwapChain->getSwapChainExtent().width);
		viewport.height = static_cast<float>(egeSwapChain->getSwapChainExtent().height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		VkRect2D scissor{ {0, 0}, egeSwapChain->getSwapChainExtent() };
		vkCmdSetViewport(commandBuffers[imageIndex], 0, 1, &viewport);
		vkCmdSetScissor(commandBuffers[imageIndex], 0, 1, &scissor);

		renderGameObjects(commandBuffers[imageIndex]);

		vkCmdEndRenderPass(commandBuffers[imageIndex]);
		if (vkEndCommandBuffer(commandBuffers[imageIndex]) != VK_SUCCESS) {
			throw std::runtime_error("failed to record command buffer!");
		}
	}

	void EnchantedEngine::renderGameObjects(VkCommandBuffer commandBuffer) {
		egePipeline->bind(commandBuffer);

		for (auto& obj : gameObjects)
		{
			obj.transform2d.rotation = glm::mod(obj.transform2d.rotation + 0.001f, glm::two_pi<float>());


			SimplePushConstantData pushConstant{};
			pushConstant.offset = obj.transform2d.translation;

			pushConstant.color
				= obj.color;
			pushConstant.transform = obj.transform2d.mat2();
			vkCmdPushConstants(commandBuffer, pipelineLayout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0, sizeof(SimplePushConstantData), &pushConstant);
			obj.model->bind(commandBuffer);
			obj.model->draw(commandBuffer);
		}
	}

	void EnchantedEngine::drawFrame() {
		uint32_t imageIndex;
		auto result = egeSwapChain->acquireNextImage(&imageIndex);
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			recreateSwapChain();
			return;
		}
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			throw std::runtime_error("failed to acquire swap chain image!");
		}
		recordCommandBuffer(imageIndex);
		result = egeSwapChain->submitCommandBuffers(&commandBuffers[imageIndex], &imageIndex);

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || egeWindow.wasWindowResized()) {
			
			egeWindow.resetWindowResizedFlag();
			recreateSwapChain();
			return;
		}
		if (result != VK_SUCCESS) {
			throw std::runtime_error("failed to present swap chain image!");
		}
	}


}