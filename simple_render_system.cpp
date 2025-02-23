#include "simple_render_system.hpp"


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

	SimpleRenderSystem::SimpleRenderSystem(EgeDevice& device, VkRenderPass renderPass) : egeDevice{ device } {
		createPipelineLayout();
		createPipeline(renderPass);
	}


	SimpleRenderSystem::~SimpleRenderSystem() {
		vkDestroyPipelineLayout(egeDevice.device(), pipelineLayout, nullptr);
	}



	void SimpleRenderSystem::createPipelineLayout() {
		VkPushConstantRange pushConstantRange{};
		pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		pushConstantRange.offset = 0;
		pushConstantRange.size = sizeof(SimplePushConstantData);

		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 0;
		pipelineLayoutInfo.pSetLayouts = nullptr;
		pipelineLayoutInfo.pushConstantRangeCount = 1;
		pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
		if (vkCreatePipelineLayout(egeDevice.device(), &pipelineLayoutInfo, nullptr, &pipelineLayout) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create pipeline layout!");
		}
	}

	void SimpleRenderSystem::createPipeline(VkRenderPass renderPass) {
		assert(pipelineLayout != nullptr && "Cannot create pipeline before pipeline layout");
		PipelineConfigInfo pipelineConfig{};
		EgePipeline::defaultPipelineConfigInfo(pipelineConfig);
		pipelineConfig.renderPass = renderPass;
		pipelineConfig.pipelineLayout = pipelineLayout;
		egePipeline = std::make_unique<EgePipeline>(
			egeDevice, "CompiledShaders/simple_shader.vert.spv", "CompiledShaders/simple_shader.frag.spv", pipelineConfig
		);

	}




	void SimpleRenderSystem::renderGameObjects(VkCommandBuffer commandBuffer, std::vector<EgeGameObject>& gameObjects) {
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




}