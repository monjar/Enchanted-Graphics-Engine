#pragma once

#include "scene/GameObject.hpp"

namespace ege {

    class Input;

    // Free-fly camera driven by named actions.
    //
    // Replaces KeyboardMovementController, which polled glfwGetKey directly
    // against a raw GLFWwindow*, offered no mouse input at all, and turned the
    // camera with the arrow keys.
    //
    // Movement is in the XZ plane plus a vertical axis, always relative to
    // where the camera is facing. Looking is either mouse-driven while the
    // cursor is captured, or on the arrow keys, so the old bindings still work.
    class CameraController {
    public:
        // Registers the default bindings. Call once, before update().
        static void registerDefaultActions(Input& input);

        void update(Input& input, float dt, GameObject& viewer);

        float moveSpeed = 3.f;
        float lookSpeed = 1.5f;
        // Radians per pixel of mouse movement.
        float mouseSensitivity = 0.0025f;
        // Just under a right angle, so looking straight up or down cannot flip
        // the camera through the pole.
        float pitchLimit = 1.5f;

    private:
        bool looking = false;
    };

}  // namespace ege
