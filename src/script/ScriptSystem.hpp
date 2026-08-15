#pragma once

#include "physics/PhysicsSystem.hpp"
#include "scene/World.hpp"

#include <vector>

namespace ege {

    // Runs the behaviours attached to entities.
    //
    // Only while playing, and that is the whole reason it is a separate system
    // rather than something the world does. In the editor an entity with a
    // behaviour is a description of what will happen; pressing Play is what
    // makes it happen, and Stop is what puts it back.
    class ScriptSystem {
    public:
        // Creates any instances that do not exist yet and calls onSpawn on the
        // ones that have not had it. Safe to call every frame: it is how a
        // behaviour attached mid-play gets started.
        void spawnPending(World& world);

        void tick(World& world, float deltaSeconds);

        void fixedTick(World& world, float deltaSeconds);

        // Tells each side of every contact about the touch, through
        // Behavior::onContact. The physics system reports a pair once; each
        // entity's behaviours receive it from their own side, with the other
        // entity named and the normal pointing away from themselves.
        void deliverContacts(World& world, const std::vector<EntityContact>& contacts);

        // Calls onDespawn on everything that has spawned and forgets the
        // instances. Called when play stops - the world is about to be replaced
        // by the snapshot, and a behaviour should hear about that.
        void despawnAll(World& world);
    };

}  // namespace ege
