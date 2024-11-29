#include "ege_engine.hpp"

#include "keyboard_movement_controller.hpp"

#include "ege_buffer.hpp"
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
	
    struct GlobalUbo {
        glm::mat4 projectionView{ 1.f };
        glm::vec3 lightDirection = glm::normalize(glm::vec3{ 1.f, -3.f, -1.f });
    };


	EnchantedEngine::EnchantedEngine() {
		loadGameObjects();
	}


	EnchantedEngine::~EnchantedEngine() {
	}

	void EnchantedEngine::run() {
        std::vector<std::unique_ptr<EgeBuffer>> uboBuffers(EgeSwapChain::MAX_FRAMES_IN_FLIGHT);
        for (int i = 0; i < uboBuffers.size(); i++) {
            uboBuffers[i] = std::make_unique<EgeBuffer>(
                egeDevice,
                sizeof(GlobalUbo),
                1,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            uboBuffers[i]->map();
        }

		SimpleRenderSystem simpleRenderSystem{ egeDevice, egeRenderer.getSwapChainRenderPass() };
        EgeCamera camera{};


        auto viewerObject = EgeGameObject::createGameObject();
        viewerObject.transform.translation = glm::vec3(0.f, -1.f, -0.f);
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

            camera.setPerspectiveProjection(glm::radians(50.f), aspectRatio, 0.1f, 40.f);

			if (auto commandBuffer = egeRenderer.beginFrame()) {
                int frameIndex = egeRenderer.getFrameIndex();
                FrameInfo frameInfo{ frameIndex, frameTime, commandBuffer, camera };

                // update
                GlobalUbo ubo{};
                ubo.projectionView = camera.getProjection() * camera.getView();

                uboBuffers[frameIndex]->writeToBuffer(&ubo);
                uboBuffers[frameIndex]->flush();

                // render
                egeRenderer.beginSwapChainRenderPass(commandBuffer);
                simpleRenderSystem.renderGameObjects(frameInfo, gameObjects);
				egeRenderer.endSwapChainRenderPass(commandBuffer);
				egeRenderer.endFrame();
			}
		}
		
		vkDeviceWaitIdle(egeDevice.device());
	}




	void EnchantedEngine::loadGameObjects() {
        std::shared_ptr<EgeModel> egeModel =
            EgeModel::createModelFromFile(egeDevice, "models/flat_vase.obj");
        auto gameObj = EgeGameObject::createGameObject();
        gameObj.model = egeModel;
        gameObj.transform.translation = { -.5f, .0f, 2.5f };
        gameObj.transform.scale = glm::vec3(3.f);
        gameObjects.push_back(std::move(gameObj));



        std::shared_ptr<EgeModel> egeModel2 =
            EgeModel::createModelFromFile(egeDevice, "models/smooth_vase.obj");
        auto gameObj2 = EgeGameObject::createGameObject();
        gameObj2.model = egeModel2;
        gameObj2.transform.translation = { .5f, .0f, 2.5f };
        gameObj2.transform.scale = glm::vec3(3.f);
        gameObjects.push_back(std::move(gameObj2));
	}


}