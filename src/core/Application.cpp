#include "core/Application.hpp"

#include "core/Log.hpp"
#include "core/Time.hpp"
#include "platform/CameraController.hpp"
#include "platform/Input.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "render/BloomSystem.hpp"
#include "render/Camera.hpp"
#include "render/EnvironmentLighting.hpp"
#include "render/PbrRenderSystem.hpp"
#include "render/PostProcessSystem.hpp"
#include "render/ShadowMapSystem.hpp"
#include "render/SkyboxSystem.hpp"
#include "rhi/Buffer.hpp"
#include "rhi/FrameGraph.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
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
            // The skybox unprojects pixels back into rays with this.
            glm::mat4 inverseProjection{1.f};
            // The sun: the shadow pass renders through this matrix and the
            // lighting pass projects fragments back through it for the test.
            glm::mat4 sunViewProjection{1.f};
            glm::vec4 sunDirection{0.f, 1.f, 0.f, 0.f};
            glm::vec4 sunColor{1.f, 1.f, 1.f, 0.f};  // w is intensity, 0 = off
            // A tint and scale on the image-based ambient, which is already
            // physical - so the neutral value is 1, not a small fudge factor.
            glm::vec4 ambientLightColor{1.f, 1.f, 1.f, 1.f};  // w is intensity
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
        // Each per-frame global set holds the UBO plus the four image-based
        // lighting maps: irradiance, prefiltered specular, BRDF LUT and the
        // raw environment for the skybox.
        globalPool =
            DescriptorPool::Builder(device)
                .setMaxSets(SwapChain::MAX_FRAMES_IN_FLIGHT)
                .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SwapChain::MAX_FRAMES_IN_FLIGHT)
                .addPoolSize(
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SwapChain::MAX_FRAMES_IN_FLIGHT * 5)
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
        // Generated before anything renders: the lighting environment is as
        // much a prerequisite of the frame as the meshes are.
        EnvironmentLighting environmentLighting{device};

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
                .addBinding(
                    1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .addBinding(
                    2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .addBinding(
                    3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .addBinding(
                    4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                // The sun's shadow map. Unlike the lighting maps this one is
                // a frame graph transient, so the binding is rewritten every
                // frame with whatever physical image the graph provides.
                .addBinding(
                    5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .build();

        std::vector<VkDescriptorSet> globalDescriptorSets(SwapChain::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < globalDescriptorSets.size(); i++) {
            auto bufferInfo = uboBuffers[i]->descriptorInfo();
            // The lighting maps are generated once and never change, so they
            // are written alongside the per-frame buffer and left alone.
            auto irradianceInfo = environmentLighting.irradianceInfo();
            auto prefilteredInfo = environmentLighting.prefilteredInfo();
            auto brdfLutInfo = environmentLighting.brdfLutInfo();
            auto environmentInfo = environmentLighting.environmentInfo();
            DescriptorWriter(*globalSetLayout, *globalPool)
                .writeBuffer(0, &bufferInfo)
                .writeImage(1, &irradianceInfo)
                .writeImage(2, &prefilteredInfo)
                .writeImage(3, &brdfLutInfo)
                .writeImage(4, &environmentInfo)
                .build(globalDescriptorSets[i]);
        }

        // The scene renders into a linear HDR target, not the backbuffer:
        // lighting sums exceed 1 constantly, and clamping them at the
        // swapchain is what made every bright highlight flat white. Sixteen
        // bits per channel is the float format with guaranteed
        // color-attachment support.
        constexpr VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

        PbrRenderSystem pbrRenderSystem{
            device,
            hdrFormat,
            renderer.getSwapChainDepthFormat(),
            globalSetLayout->getDescriptorSetLayout(),
            materialSetLayout->getDescriptorSetLayout()};

        SkyboxSystem skybox{
            device,
            hdrFormat,
            renderer.getSwapChainDepthFormat(),
            globalSetLayout->getDescriptorSetLayout()};

        ShadowMapSystem shadowSystem{device, renderer.getSwapChainDepthFormat()};

        BloomSystem bloom{device, hdrFormat, SwapChain::MAX_FRAMES_IN_FLIGHT};

        PostProcessSystem postProcess{
            device, renderer.getSwapChainColorFormat(), SwapChain::MAX_FRAMES_IN_FLIGHT};

        FrameGraph graph{};

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

        // Pipelines exist by this point, so the cache has something worth
        // keeping. Saved here rather than only at shutdown because a killed
        // process never unwinds.
        device.savePipelineCache();

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

            // Composes world matrices for every parented entity once per frame,
            // rather than each consumer recomputing the same parent chain.
            hierarchy::resolveTransforms(world);
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
                ubo.inverseProjection = glm::inverse(camera.getProjection());

                // The sun is the first DirectionalLight found; none means the
                // shader's sun term stays off via zero intensity.
                bool hasSun = false;
                world.each<DirectionalLight>([&](Entity, DirectionalLight& sun) {
                    if (hasSun) {
                        return;
                    }
                    hasSun = true;
                    const glm::vec3 direction = glm::normalize(sun.direction);
                    ubo.sunDirection = glm::vec4{direction, 0.f};
                    ubo.sunColor = glm::vec4{sun.color, sun.intensity};

                    // An orthographic frustum sized to the demo floor, looking
                    // along the light. Fitting it to the view frustum - and
                    // splitting it into cascades - is what replaces this once
                    // scenes outgrow a single fixed box.
                    Camera sunCamera{};
                    sunCamera.setViewTarget(-direction * 20.f, glm::vec3{0.f});
                    sunCamera.setOrthographicProjection(-12.f, 12.f, -12.f, 12.f, 1.f, 40.f);
                    ubo.sunViewProjection = sunCamera.getProjection() * sunCamera.getView();
                });
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

                // render: declare the frame, then let the graph run it. The
                // declarations are cheap enough to restate every frame, and
                // doing so is what lets passes appear and disappear freely.
                graph.beginFrame(renderer.getSwapChainExtent());

                TransientImageDesc sceneColorDesc{};
                sceneColorDesc.format = hdrFormat;
                sceneColorDesc.clearValue.color = {{0.01f, 0.01f, 0.01f, 1.0f}};
                FrameGraphResource sceneColor = graph.createTransient("sceneColor", sceneColorDesc);

                TransientImageDesc sceneDepthDesc{};
                sceneDepthDesc.format = renderer.getSwapChainDepthFormat();
                sceneDepthDesc.clearValue.depthStencil = {1.0f, 0};
                FrameGraphResource sceneDepth = graph.createTransient("sceneDepth", sceneDepthDesc);

                // Fixed-size, not swapchain-relative: shadow quality has
                // nothing to do with window size.
                TransientImageDesc shadowMapDesc{};
                shadowMapDesc.format = renderer.getSwapChainDepthFormat();
                shadowMapDesc.extent = {ShadowMapSystem::resolution, ShadowMapSystem::resolution};
                shadowMapDesc.clearValue.depthStencil = {1.0f, 0};
                FrameGraphResource shadowMap = graph.createTransient("shadowMap", shadowMapDesc);

                // Bloom works at half resolution: it is blurred anyway, and
                // half the pixels means a quarter of the blur cost.
                const VkExtent2D swapExtent = renderer.getSwapChainExtent();
                const VkExtent2D halfExtent{
                    std::max(swapExtent.width / 2, 1u), std::max(swapExtent.height / 2, 1u)};

                TransientImageDesc bloomDesc{};
                bloomDesc.format = hdrFormat;
                bloomDesc.extent = halfExtent;
                FrameGraphResource bloomBright = graph.createTransient("bloomBright", bloomDesc);
                FrameGraphResource bloomBlurred = graph.createTransient("bloomBlurred", bloomDesc);
                FrameGraphResource bloomFinal = graph.createTransient("bloomFinal", bloomDesc);

                FrameGraphResource backbuffer = graph.importImage(
                    "backbuffer",
                    renderer.currentSwapChainImage(),
                    renderer.currentSwapChainImageView(),
                    renderer.getSwapChainColorFormat(),
                    renderer.getSwapChainExtent(),
                    VkClearValue{},
                    // What the acquire semaphore is waited at, so the first
                    // backbuffer barrier chains after the acquire.
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

                const glm::mat4 sunViewProjection = ubo.sunViewProjection;

                graph.addPass(
                    "shadow",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.write(shadowMap, ResourceAccess::depthWrite);
                    },
                    [&, sunViewProjection](VkCommandBuffer cmd, const FrameGraphResources&) {
                        shadowSystem.render(cmd, world, sunViewProjection);
                    });

                graph.addPass(
                    "scene",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.read(shadowMap, ResourceAccess::sampled);
                        pass.write(sceneColor, ResourceAccess::colorWrite);
                        pass.write(sceneDepth, ResourceAccess::depthWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                        // The shadow map is a graph transient, so which
                        // physical image backs it is only known here; the
                        // per-frame set makes rebinding it safe.
                        VkDescriptorImageInfo shadowInfo{};
                        shadowInfo.sampler = shadowSystem.comparisonSampler();
                        shadowInfo.imageView = resolved.view(shadowMap);
                        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        DescriptorWriter(*globalSetLayout, *globalPool)
                            .writeImage(5, &shadowInfo)
                            .overwrite(globalDescriptorSets[frameIndex]);

                        frameInfo.commandBuffer = cmd;
                        pbrRenderSystem.render(frameInfo);
                        // After the geometry: the depth test rejects every
                        // covered pixel, so the sky shades only what remains.
                        skybox.render(cmd, frameInfo.globalDescriptorSet);
                    });

                // The bloom chain: what glows, extracted and blurred. Three
                // passes, each an addPass call - the graph derives all the
                // render-to-sample transitions between them.
                graph.addPass(
                    "bloomBright",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.read(sceneColor, ResourceAccess::sampled);
                        pass.write(bloomBright, ResourceAccess::colorWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                        bloom.renderBrightPass(cmd, frameIndex, resolved.view(sceneColor));
                    });

                graph.addPass(
                    "bloomBlurH",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.read(bloomBright, ResourceAccess::sampled);
                        pass.write(bloomBlurred, ResourceAccess::colorWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                        bloom.renderBlur(
                            cmd, frameIndex, resolved.view(bloomBright), glm::vec2{1.f, 0.f});
                    });

                graph.addPass(
                    "bloomBlurV",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.read(bloomBlurred, ResourceAccess::sampled);
                        pass.write(bloomFinal, ResourceAccess::colorWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                        bloom.renderBlur(
                            cmd, frameIndex, resolved.view(bloomBlurred), glm::vec2{0.f, 1.f});
                    });

                graph.addPass(
                    "tonemap",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.read(sceneColor, ResourceAccess::sampled);
                        pass.read(bloomFinal, ResourceAccess::sampled);
                        pass.write(backbuffer, ResourceAccess::colorWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                        postProcess.render(
                            cmd, frameIndex, resolved.view(sceneColor), resolved.view(bloomFinal));
                    });

                graph.compile();
                graph.execute(device, commandBuffer);
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
        //
        // Parented under a pivot so the hierarchy is exercised by the running
        // engine rather than only by the tests: the spheres' positions below are
        // relative to it, and moving the pivot moves the whole row.
        Entity sphereRow = world.spawn("SphereRow");
        Transform rowTransform{};
        rowTransform.translation = {0.f, .25f, 1.2f};
        sphereRow.attach<Transform>(rowTransform);

        for (int i = 0; i < 5; i++) {
            const float roughness = 0.05f + 0.95f * static_cast<float>(i) / 4.f;
            Entity ball = addMesh(
                "MetalSphere" + std::to_string(i),
                sphere,
                makeMaterial(glm::vec3{0.95f, 0.8f, 0.35f}, 1.f, roughness),
                {-1.2f + 0.6f * static_cast<float>(i), 0.f, 0.f},
                glm::vec3{.45f});
            hierarchy::setParent(world, ball.id(), sphereRow.id());
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

        // The sun. Its direction is the negation of the sky shader's sun
        // position, so the disk in the environment, the direct light and the
        // shadows all agree on where the sun is.
        {
            Entity sun = world.spawn("Sun");
            DirectionalLight sunLight{};
            sunLight.direction = glm::normalize(glm::vec3{0.6f, 0.64f, 0.48f});
            sunLight.color = glm::vec3{1.f, 0.93f, 0.82f};
            // Low sun, matching the evening sky - bright enough to cast
            // legible shadows without flattening the point lights.
            sunLight.intensity = 1.4f;
            sun.attach<DirectionalLight>(sunLight);
        }

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