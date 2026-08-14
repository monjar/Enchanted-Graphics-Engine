#pragma once

#include "reflect/BuiltinTypes.hpp"
#include "render/Model.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ege {

    // Geometry a script owns and rewrites.
    //
    // The CPU side is the truth here, which is the opposite of a `MeshRenderer`
    // pointing at an immutable asset. A behaviour writes `vertices`, calls
    // `markDirty`, and the upload happens once at the end of the frame -
    // rather than every write, which for a per-vertex loop would be every
    // vertex.
    //
    // Deliberately not an asset. An asset is something several entities share
    // and a scene names by id; this is one entity's own copy, and copying it
    // is the whole point.
    struct DynamicMesh {
        std::vector<Model::Vertex> vertices;
        std::vector<std::uint32_t> indices;

        // Rebuilt from `vertices` whenever the mesh is dirty and something has
        // a device to build it with. Not serialized - it is derived.
        std::shared_ptr<Model> gpu;

        bool dirty = true;

        void markDirty() { dirty = true; }

        // Area-weighted smooth normals from the current positions. The
        // accumulated cross products weight themselves by triangle area, so
        // nothing has to be divided by anything.
        void recalculateNormals();

        // A flat grid in the XZ plane, `segments` quads across, centred on the
        // origin and one unit wide. The starting point for anything a script
        // deforms, and enough to make the feature demonstrable.
        static DynamicMesh grid(std::uint32_t segments);
    };

}  // namespace ege

// Reflected for its name and its counts. The vertex array itself is not a
// field anyone would edit by hand, and writing several thousand vertices into
// a scene file is not what a scene file is for - a script rebuilds them.
EGE_REFLECT(ege::DynamicMesh)
EGE_REFLECT_END()
