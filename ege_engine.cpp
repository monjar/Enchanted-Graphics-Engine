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
        glm::vec4 ambientLightColor{ 1.f, 1.f, 1.f, .02f };  // w is intensity
        glm::vec3 lightPosition{ -1.f };
        alignas(16) glm::vec4 lightColor{ 0.f, 0.f, 1.f, 1.f };  // w is light intensity
    };


	EnchantedEngine::EnchantedEngine() {
        globalPool =
            EgeDescriptorPool::Builder(egeDevice)
            .setMaxSets(EgeSwapChain::MAX_FRAMES_IN_FLIGHT)
            .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, EgeSwapChain::MAX_FRAMES_IN_FLIGHT)
            .build();
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

        auto globalSetLayout =
            EgeDescriptorSetLayout::Builder(egeDevice)
            .addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS)
            .build();

        std::vector<VkDescriptorSet> globalDescriptorSets(EgeSwapChain::MAX_FRAMES_IN_FLIGHT);
        for (int i = 0; i < globalDescriptorSets.size(); i++) {
            auto bufferInfo = uboBuffers[i]->descriptorInfo();
            EgeDescriptorWriter(*globalSetLayout, *globalPool)
                .writeBuffer(0, &bufferInfo)
                .build(globalDescriptorSets[i]);
        }

        SimpleRenderSystem simpleRenderSystem{
            egeDevice,
            egeRenderer.getSwapChainRenderPass(),
            globalSetLayout->getDescriptorSetLayout() };

        EgeCamera camera{};


        auto viewerObject = EgeGameObject::createGameObject();
        viewerObject.transform.translation = glm::vec3(0.f, -1.f, -3.f);
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

            camera.setPerspectiveProjection(glm::radians(50.f), aspectRatio, 0.1f, 100.f);

			if (auto commandBuffer = egeRenderer.beginFrame()) {
                int frameIndex = egeRenderer.getFrameIndex();
                FrameInfo frameInfo{
                      frameIndex,
                      frameTime,
                      commandBuffer,
                      camera,
                      globalDescriptorSets[frameIndex],
                      gameObjects
                };

                // update
                GlobalUbo ubo{};
                ubo.projectionView = camera.getProjection() * camera.getView();

                uboBuffers[frameIndex]->writeToBuffer(&ubo);
                uboBuffers[frameIndex]->flush();

                // render
                egeRenderer.beginSwapChainRenderPass(commandBuffer);
                simpleRenderSystem.renderGameObjects(frameInfo);
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
        gameObj.transform.translation = { -.5f, .5f, .0f };
        gameObj.transform.scale = glm::vec3(3.f);
        gameObjects.emplace(gameObj.getId(), std::move(gameObj));



        egeModel =
            EgeModel::createModelFromFile(egeDevice, "models/smooth_vase.obj");
        auto gameObj2 = EgeGameObject::createGameObject();
        gameObj2.model = egeModel;
        gameObj2.transform.translation = { .5f, .5f, .0f };
        gameObj2.transform.scale = glm::vec3(3.f);
        gameObjects.emplace(gameObj2.getId(), std::move(gameObj2));

        egeModel = EgeModel::createModelFromFile(egeDevice, "models/plane.obj");
        auto floor = EgeGameObject::createGameObject();
        floor.model = egeModel;
        floor.transform.translation = { 0.f, .5f, 0.f };
        floor.transform.scale = { 3.f, 1.f, 3.f };
        gameObjects.emplace(floor.getId(), std::move(floor));
	}


}