# Enchanted Graphics Engine — Development Roadmap

> Living document. Describes how the project grows from a Vulkan renderer into a complete game engine.
>
> **Status: Phases 0, 1 and 3 complete. Phase 2 essentially complete. Phases 4, 5, 6, 7 and 8 substantially complete. Phase 9 has started — clustered shading has landed, and Phase 4's shadow and anti-aliasing debts are cleared.**
>
> **§10 is the plan from here** — written from where the engine actually is rather than from where this document originally guessed, and ordered by what unblocks what. Read that first when picking up the next piece of work.
>
> Each phase below carries an outcome note listing exactly what landed and what did not. The **frame graph** — for several updates the single most valuable outstanding item — is done, along with Vulkan 1.3, dynamic rendering, the **complete HDR pipeline** (float target → bloom → ACES), **image-based lighting** from a procedurally generated sky, a **sun with PCF-filtered shadows** rendered through the graph, and **glTF 2.0 import**. The **editor** now has an offscreen viewport with docked panels, hierarchy editing, a reflection-driven inspector, transform gizmos, a console, an asset browser, **Play/Pause/Step/Stop** and **undo/redo**. The **asset database** underneath it gives every asset a stable id in a `.egameta` sidecar, which is what finally lets a scene save what it draws — and what unblocked play mode and undo, both of which are world snapshots. A `--demo` camera tour and self-recorded frames make all of it demonstrable. **Scripting** landed on top of that: behaviours in C++ with reflected fields, editable in the inspector and saved into the scene, script-written geometry through `DynamicMesh`, and **asset hot reload** — edit a material file and the running scene changes. Script hot reload was the one thing Phase 7 asked for and did not get, for a stated reason: it needs the engine built as a shared library — which has since happened, so behaviours now live in a `sandbox/` module the engine reloads while it runs. **Phase 8 physics** landed next: Jolt behind an engine-owned `PhysicsWorld` confined to one translation unit, `RigidBody` and box/sphere/capsule colliders that are scenery without a body and simulated with one, two-way transform sync through the hierarchy, contacts delivered to `Behavior::onContact`, raycasts reachable from gameplay, and bitwise-pinned determinism — the demo opens with a boulder rolling down a plank into a crate tower. **Cascaded sun shadows** followed, clearing the oldest recorded debt: the frame graph learned layered images, and the fixed origin-anchored box became four camera-fitted cascades. **Phase 9 then opened with clustered shading**: the frame graph learned transient buffers, the RHI learned compute pipelines, and a compute pass now assigns lights to froxels so a fragment loops the lights that reach it rather than every light in the scene — which is what removed the sixteen-light ceiling the forward shader had. **Point-light cube shadows** followed, clearing the last of Phase 4's shadow debt: six depth passes per casting light into one cube array, sampled by direction. **MSAA** came after it, closing the anti-aliasing gap that headed the absent list since Phase 0, and **spot lights with their shadows** finished Phase 4's shadow work entirely. Next: the rest of Phase 9, or the remaining debts (FXAA/TAA, script hot reload) as need dictates.

## 1. Where the project stands today

Enchanted is a **Vulkan 1.3 forward renderer with a metallic-roughness PBR pipeline, image-based lighting and a frame graph**, an entity-component system, runtime reflection, VMA-backed GPU memory, textures with mip generation, a job system and a fixed-timestep clock. The scene renders linear HDR into a graph-managed float target and is tonemapped to the swapchain with the ACES fit; ambient light comes from a procedurally generated sky via irradiance and prefiltered-specular convolutions computed on the GPU at startup. It began as a port of Brendan Galea's Vulkan tutorial series and has since grown past it.

Behaviour is written in C++ against the same reflection the components use, and both assets and their edits reload while the engine runs.

Still absent, and tracked per phase in §6: post-process anti-aliasing beyond MSAA (FXAA/TAA), cooked asset packaging, script hot reload, character controllers and physics constraints.

The Vulkan foundations are idiomatic and are being kept:

| Class | Role |
|---|---|
| `ege::Window` | GLFW window + surface |
| `ege::Device` | Instance, physical/logical device (Vulkan 1.3), queues, command pool, buffer/image helpers, pipeline cache |
| `ege::SwapChain` | Swapchain images and views, frame pacing sync objects |
| `ege::Renderer` | Frame lifecycle (`beginFrame`/`endFrame`), command buffers, resize recreation |
| `ege::FrameGraph` | Passes declare reads/writes; barriers, layouts, load/store ops and transient images are derived |
| `ege::Pipeline` | Graphics pipeline + shader modules, created against attachment formats |
| `ege::EnvironmentLighting` | Procedural sky cubemap, irradiance, prefiltered specular and BRDF LUT, generated at startup |
| `ege::SkyboxSystem` | Draws the environment behind the scene at the far plane |
| `ege::ShadowMapSystem` | Depth-only shadow pass, run once per sun cascade and once per point-light cube face; the maps are layers of frame graph transients |
| `ege::ShadowCascadeSet` | The sun's cascades fitted to the camera's frustum — splits, sphere bounds and texel snapping, all device-free |
| `ege::PointShadows` | The six faces of a point light's shadow cube, and the depth the shader compares against — device-free |
| `ege::SpotShadows` | A spot's cone falloff and the single map it casts through — device-free |
| `ege::ClusterGrid` | The view frustum diced into froxels — depth slicing, per-cluster bounds and the sphere test, all device-free |
| `ege::ClusterLightSystem` | The compute pass that assigns lights to clusters, and the buffer layout the shaders agree on |
| `ege::BloomSystem` | Half-resolution bright-pass and separable blur, composited before the tonemap |
| `ege::PostProcessSystem` | Fullscreen ACES tonemap from the HDR scene target to the backbuffer |
| `ege::EditorOverlay` | In-process editor: viewport, hierarchy, inspector, gizmos, assets, console, stats |
| `ege::EditorViewport` | The offscreen image the scene renders into and the UI samples |
| `ege::PlayMode` | Snapshots the world on Play, restores it on Stop |
| `ege::UndoStack` | Whole-scene mementos, bounded, with labels |
| `ege::AssetDatabase` | Stable ids to assets; `.egameta` sidecars, cataloguing, loading, reloading |
| `ege::FileWatcher` | Notices when a file under the project directory changes |
| `ege::Behavior` / `ege::ScriptSystem` | C++ behaviours and the callbacks that drive them |
| `ege::BehaviorRegistry` | Behaviours by name, so the editor can attach one |
| `ege::DynamicMesh` | CPU-side geometry a script rewrites, uploaded once a frame |
| `ege::PhysicsWorld` | Rigid-body simulation behind an engine-owned interface; Jolt lives in one TU behind it |
| `ege::PhysicsSystem` | Keeps the ECS and the physics world agreeing: bodies from colliders, poses back to transforms |
| `ege::Guid` | The 128-bit identifier an asset reference is made of |
| `ege::FrameRecorder` | Writes rendered frames to disk, one PNG per frame |
| `ege::DemoTour` | The scripted camera move `--demo` runs |
| `ege::LogBuffer` | The recent log, in memory, for the console to show |
| `ege::Buffer` | Generic mapped/staged Vulkan buffer |
| `ege::DescriptorSetLayout` / `Pool` / `Writer` | Fluent descriptor builders |
| `ege::Model` | Vertex/index buffers, procedural primitives, tinyobj loading, vertex dedup |
| `ege::Texture` | Upload, mip generation, samplers, sRGB/linear selection |
| `ege::Material` | Metallic-roughness properties and a per-material descriptor set |
| `ege::World` / `ege::Entity` | Sparse-set ECS and the entity facade |
| `ege::TypeRegistry` | Runtime reflection |

### 1.1 Known blockers — *resolved in Phase 0*

The repository as checked out **did not run**. Four defects gated every later phase:

1. **Shader glob case mismatch.** Sources live in `Shaders/`; `CMakeLists.txt` globs `shaders/`. On case-sensitive filesystems the `Shaders` target compiles nothing.
2. **SPIR-V path mismatch.** `simple_render_system.cpp` loads `CompiledShaders/*.spv`; CMake writes `.spv` beside the GLSL sources; `CompiledShaders/` is gitignored and absent from the tree.
3. **`ENGINE_DIR` is never defined.** `ege_model.cpp` and `ege_pipeline.cpp` fall back to `"../"`; nothing in the build defines it.
4. **Assets missing.** `loadGameObjects()` loads `models/flat_vase.obj`, `models/smooth_vase.obj` and `models/plane.obj`; there is no `models/` directory, so tinyobj throws at startup.

All four are fixed; the engine renders the demo scene from a clean checkout.

### 1.2 Inherited bugs — *resolved in Phase 0*

- `EgeBuffer::getAlignmentSize()` returns `instanceSize` instead of `alignmentSize` — harmless today, breaks the moment dynamic-offset UBOs appear.
- `PipelineConfigInfo` has no member initializers. The one call site brace-initializes it and it is still an aggregate under C++17, so the fields do get zeroed today — but the deleted copy constructor makes it a non-aggregate under C++20, where the same code stops compiling.
- ~~`TransformComponent::normalMatrix()` uses the inverse-scale shortcut, which is only correct without shear.~~ Checked numerically against `transpose(inverse(M))` over 20k randomised transforms: the shortcut is **exact** for the strict T·R·S composition `mat4()` builds, so there is nothing to fix. The constraint is now documented at the function, and must be revisited if shear or general parent matrices are introduced.
- `EgeCamera` takes `float near, far` — both are macros under `<windows.h>`.
- `GLM_FORCE_RADIANS` / `GLM_FORCE_DEPTH_ZERO_TO_ONE` are repeated in five translation units; if one ever forgets them the ODR violation is silent and the depth math breaks.
- `cullMode = VK_CULL_MODE_NONE` with `FRONT_FACE_CLOCKWISE` — no backface culling at all.
- Stale `}  // namespace lve` closing comments in five files.

---

## 2. Design decisions

| Area | Decision |
|---|---|
| Scene model | **ECS** with an original, deliberately non-Unity API optimised for ergonomics |
| Physics | **Jolt Physics**, integrated behind an engine-owned interface so the backend stays replaceable |
| Rendering | **Modern PBR** — materials, IBL, cascaded shadows, HDR, post-processing |
| Editor | **Standalone editor application**, with the engine built as a library |
| Scripting | **Native C++** behaviours in a hot-reloadable shared library |

---

## 3. Target architecture

Three CMake targets. The first two exist; the editor and player arrive in Phases 5 and 10.

```
Enchanted            (static lib)  — the engine                      [exists]
EnchantedEngine      (exe)         — the demo application            [exists]
EnchantedEditor      (exe)         — tooling, links Enchanted        [Phase 5]
EnchantedPlayer      (exe)         — shipping runtime, packed project [Phase 10]
```

Target directory layout. The modules marked *exists* landed in Phase 0; the rest are added by the phase that needs them:

```
cmake/          Dependencies, CompilerWarnings, Shaders modules          [exists]
app/            entry point                                             [exists]
src/
  core/         Application, Log, Time, JobSystem, FileWatcher;
                + VFS, Profiler                                         [exists]
  reflect/      TypeRegistry, FieldInfo, EGE_REFLECT macros              [Phase 1]
  platform/     Window, KeyboardMovementController; + Input             [exists]
  rhi/          Device, SwapChain, Pipeline, Buffer, Descriptors;
                + Texture, FrameGraph                                    [exists]
  render/       Renderer, Model, Camera, SimpleRenderSystem;
                + Material, Lights, Shadows, IBL, PostFX                 [exists]
  scene/        GameObject; replaced by World, Entity, ComponentPool     [exists]
  assets/       AssetDatabase, importers, loaders                        [Phase 6]
  physics/      PhysicsWorld + Jolt backend, colliders                   [exists]
  script/       Behavior, Script, ScriptSystem, BehaviorRegistry;
                + ScriptModule and its reload                           [exists]
  audio/ ui/ anim/                                                       [Phase 10]
editor/         EnchantedEditor sources + panels                         [Phase 5]
runtime/        EnchantedPlayer                                          [Phase 10]
sandbox/        example project + example C++ scripts                    [Phase 7]
shaders/        GLSL, compiled into the build tree                       [exists]
assets/         runtime assets, resolved via EGE_ASSET_ROOT              [exists]
tests/          doctest suite                                            [exists]
docs/                                                                    [exists]
```

**Naming cleanup — done in Phase 0.** The `Ege` prefix was redundant inside `namespace ege`, so `EgeDevice` is now `ege::Device`, and `EnchantedEngine` is `ege::Application`. Files are `PascalCase.hpp/.cpp`, named after the type they declare. `Model` keeps its name rather than becoming `Mesh`; the CPU/GPU split into `render/Mesh` and `assets/MeshData` happens in Phase 4 where it is actually needed.

---

## 4. The scene API

The distinguishing verbs are **Spawn / Attach / Fetch / Detach / Despawn**, and queries — not per-object component lookups — are the primary gameplay idiom.

As shipped. Method names are camelCase, matching the rest of the codebase rather than the PascalCase of the original sketch; `SetParent` and `View` are not built yet.

