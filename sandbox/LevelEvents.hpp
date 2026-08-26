#pragma once

#include "scene/World.hpp"

namespace sandbox {

    // What happens in the level, as messages.
    //
    // Nothing in the engine knows these types exist, which is the point: an
    // event is typed by the type itself, so a project's events are its own
    // and cost the engine nothing. They are in a header rather than in the
    // one source file because a real project would have more than one.

    // A collectible was taken. Raised by the collectible; heard by whatever
    // is keeping score.
    struct Collected {
        ege::Entity collector{};
    };

    // The player left the floor and kept going. Raised by the pit; heard by
    // the player, who goes back to the start, and by the rules, which count.
    struct Fell {};

    // Every collectible has been taken. Raised by the rules; heard by the
    // gate, which has never heard of a collectible.
    struct GateOpens {};

    // The player reached the exit with the gate open.
    struct LevelWon {
        int collected = 0;
    };

    // The player ran out of lives.
    struct LevelLost {};

}  // namespace sandbox
