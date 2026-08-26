#include "physics/CharacterMotion.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace ege {

    namespace {

        // Below this the character is standing still as far as facing is
        // concerned. Turning towards a velocity of a millimetre per second is
        // turning towards numerical noise, and the visible result is a body
        // that spins on the spot as it comes to rest.
        constexpr float facingSpeedThreshold = 0.05f;

        glm::vec3 safeUp(glm::vec3 up) {
            const float length = glm::length(up);
            return length > 1e-6f ? up / length : glm::vec3{0.f, 1.f, 0.f};
        }

        // The part of `v` in the plane perpendicular to `up`.
        glm::vec3 flatten(glm::vec3 v, glm::vec3 up) {
            return v - up * glm::dot(v, up);
        }

    }  // namespace

    glm::vec3 upFromGravity(glm::vec3 gravity) {
        const float pull = glm::length(gravity);
        return pull > 1e-6f ? -gravity / pull : glm::vec3{0.f, 1.f, 0.f};
    }

    glm::vec2 applyDeadzone(glm::vec2 stick, float deadzone) {
        const float magnitude = glm::length(stick);
        if (magnitude <= deadzone || magnitude <= 0.f) {
            return glm::vec2{0.f};
        }
        // Rescaled so the usable range still reaches one at full deflection:
        // without this a deadzone also costs the player their top speed.
        const float scaled = std::min((magnitude - deadzone) / (1.f - deadzone), 1.f);
        return stick * (scaled / magnitude);
    }

    glm::vec3 moveDirection(glm::vec2 stick, glm::vec3 reference, glm::vec3 up) {
        const glm::vec3 axis = safeUp(up);
        const glm::vec3 forward = flatten(reference, axis);
        const float forwardLength = glm::length(forward);
        if (forwardLength <= 1e-6f) {
            // Looking straight up or straight down: there is no forward on
            // the ground to mean, and inventing one would send the character
            // somewhere the player did not point.
            return glm::vec3{0.f};
        }
        const glm::vec3 ahead = forward / forwardLength;
        const glm::vec3 right = glm::cross(ahead, axis);
        return ahead * stick.y + right * stick.x;
    }

    float jumpSpeed(float height, float gravity) {
        if (height <= 0.f || gravity <= 0.f) {
            return 0.f;
        }
        return std::sqrt(2.f * gravity * height);
    }

    glm::vec3 approach(glm::vec3 velocity, glm::vec3 target, float rate, float deltaSeconds) {
        const glm::vec3 gap = target - velocity;
        const float distance = glm::length(gap);
        const float step = rate * deltaSeconds;
        if (distance <= step || distance <= 1e-6f) {
            return target;
        }
        return velocity + gap * (step / distance);
    }

    float turnToward(float from, float to, float maxDelta) {
        float delta = std::fmod(to - from, glm::two_pi<float>());
        if (delta > glm::pi<float>()) {
            delta -= glm::two_pi<float>();
        } else if (delta < -glm::pi<float>()) {
            delta += glm::two_pi<float>();
        }
        const float applied = std::clamp(delta, -maxDelta, maxDelta);

        float result = std::fmod(from + applied, glm::two_pi<float>());
        if (result > glm::pi<float>()) {
            result -= glm::two_pi<float>();
        } else if (result <= -glm::pi<float>()) {
            result += glm::two_pi<float>();
        }
        return result;
    }

    float yawFromDirection(glm::vec3 direction) {
        if (std::abs(direction.x) <= 1e-6f && std::abs(direction.z) <= 1e-6f) {
            return 0.f;
        }
        return std::atan2(direction.x, direction.z);
    }

    void advanceCharacter(
        CharacterController& controller, const CharacterFrame& frame, float deltaSeconds) {
        const glm::vec3 up = safeUp(frame.up);

        float vertical = glm::dot(controller.velocity, up);
        glm::vec3 planar = flatten(controller.velocity, up);

        // ---- What was asked for ---------------------------------------

        glm::vec3 wish = flatten(controller.move, up);
        float wishLength = glm::length(wish);
        if (wishLength > 1.f) {
            // A keyboard's two axes at once is a vector of length root two,
            // and a character that walks faster diagonally is the oldest bug
            // in this genre.
            wish /= wishLength;
            wishLength = 1.f;
        }

        const float speed = controller.run ? controller.runSpeed : controller.walkSpeed;
        glm::vec3 target = wish * speed;
        if (frame.grounded) {
            // Standing on something moving means standing still relative to
            // it, not relative to the world.
            target += flatten(frame.groundVelocity, up);
        }

        float rate = wishLength > 0.f ? controller.acceleration : controller.braking;
        if (!frame.grounded) {
            rate *= controller.airControl;
        }
        planar = approach(planar, target, rate, deltaSeconds);

        // ---- Grace ------------------------------------------------------

        controller.coyote = frame.grounded ? controller.coyoteTime
                                           : std::max(controller.coyote - deltaSeconds, 0.f);
        controller.buffered = controller.jump ? controller.jumpBuffer
                                              : std::max(controller.buffered - deltaSeconds, 0.f);
        // The press is an edge and is consumed here; `move`, `run` and
        // `jumpHeld` are levels and are left alone. A driver that sets the
        // edge and forgets therefore jumps once rather than for ever, and one
        // that writes its levels on the render clock rather than this one
        // still moves smoothly.
        controller.jump = false;

        // Landing spends whatever fall speed was accumulated. Without this a
        // character standing on the floor is one step of gravity away from
        // terminal velocity for as long as it stands there, and the first
        // step off a ledge starts at the bottom of that.
        if (frame.grounded && vertical < 0.f) {
            vertical = 0.f;
        }

        // ---- The axis of gravity ---------------------------------------

        // Standing on the ground counts as well as the grace does, or a
        // character authored with no coyote time at all - a legitimate
        // choice, and the strictest one - could never jump.
        const bool canJump = frame.grounded || controller.coyote > 0.f;

        controller.jumped = false;
        if (controller.buffered > 0.f && canJump) {
            vertical = jumpSpeed(controller.jumpHeight, frame.gravity);
            // Both spent: one press is one jump, and the grace that allowed
            // it must not allow a second on the next step.
            controller.coyote = 0.f;
            controller.buffered = 0.f;
            controller.jumped = true;
        } else {
            float gravity = frame.gravity;
            if (vertical > 0.f && !controller.jumpHeld) {
                gravity *= controller.jumpCutGravity;
            }
            vertical = std::max(vertical - gravity * deltaSeconds, -controller.terminalSpeed);
        }

        // ---- What came of it -------------------------------------------

        controller.velocity = planar + up * vertical;
        controller.planarSpeed = glm::length(planar);
        controller.grounded = frame.grounded;
        controller.groundNormal = frame.groundNormal;

        // Facing follows where the body is going rather than where the stick
        // points, so a character turning a corner leans into it instead of
        // pivoting on the spot and then setting off.
        if (controller.faceMotion && controller.planarSpeed > facingSpeedThreshold) {
            controller.facing = turnToward(
                controller.facing, yawFromDirection(planar), controller.turnRate * deltaSeconds);
        }
    }

}  // namespace ege