```cpp
World world;

Entity player = world.spawn("Player");
player.attach<Transform>(Transform{.translation = {0, 1, 0}});
player.attach<MeshRenderer>(MeshRenderer{mesh, material, true});

Transform& t = player.fetch<Transform>();       // asserts present
if (auto* r = player.find<RigidBody>()) { ... } // nullable
player.detach<MeshRenderer>();
player.despawn();

// Queries - the main way systems are written
world.each<Transform, PointLight>([](Entity e, Transform& t, PointLight& l) { ... });

world.Each<Transform>(Without<Frozen>{}, [](Entity e, Transform& t) { ... });

// Systems attach to explicit schedule phases
world.Schedule()
     .OnFixedTick(PhysicsSyncSystem)
     .OnTick(SpinSystem)
     .OnRender(FrustumCullSystem);
```

**Implementation:** a hand-written **sparse-set ECS**, not a wrapper over EnTT. `EntityId` is a 32-bit handle (`index:24 | generation:8`); the user-facing `Entity` is a `{World*, EntityId}` pair so the call sites above read well. Each component type gets a `ComponentPool` (dense array + sparse index map); `View` intersects by iterating the smallest pool. Owning the registry ourselves is what makes reflection, serialization and editor inspection cheap — a façade over a third-party ECS would fight us on all three.

**Core components:** `Name`, `Transform` (local TRS + cached world matrix + dirty flag), `Hierarchy` (parent / firstChild / nextSibling), `MeshRenderer`, `DirectionalLight`, `PointLight`, `SpotLight`, `Camera`, `RigidBody`, `BoxCollider` / `SphereCollider` / `CapsuleCollider` / `MeshCollider`, `Script<T>`, `Disabled` (tag).

---

## 5. The scripting API

```cpp
class Spinner : public ege::Behavior {
public:
    float degreesPerSecond = 90.f;

    void OnSpawn() override {}
    void OnTick(float dt) override {
        Fetch<Transform>().rotation.y += glm::radians(degreesPerSecond) * dt;
    }
    void OnFixedTick(float dt) override {}
    void OnContact(const Contact& c) override {}
    void OnDespawn() override {}
};

EGE_BEHAVIOR(Spinner, EGE_FIELD(degreesPerSecond))   // registers name + editable fields
```

Vertex manipulation from script:

```cpp
auto& dyn = Fetch<DynamicMesh>();
for (auto& p : dyn.Positions()) p.y = sinf(Time::Now() + p.x);
dyn.RecalculateNormals();
dyn.MarkDirty();          // re-uploaded via a per-frame staging ring buffer
```

Scripts compile into a shared library (`.dll` / `.so`) loaded at runtime. Hot reload: file watcher fires, live `Behavior` field state is serialized through reflection, the module is unloaded, rebuilt, reloaded, and state is restored. Scripts see only `engine/include/enchanted/`; the ABI boundary is same-compiler/same-flags by contract, enforced by a version stamp checked at load time.

---

## 6. Phases

Every phase ends with a **runnable engine** and a demonstrable result. Estimates assume solo part-time work.

### Phase 0 — Foundation reset (~2–3 weeks)

- Fix the four blockers in §1.1 and the inherited bugs in §1.2.
- Standardise shader compilation: lowercase `shaders/`, `.spv` emitted into the build tree, shader compilation as a dependency of the main target rather than a separate manual one, `.spv` binaries no longer committed.
- Replace the `.env.cmake` machine-path scheme with **CPM/FetchContent** for every dependency — GLFW, glm, Dear ImGui (docking), ImGuizmo, spdlog, VulkanMemoryAllocator, SPIRV-Reflect, stb, cgltf, nlohmann/json, Jolt, doctest. Keep `find_package(Vulkan)` for the SDK. Delete `envWindowsExample.cmake` and `envUnixExample.cmake`.
- Delete the stale `EnchantedGraphicsEngine.sln`, `.vcxproj` and `.vcxproj.filters` — the CMake migration already happened.
- Split into the three targets; move sources into the §3 layout; drop the `Ege` prefix.
- `.clang-format`, `.clang-tidy`, `CMAKE_EXPORT_COMPILE_COMMANDS`, `-Wall -Wextra` / `/W4`, warnings-as-errors, ASan/UBSan in Debug.
- GitHub Actions CI: Linux + Windows configure/build/test, plus a clang-format check and a headless smoke run under lavapipe with `VK_LAYER_KHRONOS_validation` enabled.
- README, `docs/` skeleton, LICENSE.

**Done when:** a clean checkout builds and runs on Linux *and* Windows with no local path configuration, and CI is green.

**Outcome.** Done. Delivered beyond the original list: the engine gained procedural box/plane/sphere primitives so a clean checkout needs no binary assets at all; the 131 warnings the new flag set surfaced were all fixed and promoted to errors; a doctest suite and five-configuration CI landed, including a headless render smoke test under lavapipe with validation layers treated as failures.

Three things worth recording, because two of them contradict what this document originally claimed:

- **`normalMatrix()` was never buggy.** The inverse-scale shortcut is *exact* for the strict T·R·S composition `mat4()` builds — verified against `transpose(inverse(M))` over 20k randomised transforms. The claim is retracted in §1.2 and the precondition is now documented at the function and pinned by a test.
- **`PipelineConfigInfo` was not UB under C++17.** A deleted copy constructor is user-declared but not user-provided, so the struct stayed an aggregate and `config{}` did zero it. The genuine problem was that it stopped compiling under C++20, which is now a CI configuration.
- **Fixing the shadowing warnings uncovered a real defect** that had been hidden by it: `DescriptorPool`'s constructor initialised its device reference from itself. It compiled only because the parameter and the member shared a name.

### Phase 1 — Core layer & reflection (~3–4 weeks)

- `core/`: `Log` (spdlog), `EGE_ASSERT` / `EGE_VERIFY`, `Time`, `Guid`, typed `Handle<T>`, `EventBus`, `VirtualFileSystem` (mount points, replacing `ENGINE_DIR` string concatenation), `JobSystem` (work-stealing thread pool), Tracy profiler hooks.
- `reflect/` — the keystone for editor, serialization and scripting alike:
  ```cpp
  EGE_REFLECT(Transform)
      EGE_FIELD(position)
      EGE_FIELD(rotation, Range(-180.f, 180.f))
      EGE_FIELD(scale)
  EGE_REFLECT_END()
  ```
  A runtime `TypeRegistry` maps type name → size, ctor/dtor, field list with offsets, and generic get/set. Inspector UI, scene files and script state preservation across hot reload are all generated from it.
- `platform/`: a real `Input` abstraction — keyboard, mouse, gamepad; edge-triggered `Pressed`/`Released` alongside `Held`; mouse delta and scroll; named action mappings. This removes the `GLFWwindow*` / `GLFW_KEY_*` leakage from `keyboard_movement_controller.hpp`. `FileWatcher` and `DynamicLibrary` also land here, for Phase 7.

**Done when:** the existing demo runs on the new core, the camera has proper mouse-look, and unit tests cover reflection round-trips and the job system.

**Outcome.** Done, with three parts deferred and one design change.

Delivered: `core/Log` and `core/Assert` on spdlog; `reflect/` with `TypeRegistry`, `TypeInfo`, `FieldInfo`, chained field attributes and `EGE_REFLECT`; `platform/Input` with edge-triggered state, mouse delta, capture modes and named action bindings, plus `platform/CameraController` giving the engine mouse-look; `core/Time` with the fixed-step accumulator; and `core/JobSystem` with `parallelFor` and cooperative waiting. Test count went from 9 to 44.

**Deferred to the phase that needs them:** `EventBus`, `Guid`/`Handle<T>` and the `VirtualFileSystem` — none has a caller yet, and the `EGE_ASSET_ROOT` mechanism from Phase 0 covers path resolution until the asset database lands in Phase 6. `FileWatcher` and `DynamicLibrary` move to Phase 7, where script hot-reload is the thing that actually defines their interface.

**Design change:** attributes chain off `EGE_FIELD` rather than being trailing macro arguments. A variadic macro invoked with no variadic argument is ill-formed before C++20 and `-Wpedantic` rejects it, so `EGE_FIELD(scale)` would not compile. The chained form reads better and is what the API keeps.

Three things worth recording:

- **Logging cannot require an explicit `init()`.** Subsystems log from their constructors, and members are constructed before the owner's constructor body runs, so `Device` logs before anything in `Application`'s body could initialise a logger. The first version segfaulted immediately for this reason. The accessors now initialise on first use.
- **A fixed-size thread pool deadlocks on nested blocking submits.** Twenty outer jobs on four workers, each blocking on `future::get()` for children no worker was free to run. Since that is the normal shape of asset loading, `JobSystem::waitFor` runs queued work on the calling thread while waiting.
- **`1.f/60.f` is not exactly representable and rounds up**, so half a second holds 29 fixed steps rather than 30. A test asserted 30 and was wrong; the exact-count test now uses 1/64.

### Phase 2 — RHI modernization (~4–5 weeks)

- **VulkanMemoryAllocator** — today every buffer gets its own `vkAllocateMemory`, which hits `maxMemoryAllocationCount` at scale.
- Raise the baseline to **Vulkan 1.3** (dynamic rendering, synchronization2, timeline semaphores) with `VK_KHR_portability_enumeration` so MoltenVK works. Dynamic rendering removes most of the 428 lines of render-pass and framebuffer bookkeeping in `ege_swap_chain.cpp`.
- `Texture` / `Sampler` / `Image` classes — `EgeDevice::createImageWithInfo` and `copyBufferToImage` already exist and are unused. Mipmap generation, sRGB vs linear handling, cubemaps, HDR formats.
- **SPIRV-Reflect**-driven pipeline creation: derive descriptor set layouts, push-constant ranges and vertex input from the shader. Today `ege_pipeline.cpp` includes `ege_model.hpp` and hardwires `Vertex`; that coupling has to go.
- Bindless descriptors (descriptor indexing) for textures and materials; per-frame descriptor allocator; ring buffer for transient uploads.
- **FrameGraph-lite:** passes declare resource reads and writes; the graph inserts barriers and manages transient render targets. This is what makes shadows → G-buffer → lighting → post composable rather than hardcoded.
- On-disk pipeline cache; shader hot reload.

**Done when:** a textured quad renders through the frame graph, validation-clean, with GPU memory managed by VMA.

**Outcome — essentially complete.** What remains is deferred with a reason, not pending.

*Done:* VMA now backs every buffer and image allocation. This was the urgent part: one `vkAllocateMemory` per buffer hits `maxMemoryAllocationCount` (commonly 4096) at a few thousand meshes while plenty of memory remains free. `rhi/Texture` adds upload, mip generation by successive blits, samplers with device-clamped anisotropy, and per-texture sRGB-versus-linear selection — which finally uses `createImageWithInfo`, `copyBufferToImage` and the `samplerAnisotropy` feature, all of which had been requested or written and never called.

The **on-disk pipeline cache** also landed: pipelines no longer recompile from SPIR-V on every launch, a corrupt or stale blob is rejected by the driver rather than breaking start-up, and the cache is written explicitly after pipeline creation as well as at shutdown, because a killed process never unwinds. One embarrassment worth recording: the first version created, loaded and saved the cache but passed `VK_NULL_HANDLE` to `vkCreateGraphicsPipelines`, so no pipeline ever entered it and the file on disk was a 32-byte header. Everything around the feature worked; the feature itself was a fix-up commit later.

The **baseline is Vulkan 1.3 with dynamic rendering and synchronization2**. `vkCmdBeginRendering` replaced the render pass and both framebuffer arrays, `SwapChain` shrank to what the presentation engine actually forces on us, pipelines are created against attachment formats, and queue submission moved to `vkQueueSubmit2`. Portability enumeration is enabled when the loader offers it, so MoltenVK stays reachable. Device selection requires 1.3 and says which device it skipped and why.

The **frame graph** is in. Passes declare reads and writes; execution order, dead-pass culling, barriers, layout transitions, load/store ops, transient image allocation and the `vkCmdBeginRendering` call are all derived. The graph is rebuilt every frame — declaration is cheap and it lets passes come and go — while physical images persist in a description-keyed cache, so steady state allocates nothing. Compilation is deliberately device-free and the plan it produces is inspectable, which is what makes the scheduling unit-testable on a machine with no GPU: culling chains, first-writer-clears/second-writer-loads, render-to-sample transition scopes and never-stored depth are all pinned in `tests/test_framegraph.cpp`. The subtle part worth remembering: a recycled transient's first use each frame discards content via `UNDEFINED`, but must still chain after whatever the *previous* frame left that physical image doing — a cross-frame hazard that no within-frame reasoning catches.

*Deferred:*

- **SPIRV-Reflect** driven pipeline layouts, and **bindless descriptors**. Neither blocks anything today; bindless becomes worthwhile when material count grows past what per-material descriptor sets handle comfortably. The worst of the coupling SPIRV-Reflect would remove is already gone — vertex input moved out of `Pipeline` and into the systems that own vertex data.
- **Shader hot reload** — wants the `FileWatcher` that Phase 7 builds for script hot reload; doing it twice would be waste.

### Phase 3 — ECS world & scene (~4–5 weeks)

