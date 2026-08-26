#pragma once

#include "physics/PhysicsWorld.hpp"
#include "scene/World.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace ege {

    // A contact between two entities, resolved from the physics world's
    // opaque events. What gameplay receives.
    struct EntityContact {
        Entity a{};
        Entity b{};
        glm::vec3 point{0.f};
        // From a towards b.
        glm::vec3 normal{0.f};
    };

    // Keeps the ECS and the physics world agreeing about who is where.
    //
    // The simulation exists only while the scene plays. start() builds a body
    // for every entity that presents a collider, stop() throws the whole
    // physics world away, and Stop's snapshot restore puts the transforms
    // back - so play can never leave physics state behind in edit mode, for
    // the same reason behaviours cannot: nothing advances the world unless
    // Play asked for it. The one thing physics adds to that contract is the
    // body handle cached in RigidBody, and stop() clears it.
    //
    // Each fixed tick reconciles rather than listens: entities spawned since
    // the last tick get bodies, despawned ones lose them, kinematic bodies
    // are given their Transform as a target, the world steps, and dynamic
    // bodies write their poses back. Declaring the whole state cheaply every
    // tick is the same trade the frame graph makes every frame, and it needs
    // no attach/detach hooks the ECS would otherwise have to grow.
    class PhysicsSystem {
    public:
        // Builds the physics world and a body for every collider-bearing
        // entity, and publishes the backend through World::physics().
        void start(World& world, const PhysicsWorld::Settings& settings);

        void start(World& world) { start(world, PhysicsWorld::Settings{}); }

        // Destroys the physics world. The components stay; their body
        // handles are reset.
        void stop(World& world);

        bool running() const { return backend != nullptr; }

        // Advances the simulation one fixed step and returns the contacts
        // that began during it, both sides resolved to entities. Does nothing
        // and returns nothing while stopped.
        std::vector<EntityContact> fixedTick(World& world, float deltaSeconds);

        PhysicsWorld* physicsWorld() { return backend.get(); }

    private:
        // Creates bodies for entities that gained colliders and removes
        // bodies whose entity died or lost them.
        void reconcile(World& world);

        void createBody(World& world, EntityId entity);

        void createCharacter(World& world, EntityId entity);

        // Turns every character's intent into a move: the engine's own motion
        // arithmetic decides the velocity, the backend decides how far that
        // gets, and the result goes back onto the component and the
        // Transform.
        void moveCharacters(World& world, float deltaSeconds);

        // What was built for an entity, and how it was told to move -
        // remembered so a RigidBody edited mid-play from dynamic to
        // kinematic is noticed and rebuilt.
        struct BodyRecord {
            PhysicsBodyId id = invalidPhysicsBody;
            BodyMotion motion = BodyMotion::stationary;
        };

        // What was built for a character, and where the system last put it -
        // remembered so that a Transform written by gameplay is noticed as a
        // teleport rather than quietly overwritten by where the capsule
        // happens to be.
        struct CharacterRecord {
            PhysicsCharacterId id = invalidPhysicsCharacter;
            glm::vec3 position{0.f};
        };

        std::unique_ptr<PhysicsWorld> backend;
        // Every body the system made, keyed by owner. RigidBody caches the
        // same handle for gameplay's convenience, but this map is the record
        // - a despawned entity takes its components with it, and the body
        // left behind is exactly what this exists to find.
        std::unordered_map<EntityId, BodyRecord> bodies;
        std::unordered_map<EntityId, CharacterRecord> characters;
    };

}  // namespace ege
