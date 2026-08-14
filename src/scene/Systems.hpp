#pragma once

#include "scene/World.hpp"

namespace ege::systems {

    // Systems that run only while the editor is playing.
    //
    // There is exactly one for now, and it is deliberately trivial: play mode
    // has to be observable before it can be trusted, and until scripting
    // arrives in Phase 7 nothing else in the engine changes the world over
    // time. `Spin` is the placeholder that makes pressing Play do something
    // visible and pressing Stop demonstrably undo it.

    // Advances every Spin component's owner by one fixed step.
    void spin(World& world, float deltaSeconds);

}  // namespace ege::systems
