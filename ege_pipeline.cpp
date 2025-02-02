#include "ege_pipeline.hpp"

#include<fstream>
#include<stdexcept>
#include<iostream>

namespace ege {

	EgePipeline::EgePipeline(const std::string& vertFilePath, const std::string& fragFilePath) {
		createGraphicsPipeline(vertFilePath, fragFilePath);
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


	void EgePipeline::createGraphicsPipeline(const std::string& vertFilePath, const std::string& fragFilePath) {

		auto vertShaderCode = readFile(vertFilePath);
		auto fragShaderCode = readFile(fragFilePath);

		std::cout << "Vertex Shader File Size: " << vertShaderCode.size() << '\n';
		std::cout << "Fragment Shader File Size: " << fragShaderCode.size() << '\n';

	}
}