- Implement `World`, `Entity`, `ComponentPool`, `View` / `Each`, `With` / `Without` filters and `Schedule` — the API in §4.
- Retire `EgeGameObject` and its `unordered_map<id_t, EgeGameObject>`. The current `TransformComponent::mat4()` recomputes a full TRS composition per object per frame inside the draw loop; the new `Transform` caches a world matrix behind a dirty flag, propagated through `Hierarchy`.
- Scene serialization to JSON, generated entirely from reflection. `.egescene` files; prefabs as sub-scenes.
- Move lights out of the hardcoded `GlobalUbo` — where light position and colour currently keep their C++ initialiser values forever — into real `DirectionalLight` / `PointLight` / `SpotLight` components gathered per frame.
- **Debug ImGui overlay**: hierarchy tree plus a reflection-driven inspector, in-process. Not the editor yet, but it makes ECS work visible immediately and prototypes Phase 5's panels.

**Done when:** a scene of hundreds of entities with multiple lights loads from a `.egescene` file, is inspectable, and saves back byte-identically.

**Outcome — complete**, apart from two items deferred to the phase that gives them a caller.

*Done:* the ECS — `World`, `EntityId`, `ComponentPool`, the `spawn`/`attach`/`fetch`/`find`/`detach`/`despawn` API, `each()` with `With`/`Without` filters — and the migration of the scene, renderer and lights onto it. `GameObject` is gone. **Scene serialization** saves and loads `.egescene` JSON entirely through reflection: nothing in the serializer knows what a `Transform` is, and a new component becomes serializable by being reflected and registered. **Hierarchy** adds parenting with cycle refusal and cached world matrices.

*Deferred:* the explicit **`Schedule`** for system phases and the **debug ImGui overlay**. The schedule has no second system to order against the renderer yet, and the overlay is subsumed by the editor in Phase 5 — building a throwaway version first would be wasted.

Three design notes worth carrying forward:

- `EntityId` packs a 24-bit index with an 8-bit generation, bumped on despawn, so a handle held across a despawn is *detectably* stale rather than silently addressing whatever recycled the slot. A slot that exhausts its generations is retired rather than reused.
- `each()` iterates a snapshot of the driving pool so a callback may despawn or attach without invalidating the walk. That copy is a real per-query cost; a deferred command buffer is the eventual answer.
- Hierarchy makes the Phase 0 normal-matrix finding bite: a non-uniform parent scale combined with a child rotation introduces shear, which is exactly the case the inverse-scale shortcut excludes. Parented entities use the general `transpose(inverse(M))`; unparented ones keep the cheap path.

~~`MeshRenderer`'s model and material are deliberately **not** serialized.~~ They were runtime handles, and a reloaded scene restored transforms, names and lights but nothing to draw. Phase 6's asset database replaced them with id-backed references, and parenting - which this section claimed was recorded by entity order and in fact was not recorded at all - is now written as a position in the file's own entity array.

Two design notes worth carrying forward:

- `EntityId` packs a 24-bit index with an 8-bit generation, bumped on despawn, so a handle held across a despawn is *detectably* stale rather than silently addressing whatever recycled the slot. A slot that exhausts its generations is retired rather than reused.
- `each()` iterates a snapshot of the driving pool so a callback may despawn or attach without invalidating the walk. That copy is a real per-query cost; a deferred command buffer is the eventual answer.

### Phase 4 — PBR renderer (~6–8 weeks)

- **glTF 2.0 loading** (cgltf) replacing OBJ-only: submeshes, material slots, texture references, node hierarchy → entity hierarchy. tinyobj stays as a secondary importer. Note that `EgeModel` currently flattens every tinyobj shape into one buffer and discards materials entirely.
- `Material` asset: metallic-roughness parameters plus base colour, normal, metallic-roughness, occlusion and emissive maps, backed by a bindless material buffer.
- **PBR shading**: Cook-Torrance GGX, Smith visibility, Fresnel-Schlick; normal mapping (tangents from glTF or generated); emissive; alpha modes (opaque / mask / blend).
- **IBL**: HDR equirect → cubemap, irradiance convolution, prefiltered specular mip chain, precomputed BRDF LUT, skybox pass.
- **Shadows**: cascaded shadow maps for directional lights (PCF, stable cascades), cube shadow maps for points, spot shadows.
- **HDR pipeline**: offscreen `R16G16B16A16_SFLOAT` target → bloom → ACES tonemap → sRGB present. MSAA, then FXAA/TAA.
- **Culling & batching**: frustum culling, material/pipeline sort buckets, GPU instancing, a back-to-front transparent pass. The current draw loop iterates an `unordered_map` in nondeterministic order with zero culling.
- Editor-only debug rendering: wireframe, normals, light gizmos, bounding volumes.

**Done when:** a Sponza-class glTF scene renders with PBR materials, IBL, shadows and tonemapping at interactive rates.

**Outcome — partial.**

*Done:* the shading model. Cook-Torrance with Trowbridge-Reitz GGX, Smith/Schlick-GGX geometry, Schlick Fresnel, and correct energy conservation between the diffuse and specular lobes with metals carrying no diffuse term. `render/Material` holds metallic-roughness properties plus four texture slots behind a per-material descriptor set, ordered after the per-frame set by update frequency. Normal mapping derives its tangent frame from screen-space derivatives. Up to sixteen point lights, replacing the single hardcoded one — a ceiling clustered shading later removed outright. Backface culling.

**Frustum culling and material sorting** landed too. Meshes carry local-space bounds; the render system gathers visible objects, rejects those whose transformed bounds fall outside the frustum, sorts survivors by material then mesh, and submits. Sorting matters because descriptor set binds dominate a draw and component-pool order has no reason to group objects sharing a material. Per-frame statistics record candidates, culled, drawn and material binds.

**The HDR pipeline is complete as Phase 4 wrote it**: the scene renders linear radiance into an `R16G16B16A16_SFLOAT` transient, **bloom** extracts and blurs what exceeds 1.0 through a half-resolution bright-pass and a separable Gaussian (three `addPass` calls, two small shaders), and a fullscreen pass composites it in linear light and applies the ACES fit into the sRGB backbuffer.

Separating shading from display transform exposed a bug that had been shipping since the PBR shader landed: it applied a manual `pow(1/2.2)` gamma encode *and* wrote to an sRGB swapchain image, whose hardware encode applied the curve a second time. Every frame had been double-encoded — visibly washed out — and it read as "lighting needs tuning" rather than "encode applied twice", which is exactly why the display transform should exist in one place. The tonemap pass writes linear values and the sRGB format performs the only encode; the surface chooser now warns if it cannot get an sRGB format, because correctness depends on it.

**IBL landed**, from a procedurally generated environment. A sky with a sun is rendered into a mipmapped cubemap at startup, cosine-convolved into a 16-pixel irradiance map, GGX-prefiltered into a specular mip chain (one roughness per level), and paired with the split-sum BRDF LUT; the PBR ambient term is now the real split-sum evaluation and the sky draws behind the scene at the far plane. The whole precompute is GPU fullscreen passes and finishes in about a second even on CI's CPU rasterizer. The environment is procedural for the same reason the meshes are — a clean checkout ships no binary assets — and **HDR equirect import** joins the Phase 6 asset work, where environment maps become assets like any other. The demo's payoff is the one this document promised: the near-mirror sphere now reflects a sky instead of rendering nearly black.

**Directional sun shadows landed** as the frame graph's first depth-only pass: a `DirectionalLight` component, a fixed-size D32 transient rendered from the sun's orthographic view, and a 3×3 PCF test through a comparison sampler whose border compares as lit. The pass declares a depth write, the scene pass declares a sampled read, and every barrier between them is derived — the first feature to land as exactly the `addPass` call the graph was built to make cheap. Acne is handled by polygon-offset bias at raster time, not by fudging the lighting shader. ~~Known simplification, promoted from groundwork to roadmap item: the sun frustum is a fixed box sized to the demo floor; cascaded shadow maps fitted to the view frustum replace it when scenes outgrow one box.~~ **Replaced by cascades** — see the note below: the fixed box was anchored at the world origin, so a camera anywhere else looked at ground with no shadow map over it at all, a limit the demo never showed because the demo never left the box.

**glTF 2.0 import landed**, split deliberately into a CPU parse and a GPU instantiate. `assets/GltfLoader` (cgltf) reads meshes, metallic-roughness materials with their textures, and the node hierarchy — each local TRS decomposed into the engine's YXZ Euler convention, verified by a decompose-recompose test — and the instantiate half builds `Texture`/`Material`/`Model` objects and spawns entities under a root that carries the +Y-up→-Y-up correction as a half-turn roll, preserving winding. Any `.gltf`/`.glb` in `assets/models/` imports at startup; the demo gains a copper torus that is itself a self-contained text glTF, so a clean checkout still ships no binary asset and CI exercises the import path on every run. The parse half is pinned by unit tests against an embedded asset — accessors, generated normals, material factors, color-space policy, hierarchy — with no device anywhere. Imported scenes still don't survive a save/load round trip, because `MeshRenderer` handles are runtime-only; giving them stable references is precisely the Phase 6 asset database's job, along with cooked caches so imports stop re-parsing source on every run.

*Not done:*

- ~~**Cascaded** shadow maps for the sun~~ — **done**, recorded above. ~~**Cube shadow maps for point lights**~~ — **done** too, and built on exactly what was predicted: the frame graph's layered images, taught to be sampled as cubes. ~~Spot shadows remain, and are the smallest of the three.~~ **Done too**, and they were: a spot has one direction and a bounded angle, so one ordinary perspective map covers it.
- ~~**MSAA**~~ — **done**, and the prediction held exactly: multisampled attachments plus a resolve, entirely within the attachment management the graph already owned. **FXAA/TAA** remain; TAA in particular wants motion vectors, which is Phase 9 work.
- **Tangents from glTF** — the shader still derives its tangent frame from screen-space derivatives; imported tangents fix mirrored UVs when textured assets arrive in numbers.
- **Instancing** — the draw list is sorted and ready for it, but nothing merges consecutive identical draws yet.

**Cascaded sun shadows landed later**, clearing the debt this phase recorded. The view frustum is split by depth and each slice gets its own map, so texel density follows the camera rather than the scene's bounding box; the near cascade is an order of magnitude tighter than the far one. Four depth passes write four layers of one array transient — which is what the **frame graph's layered images** were added for, and what cube shadow maps for point lights would be built on next — and the shader picks a layer per fragment through a `sampler2DArrayShadow`, blending across the last tenth of each cascade so the seam between two texel densities does not draw a line across the ground.

Two decisions separate cascades that look right from cascades that shimmer, and both are pinned by tests rather than by eye. Each slice is bounded by a **sphere**, because a box fitted to the frustum corners changes size as the camera turns — a frustum's diagonal is longer than its side — and every shadow swims with it. And the map is snapped to a **whole-texel lattice**, or a sub-texel shift between frames re-rasterises every edge and the shadows crawl. The snapping has to be anchored to a rotation about the origin, not to a look-at aimed at the slice: a look-at puts its target at the origin of light space by construction, so snapping there rounds zero to zero and silently does nothing. The first version did exactly that, and the test asserting the map moves in whole texels is what caught it.

Worth stating plainly: the demo pictures barely changed. The demo scene fits inside the old 24-unit box, so what the cascades fixed is not visible in it — the tests carry that instead, one putting the camera five hundred units from the origin and checking the ground in front of it is still covered.

One bug worth recording, found by looking at the output rather than by the build passing: the default metallic-roughness texture was `(1, 1, 0)`, read as "fully rough, non-metal". But metallic samples from blue and is *multiplied* by `metallicFactor`, so a zero there forced every material to be a dielectric regardless of its factor, and the metal spheres shaded as diffuse plastic. Fallback textures have to be the multiplicative identity.

**Spot lights landed**, the last light type §4's component list named and the engine never had, and with them the last of this phase's shadow debt. A spot is the cheapest of the three to shadow: the sun needs a cascade per depth slice because it lights everything the camera sees, a point light needs six faces because it lights every direction, and a spot already has one direction and one bounded angle — so one ordinary perspective map covers it.

Points and spots share one buffer, one culling pass and one shading loop rather than getting a path each, because a spot *is* a point light with a direction and a cone. The cluster culling needed nothing at all: a cone is bounded by the sphere of its range, so the existing sphere test covers both — it may hand a fragment a spot whose cone misses it, which costs one iteration that shades to zero and never a light that should have been there.

Two angles rather than one, because a cone with a single hard edge looks like a cardboard cutout. Both travel as cosines, since that is what a dot product gives and converting back would cost an inverse cosine per fragment.

Two things worth recording, both about signs:

- **The falloff's direction is a test, not a comment.** Run backwards it gives a light bright at its rim and dark in its middle, which reads as a strange material rather than as a broken light. A larger angle is a smaller cosine, so the inner cosine is the upper bound — and an authored pair that crosses over is pinned to a hard-edged cone, which is merely plain rather than wrong.
- **The demo's spot was aimed at the sky in the first version.** This scene treats -Y as up, so "pitch it down" is a *negative* rotation, and the positive one written first pointed the ceiling light at the ceiling — which shows up as a light that appears to do nothing rather than one that looks wrong. The aiming convention is now a test, named by axis rather than by "up", since the two words point opposite ways here depending on which you mean.

