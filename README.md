# Enchanted Graphics Engine

A Vulkan-based 3D graphics engine built with C++17, designed to evolve into a full-featured renderer for a game engine.

## Current Features

### Core Vulkan Rendering Pipeline
- **Vulkan 1.0 graphics pipeline** with vertex and fragment shader stages
- **Swap chain management** with double-buffering (2 frames in flight), automatic recreation on window resize, and FIFO present mode
- **Render pass** with color and depth-stencil attachments
- **Dynamic viewport and scissor** configuration
- **Synchronization** via semaphores (image-available / render-finished) and in-flight fences

### Shading & Lighting
- **GLSL 450 shaders** compiled to SPIR-V via `glslangValidator`
- **Uniform Buffer Objects** (global UBO per frame with projection-view matrix, ambient light, point light position and color)
- **Push constants** for per-object model and normal matrices
- **Diffuse lighting** (Lambertian) with a single point light, distance attenuation (1/r²), and ambient term
- **Normal matrix** for correct normal transformation under non-uniform scaling

### Resource Management
- **GPU buffer abstraction** (vertex, index, uniform, staging buffers) with proper memory property selection
- **Staging buffer transfers** (host-visible staging → device-local GPU memory)
- **Descriptor system** with Builder-pattern layouts, pools, and writers
- **Memory alignment** handling for uniform buffer offsets

### Model & Geometry
- **OBJ model loading** via TinyObjLoader (positions, normals, UVs, colors)
- **Vertex deduplication** using hash-based comparison
- **Index buffer rendering** for efficient draw calls

### Camera System
- **Perspective projection** (configurable FOV, near/far planes)
- **Orthographic projection**
- **View matrices**: direction-based, look-at target, and YXZ Euler angle rotation

### Game Object System
- **Entity management** with auto-incrementing IDs and an `unordered_map`-based registry
- **Transform component** (translation, rotation, scale) with model and normal matrix generation
- **Shared model references** (`shared_ptr`) for instanced geometry

### Input
- **Keyboard movement controller** — WASD + Q/E for 3D movement, arrow keys for look rotation
- **Frame-time–based** smooth movement and rotation with configurable speeds
- **Pitch clamping** (±85°) and yaw wrapping

### Window & Platform
- **GLFW window** with configurable size and title
- **Framebuffer resize** callback with swap chain recreation
- **Cross-platform** build support (Windows via Visual Studio/MinGW, Linux/Unix via CMake)

### Debugging
- **Vulkan validation layers** (`VK_LAYER_KHRONOS_validation`) enabled in debug builds
- **Debug messenger** (`VK_EXT_debug_utils`) for real-time validation output

---

## Feature Roadmap

Features are grouped into priority tiers. Each tier builds on the previous one, moving the engine from a basic renderer toward a production-quality game engine backend.

### Priority 1 — Core Rendering Essentials

These are the most fundamental missing pieces; almost every 3D application needs them.

| # | Feature | Description |
|---|---------|-------------|
| 1 | **Texture mapping** | 2D texture loading (e.g. via stb_image), `VkImage`/`VkSampler` creation, UV-based sampling in shaders |
| 2 | **Material system** | Per-object material properties (diffuse texture, color, shininess) passed through descriptors or push constants |
| 3 | **Multiple lights** | Support for arrays of point lights, directional lights, and spot lights in the UBO |
| 4 | **Specular highlights** | Blinn-Phong specular term (or upgrade to full Blinn-Phong model) |
| 5 | **glTF model loading** | Replace or supplement OBJ with glTF 2.0 (via tinygltf or Assimp) for scenes, materials, and animations |

### Priority 2 — Essential Visual Quality

Features that bring the renderer to a visually competitive baseline.

| # | Feature | Description |
|---|---------|-------------|
| 6 | **Shadow mapping** | Depth-only render pass from each light's perspective; PCF or variance shadow filtering |
| 7 | **Normal mapping** | Tangent-space normal maps with TBN matrix construction in the vertex shader |
| 8 | **Skybox / environment maps** | Cubemap loading, dedicated pipeline, and background rendering |
| 9 | **Mipmapping & anisotropic filtering** | Mipmap generation for textures and `VkSamplerCreateInfo` anisotropy settings |
| 10 | **Alpha blending & transparency** | Sorted or order-independent transparency pass |
| 11 | **HDR & tone mapping** | Render to floating-point framebuffers, apply tone-mapping (Reinhard, ACES, etc.) |
| 12 | **Gamma correction** | Ensure sRGB-correct pipeline (linear-space lighting → sRGB output) |

### Priority 3 — Advanced Rendering Techniques

Techniques that significantly improve visual fidelity and rendering flexibility.

| # | Feature | Description |
|---|---------|-------------|
| 13 | **Physically Based Rendering (PBR)** | Metallic-roughness workflow, Cook-Torrance BRDF, image-based lighting (IBL) |
| 14 | **Deferred rendering** | G-buffer pass (albedo, normal, depth, metallic-roughness) + lighting pass for many-light scenes |
| 15 | **Screen-Space Ambient Occlusion (SSAO)** | Post-process pass using depth + normals for contact shadows |
| 16 | **Post-processing framework** | Bloom, vignette, motion blur, depth of field via offscreen render targets |
| 17 | **Anti-aliasing** | MSAA, FXAA, or TAA (currently no anti-aliasing) |
| 18 | **Frustum culling** | CPU-side AABB/sphere tests against camera frustum before issuing draw calls |
| 19 | **Instanced rendering** | `vkCmdDrawIndexedInstanced` for efficiently rendering many copies of the same mesh |

### Priority 4 — Game Engine Infrastructure

