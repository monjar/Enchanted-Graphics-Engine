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

        // Advances the simulation one fixed step. Call with the same delta
        // every time: a fixed-step simulation fed a variable step is neither
        // fixed nor reproducible.
        virtual void step(float deltaSeconds) = 0;

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