**MSAA landed last**, closing the item that headed the absent list since Phase 0. The scene rasterises at four coverage samples per pixel and is averaged back down as the attachment is stored — not as a second pass over the image, which is why multisampling costs so much less than rendering large and scaling down. Phase 4's own prediction held exactly: it was multisampled attachments plus a resolve, entirely inside the attachment management the frame graph already owned, and nothing about barriers, culling or layouts needed to change.

The device is asked rather than told. A count must be supported for colour and depth both — an implementation may allow more on one than the other, and an attachment pair has to agree — and the answer is capped at 4x, a judgement rather than a limit. A device offering only one sample switches multisampling off and every path degrades to what it was.

Worth recording why this was cheap: multisampling and clustered forward shading get along, which is not true of every renderer. A deferred one would have to shade every sample or resolve a G-buffer, and neither is cheap. Forward shading runs the fragment shader once per covered pixel and lets the hardware average coverage — so the phase's earlier choice paid for this one.

### Phase 5 — Editor application (~6–8 weeks)

- ImGui **docking** branch, layout persistence, dark theme.
- **Viewport** — the scene renders to an offscreen target sampled as an ImGui image; editor camera with orbit / fly / focus; multiple viewports.
- **Hierarchy** — tree with drag-drop reparenting, multi-select, create/duplicate/delete, search.
- **Inspector** — fully reflection-driven; component add/remove; custom drawers for vectors, colours, asset references and enums; `Range` / `Color` / `Tooltip` attributes.
- **Asset browser** — thumbnails, drag-drop into viewport and inspector, import dialogs.
- **Console** — the `Log` sink, filterable by level.
- **Gizmos** — ImGuizmo translate/rotate/scale, local/world toggle, snapping.
- **Play / Pause / Step / Stop** — the world is snapshotted on Play and restored on Stop; edit-mode and play-mode systems are distinct.
- **Profiler / stats** — frame time graph, draw calls, triangles, GPU memory.
- Undo/redo command stack over reflection-based property edits.

**Done when:** a scene can be built, arranged, saved, played and stopped entirely inside the editor without touching code.

**Outcome — substantially complete.** It began as one, as this phase's own sequencing note advises (thin and early beats complete and late): Dear ImGui (docking) drawn as the frame's last declared pass, with a hierarchy tree, a reflection-driven inspector and a stats panel over the backbuffer. It is deliberately in-process — the panels exercise the ECS and reflection APIs now and move into the standalone `EnchantedEditor` application when it exists, rather than being rewritten.

*Done:*

**The viewport.** The scene renders into an offscreen image the UI samples as a texture instead of onto the backbuffer, and the panels dock around it. This is what separates an editor from a debug overlay: the view has its own aspect ratio independent of the window, the panels stop obscuring the thing they describe, and everything the editor eventually draws over the render has somewhere to live. The frame graph made the swap a change of declaration — the tonemap pass writes the viewport image, the UI pass declares a sampled read of it, every transition between them is derived, and with the editor hidden the tonemap targets the backbuffer again and the frame is byte-for-byte what it was before. The image carries the swapchain's sRGB format on purpose: the tonemap writes linear values the attachment encodes, ImGui's sampler decodes them, the sRGB backbuffer encodes them again, so the detour through a texture is an exact identity rather than a colour shift. Resizing a panel edge replaces the image and the descriptor set ImGui draws it with; both are retired and freed once the frames that could still reference them have gone, rather than stalling the device on every frame of a drag.

**Editing.** Entities are created, deleted (with their subtree) and reparented by dragging in the hierarchy; components are added and removed in the inspector, both straight off the component registry's type-erased thunks, so a newly registered component appears in the add list with nothing written per type. Structural edits are recorded during the walk that asks for them and applied after it — spawning mid-walk invalidates the sibling links the traversal is following, and attaching mid-loop rearranges the pool the inspector is reading out of.

**Gizmos** (ImGuizmo): translate, rotate and scale with a world/local toggle and snapping. Two conventions had to be got right and both fail quietly: ImGuizmo projects with OpenGL's clip space, where +Y is the top of the screen and Vulkan's is the bottom, so the projection handed to it is flipped in Y — without which the handles sit mirrored about the horizon, subtle enough near the centre of the view to pass for correct. And its habit of turning axes towards the camera is switched off, because in a scene whose up is -Y an arrow pointing up the screen could mean either sign. Manipulation happens in world space with the parent divided back out, since a gizmo moves the entity where it *appears* to be.

**Console:** the log, in a bounded in-memory ring fed by a third spdlog sink alongside the terminal and the file, filterable by severity and substring. It sits in `core/` behind the engine's own severity enum rather than spdlog's, because the console is a consumer of the log, not of the logging library.

**Layout** is built by the dock builder on first run and restored from `imgui.ini` after, so a fresh checkout opens arranged and an arranged editor stays that way.

One bug worth recording, found because the viewport made it impossible to miss: editing a parented entity's `Transform` in the inspector changed the numbers and moved nothing. World matrices are cached behind a dirty flag on `Hierarchy`, and writing fields through reflection never set it. Fields now report whether they changed, and an edited entity and its descendants are marked dirty. This is the general shape of the problem reflection-driven editing has — a generic writer knows how to set a field but not what setting it invalidates.

**Play / Pause / Step / Stop.** Play snapshots the world, Stop restores it, Step advances exactly one fixed tick. This was blocked on Phase 6 and said so: a snapshot is a scene file, and until a scene file could describe what a `MeshRenderer` draws, Stop would have handed back a world with no geometry in it. Snapshotting through the serializer rather than by copying pools is slower and deliberate — it means anything a scene file cannot describe does not survive Play, which is a property worth having visible, because it is exactly the set of things that will not survive a save either. Edit and play mode stay distinct by the simplest rule that works: nothing advances the world unless Play asked for it. The one play-mode system is honestly a placeholder — a `Spin` component with an angular velocity — because play mode has to be observable before it can be trusted and, until Phase 7, nothing else in the engine changes the world over time.

**Undo and redo**, as whole-scene mementos rather than inverse commands. The serializer is fast, complete and reflection-driven, so the state before a change is one call away and correct by construction for every kind of edit — a dragged slider, a deleted subtree, a component attached, a reparent — with no command class per operation, each of which is somewhere a bug can hide. Inverse commands earn their cost when copying a scene is expensive; this one is eight kilobytes. The price is stated rather than hidden: restoring respawns every entity, so the selection is re-found by name afterwards. A drag is one step, not one per frame, because the memento is taken when the interaction begins and kept only if something changed before it ended.

**The asset browser**, listing what the project holds by kind, with drag-and-drop into the inspector's mesh and material slots.

*Not done:* multi-select, custom drawers for enums, and the standalone `EnchantedEditor` executable — the panels are still hosted in the engine's own application, which is where they were always going to start. Moving them is a build-system change now rather than a rewrite, which was the point of building them in-process.

### Phase 6 — Asset pipeline (~4 weeks)

- **GUID-based asset database**: every source asset gets a `.egameta` sidecar carrying a stable GUID, so references survive moves and renames.
- Importers: glTF/OBJ → mesh, PNG/JPG/HDR → texture (KTX2 + BC compression for shipping), GLSL → SPIR-V, plus material / scene / prefab natives.
- Async loading on the `JobSystem`, with placeholder assets while loading and reference counting for eviction.
- Hot reload for textures, shaders, materials and meshes.
- Cooked-asset packaging for `EnchantedPlayer` — a single archive, no source assets shipped.

**Done when:** dropping a `.gltf` into the project folder imports it automatically and it is immediately draggable into the scene.

**Outcome — substantially complete.** The identity half is done, which is the half everything else was blocked on.

**GUID-based asset database.** Scanning the project catalogues every importable file and writes a `.egameta` sidecar for any that lacks one, so an id exists before the first reference to it does — requiring an explicit import step before assets have identities is how a pipeline becomes something people work around. The property the whole thing exists for is pinned by a test: move a file, rename it, rescan, and every reference already written still resolves. Paths are what the database answers, never what it stores.

Three ways to get an id, for three situations. Random and minted into a sidecar, for a file just met. Derived from a name, for assets that exist only in code — the procedural primitives the demo is built from have no file to keep a sidecar beside and still have to be referenceable from a scene. Derived from a container's id plus an index, for the meshes and materials inside a glTF, so reimporting a model does not orphan every reference into it. The name derivation is part of the contract rather than an implementation detail, and the test pins its actual value: changing it would silently break every scene file already written.

**References that survive.** `MeshRenderer` holds asset references rather than pointers, and only the id is written — a pointer is this process's answer to that id and means nothing in the next one. That asymmetry is why the component used to be unserializable, and fixing it is what let a saved scene come back drawable, which in turn unblocked play mode and undo. Parenting is recorded too, as a position in the file's own entity array, because an `EntityId` is an index into a running world's slot table and means nothing on disk.

A native `.egematerial` format loads through the same reflection-driven serializer the scene uses, so a field added to `MaterialProperties` is readable without touching the loader.

The database is split at the device boundary on purpose: cataloguing, sidecars and resolution need no GPU and are unit-tested; loading needs one, returns null without one, and is covered by the headless render test. Without that split none of it would be testable on a machine with no Vulkan device.

*Not done, each for a stated reason:*

- **Cooked-asset packaging** — it exists to feed `EnchantedPlayer`, which is Phase 10. Building the packer before the thing that unpacks it is how formats get designed against nothing.
- **KTX2 and BC compression** — the same: it is a shipping concern, and the engine ships nothing yet.
- **Hot reload** for textures, shaders, materials and meshes — wants the `FileWatcher` that Phase 7 builds for script hot reload. Doing it twice would be waste, which is the same reason Phase 2 deferred shader hot reload.
- **Async loading on the JobSystem** — and this one is a hazard, not a preference. Uploading from a worker thread needs a command pool per thread; `Device` has one, and using it from several threads is a data race the validation layers would not necessarily catch. It lands when the RHI grows per-thread pools, not before.

### Phase 7 — C++ scripting (~5–6 weeks)

- `Behavior` base class with `OnSpawn` / `OnTick` / `OnFixedTick` / `OnContact` / `OnDespawn`; the `Script<T>` component; a `BehaviorRegistry` populated by `EGE_BEHAVIOR` so the editor can list and attach behaviours by name.
- Script fields exposed via reflection — editable in the inspector, serialized into the scene.
- **Hot reload**: `FileWatcher` on the project's script directory → invoke CMake → build the shared library → serialize behavior state → unload → reload → deserialize. The engine ABI is version-stamped and mismatched modules are refused with a clear error.
- **Mesh manipulation**: a `DynamicMesh` component with CPU-side position / normal / uv / colour spans, `RecalculateNormals()`, `RecalculateBounds()`, and `MarkDirty()` driving a per-frame staging upload. Procedural mesh construction from script.
- Script-facing utilities: `Time`, `Input`, `Physics::Raycast`, `World::Spawn` / `Despawn`, prefab instantiation, `FindByName`, coroutine-style timers.
- Sandbox project demonstrating a player controller, a spinning object and a procedurally deformed mesh.

**Done when:** editing a `.cpp` script and saving it updates behaviour in a running editor play session within a couple of seconds, without losing entity state.

**Outcome — substantially complete, and explicitly short of its "done when".** Scripting works: behaviours run, are editable, serialize, and drive geometry. Reloading them at runtime does not, and the reason is a build-system change the whole project shares rather than anything about scripting.

**Behaviours.** `Behavior` is a base class with `onSpawn` / `onTick` / `onFixedTick` / `onDespawn`, and two accessors — `self()` for the entity it is on and `world()` for the scene it is in. Nothing else: a behaviour reaches everything through the same public API a system does, which is what makes the demo's behaviours portable into a project later by moving the file.

The sketch's `Script<T>` is a plain `Script` component holding a list of slots instead. A template would have made "two behaviours on one entity" a different component type per combination, and every query over scripted entities would have had to name it. The list also gives the inspector something to add to.

`EGE_BEHAVIOR(Type)` registers a factory at static-init time, keyed by name, and re-registering a name **replaces** rather than duplicates. That is not tidiness — it is what a reloaded module needs, and building it in now means the reload does not have to fight the registry when it arrives.

`ScriptSystem` gathers the behaviours to call into a vector before calling any of them. A callback that spawns or despawns an entity would otherwise be mutating the storage it is being iterated from, and "a script may spawn things" is not a caveat worth shipping.

**Fields, in the inspector and in the scene.** Through the same reflection the components use: declare a behaviour with `EGE_REFLECT` and its fields get sliders, colour pickers and tooltips, and are written into the scene file, with no per-behaviour code in either the inspector or the serializer. A behaviour whose type is not in the running build keeps its saved JSON **verbatim** rather than dropping it, so opening a scene without the code that defines a behaviour and saving it back does not silently erase the setup — which is the failure mode that makes people stop trusting a scene format.

**Mesh manipulation.** `DynamicMesh` holds CPU-side vertices and indices with `recalculateNormals()` and `markDirty()`; the upload happens once per frame in a system rather than once per write, because a behaviour deforming a surface writes every vertex and uploading per write would upload per vertex. The buffer behind it is host-visible and permanently mapped rather than staged, since the whole point is that it changes every frame. The demo's rippling sheet is a script rewriting 2 401 vertices per tick.

