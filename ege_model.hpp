#pragma once 

#include "ege_engine_device.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <vector>


namespace ege {


	class EgeModel {
	public:
		
		struct Vertex {
			glm::vec3 position;
			glm::vec3 color;

			
			static std::vector<VkVertexInputBindingDescription> getBindingDescriptions();
			static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions();

		};
		EgeModel(EgeDevice& device, const std::vector<Vertex>& vertices);
		~EgeModel();

		//Delete copy constructor and operator
		EgeModel(const EgeModel& other) = delete;
		EgeModel& operator = (const EgeModel&) = delete;

		void bind(VkCommandBuffer commandBuffer);
		void draw(VkCommandBuffer commandBuffer);

	private:

		EgeDevice& egeDevice;
		VkBuffer vertexBuffer;
		VkDeviceMemory vertexBufferMemory;
		uint32_t vertexCount;

		void createVertexBuffers(const std::vector<Vertex>& vertices);
	};
}