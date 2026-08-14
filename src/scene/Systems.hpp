#pragma once

#include "scene/World.hpp"

namespace ege {

    class Device;

}  // namespace ege

namespace ege::systems {

    // Work the engine does over whole component sets, as opposed to the
    // per-entity work a behaviour does.

    // Rebuilds the GPU model of every dirty DynamicMesh and points its
    // renderer at it.
    //
    // Once per frame rather than on every write, because a behaviour deforming
    // a surface writes every vertex and uploading per write would upload per
    // vertex. The dirty flag is what turns "a script changed this" into "this
    // needs one upload".
    void uploadDynamicMeshes(World& world, Device& device);

}  // namespace ege::systems