**Asset hot reload — the piece Phase 6 deferred to here.** `FileWatcher` polls modification times on an interval rather than subscribing to inotify, `ReadDirectoryChangesW` and FSEvents: three platform APIs with three sets of quirks, against one loop costing a stat per file every half second. A material is reloaded **in place**, so every holder sees the edit and no reference re-resolves; meshes and textures are immutable once uploaded, so those are dropped and the world's references are repointed. The device is idled first — rewriting a descriptor set that frames in flight are reading is undefined, and a stall on the frame after someone presses Ctrl+S is invisible. A broken edit costs the edit, not the run.

For this to be demonstrable at all, the demo's floor is now a real `.egematerial` file rather than a material built in code. Hot reload with nothing in the repository to edit is a claim, not a feature.

*Not done, each for a stated reason:*

- **Script hot reload**, and therefore the phase's "done when" — *landed later, once the engine became a shared library; see §10.2.* `Enchanted` was a **static** library. A `dlopen`'d script module linked against it would get its own copies of `TypeRegistry`, `ComponentRegistry`, `Serializer` and `BehaviorRegistry`, so nothing the module registered would be visible to the engine and nothing the engine registered would be visible to the module — every behaviour would look unknown, and every component the module touched would look unreflected. Making it work means building the engine as a **shared** library, with `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` on Windows and a version-stamped ABI check at load. That is a change to how the whole project is built and linked, on every platform, and it belongs in its own commit rather than smuggled in behind a scripting feature. The watcher that will drive it is built and working; what is missing is the thing it would drive.
- **`onContact`** — there are no contacts. It arrives with Jolt in Phase 8, where something can actually raise it.
- **`Physics::Raycast`** — the same.
- **Prefab instantiation and coroutine-style timers** — neither has a caller yet. `Time`, `World::spawn`/`despawn` and `findByName` are all reachable from a behaviour today, which covers what the demo behaviours need; the rest is API surface written against nothing.
- **A `sandbox/` project** — *landed later, as the script module hot reload loads.* It was deferred here because it needs the standalone editor to open it, which is Phase 10. The behaviours that would live there live in `src/script/Behaviors.cpp` instead, written against nothing but the public API, so moving them out later is a file move rather than a rewrite.

### Phase 8 — Physics via Jolt (~5–6 weeks)

- An engine-owned `PhysicsWorld` interface with a Jolt backend behind it, so the backend stays replaceable — including by a hand-written one later.
- Components: `RigidBody` (static / kinematic / dynamic, mass, damping, constraints), `BoxCollider`, `SphereCollider`, `CapsuleCollider`, `ConvexHullCollider`, `MeshCollider`, `CharacterController`, `Trigger`.
- **Fixed-timestep accumulator** at 60 Hz with render-transform interpolation; the current loop has no fixed step at all. `OnFixedTick` runs here.
- Two-way transform sync: editor and script writes push into Jolt; simulation results write back to `Transform`.
- Collision layers and filter masks; contact and trigger callbacks routed to `Behavior::OnContact`.
- Queries: raycast, shapecast, overlap.
- Constraints: hinge, slider, distance, fixed, 6-DOF.
- Editor: collider gizmos, physics debug draw, per-body inspector.

**Done when:** the sandbox has a character controller walking on physics geometry and pushing dynamic boxes, with colliders visualised in-editor.

**Outcome — substantially complete.** Rigid bodies simulate, collide, sleep, wake, report their touches to behaviours and answer raycasts, all through an engine-owned interface; the demo drops a boulder on a crate tower to prove it. The "done when" names a character controller in the sandbox, and the sandbox is a Phase 10 dependency - what exists is the simulation that controller will stand on.

**The interface, then the backend.** `PhysicsWorld` is the engine's own: bodies created whole from plain `BodySettings`, stepped at a fixed delta, queried by pose, raycast and drained contact events. Jolt sits behind it in exactly one translation unit, and no Jolt header is reachable from anywhere else - the roadmap's replaceability constraint enforced by the compiler rather than by discipline. The interface tests are deliberately backend-blind for the same reason: they are the contract a hand-written backend would have to meet.

**Components divide the labour by what the words mean.** A *collider* (box, sphere or capsule) says what shape an entity presents; a *RigidBody* says the simulation may move it. A collider alone is scenery - the demo floor is landed on without ever being simulated - and `kinematic` on the RigidBody makes the entity the caller's to move: it is handed its Transform as a target each tick with the velocity the move implies, so what it sweeps through is pushed rather than skipped. All of it is reflected, so the inspector and the scene file got physics for free, except the cached body handle, which is deliberately unreflected: a handle is this simulation's answer to this component and means nothing in the next run.

**The sync reconciles rather than listens.** Each fixed tick, entities that gained colliders get bodies at their current world pose, despawned ones lose them, and a RigidBody that changed its mind about how it moves is rebuilt - the same declare-cheaply-every-frame trade the frame graph makes, and it spares the ECS growing attach/detach hooks. Dynamic bodies write back through the parent's inverse matrix, because a Transform's fields are local and physics does not know what a parent is. Scale is applied to the shape when the body is built, since a rigid body cannot change size.

**Physics lives and dies with play.** Play builds the physics world, Stop throws it away, and the snapshot restore puts the transforms back - so simulation can never leak into the scene being authored, for the same reason behaviours cannot: nothing advances the world unless Play asked for it.

**Contacts reach gameplay deterministically.** Jolt raises them from its worker threads mid-step; they are buffered under a lock, drained after the step, sorted - job scheduling reports the same touches in a different order run to run, and sorting extends the determinism guarantee to gameplay - and delivered to `Behavior::onContact`, each side hearing about the touch from its own side with the normal pointing away from itself. Whole-scene determinism is pinned by a test that runs eight bouncing spheres twice and compares positions *bitwise*: approximate equality would not be determinism. Sensors landed as a RigidBody flag: a trigger volume is a body that reports and stops nothing.

**Queries.** `raycast` is reachable from gameplay as `world().physics()->raycast(...)` - the scene carries a forward-declared pointer to the physics world while it simulates, published on start and retracted on stop, so behaviours reach physics without the ECS growing a dependency on it.

*Not done, each for a stated reason:*

- ~~**CharacterController**~~ - **landed in v0.5**, and the reason given here turned out to be half wrong: a character controller is not a sandbox convenience, it is an engine feature the sandbox uses. What was right is that nothing about the body interface blocked it - the virtual character arrived alongside bodies rather than through them. See the v0.5 table in §11.3.
- **ConvexHullCollider / MeshCollider** - both want an asset-derived hull or triangle soup, which is the asset pipeline's cooked-data story; a hand-authored primitive covers everything the engine currently draws.
- **Constraints** (hinge, slider, distance, fixed, 6-DOF) - API surface with no caller yet; the backend supports them whenever something needs a door.
- **Collision layers and filter masks** - the two layers that exist (moving, non-moving) are a broad-phase optimisation, not a gameplay feature. Named layers arrive when two things exist that must not collide.
- **Shapecast and overlap queries** - raycast has a caller-shaped hole today; the others do not.
- **Render-transform interpolation** - `Time::fixedAlpha()` has existed since Phase 1 for exactly this, but nothing else in the engine interpolates and physics should not be the odd one out. It lands as one change when rendering learns to interpolate everything the simulation moves.
- **Collider gizmos and physics debug draw** - editor work, worth doing when colliders are being authored in the editor rather than in code.

### Phase 9 — Advanced rendering (~8+ weeks)

- **Clustered forward+** or deferred shading for hundreds of lights — the frame graph makes this a swap rather than a rewrite.
- SSAO/GTAO, screen-space reflections, volumetric fog, decals.
- TAA, motion vectors, depth pre-pass, HZB occlusion culling.
- GPU-driven rendering: indirect draws, compute-based culling, meshlets.
- **Skeletal animation**: skinning, animation clips, blend trees, an animation state machine, IK.
- GPU particle system, trails, sprite and decal atlases.
- Terrain: heightmap rendering, LOD, splat-mapped materials.
- Global illumination — light probes and lightmaps first, then DDGI or voxel GI.

**Outcome — clustered forward+ landed; the rest of the phase has not.**

The view frustum is diced into a 16×9×24 grid of froxels, a compute pass assigns every point light to the cells its volume reaches, and the fragment shader loops its own cell's list. A pixel now pays for the lights that reach it rather than for every light in the scene, which is what removed the sixteen the forward shader was capped at. The lights moved out of the uniform block into a storage buffer; the bound that remains is on memory, not on per-fragment work.

Two pieces of foundation came first, and both are the phase's own prediction coming true — the roadmap said the frame graph would make this a swap rather than a rewrite, and the swap is what it cost. The **RHI learned compute pipelines**, which it had none of; the graph learned **transient buffers**, so the compute pass's output is a declared resource and the compute-to-fragment barrier is derived like every other. Buffers are not images with a different struct: they have no layout, and every barrier the graph emitted was triggered by a layout change, so a buffer's first touch each frame emitted none — which is exactly where the cross-frame hazard lives. That first touch now asks for a barrier explicitly, and a test pins it.

The froxel geometry lives in `render/ClusterGrid`, device-free and unit-tested, and the culling shader is written against it rather than re-deriving it. Slices are spaced exponentially, because an even split puts almost every cell in the far half of the frustum; the shader cannot afford the logarithms that inverts, so the scale-and-bias form is exposed and a test pins the two forms to the same answer across the range.

Three things worth recording, two of them bugs this work exposed rather than introduced:

- **The cascade shader had been measuring depth with the wrong sign since it landed.** It computed `-(view * pos).z`, which is right for GLM's projections and wrong for this engine's: `Camera::setPerspectiveProjection` sets `w = z`, so the camera looks down **+Z** and everything in front of it has positive view-space z. Every depth therefore came out negative, compared less than the first split, and sent every fragment to cascade 0 — silently unshadowing everything past it, since the lookup fell outside that map and the sampler's border compares as lit. It never showed because the demo fits inside the first cascade, and validation had nothing to say because nothing was illegal, only wrong. The cluster grid had to answer the same question, which is how it surfaced. A GLSL bug is not directly unit-testable here, so what is pinned is the convention: a test builds the engine's own camera and checks the cascade chosen by that measure really contains the point. `ClusterGrid` sidesteps the question entirely by scaling rays by `|z|`, and is tested under both conventions.
- **Updating a descriptor set after a command buffer has bound it invalidates the command buffer.** The culling pass initially borrowed the renderer's global set; the scene pass then wrote the shadow map into that same set, and the frame died — eleven thousand validation errors from one root cause. The cull pass owns its own set now. Two sets, each written and then bound, cannot collide.
- **The CI smoke test was checking nothing in Release.** It grepped for the `"validation layer:"` prefix the engine's own debug messenger prints, and the engine only installs that messenger in Debug. In Release the loader enables the layer through the environment and the layer prints in its own format, which that pattern does not match — so the eleven thousand errors above scored zero against it. The pattern now matches both formats.

The demo gained forty short-range accent lights, well past the old cap, because a demo with three lights demonstrates nothing about a change whose point is that light count stopped mattering — and because the headless CI run would otherwise exercise the culling path with a list short enough to fit anywhere. Neighbouring lights step around the hue circle, so a cluster assigned wrongly at a cell boundary would draw hard-edged blocks of colour across the floor rather than a smooth gradient.

*Not done at the time:* everything else in this phase — SSAO, screen-space reflections, volumetric fog, decals, TAA and motion vectors, depth pre-pass and HZB occlusion culling, GPU-driven rendering, skeletal animation, particles, terrain and global illumination. Clustered shading is one item of a long list, and the list is the work of a phase measured in months. The depth pre-pass, SSAO and occlusion culling have since landed; see below.

**Point-light cube shadows landed next**, on the same foundation and clearing the last of the shadow debt Phase 4 recorded. A point light casts in every direction at once, so its map is the whole sphere around it: six ninety-degree views sharing the light's position, rendered into six layers of one image and sampled by direction rather than by index. The frame graph's layered images — added for the cascades, and predicted there as what cube shadows would be built on — needed only to learn that an array of layers can be viewed as a cube. More than six layers makes it a cube array, so every casting light's cube lives in one image behind one binding and one barrier.

Shadows are per light rather than universal, because six depth passes each is a real cost. A light casts only if asked and only the first few asking get it; the demo's three composing lights cast and its forty accent lights do not, which would otherwise be two hundred and forty passes for shadows nobody would look at.

Two things worth recording:

- **A cube map is the one place the hardware has a strong and non-obvious opinion.** Which face a direction samples, and where on that face it lands, are fixed by the specification, and six views that each look the right way while disagreeing about which way is up still produce a picture — one where shadows are mirrored on two faces and correct on four, which is nearly impossible to debug by eye. So the tests transcribe the specification's own face and coordinate rules and check the matrices against them, rather than checking the matrices against themselves.
- **The depth compared against is along the face's axis, not the straight line to the light.** A perspective depth buffer stores the former. Comparing against the latter makes the corner of every face further away than the map says it is, so every face edge shadows itself. The shader recomputes the stored depth from a closed form rather than a matrix multiply, and a test pins that form against what the face matrices actually write.

