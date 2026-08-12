#include "core/Application.hpp"

#include "core/Log.hpp"
#include "core/Time.hpp"
#include "platform/CameraController.hpp"
#include "platform/Input.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "render/Camera.hpp"
#include "render/PbrRenderSystem.hpp"
#include "rhi/Buffer.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <stdexcept>

namespace ege {

    namespace {

        // Projection and view are separate rather than premultiplied, and the
        // inverse view is included, because the PBR shader needs the camera
        // position for the view vector - which is the inverse view's
        // translation column.
        struct GlobalUbo {
            glm::mat4 projection{1.f};
            glm::mat4 view{1.f};
            glm::mat4 inverseView{1.f};
            glm::vec4 ambientLightColor{1.f, 1.f, 1.f, .03f};  // w is intensity
            GpuPointLight pointLights[maxPointLights]{};
            alignas(16) int numLights = 0;
        };

    }  // namespace

    Application::Application() {
        Log::init();
        EGE_INFO("Enchanted Engine starting up");

        // Makes the leaf types findable by name before anything has touched
        // them, which scene loading and the editor's type pickers rely on.
        registerBuiltinTypes();
        EGE_DEBUG("Reflection: {} types registered", TypeRegistry::instance().all().size());
        globalPool =
            DescriptorPool::Builder(device)
                .setMaxSets(SwapChain::MAX_FRAMES_IN_FLIGHT)
                .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SwapChain::MAX_FRAMES_IN_FLIGHT)
                .build();

