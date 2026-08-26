#pragma once

#include "physics/PhysicsWorld.hpp"
#include "reflect/BuiltinTypes.hpp"

#include <glm/glm.hpp>

#include <string>

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

    // Which named collision layer an entity's collider is in.
    //
    // A name rather than a number, because the number is an index into a
    // table the scene does not own: a world may define its layers in any
    // order, and a scene file that said "layer 3" would mean something
    // different the day someone inserted a layer above it. The name is
    // resolved when the body is built, and a name the world has never heard
    // of falls back to the default layer and says so.
    //
    // An entity without one is in the default layer, which collides with
    // everything - so a scene that never mentions layers behaves exactly as
    // it did before they existed.
    struct PhysicsLayer {
        std::string name = "Default";
    };

    // A volume that notices who is inside it and stops nothing.
    //
    // A collider plus this is a trigger: a pressure plate, a checkpoint, the
    // mouth of a pit. Behaviours on it hear `onTriggerEnter` when something
    // arrives and `onTriggerExit` when the last of it leaves, and behaviours
    // on the thing that arrived hear the same pair from their own side - so
    // either end can be the one that knows what to do.
    //
    // A trigger is built as a kinematic sensor rather than a static one,
    // because a static sensor in Jolt notices only bodies that are moving,
    // and a plate that misses a player who walked on and stopped is a plate
    // that does not work. It follows its Transform like any kinematic body,
    // so a trigger can ride a lift.
    //
    // `RigidBody::sensor` says the same thing about a body that is already
    // simulated. This is the other case, and the common one: a volume that
    // is nothing but a trigger and does not want a rigid body at all.
    //
    // One thing to know, because it is the backend's rule rather than this
    // engine's: a trigger notices things that are *awake*. A crate that
    // settles inside one and falls asleep is reported as having left, and is
    // not reported as arriving again until something wakes it. Characters
    // never sleep, so a plate a player stands on stays pressed; a plate held
    // down by a crate is a design that wants a weight rather than a count.
    struct Trigger {
        // Which layers may set it off, by name. Empty means anything the
        // collision matrix already allows - which is the answer for a plate
        // in a room with one player in it, and the wrong one for a door that
        // opens for the player and not for the crates they push.
        std::string only;
    };

    // A capsule that walks.
    //
    // Not a RigidBody with a capsule collider, which is the thing everybody
    // tries first: a rigid capsule topples over, catches on every step, slides
    // down every ramp it stands on and accelerates for as long as a key is
    // held. Every fix for those is a rule about what a *character* may do
    // rather than what a body must, which is why this is its own component
    // over Jolt's virtual character - collision detection and sliding, with
    // the solver never moving it.
    //
    // Three groups of fields, and the division is the whole design:
    //
    //  - **Shape and tuning** are data. Authored in the inspector, saved in
    //    the scene, and the only part reflected.
    //  - **Intent** is what whoever drives it asked for, written afresh every
    //    tick. A player's input, a patrol behaviour and an AI write the same
    //    four fields, which is how the demo can record a deterministic walk on
    //    a machine with no gamepad and a player can still take the controls.
    //  - **State** is what happened, written back by the physics system for
    //    gameplay and the animator to read.
    //
    // Intent and state are deliberately unreflected, for the same reason
    // RigidBody's body handle is: they are this tick's answer, not the
    // entity's description, and a scene file that remembered a half-finished
    // jump would be remembering the wrong thing.
    //
    // What a character is not, yet: visible to anything else. Other bodies do
    // not collide with it, and it raises no contacts a behaviour can hear -
    // being pushed *by* a character is something the character does to a body
    // rather than a touch the world reports. Both want the inner proxy body
    // Jolt can pair a virtual character with, and that arrives when something
    // needs to shoot at one.
    struct CharacterController {
        // ---- Shape ----------------------------------------------------
        // A capsule standing along the entity's up axis, scaled by the
        // entity's world scale like every other collider. The default is
        // roughly a 1.7 metre person.
        float radius = 0.3f;
        float halfHeight = 0.55f;

        // ---- Tuning ---------------------------------------------------
        float walkSpeed = 3.f;
        float runSpeed = 6.f;
        // How fast the planar velocity chases what was asked for, and how
        // fast it gives up when nothing is. Braking is the larger of the two
        // because a character that coasts to a stop feels like it is on ice.
        float acceleration = 30.f;
        float braking = 45.f;
        // The fraction of that available with nothing underfoot. Zero is a
        // committed jump, one is flying.
        float airControl = 0.35f;
        // Metres of rise a jump from a standstill reaches. A height rather
        // than an impulse, because a height is a thing a level designer can
        // measure against a ledge.
        float jumpHeight = 1.f;
        // Gravity is multiplied by this while rising with the jump released,
        // which is the whole of what makes a tap shorter than a hold.
        float jumpCutGravity = 2.6f;
        // Seconds after walking off a ledge during which a jump still works,
        // and seconds a jump pressed just before landing is remembered for.
        // Both exist because the player's idea of when they pressed the
        // button is not the simulation's, and the simulation is the one that
        // can afford to be generous.
        float coyoteTime = 0.12f;
        float jumpBuffer = 0.15f;
        float terminalSpeed = 40.f;
        // Radians per second the body turns towards where it is going.
        float turnRate = 12.f;
        // Steeper than this is a wall: the character slides down it rather
        // than walking up.
        float maxSlopeAngle = 0.785398f;
        // How high a step can be walked straight up without a jump.
        float stepHeight = 0.3f;
        // How far below its feet it looks for the floor before admitting it
        // is airborne. Without it, walking over the crest of a ramp launches
        // the character.
        float stickToFloor = 0.5f;
        // What it weighs when it leans on a dynamic body, and the most force
        // it can push with. Together they are why a character shoves a crate
        // and not a wall.
        float mass = 70.f;
        float pushForce = 100.f;
        // Whether the entity turns to face where it is going. Off for a
        // strafing shooter, on for anything that has a front.
        bool faceMotion = true;

        // ---- Intent: written by whoever drives it, every tick ----------
        // Where to go, in world space. Its length scales the speed, so half a
        // stick is half a walk; longer than one is clamped, which is what
        // stops a diagonal on a keyboard outrunning a straight line.
        glm::vec3 move{0.f};
        bool run = false;
        // True on the tick the jump was asked for. The buffer remembers it,
        // so a driver may set it for one tick and forget it.
        bool jump = false;
        // Whether the button is still held, which is what makes a long press
        // a higher jump than a tap.
        bool jumpHeld = false;

        // ---- State: written by the physics system ----------------------
        glm::vec3 velocity{0.f};
        bool grounded = false;
        // True on the tick a jump left the ground - the cue an animator or a
        // sound wants, and one a driver would have to guess at otherwise.
        bool jumped = false;
        // Speed across the ground, which is what picks a walk from a run.
        float planarSpeed = 0.f;
        // Where the body faces, as the yaw an engine Transform takes.
        float facing = 0.f;
        glm::vec3 groundNormal{0.f, 1.f, 0.f};

        // Jump timers, in seconds remaining. State rather than tuning: how
        // much grace is left is not something to author.
        float coyote = 0.f;
        float buffered = 0.f;

        // The character behind this component while the scene simulates.
        // Runtime state, like RigidBody's body handle and for the same
        // reason.
        PhysicsCharacterId character = invalidPhysicsCharacter;
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

EGE_REFLECT(ege::PhysicsLayer)
EGE_FIELD(name).tooltip("Which named collision layer this entity's collider is in");
EGE_REFLECT_END()

EGE_REFLECT(ege::Trigger)
EGE_FIELD(only).tooltip("Only this collision layer sets it off; empty means anything");
EGE_REFLECT_END()

// Shape and tuning only - see the note on the struct. Showing `grounded` and
// `planarSpeed` read-only in the inspector would be genuinely useful and is
// deliberately not done: reflection is also what serialization writes, and
// there is no way yet to say "draw this, do not save it". A live readout is
// worth having the day that distinction exists, not at the cost of scene
// files that remember a half-finished jump.
EGE_REFLECT(ege::CharacterController)
EGE_FIELD(radius).range(0.05f, 5.f);
EGE_FIELD(halfHeight).range(0.f, 5.f).tooltip("Half the cylinder between the two caps");
EGE_FIELD(walkSpeed).range(0.f, 20.f).tooltip("Metres per second at full stick");
EGE_FIELD(runSpeed).range(0.f, 40.f);
EGE_FIELD(acceleration).range(1.f, 200.f).tooltip("How fast it reaches the speed asked for");
EGE_FIELD(braking).range(1.f, 200.f).tooltip("How fast it stops when nothing is asked for");
EGE_FIELD(airControl).range(0.f, 1.f).tooltip("Fraction of that available in mid-air");
EGE_FIELD(jumpHeight).range(0.f, 10.f).tooltip("Metres a jump from a standstill rises");
EGE_FIELD(jumpCutGravity).range(1.f, 8.f).tooltip("Gravity while rising with the jump released");
EGE_FIELD(coyoteTime).range(0.f, 0.5f).tooltip("Grace after a ledge during which a jump works");
EGE_FIELD(jumpBuffer).range(0.f, 0.5f).tooltip("How long a jump pressed before landing waits");
EGE_FIELD(terminalSpeed).range(1.f, 200.f);
EGE_FIELD(turnRate).range(0.f, 40.f).tooltip("Radians per second it turns to face its motion");
EGE_FIELD(maxSlopeAngle).range(0.f, 1.5f).tooltip("Radians; steeper than this is a wall");
EGE_FIELD(stepHeight).range(0.f, 2.f).tooltip("How high a step it walks straight up");
EGE_FIELD(stickToFloor).range(0.f, 2.f).tooltip("How far below its feet it looks for the floor");
EGE_FIELD(mass).range(1.f, 500.f).tooltip("What it weighs when it leans on a dynamic body");
EGE_FIELD(pushForce).range(0.f, 2000.f).tooltip("The most force it can push with");
EGE_FIELD(faceMotion).tooltip("Turn the entity towards where it is going");
EGE_REFLECT_END()
