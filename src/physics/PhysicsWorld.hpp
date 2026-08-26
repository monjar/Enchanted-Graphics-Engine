#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ege {

    // Handle to a body inside a PhysicsWorld. Meaningful only to the world
    // that issued it, and only until that body is removed.
    using PhysicsBodyId = std::uint32_t;

    inline constexpr PhysicsBodyId invalidPhysicsBody = 0xFFFFFFFFu;

    // How a body participates in the simulation. A stationary body never
    // moves and costs nothing while nothing touches it; a kinematic body is
    // moved by the caller and pushes dynamic bodies out of its way without
    // being pushed back; a dynamic body is the simulation's to move.
    enum class BodyMotion { stationary, kinematic, dynamic };

    // The collision geometry a body is made of. A tagged struct rather than a
    // class hierarchy: three primitives with four numbers between them do not
    // justify virtual dispatch, and a value type can sit inside BodySettings.
    struct BodyShape {
        enum class Kind { box, sphere, capsule };

        Kind kind = Kind::box;
        // Box only.
        glm::vec3 halfExtents{0.5f};
        // Sphere and capsule.
        float radius = 0.5f;
        // Capsule only: half the cylinder between the two hemispherical caps,
        // measured along the local Y axis the capsule stands on.
        float halfHeight = 0.5f;
        // Where the shape sits relative to the body's origin, so a collider
        // need not be centred on the entity it belongs to.
        glm::vec3 offset{0.f};

        static BodyShape box(glm::vec3 halfExtentsRef, glm::vec3 offsetRef = glm::vec3{0.f});
        static BodyShape sphere(float radiusRef, glm::vec3 offsetRef = glm::vec3{0.f});
        static BodyShape capsule(
            float radiusRef, float halfHeightRef, glm::vec3 offsetRef = glm::vec3{0.f});
    };

    // Everything needed to create a body. Plain data, so a caller can build
    // one from components without talking to the backend.
    struct BodySettings {
        BodyShape shape{};
        BodyMotion motion = BodyMotion::dynamic;
        glm::vec3 position{0.f};
        glm::quat rotation{1.f, 0.f, 0.f, 0.f};
        // Dynamic bodies only; the backend derives inertia from the shape.
        float mass = 1.f;
        float friction = 0.5f;
        float restitution = 0.f;
        float linearDamping = 0.05f;
        float angularDamping = 0.05f;
        // Scales gravity per body. Zero floats, one falls normally.
        float gravityFactor = 1.f;
        // A sensor detects overlap and reports contacts but pushes nothing:
        // a trigger volume rather than an obstacle.
        bool sensor = false;
        // Round-tripped through contact events and raycast hits, so the
        // caller can find its own object again. The physics world never
        // interprets it.
        std::uint64_t userData = 0;
    };

    // Position and orientation, without scale: a rigid body cannot change
    // size, which is why scale is applied to the shape at creation instead.
    struct BodyPose {
        glm::vec3 position{0.f};
        glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    };

    // A new touch between two bodies, reported once when it begins - a
    // trigger for gameplay, not a stream of contact manifolds.
    struct ContactEvent {
        std::uint64_t userDataA = 0;
        std::uint64_t userDataB = 0;
        // One representative point of the manifold, in world space.
        glm::vec3 point{0.f};
        // From A towards B.
        glm::vec3 normal{0.f};
    };

    // Handle to a walking capsule inside a PhysicsWorld. A separate handle
    // space from bodies, because a character is not a body: the solver never
    // moves it, and asking for its velocity means asking a different object.
    using PhysicsCharacterId = std::uint32_t;

    inline constexpr PhysicsCharacterId invalidPhysicsCharacter = 0xFFFFFFFFu;

    // Everything needed to create a character. Plain data, like BodySettings,
    // and for the same reason.
    struct CharacterSettings {
        // A capsule standing along `up`: two hemispherical caps of `radius`
        // separated by a cylinder of 2 * halfHeight.
        float radius = 0.3f;
        float halfHeight = 0.55f;
        // The entity's origin, at the capsule's centre - the same place a
        // CapsuleCollider puts it, so a character and a collider fitted to
        // the same mesh sit in the same place.
        glm::vec3 position{0.f};
        // Which way the capsule stands, and which way "up a slope" means.
        glm::vec3 up{0.f, 1.f, 0.f};
        // Radians. Steeper than this is a wall to be slid down.
        float maxSlopeAngle = 0.785398f;
        // How high a step it walks straight up, and how far below its feet it
        // looks for the floor before calling itself airborne.
        float stepHeight = 0.3f;
        float stickToFloor = 0.5f;
        // What it weighs when it leans on a dynamic body, and the most force
        // it can push with.
        float mass = 70.f;
        float pushForce = 100.f;
        std::uint64_t userData = 0;
    };

    // Whether the character can walk where it stands. The distinction between
    // steep and airborne matters to gameplay: one is a slope being slid down,
    // the other is a fall, and they want different animations and different
    // rules about jumping.
    enum class CharacterGroundState { grounded, steep, airborne };

    // What a character found underfoot after its last move.
    struct CharacterGround {
        CharacterGroundState state = CharacterGroundState::airborne;
        glm::vec3 normal{0.f, 1.f, 0.f};
        // What the surface itself is doing, so standing on a moving platform
        // can mean moving with it.
        glm::vec3 velocity{0.f};
        // The ground body's user datum - which entity is underfoot - or zero
        // when nothing is.
        std::uint64_t userData = 0;
    };

    struct RaycastHit {
        // Along the ray, in world units.
        float distance = 0.f;
        glm::vec3 point{0.f};
        glm::vec3 normal{0.f};
        std::uint64_t userData = 0;
    };

    // Rigid-body simulation behind an engine-owned interface.
    //
    // The backend today is Jolt, and nothing outside JoltPhysicsWorld.cpp
    // knows it: no Jolt type appears here, so the backend stays replaceable -
    // including by a hand-written one - without touching a caller. That is
    // the roadmap's stated design constraint for Phase 8, and it is also what
    // keeps Jolt's headers out of every file that touches physics.
    //
    // The interface is deliberately small. Bodies are created whole from
    // BodySettings rather than mutated property by property, because the one
    // caller that exists rebuilds its bodies each time play begins; property
    // setters can arrive when an editor needs to tweak a live body.
    class PhysicsWorld {
    public:
        struct Settings {
            // Conventional Y-up default. A scene whose up is elsewhere - the
            // demo's is -Y - passes its own.
            glm::vec3 gravity{0.f, -9.81f, 0.f};
            std::uint32_t maxBodies = 4096;
        };

        // The one place a backend is chosen. Two overloads rather than a
        // default argument, because a default built from Settings' own member
        // initializers is ill-formed inside the enclosing class.
        static std::unique_ptr<PhysicsWorld> create(const Settings& settings);

        static std::unique_ptr<PhysicsWorld> create() { return create(Settings{}); }

        virtual ~PhysicsWorld() = default;

        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        virtual PhysicsBodyId addBody(const BodySettings& settings) = 0;

        virtual void removeBody(PhysicsBodyId body) = 0;

        virtual std::size_t bodyCount() const = 0;

        virtual BodyPose pose(PhysicsBodyId body) const = 0;

        // Teleports. For following a moving target from gameplay use
        // moveKinematic, which gives the body the velocity the move implies
        // so that whatever it hits on the way is pushed rather than skipped.
        virtual void setPose(PhysicsBodyId body, const BodyPose& target) = 0;

        virtual void moveKinematic(
            PhysicsBodyId body, const BodyPose& target, float deltaSeconds) = 0;

        virtual glm::vec3 linearVelocity(PhysicsBodyId body) const = 0;

        virtual void setLinearVelocity(PhysicsBodyId body, glm::vec3 velocity) = 0;

        virtual void addImpulse(PhysicsBodyId body, glm::vec3 impulse) = 0;

        // ---- Characters ---------------------------------------------------
        //
        // A character is a capsule the world collides against but the solver
        // never moves: it goes exactly where its velocity says, minus
        // whatever was in the way. That is the difference between a body that
        // happens to be capsule-shaped and something that can walk - the
        // former topples, catches on steps and slides down every ramp, and
        // fighting each of those with friction and constraints is how a
        // rigid-body character ends up worse than this one.

        virtual PhysicsCharacterId addCharacter(const CharacterSettings& settings) = 0;

        virtual void removeCharacter(PhysicsCharacterId character) = 0;

        virtual std::size_t characterCount() const = 0;

        virtual glm::vec3 characterPosition(PhysicsCharacterId character) const = 0;

        // Teleports, and re-finds what is underfoot afterwards.
        virtual void setCharacterPosition(PhysicsCharacterId character, glm::vec3 position) = 0;

        // After a move this is what the character *managed* to do rather than
        // what it was asked to: a run into a wall reads back as a stop, which
        // is what makes the next step accelerate from rest rather than from
        // an imaginary six metres per second.
        virtual glm::vec3 characterVelocity(PhysicsCharacterId character) const = 0;

        virtual void setCharacterVelocity(PhysicsCharacterId character, glm::vec3 velocity) = 0;

        virtual CharacterGround characterGround(PhysicsCharacterId character) const = 0;

        // Moves the character by its velocity for one step, resolving
        // collisions, sliding along walls and steep slopes, walking up steps
        // and staying attached to the floor over a crest.
        virtual void updateCharacter(PhysicsCharacterId character, float deltaSeconds) = 0;

        // Advances the simulation one fixed step. Call with the same delta
        // every time: a fixed-step simulation fed a variable step is neither
        // fixed nor reproducible.
        virtual void step(float deltaSeconds) = 0;

        // What the world was built with. A character needs it to know which
        // way is down and how hard, and asking the world is better than every
        // caller carrying a copy that can disagree with it.
        virtual glm::vec3 gravity() const = 0;

        // Contacts that began since the last drain, in a deterministic order.
        // Collected during step() from whatever threads the backend simulates
        // on and handed over here, so the caller touches gameplay state from
        // its own thread only.
        virtual std::vector<ContactEvent> drainContacts() = 0;

        // First body along the ray, or nothing. Direction need not be
        // normalised; the ray ends at origin + direction * maxDistance.
        virtual std::optional<RaycastHit> raycast(
            glm::vec3 origin, glm::vec3 direction, float maxDistance) const = 0;

    protected:
        PhysicsWorld() = default;
    };

}  // namespace ege