        // Four image samplers per material, and room for a reasonable number
        // of materials before the pool has to grow.
        constexpr uint32_t maxMaterials = 128;
        materialPool = DescriptorPool::Builder(device)
                           .setMaxSets(maxMaterials)
                           .addPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxMaterials * 4)
                           .build();
        materialSetLayout = Material::createLayout(device);
        Material::createDefaults(device);

        loadGameObjects();
    }

    Application::~Application() {
        // Shared fallback textures outlive every material, so they have to be
        // released before the device goes away.
        Material::destroyDefaults();
    }

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

        PbrRenderSystem pbrRenderSystem{
            device,
            renderer.getSwapChainRenderPass(),
            globalSetLayout->getDescriptorSetLayout(),
            materialSetLayout->getDescriptorSetLayout()};

        Camera camera{};

        auto viewerObject = GameObject::createGameObject();
        viewerObject.transform.translation = glm::vec3(0.f, -1.f, -3.f);
        // Pitched down slightly so the default view frames the scene instead of
        // leaving it along the bottom edge.
        viewerObject.transform.rotation.x = -.35f;
        CameraController cameraController{};
        CameraController::registerDefaultActions(window.input());

        Time time{};

        while (!window.shouldClose()) {
            Window::pollEvents();

            // After the poll, so that a pause spent inside it is measured as
            // part of the frame it belongs to.
            time.beginFrame();
            const float frameTime = time.delta();

            window.input().newFrame();

            // Fixed-rate simulation. Nothing runs here yet - physics attaches
            // in Phase 8 - but the loop shape is what makes that possible, and
            // it is far easier to establish before there is anything depending
            // on the old variable-rate behaviour.
            while (time.consumeFixedStep()) {
                // fixedTick(time.fixedStep());
            }

            // Rendering and camera control stay on the variable delta: they
            // should run as often as the display allows.
            cameraController.update(window.input(), frameTime, viewerObject);
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
                ubo.projection = camera.getProjection();
                ubo.view = camera.getView();
                ubo.inverseView = glm::inverse(camera.getView());
                ubo.numLights = static_cast<int>(sceneLights.size());
                for (size_t light = 0; light < sceneLights.size(); light++) {
                    ubo.pointLights[light] = sceneLights[light];
                }

                uboBuffers[frameIndex]->writeToBuffer(&ubo);
                uboBuffers[frameIndex]->flush();

                // render
                renderer.beginSwapChainRenderPass(commandBuffer);
                pbrRenderSystem.render(frameInfo);
                renderer.endSwapChainRenderPass(commandBuffer);
                renderer.endFrame();
            }
        }

        vkDeviceWaitIdle(device.device());
    }

    void Application::loadGameObjects() {
        // Built from procedural primitives rather than OBJ files so that a
        // clean checkout runs with no binary assets present. Note this scene
        // treats -Y as up, matching the camera and light placement inherited
        // from the tutorial code.
        auto makeMaterial = [this](glm::vec3 albedo, float metallic, float roughness) {
            auto material = std::make_shared<Material>(device, *materialPool, *materialSetLayout);
            material->properties.baseColorFactor = glm::vec4{albedo, 1.f};
            material->properties.metallicFactor = metallic;
            material->properties.roughnessFactor = roughness;
            materials.push_back(material);
            return material;
        };

        auto addObject = [this](
                             std::shared_ptr<Model> model,
                             std::shared_ptr<Material> material,
                             glm::vec3 translation,
                             glm::vec3 scale,
                             glm::vec3 rotation = glm::vec3{0.f}) {
            auto object = GameObject::createGameObject();
            object.model = std::move(model);
            object.material = std::move(material);
            object.transform.translation = translation;
            object.transform.scale = scale;
            object.transform.rotation = rotation;
            gameObjects.emplace(object.getId(), std::move(object));
        };

        std::shared_ptr<Model> plane = std::make_shared<Model>(device, Model::Builder::plane());
        std::shared_ptr<Model> box = std::make_shared<Model>(device, Model::Builder::box());
        std::shared_ptr<Model> sphere =
            std::make_shared<Model>(device, Model::Builder::sphere(32, 64));

        // Floor, rotated a half turn about X so its +Y normal points along -Y,
        // which is up in this scene and therefore towards the lights.
        addObject(
            plane,
            makeMaterial(glm::vec3{0.35f}, 0.f, 0.85f),
            {0.f, .5f, 0.f},
            {8.f, 1.f, 8.f},
            {glm::pi<float>(), 0.f, 0.f});

        addObject(
            box,
            makeMaterial(glm::vec3{0.9f, 0.25f, 0.2f}, 0.f, 0.4f),
            {-.9f, .25f, 0.f},
            glm::vec3{.5f});

        // A row of spheres sweeping roughness, which is the clearest way to see
        // whether the GGX distribution and the geometry term are behaving:
        // the highlight should tighten smoothly from left to right.
        for (int i = 0; i < 5; i++) {
            const float roughness = 0.05f + 0.95f * static_cast<float>(i) / 4.f;
            addObject(
                sphere,
                makeMaterial(glm::vec3{0.95f, 0.8f, 0.35f}, 1.f, roughness),
                {-1.2f + 0.6f * static_cast<float>(i), .25f, 1.2f},
                glm::vec3{.45f});
        }

        addObject(
            sphere,
            makeMaterial(glm::vec3{0.2f, 0.5f, 0.95f}, 0.f, 0.15f),
            {.9f, .25f, 0.f},
            glm::vec3{.5f});

        // Three lights rather than the single hardcoded one, so the specular
        // response is visible from more than one direction. Remember -Y is up,
        // so a negative Y places a light above the floor.
        sceneLights.push_back(
            {glm::vec4{-1.5f, -1.6f, -1.2f, 1.f}, glm::vec4{1.f, 0.95f, 0.85f, 6.f}});
        sceneLights.push_back({glm::vec4{1.8f, -1.2f, 0.8f, 1.f}, glm::vec4{0.4f, 0.6f, 1.f, 5.f}});
        sceneLights.push_back({glm::vec4{0.f, -0.9f, 2.2f, 1.f}, glm::vec4{1.f, 0.5f, 0.3f, 3.f}});

        EGE_INFO(
            "Scene loaded: {} objects, {} materials, {} lights",
            gameObjects.size(),
            materials.size(),
            sceneLights.size());
    }

}  // namespace ege