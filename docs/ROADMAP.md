# Enchanted Graphics Engine — Development Roadmap

> Living document. Describes how the project grows from a Vulkan renderer into a complete game engine.
>
> **Status: Phases 0, 1 and 3 complete. Phase 2 essentially complete. Phases 4, 5 and 6 substantially complete. Phase 7 next.**
>
> Each phase below carries an outcome note listing exactly what landed and what did not. The **frame graph** — for several updates the single most valuable outstanding item — is done, along with Vulkan 1.3, dynamic rendering, the **complete HDR pipeline** (float target → bloom → ACES), **image-based lighting** from a procedurally generated sky, a **sun with PCF-filtered shadows** rendered through the graph, and **glTF 2.0 import**. The **editor** now has an offscreen viewport with docked panels, hierarchy editing, a reflection-driven inspector, transform gizmos, a console, an asset browser, **Play/Pause/Step/Stop** and **undo/redo**. The **asset database** underneath it gives every asset a stable id in a `.egameta` sidecar, which is what finally lets a scene save what it draws — and what unblocked play mode and undo, both of which are world snapshots. A `--demo` camera tour and self-recorded frames make all of it demonstrable. Next: **Phase 7 scripting**, which is what the placeholder `Spin` component is standing in for.

## 1. Where the project stands today

Enchanted is a **Vulkan 1.3 forward renderer with a metallic-roughness PBR pipeline, image-based lighting and a frame graph**, an entity-component system, runtime reflection, VMA-backed GPU memory, textures with mip generation, a job system and a fixed-timestep clock. The scene renders linear HDR into a graph-managed float target and is tonemapped to the swapchain with the ACES fit; ambient light comes from a procedurally generated sky via irradiance and prefiltered-specular convolutions computed on the GPU at startup. It began as a port of Brendan Galea's Vulkan tutorial series and has since grown past it.

Still absent, and tracked per phase in §6: anti-aliasing, cascaded and point-light shadows, cooked asset packaging, hot reload, scripting and physics.

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
| `ege::ShadowMapSystem` | Depth-only sun shadow pass; the map itself is a frame graph transient |
| `ege::BloomSystem` | Half-resolution bright-pass and separable blur, composited before the tonemap |
| `ege::PostProcessSystem` | Fullscreen ACES tonemap from the HDR scene target to the backbuffer |
| `ege::EditorOverlay` | In-process editor: viewport, hierarchy, inspector, gizmos, assets, console, stats |
| `ege::EditorViewport` | The offscreen image the scene renders into and the UI samples |
| `ege::PlayMode` | Snapshots the world on Play, restores it on Stop |
| `ege::UndoStack` | Whole-scene mementos, bounded, with labels |
| `ege::AssetDatabase` | Stable ids to assets; `.egameta` sidecars, cataloguing, loading |
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
  core/         Application; + Log, Time, JobSystem, VFS, Profiler       [exists]
  reflect/      TypeRegistry, FieldInfo, EGE_REFLECT macros              [Phase 1]
  platform/     Window, KeyboardMovementController; + Input, FileWatcher [exists]
  rhi/          Device, SwapChain, Pipeline, Buffer, Descriptors;
                + Texture, FrameGraph                                    [exists]
  render/       Renderer, Model, Camera, SimpleRenderSystem;
                + Material, Lights, Shadows, IBL, PostFX                 [exists]
  scene/        GameObject; replaced by World, Entity, ComponentPool     [exists]
  assets/       AssetDatabase, importers, loaders                        [Phase 6]
  physics/      PhysicsWorld + Jolt backend, colliders                   [Phase 8]
  script/       Behavior, ScriptModule, hot-reload                       [Phase 7]
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

*Done:* the shading model. Cook-Torrance with Trowbridge-Reitz GGX, Smith/Schlick-GGX geometry, Schlick Fresnel, and correct energy conservation between the diffuse and specular lobes with metals carrying no diffuse term. `render/Material` holds metallic-roughness properties plus four texture slots behind a per-material descriptor set, ordered after the per-frame set by update frequency. Normal mapping derives its tangent frame from screen-space derivatives. Up to sixteen point lights, replacing the single hardcoded one. Backface culling.

**Frustum culling and material sorting** landed too. Meshes carry local-space bounds; the render system gathers visible objects, rejects those whose transformed bounds fall outside the frustum, sorts survivors by material then mesh, and submits. Sorting matters because descriptor set binds dominate a draw and component-pool order has no reason to group objects sharing a material. Per-frame statistics record candidates, culled, drawn and material binds.

**The HDR pipeline is complete as Phase 4 wrote it**: the scene renders linear radiance into an `R16G16B16A16_SFLOAT` transient, **bloom** extracts and blurs what exceeds 1.0 through a half-resolution bright-pass and a separable Gaussian (three `addPass` calls, two small shaders), and a fullscreen pass composites it in linear light and applies the ACES fit into the sRGB backbuffer.

Separating shading from display transform exposed a bug that had been shipping since the PBR shader landed: it applied a manual `pow(1/2.2)` gamma encode *and* wrote to an sRGB swapchain image, whose hardware encode applied the curve a second time. Every frame had been double-encoded — visibly washed out — and it read as "lighting needs tuning" rather than "encode applied twice", which is exactly why the display transform should exist in one place. The tonemap pass writes linear values and the sRGB format performs the only encode; the surface chooser now warns if it cannot get an sRGB format, because correctness depends on it.

