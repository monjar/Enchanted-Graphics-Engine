#include "ege_engine.hpp"
#include <array>
#include <stdexcept>

namespace ege {



	EnchantedEngine::EnchantedEngine() {
		loadModels();
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


	void EnchantedEngine::loadModels() {
		std::vector<EgeModel::Vertex> vertices{{{0.0f, -0.5f}}, {{0.5f, 0.5f}}, {{-0.5f, 0.5f}}};

		vertices = getSierpinskiVertices(4);
		egeModel = std::make_unique<EgeModel>(egeDevice, vertices);
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
			clearValues[0].color = { 0.0f, 0.0f, 0.0f, 1.0f };
			clearValues[1].depthStencil = { 1.0f, 0 };
			renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
			renderPassInfo.pClearValues = clearValues.data();

			vkCmdBeginRenderPass(commandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

			egePipeline->bind(commandBuffers[i]);
			egeModel->bind(commandBuffers[i]);
			egeModel->draw(commandBuffers[i]);


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