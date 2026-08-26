// The arithmetic between "the player is holding forward" and "the capsule
// should be travelling at this velocity".
//
// All of it is a function of numbers, so all of it is checked against numbers
// worked out by hand rather than against how it felt to walk around in. What
// is deliberately not here is collision: how far that velocity actually gets
// is the backend's answer, and test_physicsworld.cpp asks it.

#include "physics/CharacterMotion.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <doctest/doctest.h>

#include <cmath>

using ege::advanceCharacter;
using ege::applyDeadzone;
using ege::approach;
using ege::CharacterController;
using ege::CharacterFrame;
using ege::jumpSpeed;
using ege::moveDirection;
using ege::turnToward;
using ege::upFromGravity;
using ege::yawFromDirection;

namespace {

    constexpr float step = 1.f / 60.f;

    // Y-up, one gravity, standing on flat ground.
    CharacterFrame groundedFrame() {
        CharacterFrame frame{};
        frame.up = {0.f, 1.f, 0.f};
        frame.gravity = 9.81f;
        frame.grounded = true;
        frame.groundNormal = {0.f, 1.f, 0.f};
        return frame;
    }

    CharacterFrame airFrame() {
        CharacterFrame frame = groundedFrame();
        frame.grounded = false;
        return frame;
    }

    // The planar speed of a velocity in a Y-up frame.
    float planar(glm::vec3 velocity) {
        return glm::length(glm::vec3{velocity.x, 0.f, velocity.z});
    }

}  // namespace

TEST_CASE("a radial deadzone rescales what is left of the stick") {
    // Inside: nothing at all, however the components are distributed.
    CHECK(glm::length(applyDeadzone({0.1f, 0.1f}, 0.2f)) == doctest::Approx(0.f));

    // Just outside: a crawl, not a fifth of walking speed. Half way from the
    // threshold to full deflection is half output.
    const glm::vec2 half = applyDeadzone({0.6f, 0.f}, 0.2f);
    CHECK(half.x == doctest::Approx(0.5f));
    CHECK(half.y == doctest::Approx(0.f));

    // Full deflection still reaches one: a deadzone must not also cost the
    // player their top speed.
    CHECK(glm::length(applyDeadzone({1.f, 0.f}, 0.2f)) == doctest::Approx(1.f));

    // Radial, not per-axis: a stick pushed into a corner is at magnitude one
    // and comes back out at magnitude one, along the diagonal it went in on.
    const glm::vec2 diagonal =
        applyDeadzone({glm::root_two<float>() / 2.f, glm::root_two<float>() / 2.f}, 0.2f);
    CHECK(glm::length(diagonal) == doctest::Approx(1.f));
    CHECK(diagonal.x == doctest::Approx(diagonal.y));
}

TEST_CASE("movement is relative to where the camera looks") {
    const glm::vec3 up{0.f, 1.f, 0.f};
    // Looking along +Z, which is what a yaw of zero means everywhere else in
    // the engine.
    const glm::vec3 forward{0.f, 0.f, 1.f};

    const glm::vec3 ahead = moveDirection({0.f, 1.f}, forward, up);
    CHECK(ahead.x == doctest::Approx(0.f));
    CHECK(ahead.z == doctest::Approx(1.f));

    // Right, for a Y-up frame, is cross(forward, up) = +X... which is -X for
    // this handedness. The number matters less than that it is perpendicular
    // and consistent with the engine's own right vector.
    const glm::vec3 right = moveDirection({1.f, 0.f}, forward, up);
    CHECK(glm::dot(right, ahead) == doctest::Approx(0.f));
    CHECK(glm::length(right) == doctest::Approx(1.f));

    // A camera pitched down still means forward along the ground: the
    // component along up is dropped, not projected into a dive.
    const glm::vec3 pitched =
        moveDirection({0.f, 1.f}, glm::normalize(glm::vec3{0.f, -1.f, 1.f}), up);
    CHECK(pitched.y == doctest::Approx(0.f));
    CHECK(pitched.z == doctest::Approx(1.f));

    // Half a stick is half a walk.
    CHECK(glm::length(moveDirection({0.f, 0.5f}, forward, up)) == doctest::Approx(0.5f));

    // Straight down has no forward on the ground to offer, and inventing one
    // would send the character somewhere nobody pointed.
    CHECK(glm::length(moveDirection({0.f, 1.f}, up, up)) == doctest::Approx(0.f));
}