**IBL landed**, from a procedurally generated environment. A sky with a sun is rendered into a mipmapped cubemap at startup, cosine-convolved into a 16-pixel irradiance map, GGX-prefiltered into a specular mip chain (one roughness per level), and paired with the split-sum BRDF LUT; the PBR ambient term is now the real split-sum evaluation and the sky draws behind the scene at the far plane. The whole precompute is GPU fullscreen passes and finishes in about a second even on CI's CPU rasterizer. The environment is procedural for the same reason the meshes are — a clean checkout ships no binary assets — and **HDR equirect import** joins the Phase 6 asset work, where environment maps become assets like any other. The demo's payoff is the one this document promised: the near-mirror sphere now reflects a sky instead of rendering nearly black.

**Directional sun shadows landed** as the frame graph's first depth-only pass: a `DirectionalLight` component, a fixed-size D32 transient rendered from the sun's orthographic view, and a 3×3 PCF test through a comparison sampler whose border compares as lit. The pass declares a depth write, the scene pass declares a sampled read, and every barrier between them is derived — the first feature to land as exactly the `addPass` call the graph was built to make cheap. Acne is handled by polygon-offset bias at raster time, not by fudging the lighting shader. Known simplification, promoted from groundwork to roadmap item: the sun frustum is a fixed box sized to the demo floor; cascaded shadow maps fitted to the view frustum replace it when scenes outgrow one box.

**glTF 2.0 import landed**, split deliberately into a CPU parse and a GPU instantiate. `assets/GltfLoader` (cgltf) reads meshes, metallic-roughness materials with their textures, and the node hierarchy — each local TRS decomposed into the engine's YXZ Euler convention, verified by a decompose-recompose test — and the instantiate half builds `Texture`/`Material`/`Model` objects and spawns entities under a root that carries the +Y-up→-Y-up correction as a half-turn roll, preserving winding. Any `.gltf`/`.glb` in `assets/models/` imports at startup; the demo gains a copper torus that is itself a self-contained text glTF, so a clean checkout still ships no binary asset and CI exercises the import path on every run. The parse half is pinned by unit tests against an embedded asset — accessors, generated normals, material factors, color-space policy, hierarchy — with no device anywhere. Imported scenes still don't survive a save/load round trip, because `MeshRenderer` handles are runtime-only; giving them stable references is precisely the Phase 6 asset database's job, along with cooked caches so imports stop re-parsing source on every run.

*Not done:*

- **Cascaded** shadow maps for the sun, **cube shadow maps for point lights**, and spot shadows.
- **MSAA/FXAA/TAA** — unblocked by the graph; MSAA in particular is multisampled attachments plus a resolve, which is exactly the attachment management the graph owns.
- **Tangents from glTF** — the shader still derives its tangent frame from screen-space derivatives; imported tangents fix mirrored UVs when textured assets arrive in numbers.
- **Instancing** — the draw list is sorted and ready for it, but nothing merges consecutive identical draws yet.

One bug worth recording, found by looking at the output rather than by the build passing: the default metallic-roughness texture was `(1, 1, 0)`, read as "fully rough, non-metal". But metallic samples from blue and is *multiplied* by `metallicFactor`, so a zero there forced every material to be a dielectric regardless of its factor, and the metal spheres shaded as diffuse plastic. Fallback textures have to be the multiplicative identity.

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

### Phase 9 — Advanced rendering (~8+ weeks)

- **Clustered forward+** or deferred shading for hundreds of lights — the frame graph makes this a swap rather than a rewrite.
- SSAO/GTAO, screen-space reflections, volumetric fog, decals.
- TAA, motion vectors, depth pre-pass, HZB occlusion culling.
- GPU-driven rendering: indirect draws, compute-based culling, meshlets.
- **Skeletal animation**: skinning, animation clips, blend trees, an animation state machine, IK.
- GPU particle system, trails, sprite and decal atlases.
- Terrain: heightmap rendering, LOD, splat-mapped materials.
- Global illumination — light probes and lightmaps first, then DDGI or voxel GI.

### Phase 10 — Completing the engine (ongoing)

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

**Golden-image regression testing** from Phase 4 onward: render fixed scenes at fixed camera positions in CI and diff against committed references with a perceptual threshold. This is what keeps a renderer from silently regressing as the frame graph grows.

---

## 9. Sequencing notes

- Phases 0–3 are strictly ordered; each is a hard prerequisite for the next. Reflection (Phase 1) in particular gates the editor, serialization *and* scripting, so it should not be deferred or done half-way.
- Phases 4 (PBR) and 5 (editor) can interleave once Phase 3 lands. The editor is more useful the earlier it exists, so an early-but-thin editor beats a late complete one.
- Phase 8 (physics) only depends on Phases 1–3, so it can move earlier if gameplay matters more than visual fidelity.
- Realistic total for Phases 0–8 solo part-time: **12–18 months**. Phases 9–10 are open-ended.
- The sandbox project should be kept working at every step — it is the honest test of whether the engine is actually usable.
