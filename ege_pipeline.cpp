#include "ege_pipeline.hpp"

#include<fstream>
#include<stdexcept>
#include<iostream>

namespace ege {

	EgePipeline::EgePipeline(EgeDevice& device, const std::string& vertFilePath, const std::string& fragFilePath, const PipelineConfigInfo& configInfo) : egeDevice{ device } {

		createGraphicsPipeline(vertFilePath, fragFilePath, configInfo);
	}


	std::vector<char> EgePipeline::readFile(const std::string& filePath) {
		//read file in binary and seek end
		std::ifstream file(filePath, std::ios::ate | std::ios::binary);
		if (!file.is_open()) {
			throw std::runtime_error("failed to open file: " + filePath);
		}

		// tellg gets the last position, since we already did seek end its the end
		size_t fileSize = static_cast<size_t>(file.tellg());

		std::vector<char> buffer(fileSize);

		file.seekg(0);
		file.read(buffer.data(), fileSize);
		
		file.close();
		return buffer;
	}


	void EgePipeline::createGraphicsPipeline(const std::string& vertFilePath, const std::string& fragFilePath, const PipelineConfigInfo& configInfo) {

		auto vertShaderCode = readFile(vertFilePath);
		auto fragShaderCode = readFile(fragFilePath);

		std::cout << "Vertex Shader File Size: " << vertShaderCode.size() << '\n';
		std::cout << "Fragment Shader File Size: " << fragShaderCode.size() << '\n';

	}

	void EgePipeline::createShaderModule(const std::vector<char>& code, VkShaderModule* shaderModule) {
		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = code.size();
		//this cast is only valid because we are using vector and not c_style array
		createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

		if (vkCreateShaderModule(egeDevice.device(), &createInfo, nullptr, shaderModule) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create shader module.");
		}
	}


	PipelineConfigInfo EgePipeline::defaultPipelineConfigInfo(uint32_t width, uint32_t height) {
		PipelineConfigInfo configInfo{};

		return configInfo;
	}
}