Architectural features needed to turn the renderer into a usable engine component.

| # | Feature | Description |
|---|---------|-------------|
| 20 | **Scene graph** | Parent-child transform hierarchy with dirty-flag propagation |
| 21 | **Entity Component System (ECS)** | Data-oriented entity storage (e.g. entt) replacing the current `unordered_map<id, GameObject>` |
| 22 | **Resource / asset manager** | Async loading, reference counting, caching for models, textures, and shaders |
| 23 | **ImGui integration** | Debug overlay, inspector panels, and real-time parameter tuning |
| 24 | **Text rendering** | SDF font atlas or FreeType-based glyph rendering for UI and HUD |
| 25 | **Multi-threaded command recording** | Secondary command buffers recorded on worker threads, submitted from the main thread |
| 26 | **Vulkan Memory Allocator (VMA)** | Replace manual `vkAllocateMemory` calls with AMD's VMA for sub-allocation and defragmentation |

### Priority 5 — Advanced Effects & Content

Higher-level visual effects and content-creation support.

| # | Feature | Description |
|---|---------|-------------|
| 27 | **Skeletal animation** | Bone hierarchies, skinning matrices, keyframe interpolation (from glTF or custom format) |
| 28 | **Particle system** | GPU-driven or CPU-driven particles with billboard quads, emitters, and forces |
| 29 | **Terrain rendering** | Heightmap-based terrain with LOD (clipmaps or quadtree), texture splatting |
| 30 | **Water rendering** | Planar reflections / refractions, wave simulation, screen-space reflections |
| 31 | **Volumetric effects** | Fog, god rays (volumetric light scattering), atmospheric scattering |
| 32 | **Global illumination** | Screen-space GI, light probes, or voxel cone tracing for indirect lighting |

### Priority 6 — Optimization & Cutting-Edge

Performance and next-gen features for a polished, production-grade engine.

| # | Feature | Description |
|---|---------|-------------|
| 33 | **Level of Detail (LOD)** | Automatic mesh simplification and LOD selection based on distance |
| 34 | **Occlusion culling** | GPU-based occlusion queries or hierarchical-Z culling |
| 35 | **GPU-driven rendering** | Indirect draw calls, compute-based culling, bindless resources |
| 36 | **Compute shader pipeline** | General-purpose compute support for physics, particles, post-processing |
| 37 | **Pipeline caching** | `VkPipelineCache` serialization to disk for faster startup |
| 38 | **Render graph** | Frame-graph system for automatic resource lifetime management and pass ordering |
| 39 | **Ray tracing** | `VK_KHR_ray_tracing_pipeline` for hybrid ray-traced reflections, shadows, or GI |

---

## Building

### Prerequisites

- C++17 compiler (GCC 7+, Clang 5+, MSVC 2017+)
- CMake 3.11+
- Vulkan SDK
- GLFW 3.3+
- GLM

### Setup

1. Copy the appropriate environment template and fill in your local paths:
   ```bash
   # Linux / macOS
   cp envUnixExample.cmake .env.cmake

   # Windows
   copy envWindowsExample.cmake .env.cmake
   ```
2. Edit `.env.cmake` to set `VULKAN_SDK_PATH`, `GLFW_PATH`, `GLM_PATH`, and optionally `TINYOBJ_PATH`.

### Linux / macOS

```bash
./unixBuild.sh
```

Or manually:

```bash
mkdir -p build && cd build
cmake ..
make
```

### Windows (MinGW)

```bat
mingwBuild.bat
```

### Windows (Visual Studio)

Open `EnchantedGraphicsEngine.sln` in Visual Studio and build.

### Shader Compilation

Shaders are compiled automatically during the CMake build. The build system searches for `glslangValidator` in the Vulkan SDK path and common system directories.

## Dependencies

| Library | Purpose | Type |
|---------|---------|------|
| [Vulkan SDK](https://vulkan.lunarg.com/) | Graphics API | Required |
| [GLFW](https://www.glfw.org/) | Window & input | Required |
| [GLM](https://github.com/g-truc/glm) | Math (vectors, matrices) | Required |
| [TinyObjLoader](https://github.com/tinyobjloader/tinyobjloader) | OBJ model loading | Bundled |

## Project Structure

```
├── CMakeLists.txt              # Build configuration
├── Shaders/                    # GLSL shaders (+ compiled .spv)
│   ├── simple_shader.vert
│   └── simple_shader.frag
├── external/                   # Bundled third-party headers
│   └── tinyobjectloader/
├── src/
│   ├── main.cpp                # Entry point
│   ├── ege_engine.*            # Engine orchestration & game loop
│   ├── ege_window.*            # GLFW window wrapper
│   ├── ege_engine_device.*     # Vulkan device & instance
│   ├── ege_renderer.*          # Frame lifecycle & swap chain render pass
│   ├── ege_swap_chain.*        # Swap chain, framebuffers, sync
│   ├── ege_pipeline.*          # Graphics pipeline creation
│   ├── ege_model.*             # Mesh loading & vertex/index buffers
│   ├── ege_buffer.*            # GPU buffer abstraction
│   ├── ege_descriptors.*       # Descriptor sets, layouts, pools
│   ├── ege_camera.*            # Camera projections & view matrices
│   ├── ege_game_object.*       # Entity with transform & model
│   ├── ege_frame_info.hpp      # Per-frame data struct
│   ├── ege_utils.hpp           # Hash utilities
│   ├── simple_render_system.*  # Draw-call issuing system
│   └── keyboard_movement_controller.*  # Input handling
└── unixBuild.sh / mingwBuild.bat  # Convenience build scripts
```
