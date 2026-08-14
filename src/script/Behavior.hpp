#pragma once

#include "scene/World.hpp"

namespace ege {

    // Gameplay code attached to an entity.
    //
    // Virtual calls rather than the ECS's data-oriented iteration, on purpose.
    // Systems are how the engine expresses work that applies to *every* entity
    // with a component; behaviours are how a project expresses work that
    // applies to *this* entity and reads like the thing it is doing. The cost
    // is one indirect call per behaviour per tick, which is nothing against a
    // scene's worth of draws, and the benefit is that gameplay written against
    // this looks like gameplay rather than like a query.
    //
    // The callbacks are deliberately few. Every one of them is a promise about
    // when it runs, and a promise is easier to keep than to withdraw.
    class Behavior {
    public:
        virtual ~Behavior() = default;

        Behavior(const Behavior&) = delete;
        Behavior& operator=(const Behavior&) = delete;

        // Once, when the entity starts playing - not when the component is
        // attached. Attaching happens in the editor, where nothing should run.
        virtual void onSpawn() {}

        // Every rendered frame, with the real elapsed time. For anything that
        // should look smooth.
        virtual void onTick(float deltaSeconds) { (void)deltaSeconds; }

        // Every fixed simulation step. For anything that must be
        // frame-rate-independent - which, once physics arrives, is most of it.
        virtual void onFixedTick(float deltaSeconds) { (void)deltaSeconds; }

        // When play stops, or the entity is despawned while playing.
        virtual void onDespawn() {}

        // The entity this is attached to. Valid from onSpawn onwards, and the
        // entity handle carries its world, so `self().fetch<Transform>()` is
        // the whole of the usual first line of a behaviour.
        Entity self() const { return owner; }

        World& world() const { return *scene; }

    protected:
        Behavior() = default;

    private:
        friend class ScriptSystem;

        Entity owner{};
        World* scene = nullptr;
    };

}  // namespace ege
