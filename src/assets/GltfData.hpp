#pragma once

#include "render/Material.hpp"
#include "render/Model.hpp"
#include "scene/Components.hpp"

#include <string>
#include <vector>

namespace ege {

    // What a glTF file contains, after parsing and before any GPU object
    // exists. Everything here is plain CPU data: the parse half of the
    // importer produces it, the instantiate half consumes it, and the tests
    // check it without a device. This split is the seed of the Phase 6
    // asset pipeline, where parsed data gets cached and cooked rather than
    // rebuilt from source on every run.

    // Decoded RGBA8 pixels.
    struct GltfImageData {
        std::vector<unsigned char> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        // Set from usage: base colour and emissive maps are sRGB-encoded,
        // normal and metallic-roughness data is linear. An image referenced
        // both ways keeps sRGB, which is the visually-forgiving direction.
        bool srgb = false;
    };

    struct GltfMaterialData {
        std::string name;
        MaterialProperties properties{};
        // Indices into GltfSceneData::images; -1 means the material keeps
        // the engine's fallback for that slot.
        int baseColorImage = -1;
        int normalImage = -1;
        int metallicRoughnessImage = -1;
        int emissiveImage = -1;
    };

    // One draw's worth of geometry: a glTF primitive, with its material slot.
    struct GltfPrimitiveData {
        std::vector<Model::Vertex> vertices;
        std::vector<uint32_t> indices;
        int material = -1;
    };

    struct GltfMeshData {
        std::string name;
        std::vector<GltfPrimitiveData> primitives;
    };

    struct GltfNodeData {
        std::string name;
        // The node's local TRS, already converted to the engine's YXZ Euler
        // convention.
        Transform transform{};
        int mesh = -1;
        std::vector<uint32_t> children;
    };

    struct GltfSceneData {
        std::vector<GltfImageData> images;
        std::vector<GltfMaterialData> materials;
        std::vector<GltfMeshData> meshes;
        std::vector<GltfNodeData> nodes;
        std::vector<uint32_t> rootNodes;
    };

}  // namespace ege
