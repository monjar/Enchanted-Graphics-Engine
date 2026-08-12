#include "platform/CameraController.hpp"

#include "platform/Input.hpp"

#include <glm/gtc/constants.hpp>

#include <limits>

namespace ege {

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
    }

    void CameraController::update(Input& input, float dt, GameObject& viewer) {
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
            viewer.transform.rotation += lookSpeed * dt * glm::normalize(rotate);
        }

        if (looking) {
            const glm::vec2 delta = input.mouseDelta();
            // Screen Y grows downward while pitch grows upward, hence the sign.
            viewer.transform.rotation.x -= delta.y * mouseSensitivity;
            viewer.transform.rotation.y += delta.x * mouseSensitivity;
        }

        viewer.transform.rotation.x =
            glm::clamp(viewer.transform.rotation.x, -pitchLimit, pitchLimit);
        // Keep yaw bounded so it cannot drift into the range where float
        // precision starts to visibly quantise the rotation.
        viewer.transform.rotation.y = glm::mod(viewer.transform.rotation.y, glm::two_pi<float>());

        // ---- Move ----------------------------------------------------------

        const float yaw = viewer.transform.rotation.y;
        const glm::vec3 forward{glm::sin(yaw), 0.f, glm::cos(yaw)};
        const glm::vec3 right{forward.z, 0.f, -forward.x};
        // This scene treats -Y as up.
        const glm::vec3 up{0.f, -1.f, 0.f};

        glm::vec3 direction{0.f};
        direction += forward * input.axis("MoveBackward", "MoveForward");
        direction += right * input.axis("MoveLeft", "MoveRight");
        direction += up * input.axis("MoveDown", "MoveUp");

        if (glm::dot(direction, direction) > std::numeric_limits<float>::epsilon()) {
            viewer.transform.translation += moveSpeed * dt * glm::normalize(direction);
        }
    }

}  // namespace ege