Verified by turning the sun off and looking at what was left: shadows under the spheres, the box and the crates, with no seam where two faces meet and no acne.

Two shapes worth carrying forward:

- **Spot lights fall out of this almost for free.** They are a point light with a cone test; the cluster assignment and the per-cluster list are already there.
- **Deferred shading is now a smaller change than it was.** The cluster grid and the light buffer are the parts a deferred lighting pass would need, and the frame graph already carries the buffers between passes.

**The depth pre-pass, screen-space ambient occlusion and occlusion culling landed together**, because the first is what the other two are built on and shipping it alone would have meant claiming a saving nothing yet measured.

**Depth first.** Clustered forward shades every fragment that survives the depth test, and that shader samples four environment maps, walks the cascades and loops the fragment's whole cluster with a cube or projective shadow lookup per light. Whether a fragment survives depends on what was drawn before it, and the draw list is sorted by material, which has nothing to do with depth — so how many times a pixel was shaded was luck. Depth now goes down in a pass that writes nothing else, and the shading pass tests `EQUAL` with depth writes off.

`EQUAL` rather than `LESS_OR_EQUAL` on purpose: a fragment merely in front of the recorded depth is one the pre-pass did not see, which would mean the two passes disagree about the scene, and losing it visibly beats shading it twice quietly. That comparison only works if both passes compute `gl_Position` to the bit. The obvious thing to copy is the shadow pass, which premultiplies the matrices on the CPU and pushes one `mat4` — and which would land fractionally elsewhere and punch holes in everything. So the expression lives in one shared include along with the push constant block it reads, and both stages declare `invariant gl_Position`, which is what forbids a compiler from reassociating those multiplies differently in one of them.

Two holes in the frame graph had to be closed first, and both are of the same family: *the graph knew about producers and readers, and depth has neither.* Nothing samples what a pre-pass writes — the pass that consumes it consumes it by loading it and testing against it. So liveness culled the pre-pass as contributing nothing, and the store-op rule, decided by the same reasoning, discarded its result. Both now account for a later pass writing the same layer, which is exactly the case where the loaded content matters, and both are pinned by tests. The second hole was that only colour could be resolved; nothing single-sampled can read multisampled depth, which is what both of the items below need.

**Screen-space ambient occlusion** is what the depth is read for first. Image-based ambient hands every fragment the same irradiance whatever is standing next to it, so the inside of a corner is as bright as an open field and nothing reads as touching anything. Points are sampled in the hemisphere over each surface and tested against the recorded depth; the fraction blocked dims the ambient term. Only the ambient term — occlusion is a statement about how much of the environment a point can see, and a direct light either reaches the point or is stopped by a shadow map that already knows. Folding it into the whole sum instead is the usual mistake and lays a grey smear along contacts the sun is lighting perfectly well.

The kernel, the per-pixel rotations and the reconstruction from a depth sample back to a view-space point live in `render/Ssao`, device-free. The reconstruction is the part that needed pinning: published SSAO is written for OpenGL's conventions — a depth range of [-1, 1] and a camera looking down -Z — and this engine has neither, so the tests do not check the code against a formula copied from the same place the code was. They project points through the engine's own `Camera` and check the reconstruction lands back where they started. Verified by differencing a frame against the same frame without it: darkening at the base of every sphere, the joints of the stacked crates and the inside of the torus, and nowhere else.

**Occlusion culling** is what it is read for second. Frustum culling drops what is outside the camera and says nothing about what is inside it and behind a wall. A depth pyramid — each texel the farthest depth anywhere beneath it — answers that a whole object at a time, before the object is submitted.

The shape it took is not the textbook one, and the reason is worth recording. A GPU implementation builds the pyramid as mips, tests bounds in a compute shader, and consumes the verdict in the same frame through indirect draws. **This engine has no indirect draws** — it issues one `vkCmdDrawIndexed` per object from a loop in C++ — so a verdict that never left the GPU could not skip anything, and building one anyway would be infrastructure against nothing. So the pyramid is halved on the GPU until it is a few tens of thousands of floats, copied into a mapped buffer, and finished and tested on the CPU where the draws actually are. Each level is its own image rather than a mip: every level is written once, read once by the level above it and finished with, so separate images cost a few more passes and save the graph tracking a layout per mip. The last level is not a transient at all — it has to outlive the frame that fills it, so it is imported the way the swapchain image is and left in `TRANSFER_SRC_OPTIMAL` for the copy that follows.

The cost is latency: the buffer is read by the frame that reuses its index, which makes the reading a couple of frames old, so the camera it was captured through travels with it and the test is made against that camera rather than the current one. Everything else errs towards drawing — a coarse texel answers for more screen than was asked about, which can only push the reading further away; a box reaching behind the camera is not tested at all; a level whose size is odd takes in three children rather than two, because a parent that has not seen a texel would report a nearer maximum than the truth, and that is the one error that loses visible geometry.

The demo scene had nothing standing behind anything, which is why it gained one thing that does. Measured over the whole tour with the validation layers on: the pyramid culls it on 43 of 181 frames, and against the same tour with culling switched off exactly one frame of 121 differs — twenty-five pixels where the sphere had just begun to show past the sheet's edge and the reading was still a frame or two behind. That is the latency, measured rather than assumed.

*Still not done in this phase:* screen-space reflections, volumetric fog, decals, TAA and motion vectors, GPU-driven rendering, skeletal animation, particles, terrain and global illumination.

**Instancing landed after them**, spending a sort that had been sitting unspent since Phase 4. The draw list has been ordered by material and then by mesh ever since, so that binding a descriptor set did not alternate; nothing had ever merged the runs that ordering produces. Consecutive objects sharing both are now one instanced draw.

What made that possible is that the transform stopped travelling with the draw. A draw carrying its object's model matrix can only ever be one object, so the matrices moved into a buffer indexed by the instance - and `gl_InstanceIndex` counts from the draw's `firstInstance` rather than from zero, which is what lets a batch point at its own run of that buffer with nothing per draw saying where it starts. Two things fell out of it: the push constant block was 176 bytes, past the 128 every implementation is required to offer and portable only by inspection of the device limit, and what is left is 48 and read by the fragment stage alone; and the depth pre-pass now has no push constants at all.

The demo said nothing about any of this on its own - all eighteen of its objects have a material each, so all eighteen are batches of one whatever the renderer does - so it gained a band of gravel: a hundred and twenty stones, one mesh, one material, scattered from a fixed seed so the recorded frames still compare. Measured over the tour: 138 candidates, about 75 rejected by the frustum and a handful more by the depth pyramid, and the fifty-odd that survive go out in fourteen draw calls. That the exhibits themselves do not merge is the observation behind the material-instancing row in §10.2.

### Phase 10 — Completing the engine (ongoing)

> This bucket is now structured: §11 sorts it into the milestones that end at
> v1.0, and says which of these deliberately wait past it.

- **Audio**: 3D spatial audio, mixer, buses, occlusion.
- **UI system**: retained-mode canvas, layout, text via a font atlas, input routing, world-space and screen-space canvases.
- **Navigation**: navmesh generation (Recast/Detour), pathfinding agents, avoidance.
- **Player build pipeline**: `EnchantedPlayer` + cooked assets → a distributable per platform.
- **Networking** (optional): snapshot replication, client prediction, lag compensation.
- Localization, save systems, input rebinding UI, quality tiers, crash reporting.
- Documentation site, API reference, and a small finished sample game as the real proof the engine works.

---

## 7. Fate of the original files

> Phase 0 already performed the moves and renames in this table; the column
> records where each original file went and what still has to happen to it.

| File | Fate |
|---|---|
| `CMakeLists.txt` | Rewritten in Phase 0 — multi-target, FetchContent deps, shader step fixed |
| `src/core/Application.*` | → `core/Application.*`; `run()` decomposes into fixed-step update / ECS schedule / frame-graph render |
| `src/scene/GameObject.*` | **Deleted** in Phase 3, replaced by `scene/World` + `Transform` / `Hierarchy` |
| `src/render/SimpleRenderSystem.*` | **Deleted** in Phase 4, replaced by frame-graph passes with material sorting |
| `src/rhi/Device.*` | → `rhi/Device.*`; VMA, Vulkan 1.3 features and portability enumeration — **all done** |
| `src/rhi/SwapChain.*` | → `rhi/SwapChain.*`; render-pass and framebuffer machinery **removed by dynamic rendering**; depth became a frame graph transient |
| `src/rhi/Pipeline.*` | → `rhi/Pipeline.*`; config struct zero-init **done**, `Model::Vertex` coupling **dropped**; SPIRV-Reflect still to come |
| `src/rhi/Buffer.*` | → `rhi/Buffer.*`; VMA-backed; `getAlignmentSize()` fixed |
| `src/rhi/Descriptors.*` | → `rhi/Descriptors.*` (also fixing the singular/plural filename mismatch); bindless + per-frame allocator |
| `src/render/Model.*` | Split into `render/Mesh` (GPU) and `assets/MeshData` (CPU, scriptable) |
| `src/render/Camera.*` | → `render/Camera`; becomes a component; `near` / `far` params renamed |
| `src/platform/KeyboardMovementController.*` | **Deleted** in Phase 1, replaced by `platform/Input` + an editor fly-camera |
| `shaders/simple_shader.{vert,frag}` | **Deleted** — superseded by the PBR shader set in Phase 4, removed with the frame graph work |
| `external/tinyobjectloader/` | Kept as a secondary importer; cgltf becomes primary |

**Reusable as-is:** the fluent descriptor `Builder` pattern, the staging-buffer upload path in `ege_model.cpp`, `hashCombine` in `ege_utils.hpp`, the `beginFrame`/`endFrame` frame-lifecycle contract, and the swapchain-recreation-on-resize logic.

---

## 8. Verification

**Per commit (CI, Linux + Windows):**

- `cmake --preset default && cmake --build build`, warnings-as-errors.
- `ctest` — doctest suites for math, reflection round-trips, ECS (spawn/despawn/generation reuse, view intersection, hierarchy invariants), serialization round-trips, asset GUID stability, physics determinism.
- `clang-format --dry-run --Werror`.
- Headless render smoke test under lavapipe with `VK_LAYER_KHRONOS_validation` enabled and validation errors treated as failures — this catches the class of Vulkan bug that silently works on one driver.

**Per phase, manually:**

| Phase | Check |
|---|---|
| 0 | Clean checkout builds and runs on Linux and Windows with no local path configuration |
| 2 | Textured quad through the frame graph, validation-clean, VMA-allocated |
| 3 | Load a `.egescene`, edit in the debug overlay, save, reload — identical result |
| 4 | Render Sponza / Damaged Helmet and compare against glTF sample-viewer references |
| 5 | Build a scene from scratch in the editor, save, quit, reopen — everything preserved; Play/Stop restores exact pre-play state |
| 6 | Move a referenced asset on disk, rescan, and the scene still finds it |
| 7 | Edit a script during a play session; behaviour updates and entity state survives |
| 8 | Character controller walks, jumps, pushes dynamic bodies, fires triggers; the same fixed-step sim run twice is identical |
| 9+ | RenderDoc frame captures; frame-time budgets tracked in the profiler panel |

~~**Golden-image regression testing** from Phase 4 onward: render fixed scenes at fixed camera positions in CI and diff against committed references with a perceptual threshold.~~ **Replaced by something narrower and more robust.** Committed reference images are a poor fit for a CI that renders on a software rasteriser: the comparison strict enough to catch a shadow landing in the wrong place is also strict enough to fail on an LLVM upgrade, and a threshold loose enough to survive one catches almost nothing.

What landed instead is a **frame sanity check**: the headless run records its own frames and a tool asks whether a picture came out — is there an image, is it more than one flat colour, is it neither crushed to black nor blown out, does it have more than a handful of colours. That is deliberately the first half-second of a person looking, not a full inspection. It catches the catastrophic class — a dead pass, a lost descriptor, a shader that never bound — which is exactly the class the previous smoke test could not see, since the engine starting, rendering, not crashing and upsetting no validation layer are all true of a black screen. Subtle correctness stays where it has been all along: in the device-free unit tests, which is why the shadow, cluster and cone maths are all written to be testable without a GPU.

Proven by breaking it on purpose — emptying the scene pass produced a run that was validation-clean and exited zero, and only the new check failed it.

---

## 9. Sequencing notes

- Phases 0–3 are strictly ordered; each is a hard prerequisite for the next. Reflection (Phase 1) in particular gates the editor, serialization *and* scripting, so it should not be deferred or done half-way.
- Phases 4 (PBR) and 5 (editor) can interleave once Phase 3 lands. The editor is more useful the earlier it exists, so an early-but-thin editor beats a late complete one.
- Phase 8 (physics) only depends on Phases 1–3, so it can move earlier if gameplay matters more than visual fidelity.
- Realistic total for Phases 0–8 solo part-time: **12–18 months**. Phases 9–10 are open-ended.
- The sandbox project should be kept working at every step — it is the honest test of whether the engine is actually usable.
- **Anything device-free should be written device-free.** Every renderer feature that has landed since the frame graph split the same way: the arithmetic in its own file with no Vulkan in it and a test suite that runs on a machine with no GPU, and a thin layer that hands the result to the device. Cascade fitting, cluster geometry, cube faces, cone falloff — each of those files is where the bugs actually were, and each was caught on a laptop rather than in a frame.
- **The demo has to demonstrate the thing.** Twice now a feature landed and the pictures did not change: the cascades, because the demo fitted inside the box they replaced, and clustered shading, because three lights prove nothing about a light cap. The fix both times was to change the scene, not the wording.

