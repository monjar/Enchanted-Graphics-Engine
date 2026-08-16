#include "core/Application.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/GltfLoader.hpp"
#include "core/DemoTour.hpp"
#include "core/FileWatcher.hpp"
#include "core/Log.hpp"
#include "core/Time.hpp"
#include "editor/EditorOverlay.hpp"
#include "editor/PlayMode.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "platform/CameraController.hpp"
#include "platform/Input.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "render/BloomSystem.hpp"
#include "render/Camera.hpp"
#include "render/ClusterGrid.hpp"
#include "render/ClusterLightSystem.hpp"
#include "render/DynamicMesh.hpp"
#include "render/EnvironmentLighting.hpp"
#include "render/PbrRenderSystem.hpp"
#include "render/PostProcessSystem.hpp"
#include "render/ShadowCascades.hpp"
#include "render/ShadowMapSystem.hpp"
#include "render/SkyboxSystem.hpp"
#include "rhi/Buffer.hpp"
#include "rhi/FrameGraph.hpp"
#include "rhi/FrameRecorder.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/Systems.hpp"
#include "script/BehaviorRegistry.hpp"
#include "script/Behaviors.hpp"
#include "script/Script.hpp"
#include "script/ScriptSystem.hpp"

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
            // The sun, one entry per cascade: each shadow map is rendered
            // through its matrix and the lighting pass projects fragments
            // back through whichever cascade covers them.
            glm::mat4 sunViewProjection[maxShadowCascades]{};
            // Where each cascade ends, in view depth. A vec4 because that is
            // how std140 would pad four floats anyway, and the shader indexes
            // it as one.
            glm::vec4 cascadeSplits{0.f};
            glm::vec4 sunDirection{0.f, 1.f, 0.f, 0.f};
            glm::vec4 sunColor{1.f, 1.f, 1.f, 0.f};  // w is intensity, 0 = off
            // A tint and scale on the image-based ambient, which is already
            // physical - so the neutral value is 1, not a small fudge factor.
            glm::vec4 ambientLightColor{1.f, 1.f, 1.f, 1.f};  // w is intensity
            // Clustered shading. The lights themselves are no longer here:
            // a fixed array in a uniform block is what capped the scene at
            // sixteen of them, and they now live in a storage buffer that
            // nothing loops over per fragment.
            // x: depth-slice scale, y: depth-slice bias, z: near, w: far.
            glm::vec4 clusterParams{0.f};
            // xyz: cells per axis, w: how many lights one cluster records.
            glm::uvec4 clusterGrid{0u};
            glm::vec4 screenSize{0.f};  // xy: the extent the scene renders at
            alignas(16) int numLights = 0;
            int cascadeCount = 0;
        };

    }  // namespace

    Application::Application(Options optionsRef) : options{optionsRef} {
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
                // Two storage buffers per frame: the scene's lights and the
                // per-cluster lists culled from them.
                .addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SwapChain::MAX_FRAMES_IN_FLIGHT * 2)
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
        // The asset database is a singleton, so it outlives this object and
        // would otherwise release its meshes, textures and materials during
        // static destruction - with the device already gone. That is a real
        // crash on exit, and it stayed hidden for as long as the only way to
        // stop the engine was to kill it.
        AssetDatabase::instance().clear();

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

        // The scene's lights, gathered per frame into a storage buffer. Host
        // visible because the CPU writes it every frame, like the uniform
        // buffer beside it; one per frame in flight for the same reason.
        std::vector<std::unique_ptr<Buffer>> lightBuffers(SwapChain::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < lightBuffers.size(); i++) {
            lightBuffers[i] = std::make_unique<Buffer>(
                device,
                sizeof(GpuPointLight),
                maxSceneLights,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            lightBuffers[i]->map();
        }

        auto globalSetLayout =
            DescriptorSetLayout::Builder(device)
                // Compute as well as graphics: the light culling pass binds
                // this same set and reads the same camera out of it.
                .addBinding(
                    0,
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT)
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
                // The scene's lights, and the per-cluster lists built from
                // them. The culling pass writes the second and reads the
                // first; the scene pass reads both.
                .addBinding(
                    6,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)
                .addBinding(
                    7,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)
                .build();

        std::vector<VkDescriptorSet> globalDescriptorSets(SwapChain::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < globalDescriptorSets.size(); i++) {
            auto bufferInfo = uboBuffers[i]->descriptorInfo();
            auto lightInfo = lightBuffers[i]->descriptorInfo();
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
                .writeBuffer(6, &lightInfo)
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

        // How the sun's cascades are cut. Shadows stop well short of the
        // camera's far plane on purpose: stretching four cascades over a
        // hundred units would spend the near map's texels on distance where
        // no one can resolve a shadow edge anyway.
        constexpr uint32_t shadowCascadeCount = 4;
        constexpr float shadowNearPlane = 0.1f;
        constexpr float shadowDistance = 40.f;
        ShadowCascadeSet cascades{};

        // How far out lights are clustered. The near plane matches the
        // camera's; the far one is where a point light's inverse-square
        // falloff has long since stopped being visible, and clustering past
        // it would spend depth slices on nothing.
        constexpr float clusterNearPlane = 0.1f;
        constexpr float clusterFarPlane = 100.f;

        ClusterLightSystem clusterLights{device, SwapChain::MAX_FRAMES_IN_FLIGHT};

        BloomSystem bloom{device, hdrFormat, SwapChain::MAX_FRAMES_IN_FLIGHT};

        PostProcessSystem postProcess{
            device, renderer.getSwapChainColorFormat(), SwapChain::MAX_FRAMES_IN_FLIGHT};

        EditorOverlay overlay{
            window, device, renderer.getSwapChainColorFormat(), renderer.getSwapChainImageCount()};

        FrameGraph graph{};

        Camera camera{};

        PlayMode playMode{};
        ScriptSystem scripts{};
        PhysicsSystem physics{};

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

        // The demo runs itself: no editor over the picture, the scene already
        // playing, and the camera on rails. Everything else about the frame is
        // identical, which is the point - it is the engine being shown, not a
        // separate presentation mode.
        DemoTour tour = DemoTour::demoScene();
        if (options.demo) {
            overlay.toggle();
            playMode.play(world);
            EGE_INFO("Demo tour: {:.1f} seconds", tour.duration());
        }

        // Recording replaces the clock as well as the output: a frame takes
        // as long as it takes, and the tour has to advance by the same amount
        // each time regardless, or the same recording made on two machines
        // would not be the same recording.
        std::unique_ptr<FrameRecorder> recorder;
        if (!options.recordDirectory.empty()) {
            if (renderer.canCaptureFrames()) {
                recorder = std::make_unique<FrameRecorder>(
                    device, options.recordDirectory, renderer.getSwapChainExtent());
            } else {
                EGE_ERROR("this driver cannot copy from swapchain images; not recording");
            }
        }

        // Assets reload while the engine runs: edit a material file, save it,
        // and the scene changes without a restart. Half a second is short
        // enough to feel immediate and long enough that a directory walk per
        // interval is free.
        FileWatcher assetWatcher{assetRoot(), std::chrono::milliseconds{500}};

        bool running = true;
        bool scriptsRunning = false;
        float sessionSeconds = 0.f;

        while (running && !window.shouldClose()) {
            Window::pollEvents();

            // After the poll, so that a pause spent inside it is measured as
            // part of the frame it belongs to.
            time.beginFrame();
            const float frameTime =
                recorder == nullptr ? time.delta() : 1.f / options.recordFrameRate;

            window.input().newFrame();

            // Behaviours start when play does, not when they are attached:
            // in the editor an entity carrying one is a description of what
            // will happen, and Play is what makes it happen.
            const bool wasPlaying = scriptsRunning;
            scriptsRunning = !playMode.isEditing();
            if (scriptsRunning) {
                scripts.spawnPending(world);
            } else if (wasPlaying) {
                scripts.despawnAll(world);
            }

            // Physics lives and dies with play, exactly as behaviours do: in
            // edit mode a RigidBody is a description of what will fall, and
            // Play is what drops it. Stop throws the physics world away and
            // the snapshot restore puts the transforms back, so simulation
            // can never leak into the scene being authored.
            if (scriptsRunning && !physics.running()) {
                PhysicsWorld::Settings physicsSettings{};
                // This scene treats -Y as up, so down - the way things
                // fall - is +Y.
                physicsSettings.gravity = {0.f, 9.81f, 0.f};
                physics.start(world, physicsSettings);
            } else if (!scriptsRunning && physics.running()) {
                physics.stop(world);
            }

            // Fixed-rate simulation, and only while the editor is playing.
            // Keeping edit mode and play mode distinct is what lets Stop put
            // the world back: nothing advances the scene unless Play asked
            // for it, so the snapshot stays the truth until then.
            //
            // Scripts run before the physics step so that what they wrote
            // this tick - a kinematic platform's transform, an impulse - is
            // what the step integrates; contacts are delivered after it,
            // when the poses they describe are the poses the world shows.
            while (time.consumeFixedStep()) {
                if (playMode.consumeTick()) {
                    scripts.fixedTick(world, time.fixedStep());
                    const std::vector<EntityContact> contacts =
                        physics.fixedTick(world, time.fixedStep());
                    scripts.deliverContacts(world, contacts);
                }
            }
            if (playMode.isPlaying()) {
                scripts.tick(world, frameTime);
            }

            if (window.input().wasPressed(Key::F1)) {
                overlay.toggle();
            }

            // Rendering and camera control stay on the variable delta: they
            // should run as often as the display allows - unless a panel owns
            // the input, in which case dragging a slider must not also fly
            // the camera. A captured cursor is not over anything, so a look
            // drag that began in the scene view keeps the camera regardless.
            if (options.demo) {
                if (!tour.advance(frameTime, viewerTransform)) {
                    running = false;
                }
            } else {
                const bool cameraOwnsInput =
                    window.input().cursorMode() != CursorMode::Normal || !overlay.wantsInput();
                if (cameraOwnsInput) {
                    cameraController.update(window.input(), frameTime, viewerTransform);
                }
            }

            sessionSeconds += frameTime;
            if (options.exitAfterSeconds > 0.f && sessionSeconds >= options.exitAfterSeconds) {
                running = false;
            }

            reloadChangedAssets(assetWatcher);

            // After the scripts, before anything reads geometry: a behaviour
            // that rewrote a surface this tick wants it drawn this frame.
            systems::uploadDynamicMeshes(world, device);

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

                    // Fitted to what the camera can actually see, split by
                    // depth so texel density follows the viewer rather than
                    // the scene's bounding box.
                    CascadeSettings cascadeSettings{};
                    cascadeSettings.count = shadowCascadeCount;
                    cascadeSettings.resolution = ShadowMapSystem::resolution;
                    cascades = fitShadowCascades(
                        glm::inverse(camera.getProjection() * camera.getView()),
                        direction,
                        shadowNearPlane,
                        // Shadows stop well short of the camera's far plane.
                        // Stretching cascades to a hundred units would spend
                        // the near map's texels on distance nobody can
                        // resolve a shadow edge at anyway.
                        shadowDistance,
                        cascadeSettings);

                    for (uint32_t i = 0; i < cascades.count; i++) {
                        ubo.sunViewProjection[i] = cascades.cascades[i].viewProjection;
                        ubo.cascadeSplits[static_cast<int>(i)] = cascades.cascades[i].splitDepth;
                    }
                    ubo.cascadeCount = static_cast<int>(cascades.count);
                });
                // Lights are entities, gathered per frame into a storage
                // buffer. No fragment loops over this many - the culling pass
                // reduces it to the few that reach each cluster - so the cap
                // here bounds allocation rather than shading cost.
                std::vector<GpuPointLight> sceneLights;
                sceneLights.reserve(maxSceneLights);
                world.each<Transform, PointLight>(
                    [&](Entity, Transform& transform, PointLight& light) {
                        if (sceneLights.size() >= maxSceneLights) {
                            return;
                        }
                        GpuPointLight gpuLight{};
                        // The cull radius rides in position.w, which is where
                        // the culling shader reads it: the light's own range,
                        // the distance past which it is treated as
                        // contributing nothing.
                        gpuLight.position = glm::vec4{transform.translation, light.range};
                        gpuLight.color = glm::vec4{light.color, light.intensity};
                        sceneLights.push_back(gpuLight);
                    });
                ubo.numLights = static_cast<int>(sceneLights.size());

                if (!sceneLights.empty()) {
                    lightBuffers[frameIndex]->writeToBuffer(
                        sceneLights.data(), sceneLights.size() * sizeof(GpuPointLight));
                    lightBuffers[frameIndex]->flush();
                }

                // How the froxel grid is cut, and what the shader needs to
                // find its own cell in it.
                ClusterGrid clusterGrid{};
                clusterGrid.nearPlane = clusterNearPlane;
                clusterGrid.farPlane = clusterFarPlane;
                const glm::vec2 sliceScaleBias = clusterSliceScaleBias(clusterGrid);
                ubo.clusterParams = glm::vec4{
                    sliceScaleBias.x,
                    sliceScaleBias.y,
                    clusterGrid.nearPlane,
                    clusterGrid.farPlane};
                ubo.clusterGrid =
                    glm::uvec4{clusterGrid.x, clusterGrid.y, clusterGrid.z, maxLightsPerCluster};
                ubo.screenSize = glm::vec4{
                    static_cast<float>(renderExtent.width),
                    static_cast<float>(renderExtent.height),
                    0.f,
                    0.f};

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
                // One layer per cascade, sampled as an array so the shader can
                // pick a layer per fragment with an ordinary texture
                // coordinate rather than indexing a list of bindings.
                const uint32_t cascadeCount = static_cast<uint32_t>(std::max(ubo.cascadeCount, 1));
                shadowMapDesc.layers = cascadeCount;
                FrameGraphResource shadowMap = graph.createTransient("shadowMap", shadowMapDesc);

                // Where the culling pass puts each cluster's light list. A
                // graph resource rather than a buffer of our own, so the
                // barrier between writing it in compute and reading it in the
                // scene's fragment shader is derived like every other.
                TransientBufferDesc clusterListDesc{};
                clusterListDesc.size = ClusterLightSystem::clusterBufferSize();
                FrameGraphResource clusterList =
                    graph.createTransientBuffer("clusterLights", clusterListDesc);

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
                    // While recording, the graph hands the image over ready to
                    // be copied and the recorder returns it to PRESENT_SRC.
                    // Reading an image that has already been presented is the
                    // kind of thing that works on one driver and corrupts on
                    // another.
                    recorder == nullptr ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                        : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

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

                // One depth pass per cascade, each into its own layer. The
                // graph derives the barrier between them and the transition
                // to sampled before the scene pass reads the array.
                for (uint32_t cascade = 0; cascade < cascadeCount; cascade++) {
                    const glm::mat4 cascadeMatrix = ubo.sunViewProjection[cascade];
                    graph.addPass(
                        "shadowCascade" + std::to_string(cascade),
                        [&, cascade](FrameGraph::PassBuilder& pass) {
                            pass.write(shadowMap, ResourceAccess::depthWrite, cascade);
                        },
                        [&, cascadeMatrix](VkCommandBuffer cmd, const FrameGraphResources&) {
                            shadowSystem.render(cmd, world, cascadeMatrix);
                        });
                }

                // Light culling, before anything shades. Not a raster pass at
                // all - it declares no attachment, so the graph runs it
                // outside vkCmdBeginRendering and derives the compute-to-
                // fragment dependency on the buffer by itself.
                graph.addPass(
                    "lightCull",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.write(clusterList, ResourceAccess::storageWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                        VkDescriptorBufferInfo clusterInfo{};
                        clusterInfo.buffer = resolved.buffer(clusterList);
                        clusterInfo.offset = 0;
                        clusterInfo.range = resolved.bufferSize(clusterList);

                        clusterLights.cull(
                            cmd,
                            frameIndex,
                            uboBuffers[frameIndex]->descriptorInfo(),
                            lightBuffers[frameIndex]->descriptorInfo(),
                            clusterInfo);
                    });

                graph.addPass(
                    "scene",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.read(shadowMap, ResourceAccess::sampled);
                        pass.read(clusterList, ResourceAccess::storageRead);
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

                        VkDescriptorBufferInfo clusterInfo{};
                        clusterInfo.buffer = resolved.buffer(clusterList);
                        clusterInfo.offset = 0;
                        clusterInfo.range = resolved.bufferSize(clusterList);

                        // Both writes happen here, before anything in this
                        // frame binds the set: updating a descriptor set that
                        // a command buffer has already bound invalidates the
                        // command buffer, so every per-frame write to this set
                        // has to land before its first use.
                        DescriptorWriter(*globalSetLayout, *globalPool)
                            .writeImage(5, &shadowInfo)
                            .writeBuffer(7, &clusterInfo)
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
                if (recorder != nullptr) {
                    recorder->recordCopy(
                        commandBuffer,
                        renderer.currentSwapChainImage(),
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                }
                renderer.endFrame();
                if (recorder != nullptr) {
                    recorder->writeFrame(renderer.getSwapChainColorFormat());
                }
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

        // The one material in the scene that comes from a file. Everything
        // else is built in code, which is fine until you want to change
        // something without rebuilding - so the floor is the surface that
        // proves asset hot reload works: edit assets/materials/floor.egematerial
        // while the engine is running and the floor changes.
        auto fileMaterial = [&assets](const std::filesystem::path& relative) {
            MaterialRef reference{};
            if (const AssetRecord* record = assets.findByPath(relative)) {
                if (std::shared_ptr<Material> material = assets.material(record->id)) {
                    reference = MaterialRef{record->id, std::move(material)};
                }
            }
            return reference;
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
        MaterialRef floorMaterial =
            fileMaterial(std::filesystem::path{"materials"} / "floor.egematerial");
        if (floorMaterial.value == nullptr) {
            // The file is missing or unreadable. The demo still runs, in the
            // same grey it would have had - a broken asset should cost you
            // that asset, not the scene.
            EGE_WARN("floor material file unavailable; using the built-in floor material");
            floorMaterial = makeMaterial("Floor", glm::vec3{0.35f}, 0.f, 0.85f);
        }
        Entity floor = addMesh(
            "Floor",
            plane,
            floorMaterial,
            {0.f, .5f, 0.f},
            {8.f, 1.f, 8.f},
            {glm::pi<float>(), 0.f, 0.f});
        // A collider and no RigidBody: scenery, landed on and never moved.
        // A box rather than the plane it draws as, buried so its upper face
        // lies exactly in the rendered surface - a body with no thickness is
        // a body fast things tunnel through.
        floor.attach<BoxCollider>(BoxCollider{{.5f, .1f, .5f}, {0.f, -.1f, 0.f}});

        Entity redBox = addMesh(
            "RedBox",
            box,
            makeMaterial("RedBox", glm::vec3{0.9f, 0.25f, 0.2f}, 0.f, 0.4f),
            {-.9f, .25f, 0.f},
            glm::vec3{.5f});
        {
            // The behaviour, not a component with the same job: this is what
            // `Spin` was standing in for.
            Script script{};
            Script::Slot slot{};
            slot.behavior = "ege::Spinner";
            auto spinner = std::make_shared<Spinner>();
            spinner->anglesPerSecond = {0.f, 1.2f, 0.f};
            slot.instance = std::move(spinner);
            script.behaviors.push_back(std::move(slot));
            redBox.attach<Script>(std::move(script));
        }

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
        {
            Script script{};
            Script::Slot slot{};
            slot.behavior = "ege::Spinner";
            auto spinner = std::make_shared<Spinner>();
            spinner->anglesPerSecond = {0.f, 0.5f, 0.f};
            slot.instance = std::move(spinner);
            script.behaviors.push_back(std::move(slot));
            sphereRow.attach<Script>(std::move(script));
        }

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

        // A surface a script owns: the mesh has no asset behind it, the
        // vertices are rewritten every tick by the Ripple behaviour, and the
        // upload happens once per frame rather than once per write.
        {
            Entity surface = world.spawn("RippleSurface");
            Transform surfaceTransform{};
            // Stood up like a banner rather than laid flat: a travelling wave
            // in a horizontal sheet is nearly invisible from a low camera,
            // and the point of this entity is to be seen moving. Set back
            // behind the sphere row and off to one side, because the first
            // placement put it between the camera and the roughness sweep -
            // an exhibit that hides the other exhibits.
            surfaceTransform.translation = {2.9f, -0.5f, 3.f};
            surfaceTransform.rotation.x = -glm::half_pi<float>();
            surfaceTransform.scale = glm::vec3{1.6f};
            surface.attach<Transform>(surfaceTransform);
            surface.attach<DynamicMesh>(DynamicMesh::grid(48));

            MeshRenderer surfaceRenderer{};
            surfaceRenderer.material =
                makeMaterial("Ripple", glm::vec3{0.25f, 0.7f, 0.6f}, 0.25f, 0.25f);
            surface.attach<MeshRenderer>(std::move(surfaceRenderer));

            Script script{};
            Script::Slot slot{};
            slot.behavior = "ege::Ripple";
            auto ripple = std::make_shared<Ripple>();
            // Shorter waves than the default, so several crests are in the
            // sheet at once: one long swell reads as the surface being bent
            // rather than as waves travelling through it. Shallow with it -
            // a deep wave at this wavelength stops looking like a surface and
            // starts looking like a blob.
            ripple->amplitude = 0.07f;
            ripple->wavelength = 0.5f;
            slot.instance = std::move(ripple);
            script.behaviors.push_back(std::move(slot));
            surface.attach<Script>(std::move(script));
        }

        addMesh(
            "BlueSphere",
            sphere,
            makeMaterial("BlueSphere", glm::vec3{0.2f, 0.5f, 0.95f}, 0.f, 0.15f),
            {.9f, .25f, 0.f},
            glm::vec3{.5f});

        // The physics exhibit: a tower of crates with a steel boulder hanging
        // over it. Nothing is scripted - the components are the whole
        // description, and Play (which the demo presses) is what drops the
        // boulder. Stop restores the tower, which is play mode's contract
        // doing its job on simulation state. Remember -Y is up: the boulder's
        // negative y is a height, and it falls towards +Y.
        {
            const MaterialRef crateMaterial =
                makeMaterial("Crate", glm::vec3{0.62f, 0.40f, 0.22f}, 0.f, 0.75f);
            auto addCrate = [&](std::string name, glm::vec3 position, float yaw) {
                Entity crate = addMesh(
                    std::move(name),
                    box,
                    crateMaterial,
                    position,
                    glm::vec3{.35f},
                    {0.f, yaw, 0.f});
                // The unit box's own half extents; the entity's scale is
                // applied when the body is built.
                crate.attach<BoxCollider>();
                RigidBody body{};
                body.mass = 2.f;
                body.friction = 0.35f;
                body.restitution = 0.05f;
                crate.attach<RigidBody>(body);
            };
            // Stacked on the floor surface at y = .5, each crate .35 tall,
            // slightly misaligned so the tower reads as stacked objects
            // rather than one drawn tower. Four high: tall enough to fall
            // over rather than merely be dented.
            addCrate("CrateBottom", {-2.3f, .325f, .6f}, 0.f);
            addCrate("CrateLower", {-2.3f, -.025f, .6f}, 0.22f);
            addCrate("CrateUpper", {-2.3f, -.375f, .6f}, -0.14f);
            addCrate("CrateTop", {-2.3f, -.725f, .6f}, 0.09f);
            addCrate("CrateSpare", {-1.9f, .325f, .78f}, 0.45f);

            // A plank ramp behind the tower, aimed at its base. Dropping the
            // boulder straight onto the tower was tried first and taught the
            // obvious-in-hindsight lesson: a stack is strong straight down,
            // which is exactly the direction a dropped ball pushes. Rolling
            // the ball down a ramp turns the same fall into a horizontal
            // blow at the base, which no tower survives. The plank is a
            // collider with no RigidBody - scenery, like the floor, only
            // tilted.
            // Aimed a little past the tower's centre line: a dead-centre hit
            // at the base lets the crates above drop back into a pile where
            // they stood - the tablecloth trick, uninvited. Off-axis, the
            // strike turns the stack as it breaks it, and the crates go
            // sideways instead of straight down.
            Entity plank = addMesh(
                "Plank",
                box,
                makeMaterial("Plank", glm::vec3{0.42f, 0.28f, 0.16f}, 0.f, 0.8f),
                {-2.44f, .21f, 1.55f},
                {.5f, .06f, 1.3f},
                {0.42f, 0.f, 0.f});
            plank.attach<BoxCollider>();

            Entity boulder = addMesh(
                "Boulder",
                sphere,
                makeMaterial("Boulder", glm::vec3{0.55f, 0.55f, 0.6f}, 1.f, 0.3f),
                {-2.44f, -.75f, 2.05f},
                glm::vec3{.7f});
            boulder.attach<SphereCollider>();
            RigidBody heavy{};
            heavy.mass = 12.f;
            heavy.friction = 0.5f;
            heavy.restitution = 0.1f;
            boulder.attach<RigidBody>(heavy);
        }

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

    void Application::reloadChangedAssets(FileWatcher& watcher) {
        const std::vector<FileWatcher::Event> changes = watcher.poll();
        if (changes.empty()) {
            return;
        }

        AssetDatabase& assets = AssetDatabase::instance();

        // Rescan first: a file that has just appeared has no id yet, and a
        // sidecar that has just appeared beside one is how an id arrives.
        bool structural = false;
        for (const FileWatcher::Event& change : changes) {
            if (change.change != FileWatcher::Change::modified) {
                structural = true;
            }
        }
        if (structural) {
            assets.scan(assetRoot());
        }

        // Reloading rewrites descriptor sets and frees images that frames
        // still in flight are reading. Waiting is a stall, and a stall on the
        // frame after someone saves a file is invisible - the alternative is
        // versioning every asset for the sake of an event that happens when a
        // human presses Ctrl+S.
        vkDeviceWaitIdle(device.device());

        std::error_code errorCode;
        for (const FileWatcher::Event& change : changes) {
            const std::filesystem::path relative =
                std::filesystem::relative(change.path, assetRoot(), errorCode);
            if (errorCode) {
                errorCode.clear();
                continue;
            }
            if (const AssetRecord* record = assets.findByPath(relative)) {
                assets.reload(record->id);
            }
        }

        // A mesh cannot be reloaded in place, so anything holding one has to
        // be pointed at the rebuilt version.
        systems::refreshAssetReferences(world);
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

            // Only asset-backed renderers are counted. A dynamic mesh has no
            // asset behind it by design - a script rebuilds its vertices - so
            // its renderer comes back empty and correctly so.
            std::size_t assetDraws = 0;
            std::size_t restoredDraws = 0;
            world.each<MeshRenderer>([&](Entity, MeshRenderer& original) {
                if (!original.mesh.id.isNull()) {
                    assetDraws++;
                }
            });
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
            if (restoredDraws != assetDraws) {
                EGE_WARN(
                    "scene round-trip lost geometry: {} of {} asset-backed renderers resolved",
                    restoredDraws,
                    assetDraws);
            }

            SceneSerializer::save(world, "demo_scene.egescene");
        } catch (const std::exception& e) {
            EGE_ERROR("scene round-trip failed: {}", e.what());
        }
    }

}  // namespace ege