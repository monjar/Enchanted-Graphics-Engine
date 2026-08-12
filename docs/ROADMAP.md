# Enchanted Graphics Engine — Development Roadmap

> Living document. Describes how the project grows from a Vulkan renderer into a complete game engine.
>
> **Status: Phases 0 and 1 complete.** See [§6](#6-phases) for what each covered and what it turned up.

## 1. Where the project stands today

Enchanted is a **Vulkan 1.0 forward renderer** grown out of Brendan Galea's Vulkan tutorial series, stopped at roughly the point where UBOs and descriptor sets were introduced. It draws indexed meshes — procedural primitives or OBJ files — lit by one hardcoded diffuse point light.

The Vulkan foundations are idiomatic and are being kept:

| Class | Role |
|---|---|
| `ege::Window` | GLFW window + surface |
| `ege::Device` | Instance, physical/logical device, queues, command pool, buffer/image helpers |
| `ege::SwapChain` | Swapchain, render pass, depth resources, sync objects |
| `ege::Renderer` | Frame lifecycle (`beginFrame`/`endFrame`), command buffers, resize recreation |
| `ege::Pipeline` | Graphics pipeline + shader modules |
| `ege::Buffer` | Generic mapped/staged Vulkan buffer |
| `ege::DescriptorSetLayout` / `Pool` / `Writer` | Fluent descriptor builders |
| `ege::Model` | Vertex/index buffers, procedural primitives, tinyobj loading, vertex dedup |

Everything above that layer — scene representation, materials, textures, asset management, input abstraction, scripting, physics, tooling — does not exist yet.

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

```cpp
World world;

Entity player = world.Spawn("Player");
player.Attach<Transform>({ .position = {0, 1, 0} });
player.Attach<MeshRenderer>(mesh, material);
player.Attach<Script<PlayerController>>();

Transform& t = player.Fetch<Transform>();       // asserts present
if (auto* r = player.Find<RigidBody>()) { ... } // nullable
player.Detach<MeshRenderer>();
world.Despawn(player);

sword.SetParent(player);                        // hierarchy lives in the ECS

// Queries — the main way systems are written
world.Each<Transform, PointLight>([](Entity e, Transform& t, PointLight& l) { ... });

for (auto [e, t, rb] : world.View<Transform, RigidBody>()) { ... }

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

### Phase 3 — ECS world & scene (~4–5 weeks)

- Implement `World`, `Entity`, `ComponentPool`, `View` / `Each`, `With` / `Without` filters and `Schedule` — the API in §4.
- Retire `EgeGameObject` and its `unordered_map<id_t, EgeGameObject>`. The current `TransformComponent::mat4()` recomputes a full TRS composition per object per frame inside the draw loop; the new `Transform` caches a world matrix behind a dirty flag, propagated through `Hierarchy`.
- Scene serialization to JSON, generated entirely from reflection. `.egescene` files; prefabs as sub-scenes.
- Move lights out of the hardcoded `GlobalUbo` — where light position and colour currently keep their C++ initialiser values forever — into real `DirectionalLight` / `PointLight` / `SpotLight` components gathered per frame.
- **Debug ImGui overlay**: hierarchy tree plus a reflection-driven inspector, in-process. Not the editor yet, but it makes ECS work visible immediately and prototypes Phase 5's panels.

**Done when:** a scene of hundreds of entities with multiple lights loads from a `.egescene` file, is inspectable, and saves back byte-identically.

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

### Phase 6 — Asset pipeline (~4 weeks)

- **GUID-based asset database**: every source asset gets a `.egameta` sidecar carrying a stable GUID, so references survive moves and renames.
- Importers: glTF/OBJ → mesh, PNG/JPG/HDR → texture (KTX2 + BC compression for shipping), GLSL → SPIR-V, plus material / scene / prefab natives.
- Async loading on the `JobSystem`, with placeholder assets while loading and reference counting for eviction.
- Hot reload for textures, shaders, materials and meshes.
- Cooked-asset packaging for `EnchantedPlayer` — a single archive, no source assets shipped.

**Done when:** dropping a `.gltf` into the project folder imports it automatically and it is immediately draggable into the scene.

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
| `src/rhi/Device.*` | → `rhi/Device.*`; VMA, Vulkan 1.3 features, portability enumeration |
| `src/rhi/SwapChain.*` | → `rhi/Swapchain.*`; most render-pass machinery removed by dynamic rendering |
| `src/rhi/Pipeline.*` | → `rhi/Pipeline.*`; zero-init the config struct, drop the `EgeModel::Vertex` coupling, add SPIRV-Reflect |
| `src/rhi/Buffer.*` | → `rhi/Buffer.*`; VMA-backed; `getAlignmentSize()` fixed |
| `src/rhi/Descriptors.*` | → `rhi/Descriptors.*` (also fixing the singular/plural filename mismatch); bindless + per-frame allocator |
| `src/render/Model.*` | Split into `render/Mesh` (GPU) and `assets/MeshData` (CPU, scriptable) |
| `src/render/Camera.*` | → `render/Camera`; becomes a component; `near` / `far` params renamed |
| `src/platform/KeyboardMovementController.*` | **Deleted** in Phase 1, replaced by `platform/Input` + an editor fly-camera |
| `shaders/simple_shader.{vert,frag}` | → `shaders/`; superseded by the PBR shader set in Phase 4 |
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
