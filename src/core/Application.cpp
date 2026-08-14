#include "core/Application.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/GltfLoader.hpp"
#include "core/Log.hpp"
#include "core/Time.hpp"
#include "editor/EditorOverlay.hpp"
#include "editor/PlayMode.hpp"
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
#include "scene/Systems.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
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

        // Before the scene: loading one resolves asset references through the
        // database, so it has to know where the project is and how to build
        // GPU objects first.
        AssetDatabase& assets = AssetDatabase::instance();
        assets.attachDevice(device, *materialPool, *materialSetLayout);
        assets.scan(assetRoot());

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

        EditorOverlay overlay{
            window, device, renderer.getSwapChainColorFormat(), renderer.getSwapChainImageCount()};

        FrameGraph graph{};

        Camera camera{};

        PlayMode playMode{};

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

            // Fixed-rate simulation, and only while the editor is playing.
            // Keeping edit mode and play mode distinct is what lets Stop put
            // the world back: nothing advances the scene unless Play asked
            // for it, so the snapshot stays the truth until then.
            while (time.consumeFixedStep()) {
                if (playMode.consumeTick()) {
                    systems::spin(world, time.fixedStep());
                }
            }

            if (window.input().wasPressed(Key::F1)) {
                overlay.toggle();
            }

            // Rendering and camera control stay on the variable delta: they
            // should run as often as the display allows - unless a panel owns
            // the input, in which case dragging a slider must not also fly
            // the camera. A captured cursor is not over anything, so a look
            // drag that began in the scene view keeps the camera regardless.
            const bool cameraOwnsInput =
                window.input().cursorMode() != CursorMode::Normal || !overlay.wantsInput();
            if (cameraOwnsInput) {
                cameraController.update(window.input(), frameTime, viewerTransform);
            }

            // Composes world matrices for every parented entity once per frame,
            // rather than each consumer recomputing the same parent chain.
            hierarchy::resolveTransforms(world);

            if (auto commandBuffer = renderer.beginFrame()) {
                const VkExtent2D swapExtent = renderer.getSwapChainExtent();

                // Before any UI is declared: the scene view hands ImGui a
                // descriptor set for the image below, and resizing it after
                // the draw list has referenced it would free the set in use.
                overlay.prepareFrame(swapExtent);
                const EditorOverlay::SceneTarget sceneTarget = overlay.sceneTarget(swapExtent);

                // The scene is framed by whatever it renders into - the editor's
                // viewport panel while that is up, the window itself when it is
                // not - so the aspect ratio follows the target, not the display.
                // Settled before the UI is built, because the scene view's
                // gizmo projects through these very matrices.
                const VkExtent2D renderExtent = sceneTarget.extent;
                camera.setViewYXZ(viewerTransform.translation, viewerTransform.rotation);
                const float aspectRatio = static_cast<float>(renderExtent.width) /
                                          static_cast<float>(std::max(renderExtent.height, 1u));
                camera.setPerspectiveProjection(glm::radians(50.f), aspectRatio, 0.1f, 100.f);

                // The ImGui frame opens with the render frame: NewFrame and
                // Render must pair, and a frame skipped for resize renders
                // no UI either.
                overlay.beginFrame();
                EditorOverlay::Context editorContext{
                    world, camera, pbrRenderSystem.stats(), playMode, frameTime};
                overlay.buildUi(editorContext);

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
                graph.beginFrame(swapExtent);

                TransientImageDesc sceneColorDesc{};
                sceneColorDesc.format = hdrFormat;
                sceneColorDesc.extent = renderExtent;
                sceneColorDesc.clearValue.color = {{0.01f, 0.01f, 0.01f, 1.0f}};
                FrameGraphResource sceneColor = graph.createTransient("sceneColor", sceneColorDesc);

                TransientImageDesc sceneDepthDesc{};
                sceneDepthDesc.format = renderer.getSwapChainDepthFormat();
                sceneDepthDesc.extent = renderExtent;
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
                const VkExtent2D halfExtent{
                    std::max(renderExtent.width / 2, 1u), std::max(renderExtent.height / 2, 1u)};

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

                // Where the display transform lands. With the editor up that is
                // the viewport image the Scene panel samples, and the UI pass
                // becomes the only thing writing the window; with the editor
                // hidden the tonemap goes straight to the backbuffer and the
                // frame is exactly what it was before the editor existed.
                FrameGraphResource displayTarget = backbuffer;
                if (sceneTarget.offscreen) {
                    displayTarget = graph.importImage(
                        "viewport",
                        sceneTarget.image,
                        sceneTarget.view,
                        renderer.getSwapChainColorFormat(),
                        sceneTarget.extent,
                        VkClearValue{},
                        // Last frame's UI sampled this image; writing over it
                        // has to wait for that read to have finished.
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }

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
                        pass.write(displayTarget, ResourceAccess::colorWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                        postProcess.render(
                            cmd, frameIndex, resolved.view(sceneColor), resolved.view(bloomFinal));
                    });

                // UI last, onto the backbuffer. Whether it loads what the
                // tonemap left there or samples the viewport image and paints
                // the window itself is one declared read either way; the
                // render-to-sample transition is the graph's problem.
                graph.addPass(
                    "ui",
                    [&](FrameGraph::PassBuilder& pass) {
                        if (sceneTarget.offscreen) {
                            pass.read(displayTarget, ResourceAccess::sampled);
                        }
                        pass.write(backbuffer, ResourceAccess::colorWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources&) { overlay.render(cmd); });

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
        //
        // Everything here is catalogued in the asset database under a
        // name-derived id, which is what lets a scene built from code rather
        // than from files still be saved and loaded: the ids are the same in
        // the next run because the names are.
        AssetDatabase& assets = AssetDatabase::instance();

        auto makeMaterial =
            [this, &assets](
                const std::string& name, glm::vec3 albedo, float metallic, float roughness) {
                auto material =
                    std::make_shared<Material>(device, *materialPool, *materialSetLayout);
                material->properties.baseColorFactor = glm::vec4{albedo, 1.f};
                material->properties.metallicFactor = metallic;
                material->properties.roughnessFactor = roughness;
                const Guid id = assets.addMaterial(name, material);
                return MaterialRef{id, std::move(material)};
            };

        auto addMesh = [this](
                           std::string name,
                           MeshRef mesh,
                           MaterialRef material,
                           glm::vec3 translation,
                           glm::vec3 scale,
                           glm::vec3 rotation = glm::vec3{0.f}) {
            Entity entity = world.spawn(std::move(name));
            Transform transform{};
            transform.translation = translation;
            transform.scale = scale;
            transform.rotation = rotation;
            entity.attach<Transform>(transform);
            entity.attach<MeshRenderer>(MeshRenderer{std::move(mesh), std::move(material), true});
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

        const auto primitive = [this, &assets](const std::string& name, Model::Builder builder) {
            auto model = std::make_shared<Model>(device, builder);
            const Guid id = assets.addMesh(name, model);
            return MeshRef{id, std::move(model)};
        };

        const MeshRef plane = primitive("plane", Model::Builder::plane());
        const MeshRef box = primitive("box", Model::Builder::box());
        const MeshRef sphere = primitive("sphere", Model::Builder::sphere(32, 64));

        // Floor, rotated a half turn about X so its +Y normal points along -Y,
        // which is up in this scene and therefore towards the lights.
        addMesh(
            "Floor",
            plane,
            makeMaterial("Floor", glm::vec3{0.35f}, 0.f, 0.85f),
            {0.f, .5f, 0.f},
            {8.f, 1.f, 8.f},
            {glm::pi<float>(), 0.f, 0.f});

        Entity redBox = addMesh(
            "RedBox",
            box,
            makeMaterial("RedBox", glm::vec3{0.9f, 0.25f, 0.2f}, 0.f, 0.4f),
            {-.9f, .25f, 0.f},
            glm::vec3{.5f});
        redBox.attach<Spin>(Spin{{0.f, 1.2f, 0.f}});

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
        // Something for Play to do, and for Stop to undo. Turning the pivot
        // rather than the spheres also demonstrates that the hierarchy is
        // carrying the motion down to its children.
        sphereRow.attach<Spin>(Spin{{0.f, 0.5f, 0.f}});

        for (int i = 0; i < 5; i++) {
            const float roughness = 0.05f + 0.95f * static_cast<float>(i) / 4.f;
            Entity ball = addMesh(
                "MetalSphere" + std::to_string(i),
                sphere,
                makeMaterial(
                    "Metal" + std::to_string(i), glm::vec3{0.95f, 0.8f, 0.35f}, 1.f, roughness),
                {-1.2f + 0.6f * static_cast<float>(i), 0.f, 0.f},
                glm::vec3{.45f});
            hierarchy::setParent(world, ball.id(), sphereRow.id());
        }

        addMesh(
            "BlueSphere",
            sphere,
            makeMaterial("BlueSphere", glm::vec3{0.2f, 0.5f, 0.95f}, 0.f, 0.15f),
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

        importGltfModels();

        EGE_INFO(
            "Scene loaded: {} entities, {} drawn, {} lights, {} assets",
            world.entityCount(),
            world.count<Transform, MeshRenderer>(),
            world.count<Transform, PointLight>(),
            AssetDatabase::instance().all().size());

        verifySceneRoundTrip();
    }

    std::filesystem::path Application::assetRoot() {
#ifdef EGE_ASSET_ROOT
        return std::filesystem::path{EGE_ASSET_ROOT};
#else
        return std::filesystem::path{"assets"};
#endif
    }

    void Application::importGltfModels() {
        // Driven by the database rather than by another directory walk: the
        // scan has already found every model and given it an id, and that id
        // is what the meshes and materials inside it are numbered from.
        const AssetDatabase& assets = AssetDatabase::instance();

        std::vector<AssetRecord> scenes;
        for (const AssetRecord& record : assets.all()) {
            if (record.kind == AssetKind::scene && !record.path.empty()) {
                scenes.push_back(record);
            }
        }

        for (const AssetRecord& record : scenes) {
            const std::filesystem::path path = assetRoot() / record.path;

            // One bad file should not take down the ones beside it, or the
            // procedural scene it would have joined.
            try {
                const GltfSceneData scene = gltf::parseFile(path.string());
                const gltf::ImportStats stats = gltf::instantiate(
                    device,
                    world,
                    scene,
                    *materialPool,
                    *materialSetLayout,
                    record.name,
                    record.id);
                EGE_INFO(
                    "Imported {}: {} entities, {} meshes, {} materials, {} textures",
                    record.path.string(),
                    stats.entities,
                    stats.meshes,
                    stats.materials,
                    stats.textures);
            } catch (const std::exception& error) {
                EGE_ERROR("{}", error.what());
            }
        }
    }

    void Application::verifySceneRoundTrip() {
        // Writes the scene, reads it back into a scratch world, writes that,
        // and compares. Cheap, and it means every run exercises the path rather
        // than leaving it to the tests - which matters because serialization
        // breaks quietly when a component gains a field nothing converts.
        //
        // The reloaded world now comes back drawable. Until the asset database
        // there was nothing to write down for a MeshRenderer but a pointer, so
        // a reloaded scene had its transforms, names and lights and nothing
        // visible in it; counting the renderers that resolved is what would
        // notice that returning.
        try {
            const std::string written = SceneSerializer::toString(world);

            World scratch;
            SceneSerializer::fromString(scratch, written);
            const std::string rewritten = SceneSerializer::toString(scratch);

            std::size_t restoredDraws = 0;
            scratch.each<MeshRenderer>([&](Entity, MeshRenderer& restored) {
                if (restored.mesh.resolved() && restored.material.resolved()) {
                    restoredDraws++;
                }
            });

            if (written == rewritten) {
                EGE_INFO(
                    "Scene round-trip verified: {} bytes, {} entities and {} draws restored",
                    written.size(),
                    scratch.entityCount(),
                    restoredDraws);
            } else {
                EGE_WARN("scene round-trip is not stable; save and load disagree");
            }
            if (restoredDraws != world.count<Transform, MeshRenderer>()) {
                EGE_WARN(
                    "scene round-trip lost geometry: {} of {} renderers resolved",
                    restoredDraws,
                    world.count<Transform, MeshRenderer>());
            }

            SceneSerializer::save(world, "demo_scene.egescene");
        } catch (const std::exception& e) {
            EGE_ERROR("scene round-trip failed: {}", e.what());
        }
    }

}  // namespace ege