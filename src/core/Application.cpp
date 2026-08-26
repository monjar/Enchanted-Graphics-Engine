#include "core/Application.hpp"

#include "anim/AnimationSystem.hpp"
#include "anim/SkeletalAnimator.hpp"
#include "assets/AssetDatabase.hpp"
#include "assets/GltfLoader.hpp"
#include "core/DemoTour.hpp"
#include "core/FileWatcher.hpp"
#include "core/Log.hpp"
#include "core/Time.hpp"
#include "editor/EditorOverlay.hpp"
#include "editor/PlayMode.hpp"
#include "physics/CharacterMotion.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "platform/CameraController.hpp"
#include "platform/FollowCamera.hpp"
#include "platform/Input.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "render/BloomSystem.hpp"
#include "render/Camera.hpp"
#include "render/ClusterGrid.hpp"
#include "render/ClusterLightSystem.hpp"
#include "render/DynamicMesh.hpp"
#include "render/EnvironmentLighting.hpp"
#include "render/GpuCullSystem.hpp"
#include "render/OcclusionSystem.hpp"
#include "render/PbrRenderSystem.hpp"
#include "render/PointShadows.hpp"
#include "render/PostProcessSystem.hpp"
#include "render/ShadowCascades.hpp"
#include "render/ShadowMapSystem.hpp"
#include "render/SkyboxSystem.hpp"
#include "render/SpotShadows.hpp"
#include "render/SsaoSystem.hpp"
#include "rhi/Buffer.hpp"
#include "rhi/FrameGraph.hpp"
#include "rhi/FrameRecorder.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/Systems.hpp"
#include "scene/TransformInterpolation.hpp"
#include "script/BehaviorRegistry.hpp"
#include "script/Behaviors.hpp"
#include "script/Script.hpp"
#include "script/ScriptModule.hpp"
#include "script/ScriptSystem.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <future>
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
            // Point-light shadow cubes. x: the near plane every cube is
            // rendered with; the far plane is each light's own range, so it
            // travels with the light rather than here.
            glm::vec4 pointShadowParams{0.f};
            // One per shadow-casting spot: the matrix its map was rendered
            // through, which the lighting pass projects fragments back into.
            // A spot needs only one - it already has a single direction and a
            // bounded angle, which is why it is the cheapest light to shadow.
            glm::mat4 spotShadowMatrices[maxShadowedSpotLights]{};
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
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SwapChain::MAX_FRAMES_IN_FLIGHT * 8)
                // Three storage buffers per frame: the scene's lights, the
                // per-cluster lists culled from them, and the transforms of
                // everything being drawn.
                .addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SwapChain::MAX_FRAMES_IN_FLIGHT * 3)
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
        // And somewhere to load: from here, an asset asked for asynchronously
        // is read, decoded and uploaded on a worker rather than on whichever
        // frame happened to reference it first.
        assets.attachJobSystem(jobs);
        assets.scan(assetRoot());

        // Before the scene, because the scene names behaviours the module
        // registers. Without it those slots simply find nothing in the
        // registry and do nothing, which is the same thing a scene naming a
        // behaviour this build does not have has always done.
        loadScriptModule();

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
                sizeof(GpuLight),
                maxSceneLights,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            lightBuffers[i]->map();
        }

        // Where every drawn object's transform goes, in submission order, for
        // both the depth pre-pass and the shading pass to index by instance.
        // Host visible and one per frame in flight, on the same terms as the
        // light buffer beside it: the CPU rewrites the whole thing each frame.
        std::vector<std::unique_ptr<Buffer>> instanceBuffers(SwapChain::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < instanceBuffers.size(); i++) {
            instanceBuffers[i] = std::make_unique<Buffer>(
                device,
                sizeof(GpuInstance),
                maxDrawInstances,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            instanceBuffers[i]->map();
        }

        // The frame's skinning matrices, every animated entity's end to end,
        // rewritten by the animation system each frame. Per frame in flight
        // for the same reason the instance buffers are.
        std::vector<std::unique_ptr<Buffer>> paletteBuffers(SwapChain::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < paletteBuffers.size(); i++) {
            paletteBuffers[i] = std::make_unique<Buffer>(
                device,
                sizeof(glm::mat4),
                AnimationSystem::paletteCapacity,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            paletteBuffers[i]->map();
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
                // Every shadow-casting point light's cube, in one array.
                .addBinding(
                    8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                // Every shadow-casting spot's map, in one array.
                .addBinding(
                    9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                // How much of its surroundings each pixel can see, estimated
                // from the depth buffer before anything shaded.
                .addBinding(
                    10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                // Every drawn object's transform, indexed by the instance.
                .addBinding(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
                // The skinning palette the skinned vertex shaders blend.
                .addBinding(12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
                .build();

        // Whether the occlusion verdict runs on the GPU, which is a question
        // of one feature: indirect commands carrying their own firstInstance,
        // without which no batch could point at its own window of the
        // compacted buffer. Nearly universal - but a device without it keeps
        // an honest renderer: frustum culling on the CPU and direct draws,
        // which is the whole engine as it stood before any of this.
        const bool gpuCulling = device.supportsIndirectFirstInstance();
        std::unique_ptr<GpuCullSystem> gpuCull;
        if (gpuCulling) {
            gpuCull = std::make_unique<GpuCullSystem>(device, SwapChain::MAX_FRAMES_IN_FLIGHT);
        } else {
            EGE_WARN(
                "drawIndirectFirstInstance is not supported; occlusion culling is off "
                "and draws go out direct");
        }

        std::vector<VkDescriptorSet> globalDescriptorSets(SwapChain::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < globalDescriptorSets.size(); i++) {
            auto bufferInfo = uboBuffers[i]->descriptorInfo();
            auto lightInfo = lightBuffers[i]->descriptorInfo();
            // What the vertex shaders index: the culling dispatch's compacted
            // output when the verdict is on the GPU, the CPU-written list
            // otherwise. Decided once, because the set is written once.
            auto instanceInfo = gpuCull
                                    ? gpuCull->instances(static_cast<uint32_t>(i)).descriptorInfo()
                                    : instanceBuffers[i]->descriptorInfo();
            auto paletteInfo = paletteBuffers[i]->descriptorInfo();
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
                .writeBuffer(11, &instanceInfo)
                .writeBuffer(12, &paletteInfo)
                .build(globalDescriptorSets[i]);
        }

        // The scene renders into a linear HDR target, not the backbuffer:
        // lighting sums exceed 1 constantly, and clamping them at the
        // swapchain is what made every bright highlight flat white. Sixteen
        // bits per channel is the float format with guaranteed
        // color-attachment support.
        constexpr VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

        // Anti-aliasing, if the device offers it. The scene is rasterised with
        // several coverage samples per pixel and averaged back down before
        // anything reads it; a device that cannot do that reports one sample
        // and every path below degrades to exactly what it was.
        //
        // Multisampling and clustered forward shading get along, which is not
        // true of every renderer: a deferred one would have to shade every
        // sample or resolve a G-buffer, and neither is cheap. Forward shading
        // runs the fragment shader once per covered pixel and lets the
        // hardware average coverage, which is the whole saving.
        const VkSampleCountFlagBits sceneSamples = device.maxUsableSampleCount();
        EGE_INFO("Multisampling: {}x", static_cast<uint32_t>(sceneSamples));

        PbrRenderSystem pbrRenderSystem{
            device,
            hdrFormat,
            renderer.getSwapChainDepthFormat(),
            globalSetLayout->getDescriptorSetLayout(),
            materialSetLayout->getDescriptorSetLayout(),
            sceneSamples,
            SwapChain::MAX_FRAMES_IN_FLIGHT};

        SkyboxSystem skybox{
            device,
            hdrFormat,
            renderer.getSwapChainDepthFormat(),
            globalSetLayout->getDescriptorSetLayout(),
            sceneSamples};

        ShadowMapSystem shadowSystem{device, renderer.getSwapChainDepthFormat()};

        // A single channel, eight bits: the occlusion estimate is one number
        // per pixel between nothing and everything, and it is blurred before
        // anything reads it. R8_UNORM is also a format every Vulkan device has
        // to support both rendering to and sampling.
        constexpr VkFormat occlusionFormat = VK_FORMAT_R8_UNORM;
        SsaoSystem ssao{device, occlusionFormat, SwapChain::MAX_FRAMES_IN_FLIGHT};

        // Builds the depth pyramid the late culling dispatch rules against.
        OcclusionSystem occlusionCulling{device, SwapChain::MAX_FRAMES_IN_FLIGHT};

        // Advances every animator and fills the frame's skinning palette.
        AnimationSystem animation;
        std::vector<glm::mat4> palette;

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
        FollowCamera followCamera{};
        // Framed for this scene rather than for a person: the character is
        // 0.6 units tall in a diorama whose crates are a third of a unit, and
        // a camera two metres back from it would be watching from across the
        // room.
        followCamera.settings.distance = 1.35f;
        followCamera.settings.height = 0.58f;
        followCamera.settings.aimHeight = 0.2f;
        followCamera.settings.lag = 8.f;
        followCamera.settings.minDistance = 0.3f;
        followCamera.settings.wallMargin = 0.05f;
        CameraController::registerDefaultActions(window.input());
        // What a character is driven by. Separate from the camera's bindings
        // because they answer to different things - the camera's move the
        // viewer, these move whoever is being played - even though the demo
        // scene, having one keyboard, shares the four movement actions.
        window.input().bindAction("Jump", Key::Space);
        window.input().bindAction("Run", Key::LeftShift);
        // Where a thumb expects them on a pad, and bound by the same names,
        // so nothing downstream is told which one is in use.
        window.input().bindAction("Jump", GamepadButton::A);
        window.input().bindAction("Run", GamepadButton::LeftThumb);
        window.input().bindAction("Run", GamepadAxis::RightTrigger, 1.f, 0.5f);

        // Behaviours read input through the world, the same way they reach
        // physics. Set once and left: the window outlives every scene, and a
        // behaviour that checks for null works in the editor, in a test and
        // in a cooked player alike.
        world.setInput(&window.input());

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
            if (!options.showEditor) {
                overlay.toggle();
            } else {
                // The panels are the subject, so put something in them - and
                // preferably the entity whose behaviour came out of a module
                // loaded at runtime, which is the one thing in the inspector
                // the engine was not built knowing about. Anything scripted
                // will do if the module is not loaded.
                EntityId scripted{};
                EntityId fromModule{};
                for (Entity entity : world.all()) {
                    const Script* script = world.find<Script>(entity.id());
                    if (script == nullptr || script->behaviors.empty()) {
                        continue;
                    }
                    if (scripted.isNull()) {
                        scripted = entity.id();
                    }
                    for (const Script::Slot& slot : script->behaviors) {
                        if (slot.behavior.rfind("sandbox::", 0) == 0) {
                            fromModule = entity.id();
                        }
                    }
                }
                overlay.select(fromModule.isNull() ? scripted : fromModule);
            }
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

        // And the same for code. The module is a build artifact rather than a
        // project file, so it is a second watcher on a second directory - the
        // one the build writes binaries into.
        FileWatcher scriptWatcher{moduleRoot(), std::chrono::milliseconds{500}};

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
                // The scene's collision matrix, declared here because a
                // world's layers are fixed once its bodies are in the broad
                // phase. Three layers and one rule between them: the gravel
                // ring is scenery the character walks over rather than
                // through, and it must not be what a pressure plate notices.
                physicsSettings.layers.add("Character");
                physicsSettings.layers.add("Props");
                physicsSettings.layers.add("Exhibit");
                // One rule, and it earns itself: the crate tower is a
                // demonstration of the simulation, and a character wandering
                // into it mid-fall would be a character rewriting the thing
                // being demonstrated. So the two share a floor and never
                // touch. Everything else collides with everything, which is
                // what an empty matrix means.
                physicsSettings.layers.setCollides("Character", "Exhibit", false);
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
                // Where everything the fixed step moves is now, before it
                // moves. The renderer draws between this and where the step
                // leaves it, so a sixty hertz simulation does not look like
                // sixty hertz on a faster display. Recorded inside the loop
                // rather than once a frame, so a frame that runs two steps
                // interpolates from the second rather than the first.
                recordPreviousTransforms(world);

                if (playMode.consumeTick()) {
                    scripts.fixedTick(world, time.fixedStep());
                    const PhysicsEvents events = physics.fixedTick(world, time.fixedStep());
                    scripts.deliverContacts(world, events.contacts);
                    // Arrivals before departures, so that a thing which
                    // entered and left inside one tick is heard about in the
                    // order it happened.
                    scripts.deliverTriggers(world, events.entered, true);
                    scripts.deliverTriggers(world, events.left, false);
                }
            }
            if (playMode.isPlaying()) {
                scripts.tick(world, frameTime);
            } else {
                // Nothing is stepping, so there is nothing to interpolate
                // towards. Keeping the recorded pose level with the real one
                // means a gizmo drag in edit mode draws where it is dragged
                // rather than a fraction behind.
                recordPreviousTransforms(world);
            }

            if (window.input().wasPressed(Key::F1)) {
                overlay.toggle();
            }

            // Rendering and camera control stay on the variable delta: they
            // should run as often as the display allows - unless a panel owns
            // the input, in which case dragging a slider must not also fly
            // the camera. A captured cursor is not over anything, so a look
            // drag that began in the scene view keeps the camera regardless.
            if (options.followCharacter) {
                // Behind the character rather than on rails or on the
                // free-fly controls.
                const Entity subject = world.findByName("Walker");
                const CharacterController* controller =
                    subject.alive() ? world.find<CharacterController>(subject.id()) : nullptr;
                if (controller != nullptr) {
                    // Where the body is pointing, unless somebody is looking
                    // somewhere else: the player's own yaw wins, because a
                    // camera that follows the body rather than the eyes turns
                    // after the player rather than with them.
                    float yaw = controller->facing;
                    if (const Script* script = world.find<Script>(subject.id())) {
                        for (const Script::Slot& slot : script->behaviors) {
                            if (const auto* player =
                                    dynamic_cast<const PlayerCharacter*>(slot.instance.get())) {
                                yaw = player->lookYaw;
                            }
                        }
                    }
                    followCamera.update(
                        glm::vec3{hierarchy::worldMatrix(world, subject.id())[3]},
                        yaw,
                        upFromGravity(
                            physics.running() ? physics.physicsWorld()->gravity()
                                              : glm::vec3{0.f, 9.81f, 0.f}),
                        frameTime,
                        physics.physicsWorld(),
                        viewerTransform);
                }
                if (options.demo) {
                    // The tour is not driving the camera here, but it still
                    // says how long the demo runs for - so it advances into a
                    // pose nobody looks at, purely to say when it is over.
                    Transform unused{};
                    if (!tour.advance(frameTime, unused)) {
                        running = false;
                    }
                }
            } else if (options.demo) {
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
            reloadChangedScripts(scriptWatcher, scripts);

            // Assets that finished loading on a worker since the last frame.
            // Anything holding a reference to one is still drawing nothing,
            // so this is what makes it appear.
            if (!AssetDatabase::instance().takeLoaded().empty()) {
                systems::refreshAssetReferences(world);
            }

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
                // The panel's numbers, merged from where each is actually
                // counted: candidates and the frustum's rejections on the
                // CPU, the occlusion verdict on the GPU - a couple of frames
                // after the fact, which the panel already says.
                PbrRenderSystem::Stats panelStats = pbrRenderSystem.stats();
                if (gpuCull) {
                    const GpuCullStatsData& counted = gpuCull->lastStats();
                    panelStats.occluded = counted.occluded;
                    panelStats.drawn = counted.drawnEarly + counted.drawnLate;
                }
                EditorOverlay::Context editorContext{
                    world, camera, panelStats, playMode, time.rawDelta(), time.framesPerSecond()};
                overlay.buildUi(editorContext);

                const uint32_t frameIndex = renderer.getFrameIndex();
                FrameInfo frameInfo{
                    frameIndex,
                    frameTime,
                    time.fixedAlpha(),
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
                std::vector<GpuLight> sceneLights;
                sceneLights.reserve(maxSceneLights);

                // The lights whose shadow maps get rendered this frame, in the
                // order their slots sit in the arrays. Casters are taken in
                // the order the scene yields them and stop at the cap, so
                // which lights cast is stable frame to frame as long as the
                // scene is. Points and spots have separate slots because they
                // cast into differently shaped maps - a cube each against a
                // single map each.
                struct PointShadowCaster {
                    glm::vec3 position{0.f};
                    float range = 0.f;
                };

                struct SpotShadowCaster {
                    glm::mat4 viewProjection{1.f};
                };

                std::vector<PointShadowCaster> shadowCasters;
                std::vector<SpotShadowCaster> spotCasters;

                world.each<Transform, PointLight>(
                    [&](Entity, Transform& transform, PointLight& light) {
                        if (sceneLights.size() >= maxSceneLights) {
                            return;
                        }
                        GpuLight gpuLight{};
                        // The cull radius rides in position.w, which is where
                        // the culling shader reads it: the light's own range,
                        // the distance past which it is treated as
                        // contributing nothing - and the far plane of its
                        // shadow cube for exactly the same reason.
                        gpuLight.position = glm::vec4{transform.translation, light.range};
                        gpuLight.color = glm::vec4{light.color, light.intensity};
                        gpuLight.params.z = static_cast<float>(GpuLightType::point);

                        if (light.castsShadows && shadowCasters.size() < maxShadowedPointLights) {
                            gpuLight.params.x = static_cast<float>(shadowCasters.size());
                            shadowCasters.push_back(
                                PointShadowCaster{transform.translation, light.range});
                        }

                        sceneLights.push_back(gpuLight);
                    });

                // Spots go into the same buffer and through the same culling.
                // A cone is bounded by the sphere of its range, so the cluster
                // test needs nothing new: it may hand a fragment a spot whose
                // cone misses it, which costs one iteration that shades to
                // zero and never a light that should have been there.
                world.each<Transform, SpotLight>(
                    [&](Entity, Transform& transform, SpotLight& light) {
                        if (sceneLights.size() >= maxSceneLights) {
                            return;
                        }
                        // Which way the entity faces, so a spot is aimed by
                        // rotating it like anything else in the scene rather than
                        // by carrying a direction the transform disagrees with.
                        const glm::vec3 forward = glm::normalize(
                            glm::vec3{transform.mat4() * glm::vec4{0.f, 0.f, 1.f, 0.f}});
                        // Cosines, because that is what a dot product gives the
                        // shader and converting back would cost an inverse cosine
                        // per fragment. The inner is held inside the outer so an
                        // authored pair that crosses over dims rather than
                        // lighting the rim and darkening the core.
                        const float outerAngle = std::max(light.outerAngle, 0.01f);
                        const float innerAngle = std::min(light.innerAngle, outerAngle);

                        GpuLight gpuLight{};
                        gpuLight.position = glm::vec4{transform.translation, light.range};
                        gpuLight.color = glm::vec4{light.color, light.intensity};
                        gpuLight.direction = glm::vec4{forward, std::cos(outerAngle)};
                        gpuLight.params.y = std::cos(innerAngle);
                        gpuLight.params.z = static_cast<float>(GpuLightType::spot);

                        if (light.castsShadows && spotCasters.size() < maxShadowedSpotLights) {
                            const auto slot = static_cast<uint32_t>(spotCasters.size());
                            gpuLight.params.x = static_cast<float>(slot);
                            const glm::mat4 matrix = spotShadowMatrix(
                                transform.translation, forward, outerAngle, light.range);
                            ubo.spotShadowMatrices[slot] = matrix;
                            spotCasters.push_back(SpotShadowCaster{matrix});
                        }

                        sceneLights.push_back(gpuLight);
                    });

                ubo.numLights = static_cast<int>(sceneLights.size());
                ubo.pointShadowParams = glm::vec4{pointShadowNearPlane, 0.f, 0.f, 0.f};

                if (!sceneLights.empty()) {
                    lightBuffers[frameIndex]->writeToBuffer(
                        sceneLights.data(), sceneLights.size() * sizeof(GpuLight));
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

                // Animation first: prepare() reads each animator's palette
                // base, so the poses must land before the list is gathered.
                // Advancing by frameTime rather than the raw clock keeps a
                // recording's animation as deterministic as its camera.
                animation.update(world, frameTime, palette);
                if (!palette.empty()) {
                    paletteBuffers[frameIndex]->writeToBuffer(
                        palette.data(), palette.size() * sizeof(glm::mat4));
                    paletteBuffers[frameIndex]->flush();
                }

                // Which objects are candidates, gathered once for the frame:
                // every pass that draws draws this list, and they have to
                // draw the same one.
                pbrRenderSystem.prepare(frameInfo, *instanceBuffers[frameIndex]);

                // Which of the pyramid's levels the late dispatch will rule
                // against: the chain's coarsest four, finest of them first.
                // A window small enough to have fewer levels repeats its
                // coarsest, which only ever means two names for one answer.
                const uint32_t reductions = OcclusionSystem::reductionSteps(renderExtent);
                std::array<uint32_t, gpuCullBoundLevels> boundSteps{};
                std::array<glm::uvec2, gpuCullBoundLevels> boundExtents{};
                for (uint32_t j = 0; j < gpuCullBoundLevels; j++) {
                    const uint32_t firstBound =
                        reductions > gpuCullBoundLevels ? reductions - gpuCullBoundLevels : 0u;
                    boundSteps[j] = std::min(firstBound + j, reductions - 1u);
                    const VkExtent2D extent =
                        OcclusionSystem::stepExtent(renderExtent, boundSteps[j]);
                    boundExtents[j] = {extent.width, extent.height};
                }

                if (gpuCull) {
                    gpuCull->setLevelExtents(boundExtents);
                    gpuCull->feed(
                        frameIndex,
                        pbrRenderSystem.cullFeed().candidates,
                        pbrRenderSystem.cullFeed().batches,
                        camera.getView(),
                        camera.getProjection(),
                        pbrRenderSystem.occlusionCullingEnabled);
                }

                // render: declare the frame, then let the graph run it. The
                // declarations are cheap enough to restate every frame, and
                // doing so is what lets passes appear and disappear freely.
                graph.beginFrame(swapExtent);

                const bool multisampled = sceneSamples != VK_SAMPLE_COUNT_1_BIT;

                TransientImageDesc sceneColorDesc{};
                sceneColorDesc.format = hdrFormat;
                sceneColorDesc.extent = renderExtent;
                sceneColorDesc.clearValue.color = {{0.01f, 0.01f, 0.01f, 1.0f}};
                // What bloom and the tonemap read: single-sampled either way.
                // With multisampling on, the scene renders into a separate
                // multisampled image and this is what it resolves into.
                FrameGraphResource sceneColor = graph.createTransient("sceneColor", sceneColorDesc);

                TransientImageDesc sceneColorMsDesc = sceneColorDesc;
                sceneColorMsDesc.samples = sceneSamples;
                FrameGraphResource sceneColorMs =
                    multisampled ? graph.createTransient("sceneColorMs", sceneColorMsDesc)
                                 : sceneColor;

                TransientImageDesc sceneDepthDesc{};
                sceneDepthDesc.format = renderer.getSwapChainDepthFormat();
                sceneDepthDesc.extent = renderExtent;
                sceneDepthDesc.clearValue.depthStencil = {1.0f, 0};
                // Depth matches the colour it is tested against, and is never
                // resolved: nothing samples it, so averaging it would produce
                // a value no sample actually had.
                sceneDepthDesc.samples = sceneSamples;
                FrameGraphResource sceneDepth = graph.createTransient("sceneDepth", sceneDepthDesc);

                // What screen-space occlusion reads. Nothing can sample a
                // multisampled image, so with multisampling on the depth
                // pre-pass resolves its depth into a single-sample copy - by
                // taking one sample rather than averaging, because an averaged
                // depth is a surface neither sample saw. With multisampling
                // off there is nothing to resolve and this is the depth
                // buffer itself.
                TransientImageDesc sceneDepthResolvedDesc = sceneDepthDesc;
                sceneDepthResolvedDesc.samples = VK_SAMPLE_COUNT_1_BIT;
                FrameGraphResource sceneDepthResolved =
                    multisampled ? graph.createTransient("sceneDepth1x", sceneDepthResolvedDesc)
                                 : sceneDepth;

                // What the pyramid is built from: the early depth phase's
                // resolve when multisampling forces one, the depth buffer
                // itself otherwise - read mid-frame, between the two depth
                // phases, which the graph turns into ordinary transitions.
                FrameGraphResource hzbInput =
                    multisampled ? graph.createTransient("hzbInput", sceneDepthResolvedDesc)
                                 : sceneDepth;

                // The occlusion estimate and the blur that follows it. Full
                // resolution, unlike bloom: this is read per shaded fragment
                // to decide how dark a contact is, and halving it puts a
                // visible step along every edge.
                TransientImageDesc occlusionDesc{};
                occlusionDesc.format = occlusionFormat;
                occlusionDesc.extent = renderExtent;
                // Nothing occludes anything until the pass runs, and a pass
                // that is culled leaves this behind for the shading pass to
                // read: white is "sees everything", which is the frame the
                // engine drew before any of this existed.
                occlusionDesc.clearValue.color = {{1.0f, 1.0f, 1.0f, 1.0f}};
                FrameGraphResource occlusionRaw =
                    graph.createTransient("occlusionRaw", occlusionDesc);
                FrameGraphResource occlusion = graph.createTransient("occlusion", occlusionDesc);

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

                // Every shadow-casting point light's cube, as one cube array:
                // six faces per light, laid out so cube `i` occupies layers
                // 6i to 6i+5, which is exactly how a cube array view reads
                // them. Always the full size even when fewer lights cast, so
                // the physical image is one the graph can keep reusing rather
                // than reallocating whenever a light is switched on.
                TransientImageDesc pointShadowDesc{};
                pointShadowDesc.format = renderer.getSwapChainDepthFormat();
                pointShadowDesc.extent = {
                    ShadowMapSystem::pointResolution, ShadowMapSystem::pointResolution};
                pointShadowDesc.clearValue.depthStencil = {1.0f, 0};
                pointShadowDesc.layers = maxShadowedPointLights * cubeFaceCount;
                pointShadowDesc.cube = true;
                FrameGraphResource pointShadowMaps =
                    graph.createTransient("pointShadowMaps", pointShadowDesc);

                // One map per shadow-casting spot, as a plain 2D array - a
                // spot has one direction and a bounded angle, so unlike a
                // point light it needs no cube and unlike the sun it needs no
                // cascades. Sized for the cap either way, for the same reason
                // the cube array is: a physical image the graph can keep
                // reusing rather than reallocating as lights come and go.
                TransientImageDesc spotShadowDesc{};
                spotShadowDesc.format = renderer.getSwapChainDepthFormat();
                spotShadowDesc.extent = {
                    ShadowMapSystem::spotResolution, ShadowMapSystem::spotResolution};
                spotShadowDesc.clearValue.depthStencil = {1.0f, 0};
                spotShadowDesc.layers = maxShadowedSpotLights;
                FrameGraphResource spotShadowMaps =
                    graph.createTransient("spotShadowMaps", spotShadowDesc);

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

                ImportedImageDesc backbufferDesc{};
                backbufferDesc.image = renderer.currentSwapChainImage();
                backbufferDesc.view = renderer.currentSwapChainImageView();
                backbufferDesc.format = renderer.getSwapChainColorFormat();
                backbufferDesc.extent = renderer.getSwapChainExtent();
                // What the acquire semaphore is waited at, so the first
                // backbuffer barrier chains after the acquire.
                backbufferDesc.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                // While recording, the graph hands the image over ready to be
                // copied and the recorder returns it to PRESENT_SRC. Reading
                // an image that has already been presented is the kind of
                // thing that works on one driver and corrupts on another.
                backbufferDesc.finalLayout = recorder == nullptr
                                                 ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                 : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                FrameGraphResource backbuffer = graph.importImage("backbuffer", backbufferDesc);

                // Where the display transform lands. With the editor up that is
                // the viewport image the Scene panel samples, and the UI pass
                // becomes the only thing writing the window; with the editor
                // hidden the tonemap goes straight to the backbuffer and the
                // frame is exactly what it was before the editor existed.
                FrameGraphResource displayTarget = backbuffer;
                if (sceneTarget.offscreen) {
                    ImportedImageDesc viewportDesc{};
                    viewportDesc.image = sceneTarget.image;
                    viewportDesc.view = sceneTarget.view;
                    viewportDesc.format = renderer.getSwapChainColorFormat();
                    viewportDesc.extent = sceneTarget.extent;
                    // Last frame's UI sampled this image; writing over it has
                    // to wait for that read to have finished.
                    viewportDesc.srcStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    viewportDesc.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    displayTarget = graph.importImage("viewport", viewportDesc);
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

                // Every cube face, every frame - including the faces of slots
                // no light is using. A slot with no caster records no
                // geometry, so its pass costs one depth clear and nothing
                // else, and in exchange every layer of the array is defined
                // whatever the scene contains. The alternative is declaring
                // passes only for the lights that cast, which leaves the
                // image unwritten when nothing does and makes the scene pass
                // read a resource no pass produced.
                for (uint32_t slot = 0; slot < maxShadowedPointLights; slot++) {
                    const bool used = slot < shadowCasters.size();
                    const std::array<glm::mat4, cubeFaceCount> faces =
                        used ? pointShadowFaceMatrices(
                                   shadowCasters[slot].position, shadowCasters[slot].range)
                             : std::array<glm::mat4, cubeFaceCount>{};

                    for (uint32_t face = 0; face < cubeFaceCount; face++) {
                        const uint32_t layer = slot * cubeFaceCount + face;
                        const glm::mat4 faceMatrix = faces[face];
                        graph.addPass(
                            "pointShadow" + std::to_string(slot) + "_" + std::to_string(face),
                            [&, layer](FrameGraph::PassBuilder& pass) {
                                pass.write(pointShadowMaps, ResourceAccess::depthWrite, layer);
                            },
                            [&, used, faceMatrix](VkCommandBuffer cmd, const FrameGraphResources&) {
                                if (used) {
                                    shadowSystem.render(cmd, world, faceMatrix);
                                }
                            });
                    }
                }

                // One pass per spot slot, on the same terms as the cube faces
                // above: unused slots record nothing and cost a clear, so the
                // array is defined whatever the scene holds.
                for (uint32_t slot = 0; slot < maxShadowedSpotLights; slot++) {
                    const bool used = slot < spotCasters.size();
                    const glm::mat4 spotMatrix =
                        used ? spotCasters[slot].viewProjection : glm::mat4{1.f};
                    graph.addPass(
                        "spotShadow" + std::to_string(slot),
                        [&, slot](FrameGraph::PassBuilder& pass) {
                            pass.write(spotShadowMaps, ResourceAccess::depthWrite, slot);
                        },
                        [&, used, spotMatrix](VkCommandBuffer cmd, const FrameGraphResources&) {
                            if (used) {
                                shadowSystem.render(cmd, world, spotMatrix);
                            }
                        });
                }

                // Depth before colour - in two phases when the verdict runs
                // on the GPU, one when it does not. The early phase draws
                // what was visible last frame; the pyramid is built from that
                // partial depth; the late dispatch rules on every candidate
                // against it; and the late phase draws whatever it newly
                // admitted. Together the two phases write the union, which is
                // exactly what the scene pass tests EQUAL against - and
                // nothing anywhere waits for a verdict to travel.
                FrameGraphResource cullCommands{};
                FrameGraphResource cullInstances{};
                if (gpuCull) {
                    ImportedBufferDesc commandsDesc{};
                    commandsDesc.buffer = gpuCull->commands(frameIndex).getBuffer();
                    commandsDesc.size = gpuCull->commands(frameIndex).getBufferSize();
                    // Seeded by the host after this index's fence; the
                    // submission itself makes host writes visible, so there
                    // is nothing for the first barrier to chain after.
                    cullCommands = graph.importBuffer("cullCommands", commandsDesc);

                    ImportedBufferDesc instancesDesc{};
                    instancesDesc.buffer = gpuCull->instances(frameIndex).getBuffer();
                    instancesDesc.size = gpuCull->instances(frameIndex).getBufferSize();
                    cullInstances = graph.importBuffer("cullInstances", instancesDesc);

                    ImportedBufferDesc visibilityDesc{};
                    visibilityDesc.buffer = gpuCull->visibilityBuffer().getBuffer();
                    visibilityDesc.size = gpuCull->visibilityBuffer().getBufferSize();
                    // The one buffer with a past that is not fenced away:
                    // the previous frame's late dispatch wrote it, and that
                    // frame may still be in flight.
                    visibilityDesc.srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    visibilityDesc.srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                    FrameGraphResource cullVisibility =
                        graph.importBuffer("cullVisibility", visibilityDesc);

                    ImportedBufferDesc statsDesc{};
                    statsDesc.buffer = gpuCull->statsBuffer(frameIndex).getBuffer();
                    statsDesc.size = gpuCull->statsBuffer(frameIndex).getBufferSize();
                    FrameGraphResource cullStats = graph.importBuffer("cullStats", statsDesc);

                    graph.addPass(
                        "earlyCull",
                        [&](FrameGraph::PassBuilder& pass) {
                            pass.read(cullVisibility, ResourceAccess::computeStorageRead);
                            pass.write(cullCommands, ResourceAccess::storageWrite);
                            pass.write(cullInstances, ResourceAccess::storageWrite);
                        },
                        [&](VkCommandBuffer cmd, const FrameGraphResources&) {
                            gpuCull->recordEarly(
                                cmd, frameIndex, instanceBuffers[frameIndex]->descriptorInfo());
                        });

                    graph.addPass(
                        "depthEarly",
                        [&](FrameGraph::PassBuilder& pass) {
                            pass.read(cullCommands, ResourceAccess::indirectRead);
                            pass.read(cullInstances, ResourceAccess::vertexRead);
                            pass.write(sceneDepth, ResourceAccess::depthWrite);
                            if (multisampled) {
                                pass.resolve(sceneDepth, hzbInput);
                            }
                        },
                        [&](VkCommandBuffer cmd, const FrameGraphResources&) {
                            frameInfo.commandBuffer = cmd;
                            pbrRenderSystem.renderDepthIndirect(
                                frameInfo,
                                uboBuffers[frameIndex]->descriptorInfo(),
                                gpuCull->instances(frameIndex).descriptorInfo(),
                                paletteBuffers[frameIndex]->descriptorInfo(),
                                gpuCull->commands(frameIndex).getBuffer(),
                                0);
                        });

                    // The pyramid, halved from the early phase's depth until
                    // the whole screen is a handful of texels. Each level is
                    // its own image rather than a mip of one, which costs a
                    // few more passes and saves the graph a layout per mip.
                    std::vector<FrameGraphResource> pyramidLevels(reductions);
                    for (uint32_t step = 0; step < reductions; step++) {
                        TransientImageDesc levelDesc{};
                        levelDesc.format = OcclusionSystem::levelFormat;
                        levelDesc.extent = OcclusionSystem::stepExtent(renderExtent, step);
                        pyramidLevels[step] =
                            graph.createTransient("hzb" + std::to_string(step), levelDesc);
                    }

                    for (uint32_t step = 0; step < reductions; step++) {
                        const FrameGraphResource source =
                            step == 0 ? hzbInput : pyramidLevels[step - 1];
                        const FrameGraphResource target = pyramidLevels[step];
                        const VkExtent2D sourceExtent =
                            step == 0 ? renderExtent
                                      : OcclusionSystem::stepExtent(renderExtent, step - 1);

                        graph.addPass(
                            "hzbReduce" + std::to_string(step),
                            [source, target](FrameGraph::PassBuilder& pass) {
                                pass.read(source, ResourceAccess::sampled);
                                pass.write(target, ResourceAccess::colorWrite);
                            },
                            [&, source, step, sourceExtent](
                                VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                                occlusionCulling.reduce(
                                    cmd, frameIndex, step, resolved.view(source), sourceExtent);
                            });
                    }

                    graph.addPass(
                        "lateCull",
                        [&](FrameGraph::PassBuilder& pass) {
                            for (uint32_t j = 0; j < gpuCullBoundLevels; j++) {
                                pass.read(
                                    pyramidLevels[boundSteps[j]], ResourceAccess::computeSampled);
                            }
                            pass.write(cullCommands, ResourceAccess::storageWrite);
                            pass.write(cullInstances, ResourceAccess::storageWrite);
                            pass.write(cullVisibility, ResourceAccess::storageWrite);
                            pass.write(cullStats, ResourceAccess::storageWrite);
                        },
                        [&, pyramidLevels, boundSteps](
                            VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                            std::array<VkImageView, gpuCullBoundLevels> levelViews{};
                            for (uint32_t j = 0; j < gpuCullBoundLevels; j++) {
                                levelViews[j] = resolved.view(pyramidLevels[boundSteps[j]]);
                            }
                            gpuCull->recordLate(
                                cmd,
                                frameIndex,
                                instanceBuffers[frameIndex]->descriptorInfo(),
                                levelViews);
                        });

                    graph.addPass(
                        "depthLate",
                        [&](FrameGraph::PassBuilder& pass) {
                            pass.read(cullCommands, ResourceAccess::indirectRead);
                            pass.read(cullInstances, ResourceAccess::vertexRead);
                            pass.write(sceneDepth, ResourceAccess::depthWrite);
                            if (multisampled) {
                                pass.resolve(sceneDepth, sceneDepthResolved);
                            }
                        },
                        [&](VkCommandBuffer cmd, const FrameGraphResources&) {
                            frameInfo.commandBuffer = cmd;
                            pbrRenderSystem.renderDepthIndirect(
                                frameInfo,
                                uboBuffers[frameIndex]->descriptorInfo(),
                                gpuCull->instances(frameIndex).descriptorInfo(),
                                paletteBuffers[frameIndex]->descriptorInfo(),
                                gpuCull->commands(frameIndex).getBuffer(),
                                gpuCull->batchCount(frameIndex));
                        });
                } else {
                    // Depth in one direct pass. Nothing samples what this
                    // produces - the scene pass consumes it by testing EQUAL
                    // against it - so the graph keeps the pass alive on the
                    // strength of that later load rather than on any read.
                    graph.addPass(
                        "depthPrePass",
                        [&](FrameGraph::PassBuilder& pass) {
                            pass.write(sceneDepth, ResourceAccess::depthWrite);
                            if (multisampled) {
                                pass.resolve(sceneDepth, sceneDepthResolved);
                            }
                        },
                        [&](VkCommandBuffer cmd, const FrameGraphResources&) {
                            frameInfo.commandBuffer = cmd;
                            pbrRenderSystem.renderDepthPrePass(
                                frameInfo,
                                uboBuffers[frameIndex]->descriptorInfo(),
                                instanceBuffers[frameIndex]->descriptorInfo(),
                                paletteBuffers[frameIndex]->descriptorInfo());
                        });
                }

                // Screen-space occlusion, between the depth and the shading
                // that reads it. Two passes: the estimate, then a blur exactly
                // as wide as the rotation pattern the estimate used.
                graph.addPass(
                    "occlusion",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.read(sceneDepthResolved, ResourceAccess::sampled);
                        pass.write(occlusionRaw, ResourceAccess::colorWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                        ssao.renderOcclusion(
                            cmd,
                            frameIndex,
                            uboBuffers[frameIndex]->descriptorInfo(),
                            resolved.view(sceneDepthResolved));
                    });

                graph.addPass(
                    "occlusionBlur",
                    [&](FrameGraph::PassBuilder& pass) {
                        pass.read(occlusionRaw, ResourceAccess::sampled);
                        pass.write(occlusion, ResourceAccess::colorWrite);
                    },
                    [&](VkCommandBuffer cmd, const FrameGraphResources& resolved) {
                        ssao.renderBlur(cmd, frameIndex, resolved.view(occlusionRaw));
                    });

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
                        pass.read(pointShadowMaps, ResourceAccess::sampled);
                        pass.read(spotShadowMaps, ResourceAccess::sampled);
                        pass.read(clusterList, ResourceAccess::storageRead);
                        pass.read(occlusion, ResourceAccess::sampled);
                        if (gpuCull) {
                            pass.read(cullCommands, ResourceAccess::indirectRead);
                            pass.read(cullInstances, ResourceAccess::vertexRead);
                        }
                        pass.write(sceneColorMs, ResourceAccess::colorWrite);
                        pass.write(sceneDepth, ResourceAccess::depthWrite);
                        if (multisampled) {
                            pass.resolve(sceneColorMs, sceneColor);
                        }
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
                        VkDescriptorImageInfo pointShadowInfo{};
                        pointShadowInfo.sampler = shadowSystem.cubeComparisonSampler();
                        pointShadowInfo.imageView = resolved.view(pointShadowMaps);
                        pointShadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                        VkDescriptorImageInfo spotShadowInfo{};
                        spotShadowInfo.sampler = shadowSystem.comparisonSampler();
                        spotShadowInfo.imageView = resolved.view(spotShadowMaps);
                        spotShadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                        VkDescriptorImageInfo occlusionInfo{};
                        occlusionInfo.sampler = ssao.resultSampler();
                        occlusionInfo.imageView = resolved.view(occlusion);
                        occlusionInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                        DescriptorWriter(*globalSetLayout, *globalPool)
                            .writeImage(5, &shadowInfo)
                            .writeBuffer(7, &clusterInfo)
                            .writeImage(8, &pointShadowInfo)
                            .writeImage(9, &spotShadowInfo)
                            .writeImage(10, &occlusionInfo)
                            .overwrite(globalDescriptorSets[frameIndex]);

                        frameInfo.commandBuffer = cmd;
                        if (gpuCull) {
                            pbrRenderSystem.renderIndirect(
                                frameInfo, gpuCull->commands(frameIndex).getBuffer());
                        } else {
                            pbrRenderSystem.render(frameInfo);
                        }
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

        device.waitIdle();
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
                auto material = std::make_shared<Material>(*materialPool, *materialSetLayout);
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

        // A behaviour the engine does not have. `sandbox::Pulse` lives in the
        // sandbox module, which is loaded at runtime - so this slot resolves
        // to something only when that module is there, and the torus simply
        // sits still when it is not. That is the whole demonstration: the
        // scene names code the engine was not built with.
        {
            Entity breathing = addMesh(
                "PulsingSphere",
                sphere,
                makeMaterial("Pulsing", glm::vec3{0.85f, 0.35f, 0.55f}, 0.1f, 0.3f),
                {-0.1f, -1.15f, 1.1f},
                glm::vec3{.4f});

            Script script{};
            Script::Slot slot{};
            slot.behavior = "sandbox::Pulse";
            // From the registry rather than `make_shared`, because there is no
            // such type here to construct - that is the point of the module.
            // Without this the slot is a name and nothing else, and the
            // inspector has no fields to show until Play makes the instance.
            if (const BehaviorRegistry::Entry* entry =
                    BehaviorRegistry::instance().find(slot.behavior);
                entry != nullptr && entry->create) {
                slot.instance = entry->create();
            }
            script.behaviors.push_back(std::move(slot));
            breathing.attach<Script>(std::move(script));
        }

        addMesh(
            "BlueSphere",
            sphere,
            makeMaterial("BlueSphere", glm::vec3{0.2f, 0.5f, 0.95f}, 0.f, 0.15f),
            {.9f, .25f, 0.f},
            glm::vec3{.5f});

        // A band of gravel around the outside of the floor: one mesh, one
        // material, a hundred and twenty of them. It is here because a scene
        // of eighteen objects says nothing about instancing - every one of
        // them has a material of its own, so every one is its own draw
        // whatever the renderer does. These share both, so they are one draw
        // call between them, and the editor's `drawn` and `batches` counts
        // show the difference directly.
        //
        // Scattered from a fixed seed rather than at random. The demo tour is
        // recorded frame for frame in CI, and a scene that differs between
        // runs is a scene no recorded frame can be compared against.
        {
            const MaterialRef gravelMaterial =
                makeMaterial("Gravel", glm::vec3{0.38f, 0.35f, 0.32f}, 0.f, 0.9f);

            // A small linear congruential generator, written out rather than
            // pulled from <random>, because what matters here is that the
            // same numbers come out on every platform - and the standard
            // library's distributions are explicitly not required to.
            uint32_t seed = 20260819u;
            auto nextUnit = [&seed]() {
                seed = seed * 1664525u + 1013904223u;
                return static_cast<float>(seed >> 8u) / static_cast<float>(1u << 24u);
            };

            constexpr int gravelCount = 120;
            for (int i = 0; i < gravelCount; i++) {
                // An annulus around the exhibits: far enough out not to stand
                // in front of anything, inside the floor's own eight-unit
                // span so nothing floats over the edge.
                const float angle =
                    (static_cast<float>(i) + nextUnit()) / gravelCount * glm::two_pi<float>();
                const float radius = 2.9f + nextUnit() * 0.95f;
                const float size = 0.07f + nextUnit() * 0.09f;

                addMesh(
                    "Gravel" + std::to_string(i),
                    box,
                    gravelMaterial,
                    // Remember -Y is up: resting on the floor at y = .5 means
                    // sitting half a stone's height above it.
                    {radius * std::cos(angle), 0.5f - size * 0.5f, radius * std::sin(angle)},
                    glm::vec3{size},
                    {0.f, nextUnit() * glm::two_pi<float>(), 0.f});
            }
        }

        // Directly behind the rippling sheet, and there for that reason: this
        // is the one thing in the scene that spends most of the tour entirely
        // hidden behind something else, so it is the only thing occlusion
        // culling has to decide about. Everything else here stands in the open,
        // where the depth pyramid quite correctly says to draw it.
        //
        // Watch the editor's `occluded` count while the camera comes round
        // onto the sheet: this drops out of the draw list while the sheet
        // covers it and is back the moment the camera can see past the edge.
        addMesh(
            "HiddenSphere",
            sphere,
            makeMaterial("HiddenSphere", glm::vec3{0.85f, 0.7f, 0.2f}, 0.2f, 0.35f),
            {2.9f, -0.5f, 3.7f},
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
                crate.attach<PhysicsLayer>(PhysicsLayer{"Exhibit"});
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
            plank.attach<PhysicsLayer>(PhysicsLayer{"Exhibit"});

            Entity boulder = addMesh(
                "Boulder",
                sphere,
                makeMaterial("Boulder", glm::vec3{0.55f, 0.55f, 0.6f}, 1.f, 0.3f),
                {-2.44f, -.75f, 2.05f},
                glm::vec3{.7f});
            boulder.attach<SphereCollider>();
            boulder.attach<PhysicsLayer>(PhysicsLayer{"Exhibit"});
            RigidBody heavy{};
            heavy.mass = 12.f;
            heavy.friction = 0.5f;
            heavy.restitution = 0.1f;
            boulder.attach<RigidBody>(heavy);
        }

        // Somebody walking around in it.
        //
        // A capsule that walks rather than a body that rolls: it holds itself
        // upright, walks up a step it could never climb over, slides along a
        // wall instead of stopping dead at it, and shoves what is in its way.
        // Driven by a Patrol behaviour writing the same four intent fields a
        // player's hands would - the controller cannot tell the difference,
        // which is the whole reason the recorded tour is a recording of the
        // thing rather than a mime of it.
        //
        // Drawn as a box until there is a rigged body to put here, which is
        // the next thing this milestone owes.
        {
            Entity walker = world.spawn("Walker");
            Transform walkerTransform{};
            walkerTransform.translation = {1.4f, 0.1f, -1.5f};
            walker.attach<Transform>(walkerTransform);

            CharacterController controller{};
            // A capsule 0.24 across and 0.6 tall, which is the imported
            // figure's own size: it is drawn by a rigged model parented under
            // this entity rather than by a mesh on it, so there is no scale
            // here for the collider to be multiplied by.
            controller.radius = 0.12f;
            controller.halfHeight = 0.18f;
            // This scene is a diorama - a crate is a third of a unit - so a
            // person-sized character walks at a person's speed relative to
            // the things around it rather than at three metres a second.
            controller.walkSpeed = 0.9f;
            controller.runSpeed = 1.8f;
            controller.acceleration = 8.f;
            controller.braking = 12.f;
            controller.jumpHeight = 0.35f;
            controller.turnRate = 8.f;
            // Enough to walk up the plank's lip, not enough to step onto a
            // crate: what it cannot climb it has to push.
            controller.stepHeight = 0.12f;
            controller.stickToFloor = 0.24f;
            controller.mass = 6.f;
            controller.pushForce = 12.f;
            walker.attach<CharacterController>(controller);
            walker.attach<PhysicsLayer>(PhysicsLayer{"Character"});

            // Who drives. The two write the same four intent fields, so the
            // controller cannot tell them apart - which is exactly why the
            // recorded tour, where there are no hands on the keyboard, is a
            // recording of the thing a player gets rather than a mime of it.
            Script script{};
            Script::Slot slot{};
            if (options.playCharacter) {
                slot.behavior = "ege::PlayerCharacter";
                slot.instance = std::make_shared<PlayerCharacter>();
            } else {
                slot.behavior = "ege::Patrol";
                auto patrol = std::make_shared<Patrol>();
                patrol->extents = {0.9f, 0.f, 0.45f};
                patrol->arriveRadius = 0.25f;
                patrol->jumpAtCorners = true;
                slot.instance = std::move(patrol);
            }
            script.behaviors.push_back(std::move(slot));
            walker.attach<Script>(std::move(script));

            // Something in its way. Light enough to be shoved by a character
            // this size and heavy enough not to be flicked across the floor,
            // and standing on the circuit's near edge where the walk crosses
            // it - the difference between a character that collides and one
            // that only stops.
            Entity crate = addMesh(
                "WalkerCrate",
                box,
                makeMaterial("WalkerCrate", glm::vec3{0.55f, 0.5f, 0.28f}, 0.f, 0.7f),
                {1.4f, 0.35f, -1.05f},
                glm::vec3{.3f},
                {0.f, 0.3f, 0.f});
            crate.attach<BoxCollider>();
            crate.attach<PhysicsLayer>(PhysicsLayer{"Props"});
            RigidBody light{};
            light.mass = 1.5f;
            light.friction = 0.6f;
            crate.attach<RigidBody>(light);

            // A pressure plate on the circuit, and a door it opens.
            //
            // The plate is a collider and a Trigger and nothing else: no
            // rigid body, no script deciding what "on" means. It notices what
            // stands on it and says so, and the behaviour attached to it
            // counts arrivals and departures - because two things standing on
            // a plate is two arrivals, and a door that shut on the first
            // departure would shut on whoever was still standing there.
            //
            // `only` is the other half of the demonstration: the crate above
            // gets shoved across the plate every lap, and the door does not
            // open for it. A plate that any passing box could open is not a
            // door with a key.
            Entity door = addMesh(
                "Door",
                box,
                makeMaterial("Door", glm::vec3{0.30f, 0.45f, 0.52f}, 0.3f, 0.35f),
                {2.9f, 0.15f, -1.5f},
                {0.12f, 0.7f, 0.62f});
            door.attach<BoxCollider>();
            RigidBody doorBody{};
            // Kinematic: the plate's behaviour writes its Transform, and a
            // kinematic body pushes what is in the way rather than passing
            // through it.
            doorBody.kinematic = true;
            door.attach<RigidBody>(doorBody);

            Entity plate = addMesh(
                "Plate",
                box,
                makeMaterial("Plate", glm::vec3{0.72f, 0.62f, 0.18f}, 0.6f, 0.3f),
                {2.3f, 0.47f, -1.5f},
                {0.5f, 0.06f, 0.5f});
            // The volume is knee-high even though the plate is flat: what a
            // trigger draws and what it notices are different shapes, and a
            // trigger only as thick as the plate would be a sliver the
            // character's feet barely clip. Local units, scaled by the entity
            // like every collider - which is why the numbers look large next
            // to a slab six hundredths of a unit thick.
            plate.attach<BoxCollider>(BoxCollider{{0.5f, 3.f, 0.5f}, {0.f, -1.5f, 0.f}});
            plate.attach<Trigger>(Trigger{"Character"});

            // A spawner on the far edge of the circuit, and the whole of
            // what a prefab is for: it names one asset and asks for a copy.
            // Nothing in the behaviour knows what is in the file - a crate, a
            // pickup, a monster - which is the difference between spawning
            // and constructing.
            //
            // The material the prefab names is built here under the name its
            // id was derived from. That is the one string this scene and
            // `scripts/make_pickup_prefab.py` have to agree on, and if they
            // stop agreeing the pickup arrives untextured rather than not at
            // all.
            makeMaterial("Pickup", glm::vec3{0.95f, 0.78f, 0.20f}, 0.7f, 0.25f);

            Entity dispenser = addMesh(
                "Dispenser",
                box,
                makeMaterial("Dispenser", glm::vec3{0.22f, 0.55f, 0.42f}, 0.2f, 0.5f),
                {0.5f, 0.47f, -1.5f},
                {0.5f, 0.06f, 0.5f});
            dispenser.attach<BoxCollider>(BoxCollider{{0.5f, 3.f, 0.5f}, {0.f, -1.5f, 0.f}});
            dispenser.attach<Trigger>(Trigger{"Character"});

            PrefabRef pickupPrefab{};
            if (const AssetRecord* record =
                    assets.findByPath(std::filesystem::path{"prefabs"} / "pickup.egeprefab")) {
                pickupPrefab = PrefabRef{record->id, assets.prefab(record->id)};
            } else {
                EGE_WARN("no pickup prefab in the project; there will be nothing to collect");
            }

            // Three of them along the circuit, stamped here rather than
            // spawned during play - the same asset, the same call, and a
            // reminder that a prefab is not a runtime-only thing. These are
            // what makes the level winnable inside one lap; the dispenser
            // above is what makes it winnable again.
            constexpr glm::vec3 pickupSpots[] = {
                {1.9f, 0.3f, -1.06f}, {1.0f, 0.3f, -1.9f}, {2.0f, 0.3f, -1.9f}};
            for (const glm::vec3& spot : pickupSpots) {
                Entity pickup = prefab::spawn(world, pickupPrefab);
                if (pickup.alive()) {
                    pickup.fetch<Transform>().translation = spot;
                    hierarchy::markDirty(world, pickup.id());
                }
            }

            Script dispenserScript{};
            Script::Slot dispenserSlot{};
            dispenserSlot.behavior = "ege::Spawner";
            auto spawner = std::make_shared<Spawner>();
            spawner->prefab = pickupPrefab;
            // Above the pad, so a copy falls onto the floor rather than
            // arriving inside it. Remember -Y is up.
            spawner->offset = {0.f, -0.55f, 0.f};
            spawner->limit = 4;
            spawner->cooldown = 2.f;
            dispenserSlot.instance = std::move(spawner);
            dispenserScript.behaviors.push_back(std::move(dispenserSlot));
            dispenser.attach<Script>(std::move(dispenserScript));

            // The win condition, and the whole point of events: three
            // objects that have never met.
            //
            // A pickup says it was collected and removes itself, knowing
            // nothing about scores. The goal counts, knowing nothing about
            // what it is counting or what happens when it is done - only that
            // when it is, it waits a beat and says so. The gate hears that
            // and opens, having never heard of a pickup. Wire any two of them
            // together with a pointer and the third becomes impossible.
            Entity gate = addMesh(
                "Gate",
                box,
                makeMaterial("Gate", glm::vec3{0.62f, 0.24f, 0.30f}, 0.35f, 0.4f),
                {2.9f, 0.15f, -0.75f},
                {0.12f, 0.7f, 0.62f});
            gate.attach<BoxCollider>();
            RigidBody gateBody{};
            gateBody.kinematic = true;
            gate.attach<RigidBody>(gateBody);

            Script gateScript{};
            Script::Slot gateSlot{};
            gateSlot.behavior = "ege::OpenOnLevelComplete";
            auto opener = std::make_shared<OpenOnLevelComplete>();
            opener->opening = {0.f, 0.72f, 0.f};
            opener->speed = 0.9f;
            gateSlot.instance = std::move(opener);
            gateScript.behaviors.push_back(std::move(gateSlot));
            gate.attach<Script>(std::move(gateScript));

            // The counter has no body and nothing to draw: it is a rule, and
            // a rule is a behaviour on an entity that is only there to hold
            // it. A level that wanted two rules would have two.
            Entity rules = world.spawn("Rules");
            rules.attach<Transform>();
            Script rulesScript{};
            Script::Slot rulesSlot{};
            rulesSlot.behavior = "ege::Goal";
            auto goal = std::make_shared<Goal>();
            goal->needed = 3;
            goal->celebrateAfter = 0.8f;
            rulesSlot.instance = std::move(goal);
            rulesScript.behaviors.push_back(std::move(rulesSlot));
            rules.attach<Script>(std::move(rulesScript));

            Script plateScript{};
            Script::Slot plateSlot{};
            plateSlot.behavior = "ege::PressurePlate";
            auto pressure = std::make_shared<PressurePlate>();
            pressure->door = "Door";
            // Down into the floor, and far enough that the whole slab goes:
            // remember -Y is up, so down is +Y.
            pressure->opening = {0.f, 0.72f, 0.f};
            pressure->speed = 1.1f;
            plateSlot.instance = std::move(pressure);
            plateScript.behaviors.push_back(std::move(plateSlot));
            plate.attach<Script>(std::move(plateScript));
        }

        // Lights are entities too. Remember -Y is up, so a negative Y is above
        // the floor.
        addLight("KeyLight", {-1.5f, -1.6f, -1.2f}, {1.f, 0.95f, 0.85f}, 6.f);
        addLight("FillLight", {1.8f, -1.2f, 0.8f}, {0.4f, 0.6f, 1.f}, 5.f);
        addLight("RimLight", {0.f, -0.9f, 2.2f}, {1.f, 0.5f, 0.3f}, 3.f);

        // A spot aimed down at the crate tower, which is the one thing in the
        // scene tall enough for a cone to fall across unevenly - a spot aimed
        // at flat ground draws a circle and demonstrates nothing about the
        // shadow map. Remember -Y is up, so this sits above the tower and
        // points back down at it.
        {
            Entity spot = world.spawn("TowerSpot");
            Transform spotTransform{};
            spotTransform.translation = {-2.3f, -2.6f, 0.2f};
            // A spot is aimed by rotating the entity, like anything else in
            // the scene: the light shines down the transform's forward axis
            // rather than carrying a direction of its own that the transform
            // could disagree with.
            //
            // The sign matters and is easy to get backwards. A negative pitch
            // turns forward towards +Y, which is *downwards* here because this
            // scene treats -Y as up; a positive one aims the light at the sky.
            // Slightly under a quarter turn, so the cone also leans towards
            // the tower rather than falling straight past it.
            spotTransform.rotation = {-glm::half_pi<float>() * 0.88f, 0.f, 0.f};
            spot.attach<Transform>(spotTransform);

            SpotLight cone{};
            cone.color = {1.f, 0.93f, 0.75f};
            cone.intensity = 22.f;
            cone.range = 9.f;
            cone.innerAngle = 0.20f;
            cone.outerAngle = 0.34f;
            spot.attach<SpotLight>(cone);
        }

        // A bank of small accent lights over the floor, well past the sixteen
        // the forward shader used to cap the scene at. They are here to be
        // counted as much as to be seen: with clustered shading a fragment
        // loops the lights that reach it rather than every light in the
        // scene, and a demo with three lights demonstrates nothing about
        // that. Short range and low intensity so they read as pools on the
        // floor rather than washing out the key/fill/rim composition above -
        // and a short range is also what gives the culling something to do,
        // since a light that reaches everywhere lands in every cluster.
        {
            constexpr int columns = 8;
            constexpr int rows = 5;
            constexpr float rangeMetres = 1.2f;
            int index = 0;
            for (int row = 0; row < rows; row++) {
                for (int column = 0; column < columns; column++) {
                    const float x =
                        -3.5f + 7.f * static_cast<float>(column) / static_cast<float>(columns - 1);
                    const float z =
                        -1.5f + 5.5f * static_cast<float>(row) / static_cast<float>(rows - 1);
                    // Around the hue circle, so neighbouring pools differ and
                    // the boundaries between clusters would be obvious if the
                    // assignment were wrong.
                    const float hue = glm::two_pi<float>() * static_cast<float>(index) /
                                      static_cast<float>(columns * rows);
                    const glm::vec3 color{
                        0.5f + 0.5f * std::cos(hue),
                        0.5f + 0.5f * std::cos(hue + glm::two_pi<float>() / 3.f),
                        0.5f + 0.5f * std::cos(hue - glm::two_pi<float>() / 3.f)};

                    Entity accent =
                        addLight("Accent" + std::to_string(index), {x, -0.32f, z}, color, 0.34f);
                    PointLight& accentLight = accent.fetch<PointLight>();
                    accentLight.range = rangeMetres;
                    // Decoration, not lighting anyone reads a shadow from -
                    // and forty of these would be two hundred and forty depth
                    // passes. The three lights that compose the shot cast;
                    // these do not.
                    accentLight.castsShadows = false;
                    index++;
                }
            }
        }

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

        const std::unordered_map<std::string, EntityId> imported = importGltfModels();

        // The character's body. Imported like any other model and then picked
        // up and put under the entity that walks - which is the whole reason
        // importing hands back its roots. The model's own node carries the
        // offset that lines its feet up with the bottom of the capsule, so
        // parenting it needs no numbers here.
        //
        // The animator the import attached is on the skinned node inside;
        // CharacterAnimation goes there, and looks up the hierarchy for the
        // controller telling it what the body is doing.
        if (const auto walkerBody = imported.find("walker"); walkerBody != imported.end()) {
            const Entity walker = world.findByName("Walker");
            if (walker.alive() && !walkerBody->second.isNull()) {
                hierarchy::setParent(world, walkerBody->second, walker.id());

                // Collected first and attached afterwards: adding a component
                // while iterating the pool of another is fine, and doing it
                // in two steps means nobody has to know that.
                std::vector<EntityId> animated;
                world.each<SkeletalAnimator>([&](Entity entity, SkeletalAnimator&) {
                    if (hierarchy::isDescendantOf(world, entity.id(), walker.id())) {
                        animated.push_back(entity.id());
                    }
                });
                for (const EntityId entity : animated) {
                    Script script{};
                    Script::Slot slot{};
                    slot.behavior = "ege::CharacterAnimation";
                    auto animation = std::make_shared<CharacterAnimation>();
                    // The speeds the clips were drawn for, which are the ones
                    // this character was tuned to walk and run at.
                    animation->walkSpeed = 0.9f;
                    animation->runSpeed = 1.8f;
                    slot.instance = std::move(animation);
                    script.behaviors.push_back(std::move(slot));
                    world.lookup(entity).attach<Script>(std::move(script));
                }
            }
        }

        EGE_INFO(
            "Scene loaded: {} entities, {} drawn, {} point lights, {} spot lights, {} assets",
            world.entityCount(),
            world.count<Transform, MeshRenderer>(),
            world.count<Transform, PointLight>(),
            world.count<Transform, SpotLight>(),
            AssetDatabase::instance().all().size());

        verifySceneRoundTrip();
    }

    void Application::reloadChangedAssets(FileWatcher& watcher) {
        const std::vector<FileWatcher::Event> changes = watcher.poll();
        if (changes.empty()) {
            return;
        }

        AssetDatabase& assets = AssetDatabase::instance();

        // Nothing may be loading while the record list is rewritten under it.
        // A load in flight holds a pointer into that list, and a rescan can
        // move it.
        jobs.waitForAll();

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
        device.waitIdle();

        std::error_code errorCode;
        for (const FileWatcher::Event& change : changes) {
            const std::filesystem::path relative =
                std::filesystem::relative(change.path, assetRoot(), errorCode);
            if (errorCode) {
                errorCode.clear();
                continue;
            }
            if (const AssetRecord* record = assets.findByPath(relative)) {
                const Guid id = record->id;
                assets.reload(id);
                // A material comes back rewritten in place by the call above
                // and needs nothing more. A mesh or a texture was dropped
                // rather than rebuilt, and rebuilding it is a file read, a
                // decode and an upload - which now happens on a worker while
                // the frame carries on, and lands through takeLoaded.
                assets.requestAsync(id);
            }
        }

        // Whatever came back in place is live already; the rest arrives over
        // the next frames and is picked up there.
        systems::refreshAssetReferences(world);
    }

    std::filesystem::path Application::moduleRoot() {
#ifdef EGE_MODULE_ROOT
        return std::filesystem::path{EGE_MODULE_ROOT};
#else
        return std::filesystem::path{"."};
#endif
    }

    void Application::loadScriptModule() {
        if (options.scriptModule == "none") {
            EGE_INFO("Script module: none requested");
            return;
        }

        const std::filesystem::path path =
            options.scriptModule.empty() ? moduleRoot() / ScriptModule::fileName("EnchantedSandbox")
                                         : std::filesystem::path{options.scriptModule};

        std::string error;
        std::unique_ptr<ScriptModule> module = ScriptModule::load(path, error);
        if (module == nullptr) {
            // Not fatal. A module that will not load costs the behaviours in
            // it, and the engine has to be able to say so and keep running -
            // it is the thing the developer is about to fix.
            EGE_WARN("script module {} not loaded: {}", path.string(), error);
            return;
        }
        scriptModules.push_back(std::move(module));
    }

    void Application::reloadChangedScripts(FileWatcher& watcher, ScriptSystem& scripts) {
        const std::vector<FileWatcher::Event> changes = watcher.poll();
        if (changes.empty() || scriptModules.empty()) {
            return;
        }

        // The directory holds the engine's own library and every executable
        // beside it, all of which a build rewrites. Only the module matters.
        const std::filesystem::path watched = scriptModules.front()->sourcePath();
        const bool moduleChanged =
            std::any_of(changes.begin(), changes.end(), [&](const FileWatcher::Event& change) {
                return change.path.filename() == watched.filename() &&
                       change.change != FileWatcher::Change::removed;
            });
        if (!moduleChanged) {
            return;
        }

        std::string error;
        std::unique_ptr<ScriptModule> reloaded = ScriptModule::load(watched, error);
        if (reloaded == nullptr) {
            // A half-written file mid-link, most likely. The watcher will see
            // it again when the build finishes.
            EGE_WARN("script module reload failed: {}", error);
            return;
        }
        scriptModules.push_back(std::move(reloaded));

        // Every live instance is of a type the new module has just replaced in
        // the registry, so every one has to be made again from the new
        // factory. Reflected fields carry across; see rebuildInstances.
        const std::size_t rebuilt = scripts.rebuildInstances(world);
        EGE_INFO("Script module reloaded: {} behaviour instance(s) rebuilt", rebuilt);
    }

    std::filesystem::path Application::assetRoot() {
#ifdef EGE_ASSET_ROOT
        return std::filesystem::path{EGE_ASSET_ROOT};
#else
        return std::filesystem::path{"assets"};
#endif
    }

    std::unordered_map<std::string, EntityId> Application::importGltfModels() {
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

        // Parsing is the slow half and needs no device, so every file is read
        // and decoded at once across the workers. Instantiating is the other
        // half - GPU objects and entities - and stays here, because the world
        // is not thread safe and because two imports racing to catalogue their
        // sub-assets would be two threads writing one record list.
        //
        // A parse that throws is caught here rather than in the job, so that
        // one bad file does not take down the ones beside it or the procedural
        // scene they would have joined.
        struct PendingImport {
            const AssetRecord* record = nullptr;
            std::future<GltfSceneData> parsed;
        };

        std::vector<PendingImport> pending;
        pending.reserve(scenes.size());
        for (const AssetRecord& record : scenes) {
            const std::filesystem::path path = assetRoot() / record.path;
            pending.push_back(
                {&record, jobs.submit([path]() { return gltf::parseFile(path.string()); })});
        }

        std::unordered_map<std::string, EntityId> roots;
        for (PendingImport& import : pending) {
            try {
                const GltfSceneData scene = import.parsed.get();
                const gltf::ImportStats stats = gltf::instantiate(
                    device,
                    world,
                    scene,
                    *materialPool,
                    *materialSetLayout,
                    import.record->name,
                    import.record->id);
                EGE_INFO(
                    "Imported {}: {} entities, {} meshes, {} materials, {} textures",
                    import.record->path.string(),
                    stats.entities,
                    stats.meshes,
                    stats.materials,
                    stats.textures);
                roots[import.record->name] = stats.root;
            } catch (const std::exception& error) {
                EGE_ERROR("{}", error.what());
            }
        }
        return roots;
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