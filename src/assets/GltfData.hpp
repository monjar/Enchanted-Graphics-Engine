#pragma once

#include "anim/AnimationClip.hpp"
#include "anim/Skeleton.hpp"
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
        // Skinning attributes, present only when the file carries them, and
        // exactly as long as `vertices` when they are. Joint indices are in
        // *skeleton* order - the importer remaps them from the file's own
        // joint numbering, so nothing downstream ever sees two orderings.
        // Weights are renormalised; a vertex whose weights summed to nothing
        // gets bound wholly to its first joint rather than to the void.
        std::vector<glm::uvec4> joints;
        std::vector<glm::vec4> weights;
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
        // Which rig deforms this node's mesh; -1 for the rigid majority.
        int skin = -1;
        std::vector<uint32_t> children;
    };

    // One rig, ready for the animation arithmetic: joints reordered so
    // parents precede children, whatever order the file held them in.
    struct GltfSkinData {
        std::string name;
        Skeleton skeleton;
        // The glTF node each joint came from, in skeleton order - what lets
        // an animation channel targeting a node find its joint.
        std::vector<int> jointNodes;
    };

    struct GltfSceneData {
        std::vector<GltfImageData> images;
        std::vector<GltfMaterialData> materials;
        std::vector<GltfMeshData> meshes;
        std::vector<GltfNodeData> nodes;
        std::vector<uint32_t> rootNodes;
        // Rigs and their clips. Clips are resolved against the first skin's
        // skeleton - see the note where they are parsed.
        std::vector<GltfSkinData> skins;
        std::vector<AnimationClip> clips;
    };

}  // namespace ege
