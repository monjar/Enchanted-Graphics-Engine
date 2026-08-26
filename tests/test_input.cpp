// Input identifier mapping, and the stick arithmetic.
//
// platform/Input.cpp casts the engine's input enums straight to GLFW's
// integers rather than going through a lookup table. That is only correct
// while the two numberings agree, and nothing in the type system enforces it -
// so it is pinned here. If GLFW ever renumbers, or someone assigns a
// convenient value to a new enumerator, this fails instead of silently
// binding the wrong key - or, for a gamepad, the wrong button on every pad at
// once.
//
// Reading a live pad needs a pad, so what is left is the arithmetic between
// the reading and the action, which needs nothing.

#include "platform/Input.hpp"
#include "platform/Key.hpp"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <doctest/doctest.h>

using ege::applyStickDeadzone;
using ege::GamepadAxis;
using ege::gamepadAxisValue;
using ege::GamepadButton;
using ege::Key;
using ege::MouseButton;

namespace {

    constexpr int raw(Key key) {
        return static_cast<int>(key);
    }

    constexpr int raw(MouseButton button) {
        return static_cast<int>(button);
    }

    constexpr int raw(GamepadButton button) {
        return static_cast<int>(button);
    }

    constexpr int raw(GamepadAxis axis) {
        return static_cast<int>(axis);
    }

}  // namespace

TEST_CASE("letter and digit keys match GLFW's numbering") {
    CHECK(raw(Key::A) == GLFW_KEY_A);
    CHECK(raw(Key::D) == GLFW_KEY_D);
    CHECK(raw(Key::Q) == GLFW_KEY_Q);
    CHECK(raw(Key::S) == GLFW_KEY_S);
    CHECK(raw(Key::W) == GLFW_KEY_W);
    CHECK(raw(Key::E) == GLFW_KEY_E);
    CHECK(raw(Key::Z) == GLFW_KEY_Z);

    CHECK(raw(Key::Num0) == GLFW_KEY_0);
    CHECK(raw(Key::Num9) == GLFW_KEY_9);
}

TEST_CASE("navigation and modifier keys match GLFW's numbering") {
    CHECK(raw(Key::Space) == GLFW_KEY_SPACE);
    CHECK(raw(Key::Escape) == GLFW_KEY_ESCAPE);
    CHECK(raw(Key::Enter) == GLFW_KEY_ENTER);
    CHECK(raw(Key::Tab) == GLFW_KEY_TAB);

    CHECK(raw(Key::Left) == GLFW_KEY_LEFT);
    CHECK(raw(Key::Right) == GLFW_KEY_RIGHT);
    CHECK(raw(Key::Up) == GLFW_KEY_UP);
    CHECK(raw(Key::Down) == GLFW_KEY_DOWN);

    CHECK(raw(Key::LeftShift) == GLFW_KEY_LEFT_SHIFT);
    CHECK(raw(Key::LeftControl) == GLFW_KEY_LEFT_CONTROL);
    CHECK(raw(Key::LeftAlt) == GLFW_KEY_LEFT_ALT);

    CHECK(raw(Key::F1) == GLFW_KEY_F1);
    CHECK(raw(Key::F12) == GLFW_KEY_F12);
}

TEST_CASE("mouse buttons match GLFW's numbering") {
    CHECK(raw(MouseButton::Left) == GLFW_MOUSE_BUTTON_LEFT);
    CHECK(raw(MouseButton::Right) == GLFW_MOUSE_BUTTON_RIGHT);
    CHECK(raw(MouseButton::Middle) == GLFW_MOUSE_BUTTON_MIDDLE);
}

