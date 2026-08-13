# Enchanted Graphics Engine

A Vulkan game engine in C++17, built from the renderer up.

![The demo scene: a box and a sphere lit by a point light on a plane](docs/images/demo-scene.png)

A forward renderer with a metallic-roughness PBR pipeline, an entity-component
system, runtime reflection, textures with mip generation, a job system and a
fixed-timestep simulation clock.

The image above is the demo scene: five metal spheres sweeping roughness from
near-mirror to fully rough, plus two dielectrics, lit by three point lights.
The smoothest metal is nearly black because a mirror with no environment to
reflect *is* nearly black — that is what image-based lighting fixes, and it is
one of the things still outstanding.

Scenes save and load as reflection-driven JSON, and entities can be parented.

Still to come: glTF import, IBL, shadows, an HDR post-processing stack, the
editor, C++ scripting and physics.
[`docs/ROADMAP.md`](docs/ROADMAP.md) lays out the plan and tracks, per phase,
exactly what has landed and what has not.

## Building

You need a C++17 compiler, CMake 3.21 or newer, and the Vulkan SDK.
Everything else — GLFW, GLM, doctest — is resolved automatically, preferring
system packages and falling back to a pinned source build.

```sh
cmake --preset default
cmake --build --preset default
./build/default/bin/EnchantedEngine
```

`glslangValidator` must be on `PATH` or in the Vulkan SDK; on Debian and
Ubuntu it is in `glslang-tools`.

### Presets

| Preset    | What it gives you |
|-----------|-------------------|
| `default` | Release |
| `debug`   | Debug, Vulkan validation layers active |
| `asan`    | Debug plus AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan`    | Debug plus ThreadSanitizer, for the job system |
| `cxx20`   | Release built as C++20 |

Each writes to `build/<preset>/`, so configurations coexist.

### Tests

```sh
ctest --test-dir build/default --output-on-failure
```

The suite covers logic that needs no GPU — primitive geometry, transform
maths — so it runs anywhere. Rendering is covered separately by a headless
smoke test in CI, which draws the demo scene under lavapipe with the
validation layers enabled and fails on any validation message.

## Controls

| Key | Action |
|-----|--------|
| `W` `A` `S` `D` | Move |
| `Q` / `E` | Down / up |
| Arrow keys | Look |
| Hold right mouse | Mouse-look (captures the cursor) |

## Layout

```
app/            entry point
src/
  core/         application root, logging, assertions, time, job system
  reflect/      runtime type information
  platform/     window and input; everything touching GLFW
  rhi/          device, swapchain, pipeline, buffer, descriptors, textures
  render/       renderer, model, camera, materials, lights, PBR render system
  scene/        world, entities, component pools, components, hierarchy, serialization
shaders/        GLSL, compiled to SPIR-V into the build tree
assets/         runtime assets, resolved via EGE_ASSET_ROOT
tests/          doctest suite
cmake/          dependency, warning and shader modules
docs/           roadmap and design notes
```

The engine builds as a static library (`Enchanted`, aliased `ege::engine`);
the executable is just `app/main.cpp`. Tests link the library directly, and
the editor and standalone runtime will do the same.

## Conventions

- Types are `PascalCase` in `namespace ege`, files are named after the type
  they declare, and includes are module-qualified: `#include "rhi/Buffer.hpp"`.
- Constructor parameters that would shadow the member they initialise take a
  `Ref` suffix.
- Formatting is enforced by `.clang-format`; CI rejects anything unformatted.
- Warnings are errors on every target. Third-party headers are included as
  `SYSTEM` so they are exempt.

`git config blame.ignoreRevsFile .git-blame-ignore-revs` keeps the tree-wide
reformat out of `git blame`.

## Third-party

| | | |
|---|---|---|
| [Vulkan SDK](https://vulkan.lunarg.com/) | graphics API | system |
| [GLFW](https://www.glfw.org/) 3.4 | windowing and input | fetched or system |
| [GLM](https://github.com/g-truc/glm) 1.0.1 | maths | fetched or system |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | OBJ import | vendored |
| [spdlog](https://github.com/gabime/spdlog) 1.14.1 | logging | fetched or system |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) 3.1.0 | GPU memory | fetched |
| [stb_image](https://github.com/nothings/stb) | image decoding | fetched |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 | scene serialization | fetched or system |
| [doctest](https://github.com/doctest/doctest) 2.4.11 | tests | fetched |

The Vulkan buffer abstraction started from Sascha Willems'
[`VulkanBuffer`](https://github.com/SaschaWillems/Vulkan/blob/master/base/VulkanBuffer.h),
and the renderer's foundations follow Brendan Galea's
[Vulkan engine series](https://github.com/blurrypiano/littleVulkanEngine).

## Licence

MIT — see [`LICENSE`](LICENSE). Vendored tinyobjloader is MIT as well; its
notice is in `external/tinyobjectloader/LICENSE`.
