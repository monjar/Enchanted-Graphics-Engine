#pragma once

#include "scene/Entity.hpp"

namespace ege {

    // The events the demo's little game is made of.
    //
    // Plain structs with no base class and no registration: the type is the
    // channel, so a listener that asks for the wrong one does not compile.
    // They live in the engine only because the demo does; a project's events
    // would sit in the project, and nothing here would need to know.

    // Something was picked up. Raised by the pickup, which knows nothing
    // about scores, and heard by whatever is counting.
    struct PickupCollected {
        // Who collected it. May already be the only thing left of it: the
        // pickup despawns itself in the same breath.
        Entity collector{};
    };

    // Every pickup that was going to exist has been collected. Raised by the
    // thing that was counting, and heard by whatever celebrates - a door, a
    // light, a scoreboard. That the door does not know what a pickup is, and
    // the pickup does not know there is a door, is the entire argument for
    // events over a pointer.
    struct LevelComplete {
        int collected = 0;
    };

}  // namespace ege