TEST_CASE("gamepad buttons match GLFW's mapping") {
    // These are not the pad's own numbering - they are the layout GLFW's
    // controller database maps every recognised pad onto, which is the whole
    // reason `A` means the bottom face button rather than button zero.
    CHECK(raw(GamepadButton::A) == GLFW_GAMEPAD_BUTTON_A);
    CHECK(raw(GamepadButton::B) == GLFW_GAMEPAD_BUTTON_B);
    CHECK(raw(GamepadButton::X) == GLFW_GAMEPAD_BUTTON_X);
    CHECK(raw(GamepadButton::Y) == GLFW_GAMEPAD_BUTTON_Y);
    CHECK(raw(GamepadButton::LeftBumper) == GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
    CHECK(raw(GamepadButton::RightBumper) == GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
    CHECK(raw(GamepadButton::Back) == GLFW_GAMEPAD_BUTTON_BACK);
    CHECK(raw(GamepadButton::Start) == GLFW_GAMEPAD_BUTTON_START);
    CHECK(raw(GamepadButton::Guide) == GLFW_GAMEPAD_BUTTON_GUIDE);
    CHECK(raw(GamepadButton::LeftThumb) == GLFW_GAMEPAD_BUTTON_LEFT_THUMB);
    CHECK(raw(GamepadButton::RightThumb) == GLFW_GAMEPAD_BUTTON_RIGHT_THUMB);
    CHECK(raw(GamepadButton::DpadUp) == GLFW_GAMEPAD_BUTTON_DPAD_UP);
    CHECK(raw(GamepadButton::DpadRight) == GLFW_GAMEPAD_BUTTON_DPAD_RIGHT);
    CHECK(raw(GamepadButton::DpadDown) == GLFW_GAMEPAD_BUTTON_DPAD_DOWN);
    CHECK(raw(GamepadButton::DpadLeft) == GLFW_GAMEPAD_BUTTON_DPAD_LEFT);
}

TEST_CASE("gamepad axes match GLFW's mapping") {
    CHECK(raw(GamepadAxis::LeftX) == GLFW_GAMEPAD_AXIS_LEFT_X);
    CHECK(raw(GamepadAxis::LeftY) == GLFW_GAMEPAD_AXIS_LEFT_Y);
    CHECK(raw(GamepadAxis::RightX) == GLFW_GAMEPAD_AXIS_RIGHT_X);
    CHECK(raw(GamepadAxis::RightY) == GLFW_GAMEPAD_AXIS_RIGHT_Y);
    CHECK(raw(GamepadAxis::LeftTrigger) == GLFW_GAMEPAD_AXIS_LEFT_TRIGGER);
    CHECK(raw(GamepadAxis::RightTrigger) == GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

TEST_CASE("the tracked ranges cover every identifier the engine names") {
    // Input indexes fixed-size arrays by these values, so Count must exceed
    // every enumerator or the assert in indexOf fires - or worse, in a release
    // build, the array is written out of bounds.
    CHECK(raw(Key::Count) > raw(Key::RightSuper));
    CHECK(raw(Key::Count) > GLFW_KEY_LAST - 1);
    CHECK(raw(MouseButton::Count) > raw(MouseButton::Button5));
    CHECK(raw(MouseButton::Count) > GLFW_MOUSE_BUTTON_LAST);
    CHECK(raw(GamepadButton::Count) > GLFW_GAMEPAD_BUTTON_LAST);
    CHECK(raw(GamepadAxis::Count) > GLFW_GAMEPAD_AXIS_LAST);
}

// ---- Stick arithmetic -----------------------------------------------------
//
// The part of gamepad handling that is a function of numbers rather than of
// a device, and therefore the part CI can check on a machine with nothing
// plugged into it.

TEST_CASE("a radial deadzone rescales what is left of the stick") {
    // Inside: nothing at all, however the components are distributed.
    CHECK(glm::length(applyStickDeadzone({0.1f, 0.1f}, 0.2f)) == doctest::Approx(0.f));

    // Just outside: a crawl, not a jump to a fifth of walking speed. Half way
    // from the threshold to full deflection is half output.
    const glm::vec2 half = applyStickDeadzone({0.6f, 0.f}, 0.2f);
    CHECK(half.x == doctest::Approx(0.5f));
    CHECK(half.y == doctest::Approx(0.f));

    // Full deflection still reaches one: a deadzone must not also cost the
    // player their top speed.
    CHECK(glm::length(applyStickDeadzone({1.f, 0.f}, 0.2f)) == doctest::Approx(1.f));

    // Radial, not per-axis: a stick pushed into a corner is at magnitude one
    // and comes back out at magnitude one, along the diagonal it went in on.
    const glm::vec2 diagonal = applyStickDeadzone({0.70710678f, 0.70710678f}, 0.2f);
    CHECK(glm::length(diagonal) == doctest::Approx(1.f));
    CHECK(diagonal.x == doctest::Approx(diagonal.y));

    // A deadzone of zero passes the stick through rather than dividing by it.
    CHECK(applyStickDeadzone({0.3f, 0.f}, 0.f).x == doctest::Approx(0.3f));
}

TEST_CASE("an axis bound as an action carries its sign and its threshold") {
    // One stick axis, two opposed actions: the half of its travel that
    // matches the scale's sign is the only half that answers.
    CHECK(gamepadAxisValue(-0.8f, -1.f, 0.25f) == doctest::Approx(0.8f));
    CHECK(gamepadAxisValue(-0.8f, 1.f, 0.25f) == doctest::Approx(0.f));
    CHECK(gamepadAxisValue(0.8f, 1.f, 0.25f) == doctest::Approx(0.8f));

    // Below the threshold is nothing rather than a little: this is what stops
    // a worn stick walking the character across the room while nobody is
    // holding it.
    CHECK(gamepadAxisValue(0.2f, 1.f, 0.25f) == doctest::Approx(0.f));
    // At it exactly is something, so a threshold names the first value that
    // counts rather than the last that does not.
    CHECK(gamepadAxisValue(0.25f, 1.f, 0.25f) == doctest::Approx(0.25f));

    // A trigger resting at -1 reads as nothing, and pressed fully as one -
    // never more, however the pad reports it.
    CHECK(gamepadAxisValue(-1.f, 1.f, 0.15f) == doctest::Approx(0.f));
    CHECK(gamepadAxisValue(1.f, 1.f, 0.15f) == doctest::Approx(1.f));
    CHECK(gamepadAxisValue(1.4f, 1.f, 0.15f) == doctest::Approx(1.f));
}
