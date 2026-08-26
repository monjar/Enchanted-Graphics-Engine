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

        // Trigger arrivals and departures, each side told from its own side.
        // `entering` chooses between onTriggerEnter and onTriggerExit,
        // because the two differ in nothing but which call to make.
        void deliverTriggers(World& world, const std::vector<TriggerEvent>& events, bool entering);

        // Calls onDespawn on everything that has spawned and forgets the
        // instances. Called when play stops - the world is about to be replaced
        // by the snapshot, and a behaviour should hear about that.
        void despawnAll(World& world);

        // Rebuilds every behaviour instance from the registry, carrying its
        // reflected fields across. Returns how many were rebuilt.
        //
        // This is what a script module reload comes down to. The old instances
        // are of types the newly loaded module has replaced in the registry,
        // so every one has to be made again from the new factory - and what a
        // behaviour's author wrote is its reflected fields, so those are
        // written out and read back into the replacement.
        //
        // What cannot survive is a behaviour's unreflected private state:
        // Bobbing's remembered origin, Orbit's accumulated angle. The
        // replacement gets onSpawn again instead, which is the call a
        // behaviour already uses to work that state out from where things
        // are - so reload lands in the same place a fresh Play would, and
        // "reflect the state you want to keep" is a rule an author can hold.
        std::size_t rebuildInstances(World& world);
    };

}  // namespace ege
