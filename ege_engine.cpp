#include "ege_engine.hpp"

#include "keyboard_movement_controller.hpp"
#include "simple_render_system.hpp"
#include "ege_camera.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <chrono>
#include <stdexcept>

namespace ege {

	
	

	EnchantedEngine::EnchantedEngine() {
		loadGameObjects();
	}


	EnchantedEngine::~EnchantedEngine() {
	}

	void EnchantedEngine::run() {
		SimpleRenderSystem simpleRenderSystem{ egeDevice, egeRenderer.getSwapChainRenderPass() };
        EgeCamera camera{};

        camera.setViewTarget(glm::vec3(-1.f, -2.f, -2.f), glm::vec3(0.f, 0.f, 2.5f));

        auto viewerObject = EgeGameObject::createGameObject();
        KeyboardMovementController cameraController{};

        auto currentTime = std::chrono::high_resolution_clock::now();


		while (!egeWindow.shouldClose()) {
			glfwPollEvents();

            // putting this after polls to make sure pauses don't affect the time
            auto newTime = std::chrono::high_resolution_clock::now();
            float frameTime = std::chrono::duration<float, std::chrono::seconds::period>(newTime - currentTime).count();
            currentTime = newTime;

            cameraController.moveInPlaneXZ(egeWindow.getGLFWwindow(), frameTime, viewerObject);
            camera.setViewYXZ(viewerObject.transform.translation, viewerObject.transform.rotation);
            float aspectRatio = egeRenderer.getAspectRatio();

            //camera.setOrthographicProjection(-aspectRatio, aspectRatio, -1, 1, -1, 1);

            camera.setPerspectiveProjection(glm::radians(50.f), aspectRatio, 0.1f, 10.f);

			if (auto commandBuffer = egeRenderer.beginFrame()) {
				//Here we want to add more render passes. shadow casting etc
				egeRenderer.beginSwapChainRenderPass(commandBuffer);
				simpleRenderSystem.renderGameObjects(commandBuffer, gameObjects, camera);
				egeRenderer.endSwapChainRenderPass(commandBuffer);
				egeRenderer.endFrame();
			}
		}
		
		vkDeviceWaitIdle(egeDevice.device());
	}

    // temporary helper function, creates a 1x1x1 cube centered at offset
    std::unique_ptr<EgeModel> createCubeModel(EgeDevice& device, glm::vec3 offset) {
        std::vector<EgeModel::Vertex> vertices{

            // left face (white)
            {{-.5f, -.5f, -.5f}, {.9f, .9f, .9f}},
            {{-.5f, .5f, .5f}, {.9f, .9f, .9f}},
            {{-.5f, -.5f, .5f}, {.9f, .9f, .9f}},
            {{-.5f, -.5f, -.5f}, {.9f, .9f, .9f}},
            {{-.5f, .5f, -.5f}, {.9f, .9f, .9f}},
            {{-.5f, .5f, .5f}, {.9f, .9f, .9f}},

            // right face (yellow)
            {{.5f, -.5f, -.5f}, {.8f, .8f, .1f}},
            {{.5f, .5f, .5f}, {.8f, .8f, .1f}},
            {{.5f, -.5f, .5f}, {.8f, .8f, .1f}},
            {{.5f, -.5f, -.5f}, {.8f, .8f, .1f}},
            {{.5f, .5f, -.5f}, {.8f, .8f, .1f}},
            {{.5f, .5f, .5f}, {.8f, .8f, .1f}},

            // top face (orange, remember y axis points down)
            {{-.5f, -.5f, -.5f}, {.9f, .6f, .1f}},
            {{.5f, -.5f, .5f}, {.9f, .6f, .1f}},
            {{-.5f, -.5f, .5f}, {.9f, .6f, .1f}},
            {{-.5f, -.5f, -.5f}, {.9f, .6f, .1f}},
            {{.5f, -.5f, -.5f}, {.9f, .6f, .1f}},
            {{.5f, -.5f, .5f}, {.9f, .6f, .1f}},

            // bottom face (red)
            {{-.5f, .5f, -.5f}, {.8f, .1f, .1f}},
            {{.5f, .5f, .5f}, {.8f, .1f, .1f}},
            {{-.5f, .5f, .5f}, {.8f, .1f, .1f}},
            {{-.5f, .5f, -.5f}, {.8f, .1f, .1f}},
            {{.5f, .5f, -.5f}, {.8f, .1f, .1f}},
            {{.5f, .5f, .5f}, {.8f, .1f, .1f}},

            // nose face (blue)
            {{-.5f, -.5f, 0.5f}, {.1f, .1f, .8f}},
            {{.5f, .5f, 0.5f}, {.1f, .1f, .8f}},
            {{-.5f, .5f, 0.5f}, {.1f, .1f, .8f}},
            {{-.5f, -.5f, 0.5f}, {.1f, .1f, .8f}},
            {{.5f, -.5f, 0.5f}, {.1f, .1f, .8f}},
            {{.5f, .5f, 0.5f}, {.1f, .1f, .8f}},

            // tail face (green)
            {{-.5f, -.5f, -0.5f}, {.1f, .8f, .1f}},
            {{.5f, .5f, -0.5f}, {.1f, .8f, .1f}},
            {{-.5f, .5f, -0.5f}, {.1f, .8f, .1f}},
            {{-.5f, -.5f, -0.5f}, {.1f, .8f, .1f}},
            {{.5f, -.5f, -0.5f}, {.1f, .8f, .1f}},
            {{.5f, .5f, -0.5f}, {.1f, .8f, .1f}},

        };
        for (auto& v : vertices) {
            v.position += offset;
        }
        return std::make_unique<EgeModel>(device, vertices);
    }



	void EnchantedEngine::loadGameObjects() {
        std::shared_ptr<EgeModel> egeModel = createCubeModel(egeDevice, { 0.0f, 0.0f, 0.0f });
        auto cubeObj = EgeGameObject::createGameObject();
        cubeObj.model = egeModel;
        cubeObj.transform.translation = { 0.0f, 0.0f, 2.5f };
        cubeObj.transform.scale = { 0.5f,0.5f, 0.5f };

        gameObjects.push_back(std::move(cubeObj));
	}


	


}