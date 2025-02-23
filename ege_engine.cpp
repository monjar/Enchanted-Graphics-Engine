#include "ege_engine.hpp"
#include "simple_render_system.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <stdexcept>

namespace ege {

	
	

	EnchantedEngine::EnchantedEngine() {
		loadGameObjects();
	}


	EnchantedEngine::~EnchantedEngine() {
	}

	void EnchantedEngine::run() {
		SimpleRenderSystem simpleRenderSystem{ egeDevice, egeRenderer.getSwapChainRenderPass() };

		while (!egeWindow.shouldClose()) {
			glfwPollEvents();

			if (auto commandBuffer = egeRenderer.beginFrame()) {
				//Here we want to add more render passes. shadow casting etc
				egeRenderer.beginSwapChainRenderPass(commandBuffer);
				simpleRenderSystem.renderGameObjects(commandBuffer, gameObjects);
				egeRenderer.endSwapChainRenderPass(commandBuffer);
				egeRenderer.endFrame();
			}
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


	


}