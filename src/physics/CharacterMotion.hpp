#pragma once

#include "physics/PhysicsComponents.hpp"

#include <glm/glm.hpp>

namespace ege {

    // How a character decides where to go: the arithmetic between "the player
    // is holding forward" and "the capsule should be travelling at this
    // velocity".
    //
    // Device-free and world-free on purpose. Everything here is a function of
    // numbers a test can write down, so the part of a character controller
    // that is actually hard to get right - accelerating, braking, air control,
    // coyote time, a jump that is higher when held - is checked against hand
    // derivations rather than against how it felt to walk around in it. What
    // is left for the backend is collision, which is Jolt's job and tested as
    // its own contract.
    //
    // The state lives on the CharacterController component rather than in a
    // struct of its own: reflection describes fields by pointer-to-member, so
    // a nested tuning struct could not be shown in the inspector, and two
    // parallel copies of fifteen tuning floats would drift apart the first
    // time one of them gained a field.

    // The world as the character experiences it this step: which way is up,
    // how hard gravity pulls, and what is underfoot.
    struct CharacterFrame {
        // Up need not be +Y. The demo scene's is -Y, and the whole of what
        // makes that work is that nothing here assumes otherwise.
        glm::vec3 up{0.f, 1.f, 0.f};
        // Metres per second squared, along -up.
        float gravity = 9.81f;
        bool grounded = false;
        glm::vec3 groundNormal{0.f, 1.f, 0.f};
        // What the surface underfoot is doing, so a moving platform carries
        // whoever stands on it rather than sliding out from under them.
        glm::vec3 groundVelocity{0.f};
    };

    // Which way a character stands: the opposite of whichever way the world
    // drops things. A world with no gravity has no opinion, so the
    // conventional up is used and a character in it stands as it would
    // anywhere else.
    glm::vec3 upFromGravity(glm::vec3 gravity);

    // A stick reading turned into a direction in the character's plane: the
    // stick's +y drives `reference` flattened into that plane, its +x the
    // right-hand perpendicular. The result keeps the stick's magnitude, so a
    // half-pushed stick is a walk rather than a run.
    //
    // `reference` is normally where the camera looks, which is what makes
    // "forward" mean forward on the screen rather than forward for the model.
    // A reference parallel to up has no forward to offer and yields zero.
    glm::vec3 moveDirection(glm::vec2 stick, glm::vec3 reference, glm::vec3 up);

    // The upward speed a jump needs to rise `height` against `gravity`:
    // v = sqrt(2 g h), the one line of this file that is straight mechanics.
    float jumpSpeed(float height, float gravity);

    // Moves `velocity` towards `target` at `rate` metres per second squared,
    // stopping exactly on it rather than overshooting and coming back. The
    // whole of acceleration and braking, which differ only in the rate.
    glm::vec3 approach(glm::vec3 velocity, glm::vec3 target, float rate, float deltaSeconds);

    // Turns `from` towards `to` by at most `maxDelta`, the short way round,
    // and leaves the result in (-pi, pi]. Angles are the one place where
    // "move towards" is not subtraction.
    float turnToward(float from, float to, float maxDelta);

    // The yaw an engine Transform takes to face `direction`: the inverse of
    // the (sin yaw, 0, cos yaw) forward the rest of the engine builds. About
    // the Y axis, like Transform's own rotation, which is why this is a yaw
    // rather than a full orientation - and why it means what it says only for
    // a scene whose up is one of the Y axes, which is both of the ones the
    // engine has.
    float yawFromDirection(glm::vec3 direction);

    // One step of character movement: reads the controller's intent and
    // tuning, writes its velocity and state, and leaves the velocity for the
    // backend to move the capsule with.
    //
    // Splits the velocity into the character's plane and the axis of gravity
    // and treats them as the different problems they are: the driver owns the
    // plane, gravity and jumping own the axis. A single three-dimensional
    // acceleration towards a target would make walking into a hill a jump.
    void advanceCharacter(
        CharacterController& controller, const CharacterFrame& frame, float deltaSeconds);

}  // namespace ege