TEST_CASE("the up axis is the opposite of gravity, and the demo's is -Y") {
    CHECK(upFromGravity({0.f, -9.81f, 0.f}).y == doctest::Approx(1.f));
    // The demo scene: things fall towards +Y, so up is -Y.
    CHECK(upFromGravity({0.f, 9.81f, 0.f}).y == doctest::Approx(-1.f));
    // No gravity, no opinion.
    CHECK(upFromGravity({0.f, 0.f, 0.f}).y == doctest::Approx(1.f));
}

TEST_CASE("a jump is specified as a height and solved as a speed") {
    // v = sqrt(2 g h): one metre against one gravity.
    CHECK(jumpSpeed(1.f, 9.81f) == doctest::Approx(std::sqrt(2.f * 9.81f)));
    // Four times the height is twice the speed - the relationship that makes
    // authoring in metres worth doing.
    CHECK(jumpSpeed(4.f, 9.81f) == doctest::Approx(2.f * jumpSpeed(1.f, 9.81f)));
    CHECK(jumpSpeed(0.f, 9.81f) == doctest::Approx(0.f));
    CHECK(jumpSpeed(1.f, 0.f) == doctest::Approx(0.f));
}

TEST_CASE("approach lands on its target rather than overshooting it") {
    // Within one step's reach: exactly the target, not past it and back.
    const glm::vec3 arrived = approach({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 100.f, step);
    CHECK(arrived.x == doctest::Approx(1.f));

    // Beyond it: one step's worth along the way, and the direction is the gap's.
    const glm::vec3 partway = approach({0.f, 0.f, 0.f}, {6.f, 0.f, 0.f}, 30.f, step);
    CHECK(partway.x == doctest::Approx(30.f * step));
    CHECK(partway.z == doctest::Approx(0.f));

    // Turning is the same operation: the velocity crosses through the middle
    // rather than swapping instantly.
    const glm::vec3 turning = approach({3.f, 0.f, 0.f}, {-3.f, 0.f, 0.f}, 30.f, step);
    CHECK(turning.x == doctest::Approx(3.f - 30.f * step));
}

TEST_CASE("turning takes the short way round and stays bounded") {
    // Just past pi, turning towards just before -pi: the short way is
    // forwards through the wrap - a twentieth of a radian - rather than
    // backwards the long way round, which is six.
    const float turned = turnToward(3.1f, -3.1f, 0.05f);
    CHECK(turned == doctest::Approx(3.15f - glm::two_pi<float>()));
    // And the result comes back inside the range it went in with, so a
    // character turning in circles never drifts out of it.
    CHECK(turned > -glm::pi<float>());
    CHECK(turned <= glm::pi<float>());

    // A turn that can be completed completes.
    CHECK(turnToward(0.f, 0.3f, 1.f) == doctest::Approx(0.3f));
    // One that cannot moves by exactly the budget, in the right direction.
    CHECK(turnToward(0.f, 3.f, 0.5f) == doctest::Approx(0.5f));
    CHECK(turnToward(0.f, -3.f, 0.5f) == doctest::Approx(-0.5f));
}

TEST_CASE("facing is the yaw the rest of the engine builds a forward from") {
    // forward = (sin yaw, 0, cos yaw), so +Z is a yaw of zero and +X a
    // quarter turn.
    CHECK(yawFromDirection({0.f, 0.f, 1.f}) == doctest::Approx(0.f));
    CHECK(yawFromDirection({1.f, 0.f, 0.f}) == doctest::Approx(glm::half_pi<float>()));
    CHECK(yawFromDirection({0.f, 0.f, -1.f}) == doctest::Approx(glm::pi<float>()));
    // Round trip: the yaw of a forward built from a yaw is that yaw.
    const float yaw = 0.9f;
    CHECK(yawFromDirection({std::sin(yaw), 0.f, std::cos(yaw)}) == doctest::Approx(yaw));
}

TEST_CASE("a character accelerates towards the speed asked for and stops there") {
    CharacterController controller{};
    controller.walkSpeed = 4.f;
    controller.acceleration = 20.f;
    controller.move = {0.f, 0.f, 1.f};

    const CharacterFrame frame = groundedFrame();

    // One step of acceleration, exactly: 20 m/s^2 for a sixtieth of a second.
    advanceCharacter(controller, frame, step);
    CHECK(planar(controller.velocity) == doctest::Approx(20.f * step));

    // Held long enough, it arrives at the walk speed and stays there rather
    // than accelerating forever.
    for (int i = 0; i < 120; i++) {
        controller.move = {0.f, 0.f, 1.f};
        advanceCharacter(controller, frame, step);
    }
    CHECK(planar(controller.velocity) == doctest::Approx(4.f));

    // Released, it brakes to a stop at the braking rate rather than coasting.
    controller.move = glm::vec3{0.f};
    for (int i = 0; i < 120; i++) {
        advanceCharacter(controller, frame, step);
    }
    CHECK(planar(controller.velocity) == doctest::Approx(0.f));
}

TEST_CASE("a diagonal is not faster than a straight line") {
    CharacterController controller{};
    controller.walkSpeed = 4.f;
    const CharacterFrame frame = groundedFrame();

    // Both axes at once, as a keyboard delivers them: a vector of length
    // root two, which must not become root two times the walk speed.
    for (int i = 0; i < 200; i++) {
        controller.move = {1.f, 0.f, 1.f};
        advanceCharacter(controller, frame, step);
    }
    CHECK(planar(controller.velocity) == doctest::Approx(4.f));
}

TEST_CASE("run speed replaces walk speed while run is held") {
    CharacterController controller{};
    controller.walkSpeed = 3.f;
    controller.runSpeed = 6.f;
    const CharacterFrame frame = groundedFrame();

    for (int i = 0; i < 200; i++) {
        controller.move = {0.f, 0.f, 1.f};
        controller.run = true;
        advanceCharacter(controller, frame, step);
    }
    CHECK(planar(controller.velocity) == doctest::Approx(6.f));
}

TEST_CASE("air control is a fraction of the acceleration on the ground") {
    CharacterController groundControl{};
    groundControl.acceleration = 20.f;
    groundControl.airControl = 0.25f;
    groundControl.move = {0.f, 0.f, 1.f};

    CharacterController airControl = groundControl;

    advanceCharacter(groundControl, groundedFrame(), step);
    advanceCharacter(airControl, airFrame(), step);

    CHECK(planar(airControl.velocity) == doctest::Approx(0.25f * planar(groundControl.velocity)));
}

TEST_CASE("standing on a moving platform means moving with it") {
    CharacterController controller{};
    controller.walkSpeed = 4.f;

    CharacterFrame frame = groundedFrame();
    frame.groundVelocity = {2.f, 0.f, 0.f};

    // Asking for nothing on a floor travelling at two metres per second
    // settles at two metres per second, not at a standstill the platform
    // slides out from under.
    for (int i = 0; i < 200; i++) {
        advanceCharacter(controller, frame, step);
    }
    CHECK(controller.velocity.x == doctest::Approx(2.f));
    CHECK(controller.velocity.z == doctest::Approx(0.f));
}

TEST_CASE("gravity accumulates in the air and is spent on landing") {
    CharacterController controller{};
    const CharacterFrame air = airFrame();

    for (int i = 0; i < 30; i++) {
        advanceCharacter(controller, air, step);
    }
    // Half a second of falling: -g t, to the accuracy of a fixed step.
    CHECK(controller.velocity.y == doctest::Approx(-9.81f * 30.f * step));

    // Landing spends it. Without this the character stands on the floor at
    // terminal velocity, and the first step off a ledge starts from there.
    advanceCharacter(controller, groundedFrame(), step);
    CHECK(controller.velocity.y == doctest::Approx(-9.81f * step));
}

TEST_CASE("a fall is capped at the terminal speed") {
    CharacterController controller{};
    controller.terminalSpeed = 12.f;
    const CharacterFrame air = airFrame();

    for (int i = 0; i < 600; i++) {
        advanceCharacter(controller, air, step);
    }
    CHECK(controller.velocity.y == doctest::Approx(-12.f));
}

TEST_CASE("a jump held reaches the height it was authored with") {
    CharacterController controller{};
    controller.jumpHeight = 1.2f;
    controller.jump = true;
    controller.jumpHeld = true;

    // The jump leaves the ground at exactly the speed the height needs.
    advanceCharacter(controller, groundedFrame(), step);
    CHECK(controller.jumped);
    CHECK(controller.velocity.y == doctest::Approx(jumpSpeed(1.2f, 9.81f)));

    // And rises to it, integrated a step at a time. A fixed step overshoots
    // the closed form slightly, which is why this is an epsilon rather than
    // an equality: the point is that the authored height is what comes out.
    float height = 0.f;
    const CharacterFrame air = airFrame();
    while (controller.velocity.y > 0.f) {
        height += controller.velocity.y * step;
        controller.jumpHeld = true;
        advanceCharacter(controller, air, step);
    }
    CHECK(height == doctest::Approx(1.2f).epsilon(0.02f));
}

TEST_CASE("a tapped jump is shorter than a held one") {
    const auto apex = [](bool held) {
        CharacterController controller{};
        controller.jumpHeight = 1.2f;
        controller.jumpCutGravity = 3.f;
        controller.jump = true;
        controller.jumpHeld = true;
        advanceCharacter(controller, groundedFrame(), step);

        float height = 0.f;
        const CharacterFrame air = airFrame();
        while (controller.velocity.y > 0.f) {
            height += controller.velocity.y * step;
            controller.jumpHeld = held;
            advanceCharacter(controller, air, step);
        }
        return height;
    };

    const float held = apex(true);
    const float tapped = apex(false);
    CHECK(tapped < held);
    // Three times the gravity on the way up is a third of the rise, give or
    // take the step the button was still held for.
    CHECK(tapped == doctest::Approx(held / 3.f).epsilon(0.05f));
}

TEST_CASE("one press is one jump") {
    CharacterController controller{};
    controller.jump = true;

    // The press is consumed by the step that acts on it, so a driver that
    // sets the field and forgets does not hold the character in the air.
    advanceCharacter(controller, groundedFrame(), step);
    CHECK(controller.jumped);
    CHECK_FALSE(controller.jump);

    advanceCharacter(controller, groundedFrame(), step);
    CHECK_FALSE(controller.jumped);
}

TEST_CASE("coyote time lets a jump work just after the ledge") {
    CharacterController controller{};
    controller.coyoteTime = 0.1f;

    // On the ground, then off it, then a jump five hundredths of a second
    // later - inside the grace.
    advanceCharacter(controller, groundedFrame(), step);
    const CharacterFrame air = airFrame();
    for (int i = 0; i < 3; i++) {
        advanceCharacter(controller, air, step);
    }
    controller.jump = true;
    advanceCharacter(controller, air, step);
    CHECK(controller.jumped);
}

TEST_CASE("coyote time runs out") {
    CharacterController controller{};
    controller.coyoteTime = 0.1f;

    advanceCharacter(controller, groundedFrame(), step);
    const CharacterFrame air = airFrame();
    for (int i = 0; i < 12; i++) {
        advanceCharacter(controller, air, step);
    }
    controller.jump = true;
    advanceCharacter(controller, air, step);
    CHECK_FALSE(controller.jumped);
}

TEST_CASE("a jump pressed just before landing is remembered") {
    CharacterController controller{};
    controller.jumpBuffer = 0.15f;
    controller.coyoteTime = 0.f;

    // Falling, and the button pressed while still in the air.
    const CharacterFrame air = airFrame();
    for (int i = 0; i < 30; i++) {
        advanceCharacter(controller, air, step);
    }
    controller.jump = true;
    advanceCharacter(controller, air, step);
    CHECK_FALSE(controller.jumped);

    // Landing a few steps later still jumps: the press waited.
    for (int i = 0; i < 4; i++) {
        advanceCharacter(controller, air, step);
    }
    advanceCharacter(controller, groundedFrame(), step);
    CHECK(controller.jumped);
}

TEST_CASE("a jump pressed too long before landing is forgotten") {
    CharacterController controller{};
    controller.jumpBuffer = 0.05f;
    controller.coyoteTime = 0.f;

    const CharacterFrame air = airFrame();
    controller.jump = true;
    advanceCharacter(controller, air, step);
    for (int i = 0; i < 10; i++) {
        advanceCharacter(controller, air, step);
    }
    advanceCharacter(controller, groundedFrame(), step);
    CHECK_FALSE(controller.jumped);
}

TEST_CASE("the body turns towards where it is going, at the rate it was given") {
    CharacterController controller{};
    controller.walkSpeed = 4.f;
    controller.turnRate = 2.f;
    controller.facing = 0.f;

    const CharacterFrame frame = groundedFrame();

    // Set off along +X, which is a quarter turn from the facing it starts
    // with. It must not arrive there in one step.
    controller.move = {1.f, 0.f, 0.f};
    advanceCharacter(controller, frame, step);
    CHECK(controller.facing == doctest::Approx(2.f * step));

    for (int i = 0; i < 200; i++) {
        controller.move = {1.f, 0.f, 0.f};
        advanceCharacter(controller, frame, step);
    }
    CHECK(controller.facing == doctest::Approx(glm::half_pi<float>()));
}

TEST_CASE("a character that is not moving does not spin") {
    CharacterController controller{};
    controller.facing = 1.f;
    const CharacterFrame frame = groundedFrame();

    for (int i = 0; i < 60; i++) {
        advanceCharacter(controller, frame, step);
    }
    CHECK(controller.facing == doctest::Approx(1.f));
}

TEST_CASE("facing can be turned off for a character that strafes") {
    CharacterController controller{};
    controller.faceMotion = false;
    controller.facing = 0.f;
    const CharacterFrame frame = groundedFrame();

    for (int i = 0; i < 60; i++) {
        controller.move = {1.f, 0.f, 0.f};
        advanceCharacter(controller, frame, step);
    }
    CHECK(controller.facing == doctest::Approx(0.f));
    CHECK(controller.velocity.x > 0.f);
}

TEST_CASE("nothing here assumes +Y is up") {
    // The demo scene's frame: gravity towards +Y, so up is -Y and a jump
    // travels towards smaller y.
    CharacterController controller{};
    controller.jumpHeight = 1.f;
    controller.walkSpeed = 4.f;

    CharacterFrame frame{};
    frame.up = upFromGravity({0.f, 9.81f, 0.f});
    frame.gravity = 9.81f;
    frame.grounded = true;
    frame.groundNormal = frame.up;

    controller.jump = true;
    controller.jumpHeld = true;
    controller.move = {0.f, 0.f, 1.f};
    advanceCharacter(controller, frame, step);

    CHECK(controller.velocity.y == doctest::Approx(-jumpSpeed(1.f, 9.81f)));
    CHECK(controller.jumped);

    // And the walk is still in the plane, which here is still XZ.
    CHECK(controller.velocity.z > 0.f);
    CHECK(controller.planarSpeed == doctest::Approx(controller.velocity.z));
}
