#include "platform/CameraController.hpp"

#include "platform/Input.hpp"

#include <glm/gtc/constants.hpp>

#include <limits>

namespace ege {

    namespace {

        // At most a unit vector, and no more than what was asked for.
        //
        // Normalising instead - which this used to do - is right for the two
        // keys of a diagonal, which arrive as a vector of length root two and
        // must not outrun a straight line. It is wrong for a stick, which
        // arrives at whatever length the player pushed it to and means it.
        glm::vec3 clampToUnit(glm::vec3 direction) {
            const float length = glm::length(direction);
            return length > 1.f ? direction / length : direction;
        }

    }  // namespace

    void CameraController::registerDefaultActions(Input& input) {
        input.bindAction("MoveForward", Key::W);
        input.bindAction("MoveBackward", Key::S);
        input.bindAction("MoveLeft", Key::A);
        input.bindAction("MoveRight", Key::D);
        input.bindAction("MoveUp", Key::E);
        input.bindAction("MoveDown", Key::Q);

        input.bindAction("LookUp", Key::Up);
        input.bindAction("LookDown", Key::Down);
        input.bindAction("LookLeft", Key::Left);
        input.bindAction("LookRight", Key::Right);

        // Hold to look with the mouse; the cursor is captured while held.
        input.bindAction("Look", MouseButton::Right);

        // And the same actions on a pad, which is the point of binding by
        // name: nothing that reads these learns that a controller exists.
        // A stick axis becomes two opposed actions through the sign of its
        // scale - GLFW reports -1 at the top of a stick and at its left.
        input.bindAction("MoveForward", GamepadAxis::LeftY, -1.f);
        input.bindAction("MoveBackward", GamepadAxis::LeftY, 1.f);
        input.bindAction("MoveLeft", GamepadAxis::LeftX, -1.f);
        input.bindAction("MoveRight", GamepadAxis::LeftX, 1.f);
        // Triggers rise, bumpers fall: up and down where a thumb expects them.
        input.bindAction("MoveUp", GamepadAxis::RightTrigger, 1.f, 0.15f);
        input.bindAction("MoveDown", GamepadAxis::LeftTrigger, 1.f, 0.15f);

        input.bindAction("LookUp", GamepadAxis::RightY, -1.f);
        input.bindAction("LookDown", GamepadAxis::RightY, 1.f);
        input.bindAction("LookLeft", GamepadAxis::RightX, -1.f);
        input.bindAction("LookRight", GamepadAxis::RightX, 1.f);
    }

    void CameraController::update(Input& input, float dt, Transform& viewer) {
        // Capture the cursor only while looking, so the window stays usable.
        const bool wantsLook = input.isActionDown("Look");
        if (wantsLook != looking) {
            looking = wantsLook;
            input.setCursorMode(looking ? CursorMode::Captured : CursorMode::Normal);
        }

        // ---- Look ----------------------------------------------------------

        glm::vec3 rotate{0.f};
        rotate.x += input.axis("LookDown", "LookUp");
        rotate.y += input.axis("LookLeft", "LookRight");

        if (glm::dot(rotate, rotate) > std::numeric_limits<float>::epsilon()) {
            viewer.rotation += lookSpeed * dt * clampToUnit(rotate);
        }

        if (looking) {
            const glm::vec2 delta = input.mouseDelta();
            // Screen Y grows downward while pitch grows upward, hence the sign.
            viewer.rotation.x -= delta.y * mouseSensitivity;
            viewer.rotation.y += delta.x * mouseSensitivity;
        }

        viewer.rotation.x = glm::clamp(viewer.rotation.x, -pitchLimit, pitchLimit);
        // Keep yaw bounded so it cannot drift into the range where float
        // precision starts to visibly quantise the rotation.
        viewer.rotation.y = glm::mod(viewer.rotation.y, glm::two_pi<float>());

        // ---- Move ----------------------------------------------------------

        const float yaw = viewer.rotation.y;
        const glm::vec3 forward{glm::sin(yaw), 0.f, glm::cos(yaw)};
        const glm::vec3 right{forward.z, 0.f, -forward.x};
        // This scene treats -Y as up.
        const glm::vec3 up{0.f, -1.f, 0.f};

        glm::vec3 direction{0.f};
        direction += forward * input.axis("MoveBackward", "MoveForward");
        direction += right * input.axis("MoveLeft", "MoveRight");
        direction += up * input.axis("MoveDown", "MoveUp");

        if (glm::dot(direction, direction) > std::numeric_limits<float>::epsilon()) {
            viewer.translation += moveSpeed * dt * clampToUnit(direction);
        }
    }

}  // namespace ege
