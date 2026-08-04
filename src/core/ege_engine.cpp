#include "core/ege_engine.hpp"

#include "rhi/ege_buffer.hpp"
#include "render/ege_camera.hpp"
#include "platform/keyboard_movement_controller.hpp"
#include "render/simple_render_system.hpp"

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

    EnchantedEngine::EnchantedEngine() {
        globalPool =
            EgeDescriptorPool::Builder(egeDevice)
                .setMaxSets(EgeSwapChain::MAX_FRAMES_IN_FLIGHT)
                .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, EgeSwapChain::MAX_FRAMES_IN_FLIGHT)
                .build();
        loadGameObjects();
    }

    EnchantedEngine::~EnchantedEngine() {}

    void EnchantedEngine::run() {
        std::vector<std::unique_ptr<EgeBuffer>> uboBuffers(EgeSwapChain::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < uboBuffers.size(); i++) {
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
        for (size_t i = 0; i < globalDescriptorSets.size(); i++) {
            auto bufferInfo = uboBuffers[i]->descriptorInfo();
            EgeDescriptorWriter(*globalSetLayout, *globalPool)
                .writeBuffer(0, &bufferInfo)
                .build(globalDescriptorSets[i]);
        }

        SimpleRenderSystem simpleRenderSystem{
            egeDevice,
            egeRenderer.getSwapChainRenderPass(),
            globalSetLayout->getDescriptorSetLayout()};

        EgeCamera camera{};

        auto viewerObject = EgeGameObject::createGameObject();
        viewerObject.transform.translation = glm::vec3(0.f, -1.f, -3.f);
        // Pitched down slightly so the default view frames the scene instead of
        // leaving it along the bottom edge.
        viewerObject.transform.rotation.x = -.35f;
        KeyboardMovementController cameraController{};

        auto currentTime = std::chrono::high_resolution_clock::now();

        while (!egeWindow.shouldClose()) {
            glfwPollEvents();

            // putting this after polls to make sure pauses don't affect the time
            auto newTime = std::chrono::high_resolution_clock::now();
            float frameTime =
                std::chrono::duration<float, std::chrono::seconds::period>(newTime - currentTime)
                    .count();
            currentTime = newTime;

            cameraController.moveInPlaneXZ(egeWindow.getGLFWwindow(), frameTime, viewerObject);
            camera.setViewYXZ(viewerObject.transform.translation, viewerObject.transform.rotation);
            float aspectRatio = egeRenderer.getAspectRatio();

            // camera.setOrthographicProjection(-aspectRatio, aspectRatio, -1, 1, -1, 1);

            camera.setPerspectiveProjection(glm::radians(50.f), aspectRatio, 0.1f, 100.f);

            if (auto commandBuffer = egeRenderer.beginFrame()) {
                const uint32_t frameIndex = egeRenderer.getFrameIndex();
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
                egeRenderer.beginSwapChainRenderPass(commandBuffer);
                simpleRenderSystem.renderGameObjects(frameInfo);
                egeRenderer.endSwapChainRenderPass(commandBuffer);
                egeRenderer.endFrame();
            }
        }

        vkDeviceWaitIdle(egeDevice.device());
    }

    void EnchantedEngine::loadGameObjects() {
        // Built from procedural primitives rather than OBJ files so that a clean
        // checkout runs with no binary assets present. Note this scene treats -Y
        // as up, matching the camera and light placement inherited from the
        // tutorial code.
        auto addObject = [this](
                             std::shared_ptr<EgeModel> model,
                             glm::vec3 translation,
                             glm::vec3 scale,
                             glm::vec3 rotation = glm::vec3{0.f}) {
            auto object = EgeGameObject::createGameObject();
            object.model = std::move(model);
            object.transform.translation = translation;
            object.transform.scale = scale;
            object.transform.rotation = rotation;
            gameObjects.emplace(object.getId(), std::move(object));
        };

        std::shared_ptr<EgeModel> plane =
            std::make_shared<EgeModel>(egeDevice, EgeModel::Builder::plane());
        std::shared_ptr<EgeModel> box =
            std::make_shared<EgeModel>(egeDevice, EgeModel::Builder::box());
        std::shared_ptr<EgeModel> sphere =
            std::make_shared<EgeModel>(egeDevice, EgeModel::Builder::sphere());

        // Floor, rotated a half turn about X so its +Y normal points along -Y,
        // which is up in this scene and therefore towards the light.
        addObject(plane, {0.f, .5f, 0.f}, {6.f, 1.f, 6.f}, {glm::pi<float>(), 0.f, 0.f});

        // Both primitives are unit-sized, so at half scale they sit on the floor
        // when raised by a quarter unit.
        addObject(box, {-.6f, .25f, 0.f}, glm::vec3{.5f});
        addObject(sphere, {.6f, .25f, 0.f}, glm::vec3{.5f});
    }

}  // namespace ege