---

## 10. The plan from here

> Phases 0–8 were written before any of this existed and describe a route. This
> section records how the near-term plan played out, increment by increment;
> the live plan now runs to v1.0 and is §11, which is the part to read first
> when picking up the next piece of work.

### 10.1 What the engine is now

A Vulkan 1.3 clustered-forward renderer with a frame graph that owns layered,
cube and multisampled images — colour and depth, with their resolves — and
transient buffers; PBR with image-based lighting; three kinds of shadow
(cascaded sun, cube point, projective spot); 4× MSAA; a depth pre-pass with
screen-space ambient occlusion and a depth pyramid built on it; bloom and an
ACES tonemap. Occlusion culling is GPU-driven and two-phase: draw last
frame's visible set, build the pyramid from that partial depth, rule on
everything against it in compute, and draw what is newly visible - same
frame, no readback, no pop-in - with each batch one indirect draw whose
instance count the culling dispatch wrote. Underneath: a sparse-set ECS, runtime reflection
driving serialization and the editor, a GUID asset database that loads on the
job system, C++ scripting, and Jolt physics drawn between its fixed steps.
The engine is a shared library and behaviours live in a module beside it, so
gameplay code can be rebuilt and reloaded without the engine restarting -
which is what the editor demo shows happening. Around it: 313 tests, eight CI
jobs across Linux, Windows and macOS, and a headless render that checks its
own output.

### 10.2 Next, in order

Each of these is a single increment — one branch, one pull request.

| # | Item | Why now | Blocked by |
|---|---|---|---|
| ~~1~~ | ~~**Depth pre-pass**~~ | **Landed.** Depth goes down first and the scene pass tests `EQUAL` with writes off, so the clustered fragment shader runs once per visible pixel. | — |
| ~~2~~ | ~~**SSAO**~~ | **Landed.** The hemisphere over each surface is sampled against the pre-pass depth and the image-based ambient is dimmed by what it finds. | — |
| ~~3~~ | ~~**HZB occlusion culling**~~ | **Landed**, though not in the shape the row assumed: with no indirect draws to consume a GPU verdict, the pyramid is finished and tested on the CPU where the draws are issued, at the cost of a couple of frames of latency. | — |
| ~~1~~ | ~~**GPU instancing**~~ | **Landed.** Consecutive objects sharing a mesh and a material are one instanced draw, with their transforms in a buffer indexed by the instance. | — |
| ~~3~~ | ~~**Per-thread command pools**~~ | **Landed**, along with a lock on the graphics queue and a fence per upload instead of waiting on the whole queue. | — |
| ~~4~~ | ~~**Async asset loading**~~ | **Landed.** glTF files are parsed across the workers at startup and a hot reload rebuilds off the main thread. | — |
| ~~5~~ | ~~**Render-transform interpolation**~~ | **Landed**, as an opt-in component rather than a physics special case: nothing is interpolated unless whatever moves it on the fixed clock says so. | — |
| ~~6~~ | ~~**Engine as a shared library**~~ | **Landed**, with type registration made idempotent because a template's function-local static is per module and both modules register. | — |
| ~~1~~ | ~~**Script hot reload**~~ | **Landed.** Behaviours come from a module the engine `dlopen`s, the watcher rebuilds every instance from the new registry, and the reflected fields cross over. | — |
| ~~-~~ | ~~**macOS builds**~~ | **Landed**, and smaller than expected: the runtime already asked for portability enumeration and enabled `VK_KHR_portability_subset`. What was missing was the build - a module suffix CMake gets wrong on Apple, a relative rpath, and a CI job. | — |
| ~~-~~ | ~~**Frame graph: compute reads, imported buffers, surviving imports**~~ | **Landed.** `computeSampled`, `computeStorageRead` and `indirectRead` accesses, `importBuffer`, and imports that state the layout they arrive in — which is how an import says its contents matter and must be loaded rather than cleared. Tested at the compile level; the caller is row 1. | — |
| ~~1~~ | ~~**GPU-driven two-phase occlusion culling**~~ | **Landed**, in the shape §10.6 predicted: no merged geometry buffer, each batch its own indirect draw, only the instance count moved onto the GPU. The readback, the CPU pyramid and the two frames of latency went with it, and the frames came out pixel-identical to the old path's. | — |

Every row that was still live here has moved into §11, which runs the same
work forward to a v1.0 and is now the plan of record. The struck rows above
stay because they are the honest history of how this section emptied.

### 10.3 Deliberately not next

- **Meshlets and a GPU-side draw stream** — the far end of GPU-driven
  rendering. Row 2 above is the part of it that pays for itself; these are the
  part that pays once the scene is large enough to need them, and this one is
  not.
- **Bindless descriptors and SPIRV-Reflect layouts** — neither blocks anything.
  Bindless earns its cost when material count outgrows per-material sets, and
  the worst of the coupling SPIRV-Reflect would remove is already gone.
- **Cooked asset packaging, KTX2, BC compression** — all exist to feed
  `EnchantedPlayer`, which is Phase 10. Building a packer before the thing that
  unpacks it is how formats get designed against nothing.
- **The standalone editor executable** — the panels are in-process on purpose
  and moving them is a build-system change, not a rewrite. It buys nothing
  until there is a project to open that is not the demo.
- **Terrain, GI, particles, decals, volumetrics** — Phase 9 lists them and they
  are real, but each is a phase-sized piece of work next to the ten above.

### 10.4 Standing debts, with their reasons

These are not scheduled because nothing needs them yet. Each is recorded so it
is a decision rather than an oversight.

| Debt | Left because |
|---|---|
| Tangents from glTF | The shader derives a tangent frame from screen-space derivatives, which breaks only on mirrored UVs — and no asset in the project has any. |
| Physics constraints, shapecast, overlap, named collision layers | API surface with no caller. The backend supports all of them whenever something needs a door or a trigger volume that ignores the player. |
| `CharacterController`, `ConvexHullCollider`, `MeshCollider` | The first belongs to the sandbox project, which Phase 10 owns; the other two want cooked hull and triangle data from a pipeline that does not exist yet. |
| Collider gizmos and physics debug draw | Worth doing when colliders are authored in the editor rather than in code. |
| Editor multi-select and enum drawers | Neither is load-bearing; both are an afternoon whenever they start to grate. |
| `EventBus`, `Handle<T>`, `VirtualFileSystem` | Deferred in Phase 1 for want of a caller, and still without one. `EGE_ASSET_ROOT` and the asset database cover what the VFS was for. |

### 10.5 What the last five changed about the plan

Recorded because the plan was wrong about two of them, and a plan that is
never wrong about anything is a plan nobody checked.

**Indirect draws would not have removed the pop-in, and the row said they
would.** The reasoning went: the occlusion verdict is computed this frame, so
draw indirectly and apply it this frame. But the verdict comes from a pyramid
built out of *this frame's depth pre-pass*, so the only passes it can reach
are the ones after that — which is the shading pass. And the shading pass
already skips hidden objects: it tests `EQUAL` against the pre-pass depth, so
every fragment of something hidden fails before the fragment shader runs. The
saving would have been the vertex work alone, and the object would still have
popped in two frames late.

What actually removes it is two phases. Draw what was visible last frame,
build the pyramid from that partial depth, test everything against it, and
draw whatever turns out to be newly visible. Nothing is ever missing, because
anything wrongly skipped in the first phase is caught in the second against
real depth, and the verdict is this frame's. Indirect draws are how the second
phase is issued, which is the honest version of "the thing occlusion culling
wanted" — they are a means, not the feature.

**Instancing showed the demo's own limit.** A hundred and twenty gravel stones
sharing one material collapse into one draw; the eighteen exhibits do not,
because each was given a material of its own to show off a different corner of
the shading model. That is not a flaw in the demo — it is what the demo is for
— but it is why "material instancing" is now a row: parameters in a buffer
indexed like the transforms are what let two objects that differ only by tint
draw together.

**Two things were cheaper than expected and one was not.** Per-thread command
pools took a lock on the queue and a fence per upload with them, and that was
all. Interpolation turned out to be a component rather than a system, because
the question "what does the fixed step move?" has no general answer and the
thing that moves something is the only thing that knows. The shared library
cost an idempotent type registry: registration lives in a function-local
static inside a template, a template instantiated in two modules gets one
each, and both register — which was harmless while there was one module and is
exactly the bug the shared library exists to avoid.

### 10.6 What GPU-driven culling actually needs, now that it is next

Written down because working out what the frame graph had to learn also
settled a question that had been left vague, and settled it in the cheaper
direction.

**It does not need a merged geometry buffer.** The obvious reading of
"indirect draws" is one `vkCmdDrawIndexedIndirect` covering the whole scene,
which needs every mesh in one vertex buffer and one index buffer — a large
change, and one that would have to come first. That is not what this needs.
Each batch already binds its own mesh and material and issues its own draw;
what changes is only where the **instance count** comes from. Seed one
`VkDrawIndexedIndirectCommand` per batch — `indexCount`, `firstIndex` and
`vertexOffset` are all known on the CPU, which is already deciding the
batches — and let the culling compute pass write `instanceCount` and
`firstInstance`. The draw call count is unchanged; the verdict is this
frame's.

So the increment is: a compacted instance buffer and a command buffer, both
transients; a compute pass sampling the depth pyramid and reading the object
bounds, writing both; and `vkCmdDrawIndexedIndirect` per batch instead of
`vkCmdDrawIndexed`. The two-frame latency goes, the readback image and its
mapped buffer go, and `OcclusionSystem` loses about half of itself.

**A merged geometry buffer is still worth having later**, for collapsing the
per-batch binds into one multi-draw — but that is a throughput win on a scene
with far more batches than this one has, and it is not what removes the
pop-in. Recorded here so the next person to read "indirect draws" does not
conclude, as this plan once did, that the large change has to come first.

**Outcome.** It landed in exactly this shape, and two details earned their
keep. The late pass's instance windows sit a whole buffer past the early
ones, seeded as constants on the CPU — so neither dispatch ever needs the
other's count, and no fix-up pass exists between them. And the visibility
history is keyed by draw-list position with no stability guarantee at all,
because it needs none: a wrong guess draws a real object's depth early or
defers one to the late pass, which rules on everything against real depth
either way. The one surprise was old rather than new — two depth passes
sharing one descriptor set, where the second write invalidated what the
first had bound, which is the same hazard that shaped the pre-pass's set in
the first place.

### 10.7 How to pick up any of these

The shape every increment in this project has taken, and the one to keep:

1. Find the arithmetic and put it in its own file with no Vulkan in it.
2. Write the tests against an independent definition — the specification, the
   engine's own camera, a formula derived by hand — rather than against the
   code being tested. Three of the bugs found this way were sign errors that
   produced a plausible picture.
3. Wire it into the frame graph, which owns barriers, layouts and attachments;
   a feature that needs new synchronisation usually needs a new graph
   capability instead, and that capability is testable without a GPU too.
4. Change the demo so the feature is visible, then look at the result.
5. Record what was learned here, including the parts that went wrong.

---

## 11. The road to v1.0

> Written 2026-08-25, after GPU-driven culling closed §10.2's last renderer
> row, by standing back and asking the only question a version number should
> answer: what would have to be true for this engine to be *completely
> usable*? Everything below is ordered by that question rather than by what
> would be most interesting to build - which is a change, and a deliberate
> one. The renderer got ahead of the engine; v1.0 is the engine catching up.

### 11.1 What v1.0 means

One person, starting from an installed engine and an empty project, builds a
small third-person game - a character walking, running and jumping through a
lit, physical level, pushing things, triggering things, hearing things, with
a menu and a HUD - and hands the build to someone who owns no compiler, on
Linux, Windows or macOS. Every feature used along the way was reached
through the editor or the public scripting API. Never once did making the
game mean editing the engine.

That is the whole definition. Anything that test does not touch is not in
v1.0, however interesting it is.

### 11.2 An honest audit against that test

*Written when §11 was, and deliberately left as it was written: it is the
starting position the milestones below are measured from, and an audit
quietly edited to keep pace with the work is an audit that can never be
wrong about anything. What has changed since is struck through in §11.3's
tables, which is where progress lives.*

**Carries its weight already.** The renderer: clustered forward PBR with
image-based lighting, three kinds of shadow, MSAA, SSAO, bloom, ACES, a
depth pre-pass, and GPU-driven two-phase occlusion culling over instanced
indirect draws - all derived through a frame graph, all validation-clean,
most of it arithmetic-tested without a GPU. The simulation: Jolt behind an
engine-owned interface, fixed-step with render interpolation, deterministic,
contacts and raycasts reachable from gameplay code. The tooling core: docked
editor with a reflection-driven inspector, gizmos, play/pause/step/stop,
undo, an asset database with stable ids, and hot reload of both assets and
C++ behaviour code. The ground under it: 313 tests, eight CI jobs across
three platforms, a headless render that checks its own pixels.

