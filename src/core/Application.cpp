#include "core/Application.hpp"

#include "core/Log.hpp"
#include "core/Time.hpp"
#include "platform/CameraController.hpp"
#include "platform/Input.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "render/Camera.hpp"
#include "render/PbrRenderSystem.hpp"
#include "rhi/Buffer.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"

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
        registerBuiltinSerializers();
        registerBuiltinComponents();
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

        loadScene();
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

        // The viewer is a plain transform rather than an entity: it is the
        // editor camera, not part of the scene being edited.
        Transform viewerTransform{};
        viewerTransform.translation = glm::vec3(0.f, -1.f, -3.f);
        // Pitched down slightly so the default view frames the scene instead of
        // leaving it along the bottom edge.
        viewerTransform.rotation.x = -.35f;
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
            cameraController.update(window.input(), frameTime, viewerTransform);
            camera.setViewYXZ(viewerTransform.translation, viewerTransform.rotation);
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
                    world};

                // update
                GlobalUbo ubo{};
                ubo.projection = camera.getProjection();
                ubo.view = camera.getView();
                ubo.inverseView = glm::inverse(camera.getView());
                // Lights are entities now, gathered per frame rather than
                // held in a parallel list. The cap is the shader's array size;
                // clustered shading in Phase 9 is what removes it.
                int lightIndex = 0;
                world.each<Transform, PointLight>(
                    [&](Entity, Transform& transform, PointLight& light) {
                        if (lightIndex >= maxPointLights) {
                            return;
                        }
                        ubo.pointLights[lightIndex].position =
                            glm::vec4{transform.translation, 1.f};
                        ubo.pointLights[lightIndex].color = glm::vec4{light.color, light.intensity};
                        lightIndex++;
                    });
                ubo.numLights = lightIndex;

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

    void Application::loadScene() {
        // Built from procedural primitives rather than asset files so that a
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

        auto addMesh = [this](
                           std::string name,
                           std::shared_ptr<Model> model,
                           std::shared_ptr<Material> material,
                           glm::vec3 translation,
                           glm::vec3 scale,
                           glm::vec3 rotation = glm::vec3{0.f}) {
            Entity entity = world.spawn(std::move(name));
            Transform transform{};
            transform.translation = translation;
            transform.scale = scale;
            transform.rotation = rotation;
            entity.attach<Transform>(transform);
            entity.attach<MeshRenderer>(MeshRenderer{std::move(model), std::move(material), true});
            return entity;
        };

        auto addLight =
            [this](std::string name, glm::vec3 position, glm::vec3 color, float intensity) {
                Entity entity = world.spawn(std::move(name));
                Transform transform{};
                transform.translation = position;
                entity.attach<Transform>(transform);
                entity.attach<PointLight>(PointLight{color, intensity, 25.f});
                return entity;
            };

        std::shared_ptr<Model> plane = std::make_shared<Model>(device, Model::Builder::plane());
        std::shared_ptr<Model> box = std::make_shared<Model>(device, Model::Builder::box());
        std::shared_ptr<Model> sphere =
            std::make_shared<Model>(device, Model::Builder::sphere(32, 64));

        // Floor, rotated a half turn about X so its +Y normal points along -Y,
        // which is up in this scene and therefore towards the lights.
        addMesh(
            "Floor",
            plane,
            makeMaterial(glm::vec3{0.35f}, 0.f, 0.85f),
            {0.f, .5f, 0.f},
            {8.f, 1.f, 8.f},
            {glm::pi<float>(), 0.f, 0.f});

        addMesh(
            "RedBox",
            box,
            makeMaterial(glm::vec3{0.9f, 0.25f, 0.2f}, 0.f, 0.4f),
            {-.9f, .25f, 0.f},
            glm::vec3{.5f});

        // A row of metal spheres sweeping roughness, which is the clearest way
        // to see whether the GGX distribution and the geometry term behave: the
        // highlight should broaden smoothly from left to right.
        for (int i = 0; i < 5; i++) {
            const float roughness = 0.05f + 0.95f * static_cast<float>(i) / 4.f;
            addMesh(
                "MetalSphere" + std::to_string(i),
                sphere,
                makeMaterial(glm::vec3{0.95f, 0.8f, 0.35f}, 1.f, roughness),
                {-1.2f + 0.6f * static_cast<float>(i), .25f, 1.2f},
                glm::vec3{.45f});
        }

        addMesh(
            "BlueSphere",
            sphere,
            makeMaterial(glm::vec3{0.2f, 0.5f, 0.95f}, 0.f, 0.15f),
            {.9f, .25f, 0.f},
            glm::vec3{.5f});

        // Lights are entities too. Remember -Y is up, so a negative Y is above
        // the floor.
        addLight("KeyLight", {-1.5f, -1.6f, -1.2f}, {1.f, 0.95f, 0.85f}, 6.f);
        addLight("FillLight", {1.8f, -1.2f, 0.8f}, {0.4f, 0.6f, 1.f}, 5.f);
        addLight("RimLight", {0.f, -0.9f, 2.2f}, {1.f, 0.5f, 0.3f}, 3.f);

        EGE_INFO(
            "Scene loaded: {} entities, {} drawn, {} lights, {} materials",
            world.entityCount(),
            world.count<Transform, MeshRenderer>(),
            world.count<Transform, PointLight>(),
            materials.size());

        verifySceneRoundTrip();
    }

    void Application::verifySceneRoundTrip() {
        // Writes the scene, reads it back into a scratch world, writes that,
        // and compares. Cheap, and it means every run exercises the path rather
        // than leaving it to the tests - which matters because serialization
        // breaks quietly when a component gains a field nothing converts.
        //
        // Note that MeshRenderer's model and material are not serialized: they
        // are runtime handles, and turning them into asset references is what
        // the asset database in Phase 6 is for. The reloaded scene therefore
        // has geometry-less renderers, which is why this runs against a scratch
        // world rather than replacing the live one.
        try {
            const std::string written = SceneSerializer::toString(world);

            World scratch;
            SceneSerializer::fromString(scratch, written);
            const std::string rewritten = SceneSerializer::toString(scratch);

            if (written == rewritten) {
                EGE_INFO(
                    "Scene round-trip verified: {} bytes, {} entities restored",
                    written.size(),
                    scratch.entityCount());
            } else {
                EGE_WARN("scene round-trip is not stable; save and load disagree");
            }

            SceneSerializer::save(world, "demo_scene.egescene");
        } catch (const std::exception& e) {
            EGE_ERROR("scene round-trip failed: {}", e.what());
        }
    }

}  // namespace ege