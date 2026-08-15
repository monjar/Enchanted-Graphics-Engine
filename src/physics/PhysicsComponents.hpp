#pragma once

#include "physics/PhysicsWorld.hpp"
#include "reflect/BuiltinTypes.hpp"

#include <glm/glm.hpp>

namespace ege {

    // What an entity is to the simulation.
    //
    // The division of labour follows what the words mean rather than adding a
    // mode enum: a *collider* says what shape an entity presents to the
    // simulation, and a *RigidBody* says the simulation may move it. A
    // collider alone is scenery - a stationary body that costs nothing while
    // nothing touches it - which is why the demo's floor needs no RigidBody
    // to be landed on. Add a RigidBody and the entity becomes the
    // simulation's to move, or the caller's, if it is kinematic.
    //
    // Sizes are in local units and are multiplied by the entity's world scale
    // when the body is built, so a collider fitted to a mesh stays fitted
    // when the entity is scaled. Bodies are built when play begins; editing
    // sizes during play reshapes nothing until the next play.

    struct RigidBody {
        // Kilograms, for dynamic bodies. Inertia is derived from the shape.
        float mass = 1.f;
        // A kinematic body follows the entity's Transform and pushes dynamic
        // bodies out of its way without ever being pushed back - a moving
        // platform rather than a crate.
        bool kinematic = false;
        float friction = 0.5f;
        // 0 lands dead, 1 bounces back with everything it arrived with.
        float restitution = 0.f;
        float linearDamping = 0.05f;
        float angularDamping = 0.05f;
        // Scales gravity for this body alone. Zero floats.
        float gravityFactor = 1.f;
        // A sensor reports contacts and stops nothing: a trigger volume.
        bool sensor = false;

        // The body behind this component while the scene simulates, so
        // gameplay code can reach world().physics() APIs that want one.
        // Runtime state, not data: deliberately unreflected, so it is neither
        // shown in the inspector nor written into a scene file - a body
        // handle is this simulation's answer to this component and means
        // nothing in the next run.
        PhysicsBodyId body = invalidPhysicsBody;
    };

    struct BoxCollider {
        glm::vec3 halfExtents{0.5f};
        // Where the shape sits relative to the entity's origin.
        glm::vec3 offset{0.f};
    };

    struct SphereCollider {
        float radius = 0.5f;
        glm::vec3 offset{0.f};
    };

    // Stands along the entity's local Y axis: two hemispherical caps of
    // `radius` separated by a cylinder of 2 * halfHeight.
    struct CapsuleCollider {
        float radius = 0.5f;
        float halfHeight = 0.5f;
        glm::vec3 offset{0.f};
    };

}  // namespace ege

EGE_REFLECT(ege::RigidBody)
EGE_FIELD(mass).range(0.001f, 1000.f).tooltip("Kilograms; inertia is derived from the shape");
EGE_FIELD(kinematic).tooltip("Follows the Transform and pushes without being pushed back");
EGE_FIELD(friction).range(0.f, 2.f);
EGE_FIELD(restitution).range(0.f, 1.f).tooltip("0 lands dead, 1 keeps all of its bounce");
EGE_FIELD(linearDamping).range(0.f, 2.f);
EGE_FIELD(angularDamping).range(0.f, 2.f);
EGE_FIELD(gravityFactor).range(-2.f, 2.f).tooltip("Scales gravity for this body; zero floats");
EGE_FIELD(sensor).tooltip("Reports contacts but stops nothing - a trigger volume");
EGE_REFLECT_END()

EGE_REFLECT(ege::BoxCollider)
EGE_FIELD(halfExtents).tooltip("Half the box's size on each local axis");
EGE_FIELD(offset).tooltip("Where the shape sits relative to the entity's origin");
EGE_REFLECT_END()

EGE_REFLECT(ege::SphereCollider)
EGE_FIELD(radius).range(0.01f, 100.f);
EGE_FIELD(offset).tooltip("Where the shape sits relative to the entity's origin");
EGE_REFLECT_END()

EGE_REFLECT(ege::CapsuleCollider)
EGE_FIELD(radius).range(0.01f, 100.f);
EGE_FIELD(halfHeight).range(0.f, 100.f).tooltip("Half the cylinder between the two caps");
EGE_FIELD(offset).tooltip("Where the shape sits relative to the entity's origin");
EGE_REFLECT_END()