**Part-built - a foundation with no house on it.** Physics has bodies but no
character controller, no triggers, no collision layers a designer can name.
Scripting has behaviours but no prefabs, no timers, no way for one system to
hear about another except polling. Input has actions but only a keyboard and
mouse to bind them to. glTF import handles meshes, materials and textures
but not skins or animations. The sandbox proves the module mechanism, but a
"project" is still a folder inside this repository, and the editor still
lives inside the engine's own demo executable. Windows and macOS build and
pass every test in CI - and no one has yet watched either draw a frame.

**Absent entirely.** Skeletal animation. Audio - there is not a line of it.
Runtime UI: the engine can render a scene but cannot draw the word "Paused"
over it, because ImGui belongs to the editor and nothing else can put text
on screen. Cooked assets and the player binary that would load them - a game
currently ships as the editor plus a source checkout. A manual.

The pattern in that audit is worth stating plainly: **the missing pieces are
not renderer features.** They are the ordinary machinery of making a game -
which is exactly what "the renderer got ahead of the engine" means, and what
the milestones below correct.

### 11.3 The milestones

Each milestone is a version tag and a demo moment - the thing you can see
working that you could not see before. Each decomposes into single-increment
pull requests in the §10.7 shape, and the rows inside a milestone can
reorder freely; the milestones themselves mostly cannot, because each stands
on the one before it.

#### v0.5 — The character — **landed**

The largest gap first. A game is usually somebody moving through a world,
and this engine could not animate a somebody.

It can now: a rigged humanoid walks, runs and jumps through the demo scene,
skinned on the GPU through both depth phases and the GPU-driven indirect
draws, driven through the same four intent fields whether a player's hands, a
patrol behaviour or a gamepad is writing them, with a third-person camera
behind it. Every row below is struck. What the milestone found missing along
the way is recorded in the rows themselves rather than quietly fixed.

| Item | What it is | Done when |
|---|---|---|
| ~~glTF skins and clips~~ | **Landed.** Rigs arrive with joints reordered parent-before-child - the invariant the file never promises and every sampling sweep relies on - with inverse binds following their joints through the reorder, vertex indices remapped to match, and weights renormalised. The sampling arithmetic (keyframes, slerp, blending, the palette) landed with it, device-free. CUBICSPLINE channels are skipped aloud rather than half-played. | Done: a hand-built rigged glTF - joints deliberately child-first - round-trips numerically, and a sampled pose lands where paper says. |
| ~~GPU skinning~~ | **Landed.** The palette rides binding 12, the skinned pipelines share the rigid layouts - one extra push range and one extra binding the rigid shaders never name, so a batch walk switches pipelines without disturbing a bound set - and skinned batches never merge, because a batch is one entity's palette run. Skinned draws go through GPU culling with their rest sphere grown by half, since animation moves vertices past the box they were modelled in. The demo gained a rigged, swaying banner, generated like the torus. | Done: the banner deforms through both depth phases and the EQUAL test under indirect draws, validation-clean, and the palette a test computes by hand is the palette the system writes. |
| ~~Clip playback and blending~~ | **Landed.** `SkeletalAnimator::play()` crossfades: two clips sampled at their own times and blended pose by pose, because halfway between two poses is a pose and halfway between two matrices is shear. A fade can restart the new clip - what a jump wants - or carry the phase across, which is what a walk becoming a run wants, since restarting a stride mid-step is a stumble. The state machine is the `CharacterAnimation` behaviour, and it turned out to be four rules rather than a graph: airborne is a jump, still is an idle, moving is a walk, moving fast is a run, each played at the rate the ground demands. | Done: reflected, saved, and driven from a behaviour that reads nothing but the controller's `grounded` and `planarSpeed`. |
| ~~Character controller~~ | **Landed.** Jolt's virtual character behind the same engine-owned interface bodies use, driven by the engine's own motion arithmetic - accelerate, brake, air control, coyote time, a jump authored as a height and cut short when released - which is device-free and tested against hand derivations. The component splits into shape and tuning (authored, saved), intent (written every tick by whoever is driving), and state (written back for gameplay to read), so a player, a patrol behaviour and an AI drive it through identical fields. `World::input()` arrived with it, the way `World::physics()` did. | Done: the demo has somebody walking a circuit on physics geometry, turning to face where it is going, jumping at the corners and shoving a crate out of its way. The §10.4 debt row closes. |
| ~~Gamepad input~~ | **Landed.** `Input` polls four pads through GLFW's *gamepad* mapping - the one its controller database derives, so `A` is the bottom face button on any recognised pad rather than whichever the firmware numbered first - and exposes buttons, raw axes and deadzoned sticks. Actions bind gamepad buttons, and bind axes with a sign and a threshold, which is how one stick axis becomes two opposed actions and how a trigger resting at −1 becomes an action resting at zero. `axis()` became the difference of two *analog* action values, so a stick asks for what it was actually pushed to while a key still asks for exactly one. | Done: the free-fly camera and the character both play from either hand position, and neither of them learned that a controller exists. |
| ~~Third-person camera~~ | **Landed.** Follows at a distance behind the player's own look yaw, smoothed with an exponential damp that closes the same fraction of the gap per second however many frames that second took - the naive lerp is frame-rate dependent, and a camera tuned on one machine lags on another. It casts from what it is aiming at out to where it wants to be and stops short of whatever is in the way. **What it found missing:** gameplay cannot own the camera. The viewer is a plain `Transform` the application drives, so the follow camera is an engine class rather than the behaviour it should be - the camera becomes a component when the player binary needs one, in v0.9. | **Done: `--follow` puts the camera behind the character and `--play` hands it to you; the recording in the README is the same run with the patrol driving.** |

#### v0.6 — The game surface

What gameplay code reaches for in the first hour of making anything, found
missing by whoever builds the v0.5 demo and fixed while the finding is warm.

| Item | What it is | Done when |
|---|---|---|
| Triggers and collision layers | A `Trigger` volume that reports enter/leave to behaviours; named layers with a designer-readable collision matrix. Both exist in Jolt; the engine just never asked. | A pressure plate opens a door in the sandbox. |
| Prefabs | A scene fragment saved as an asset and instantiated by the editor or a behaviour, ids remapped on the way in. The serializer already knows how to write a world; this is teaching it to write part of one. | Spawning a pickup is one call naming one asset. |
| Timers and events | `after(seconds, fn)` on behaviours, and a typed event a behaviour can raise and another subscribe to - the two things deferred since Phase 7 "for want of a caller", which the v0.5 demo finally is. | The sandbox game's win condition uses both. |
| Script reload keeping play state | The `onReload` hook §10.2's last row described: hand the old instance to the new one so unreflected state can cross when the author wants it to. | A behaviour mid-flight survives a reload on purpose. |
| **The level** | A small, complete, winnable level in the sandbox: goal, obstacles, pickups, a fail state. Not a showcase - a game. | **The milestone demo: someone plays it to the end and nothing along the way needed engine code.** |

#### v0.7 — Sound

The first whole subsystem from nothing, and the one whose absence a player
notices before any rendering feature's.

| Item | What it is | Done when |
|---|---|---|
| The backend | miniaudio, pinned through CPM like every dependency, behind an engine-owned interface the way Jolt is - one translation unit knows the library's name. A null backend for CI, because the headless render must keep checking itself on machines with no sound device. | The suite and the headless run pass unchanged on a silent machine. |
| Audio assets | Sound files in the asset database with ids and sidecars like everything else, loaded on the job system, hot-reloadable. | Dropping a file in the browser makes it playable. |
| Sources and listening | `AudioSource` component - clip, volume, loop, 3D or not - and a listener that follows the active camera. Distance attenuation and panning; buses (master/music/effects) with volumes a behaviour can set. | Footsteps follow the character and fade with distance; pausing ducks the music. |
| Behaviour API | Play, stop, one-shots at a position, bus control - the small surface the sandbox game actually needs, grown from its calls. | **The milestone demo: the v0.6 level with its eyes closed still tells you where things are.** |

#### v0.8 — The screen the player reads

Runtime UI: what the game draws over the scene. Deliberately not the editor's
ImGui, which stays an editor implementation detail.

| Item | What it is | Done when |
|---|---|---|
| Text | A font atlas baked from a TTF at load, glyph quads through the existing 2D-over-scene path the tonemap pass already proves out. The arithmetic - layout, kerning, wrapping - is device-free and tested like all the other arithmetic. | "Paused" can be drawn over the world in any size. |
| Screen-space canvas | Anchored rectangles, images and text in resolution-independent coordinates; draw order; show/hide from behaviours. | A HUD survives a window resize honestly. |
| Input routing | UI first, game second, with focus - a menu that eats the click the game would otherwise get. | The pause menu works with mouse, keyboard and pad. |
| World-space labels | The same text, billboarded at a transform, depth-tested or not by choice. | **The milestone demo: menu, HUD, pause screen and a floating damage number in the sandbox game.** |

#### v0.9 — The project and the player

The engine stops being this repository. Everything before this milestone made
the game possible; this one makes it *ownable* and *shippable*.

| Item | What it is | Done when |
|---|---|---|
| Projects | A project is a folder: its assets, scenes, settings, and its script module's sources. Create and open from the editor; the editor drives the module build (a CMake invocation, which the reload path already proves works under a running engine). | The sandbox game moves out of this repository and nothing notices. |
| The standalone editor | `EnchantedEditor` as its own executable on the shared engine - the build-system change §10.2 always said it was, done last so it moves panels that are finished. | The demo executable stops pretending to be an editor. |
| Cooked assets | One pack file per project - table of contents, offsets, the same bytes the loose files held. Compression and GPU texture formats only if they stay cheap; the pack format exists to feed the player, not to be clever. | The player never opens a loose file. |
| `EnchantedPlayer` | The runtime without the editor: opens a pack, loads a scene, plays. The static-build option finally earns its keep - one self-contained binary, no module loading, behaviours linked in. | Runs the cooked sandbox game on all three platforms. |
| The build button | Editor action or one script: cook + compile + assemble a runnable folder per platform. | **The milestone demo: a zip a stranger unpacks and plays, no compiler anywhere.** |

#### v1.0 — Proof and polish

Not a feature milestone. The proof that the previous five were real, and the
debts that must not ship.

| Item | What it is | Done when |
|---|---|---|
| The sample game | The sandbox game finished to small-but-complete: a few minutes of play, sound, menus, a build for each platform, its cooked zip published with the release. The engine's real acceptance test, run last. | A stranger plays it without being told anything. |
| Real-GPU verification | The demo and the sample game run and watched on actual hardware on all three platforms - CI proves the code builds and the logic holds; it has never proved a Windows driver agrees. What breaks gets fixed here. | A recorded run from each platform, kept with the release. |
| Renderer rows on merit | Velocity buffer + TAA, and material instancing, from old §10.2 - both are polish the sample game will visibly want. SSR only if the game's scenes reward it; a v1 does not ship features its own game cannot show. | The sample game looks better and draws fewer batches, or the row is struck with a reason. |
| Performance pass | Profile the sample game, not the demo, on the weakest real machine available; fix what is actually slow. First honest numbers for the README. | Frame time budgets written down and met. |
| The manual | Getting started, the scene and scripting API, the project workflow, shipping - written against a fresh install by following it. The API reference stays generated headers plus the commented source, which this codebase treats as documentation already. | Someone follows it from install to running build without asking a question. |
| The stability pass | The module ABI check §7 promised, versioned at last; deprecations resolved; the public headers read once, end to end, as the API they are about to promise to be. | Tag `v1.0.0`. |

### 11.4 Deliberately not in v1.0

Recorded so each is a decision, not an oversight - and so v1.1 has a shelf
to pick from.

- **Networking** - the largest possible subsystem, and the sample game is
  single-player. Nothing in v1.0 closes any door it needs.
- **Navigation** (navmesh, agents) - the sample game can be designed not to
  need pathfinding; a game does not become shippable through Recast.
- **Terrain, GI, particles, decals, volumetrics** - phase-sized renderer
  work, none of it on the v1 test's path. Particles are the closest call
  and the first candidate after.
- **Meshlets, bindless, a GPU draw stream** - §10.3's reasoning stands: they
  pay at a scene size the sample game will not reach.
- **Scripting languages other than C++** - hot-reloaded C++ is this engine's
  answer; a second language is a second engine's worth of API surface.
- **Mobile and console** - three desktop platforms is the honest claim.
- **Localization, save-system framework, crash reporting, input rebinding
  UI** - Phase 10 leftovers a real game wants and the sample game can carry
  in miniature (one save slot, one language) without framework versions.

### 11.5 How versions work from here

Milestones tag when their demo moment exists: `v0.5.0` through `v1.0.0`.
Rows inside a milestone land as the same single-increment pull requests as
ever - one branch, one PR, §10.7's shape, dates in the log telling the
truth about the order things happened. A milestone may ship incomplete rows
to the next tag only by striking them with a written reason, the way §10.2's
rows were struck: a plan that never admits a miss is a plan nobody checked.

---
