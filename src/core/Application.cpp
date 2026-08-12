#include "core/Application.hpp"

#include "platform/KeyboardMovementController.hpp"
#include "render/Camera.hpp"
#include "render/SimpleRenderSystem.hpp"
#include "rhi/Buffer.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <chrono>
#include <stdexcept>

namespace ege {

    struct GlobalUbo {
        glm::mat4 projectionView{1.f};
        glm::vec4 ambientLightColor{1.f, 1.f, 1.f, .02f};  // w is intensity
        glm::vec3 lightPosition{-1.f};
        alignas(16) glm::vec4 lightColor{0.f, 0.f, 1.f, 1.f};  // w is light intensity
    };

    Application::Application() {
        globalPool =
            DescriptorPool::Builder(device)
                .setMaxSets(SwapChain::MAX_FRAMES_IN_FLIGHT)
                .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SwapChain::MAX_FRAMES_IN_FLIGHT)
                .build();
        loadGameObjects();
    }

    Application::~Application() {}

    void Application::run() {
        std::vector<std::unique_ptr<Buffer>> uboBuffers(SwapChain::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < uboBuffers.size(); i++) {
            uboBuffers[i] = std::make_unique<Buffer>(
                device,
                sizeof(GlobalUbo),
                1,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            uboBuffers[i]->map();
        }

        auto globalSetLayout =
            DescriptorSetLayout::Builder(device)
                .addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS)
                .build();

        std::vector<VkDescriptorSet> globalDescriptorSets(SwapChain::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < globalDescriptorSets.size(); i++) {
            auto bufferInfo = uboBuffers[i]->descriptorInfo();
            DescriptorWriter(*globalSetLayout, *globalPool)
                .writeBuffer(0, &bufferInfo)
                .build(globalDescriptorSets[i]);
        }

        SimpleRenderSystem simpleRenderSystem{
            device, renderer.getSwapChainRenderPass(), globalSetLayout->getDescriptorSetLayout()};

        Camera camera{};

        auto viewerObject = GameObject::createGameObject();
        viewerObject.transform.translation = glm::vec3(0.f, -1.f, -3.f);
        // Pitched down slightly so the default view frames the scene instead of
        // leaving it along the bottom edge.
        viewerObject.transform.rotation.x = -.35f;
        KeyboardMovementController cameraController{};

        auto currentTime = std::chrono::high_resolution_clock::now();

        while (!window.shouldClose()) {
            glfwPollEvents();

            // putting this after polls to make sure pauses don't affect the time
            auto newTime = std::chrono::high_resolution_clock::now();
            float frameTime =
                std::chrono::duration<float, std::chrono::seconds::period>(newTime - currentTime)
                    .count();
            currentTime = newTime;

            cameraController.moveInPlaneXZ(window.getGLFWwindow(), frameTime, viewerObject);
            camera.setViewYXZ(viewerObject.transform.translation, viewerObject.transform.rotation);
            float aspectRatio = renderer.getAspectRatio();

            // camera.setOrthographicProjection(-aspectRatio, aspectRatio, -1, 1, -1, 1);

            camera.setPerspectiveProjection(glm::radians(50.f), aspectRatio, 0.1f, 100.f);

            if (auto commandBuffer = renderer.beginFrame()) {
                const uint32_t frameIndex = renderer.getFrameIndex();
                FrameInfo frameInfo{
                    frameIndex,
                    frameTime,
                    commandBuffer,
                    camera,
                    globalDescriptorSets[frameIndex],
                    gameObjects};

                // update
                GlobalUbo ubo{};
                ubo.projectionView = camera.getProjection() * camera.getView();

                uboBuffers[frameIndex]->writeToBuffer(&ubo);
                uboBuffers[frameIndex]->flush();

                // render
                renderer.beginSwapChainRenderPass(commandBuffer);
                simpleRenderSystem.renderGameObjects(frameInfo);
                renderer.endSwapChainRenderPass(commandBuffer);
                renderer.endFrame();
            }
        }

        vkDeviceWaitIdle(device.device());
    }

    void Application::loadGameObjects() {
        // Built from procedural primitives rather than OBJ files so that a clean
        // checkout runs with no binary assets present. Note this scene treats -Y
        // as up, matching the camera and light placement inherited from the
        // tutorial code.
        auto addObject = [this](
                             std::shared_ptr<Model> model,
                             glm::vec3 translation,
                             glm::vec3 scale,
                             glm::vec3 rotation = glm::vec3{0.f}) {
            auto object = GameObject::createGameObject();
            object.model = std::move(model);
            object.transform.translation = translation;
            object.transform.scale = scale;
            object.transform.rotation = rotation;
            gameObjects.emplace(object.getId(), std::move(object));
        };

        std::shared_ptr<Model> plane = std::make_shared<Model>(device, Model::Builder::plane());
        std::shared_ptr<Model> box = std::make_shared<Model>(device, Model::Builder::box());
        std::shared_ptr<Model> sphere = std::make_shared<Model>(device, Model::Builder::sphere());

        // Floor, rotated a half turn about X so its +Y normal points along -Y,
        // which is up in this scene and therefore towards the light.
        addObject(plane, {0.f, .5f, 0.f}, {6.f, 1.f, 6.f}, {glm::pi<float>(), 0.f, 0.f});

        // Both primitives are unit-sized, so at half scale they sit on the floor
        // when raised by a quarter unit.
        addObject(box, {-.6f, .25f, 0.f}, glm::vec3{.5f});
        addObject(sphere, {.6f, .25f, 0.f}, glm::vec3{.5f});
    }

}  // namespace ege