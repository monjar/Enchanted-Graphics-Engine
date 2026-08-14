#pragma once

#include "assets/GltfData.hpp"
#include "rhi/Descriptors.hpp"
#include "scene/World.hpp"

#include <string>

namespace ege::gltf {

    // Parses a .gltf or .glb file into CPU-side data. Throws
    // std::runtime_error with the reason on malformed input. External
    // buffers and images resolve relative to the file's directory.
    GltfSceneData parseFile(const std::string& path);

    // The same, from a buffer already in memory. Buffers and images must be
    // self-contained (GLB or base64 data URIs) since there is no directory
    // to resolve external references against. This is the path the tests
    // use.
    GltfSceneData parseMemory(const void* data, size_t size);

    struct ImportStats {
        EntityId root{};
        size_t entities = 0;
        size_t meshes = 0;
        size_t materials = 0;
        size_t textures = 0;
    };

    // Builds GPU objects from parsed data and spawns the node hierarchy into
    // the world under a single root entity. The root carries a half-turn
    // roll: glTF scenes are +Y-up and this engine inherited -Y-up from its
    // tutorial ancestry, and rotating rather than mirroring preserves
    // winding.
    ImportStats instantiate(
        Device& device,
        World& world,
        const GltfSceneData& scene,
        DescriptorPool& materialPool,
        DescriptorSetLayout& materialLayout,
        const std::string& rootName);

}  // namespace ege::gltf
