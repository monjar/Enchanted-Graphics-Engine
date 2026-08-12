// Input identifier mapping.
//
// platform/Input.cpp casts ege::Key and ege::MouseButton straight to GLFW's
// integers rather than going through a lookup table. That is only correct
// while the two numberings agree, and nothing in the type system enforces it -
// so it is pinned here. If GLFW ever renumbers, or someone assigns a
// convenient value to a new enumerator, this fails instead of silently
// binding the wrong key.

#include "platform/Key.hpp"

#include <GLFW/glfw3.h>

#include <doctest/doctest.h>

using ege::Key;
using ege::MouseButton;

namespace {

    constexpr int raw(Key key) {
        return static_cast<int>(key);
    }

    constexpr int raw(MouseButton button) {
        return static_cast<int>(button);
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

TEST_CASE("the tracked ranges cover every identifier the engine names") {
    // Input indexes fixed-size arrays by these values, so Count must exceed
    // every enumerator or the assert in indexOf fires - or worse, in a release
    // build, the array is written out of bounds.
    CHECK(raw(Key::Count) > raw(Key::RightSuper));
    CHECK(raw(Key::Count) > GLFW_KEY_LAST - 1);
    CHECK(raw(MouseButton::Count) > raw(MouseButton::Button5));
    CHECK(raw(MouseButton::Count) > GLFW_MOUSE_BUTTON_LAST);
}
