#include "platform/Input.hpp"

#include "core/Assert.hpp"
#include "platform/Window.hpp"

#include <algorithm>

namespace ege {

    namespace {

        // This file is the only place that relies on ege::Key and ege::MouseButton
        // sharing GLFW's numbering. Everywhere else uses the engine enums.
        std::size_t indexOf(Key key) {
            const auto raw = static_cast<std::int32_t>(key);
            EGE_ASSERT(
                raw >= 0 && raw < static_cast<std::int32_t>(Key::Count),
                "key {} is outside the tracked range",
                raw);
            return static_cast<std::size_t>(raw);
        }

        std::size_t indexOf(MouseButton button) {
            const auto raw = static_cast<std::int32_t>(button);
            EGE_ASSERT(
                raw >= 0 && raw < static_cast<std::int32_t>(MouseButton::Count),
                "mouse button {} is outside the tracked range",
                raw);
            return static_cast<std::size_t>(raw);
        }

    }  // namespace

    Input::Input(Window& windowRef) : window{windowRef} {}

    void Input::onScroll(double x, double y) {
        // Scroll arrives only as events - unlike keys and buttons it cannot be
        // polled - so it is accumulated here and drained by newFrame().
        pendingScroll += glm::vec2{static_cast<float>(x), static_cast<float>(y)};
    }

    void Input::newFrame() {
        keysBefore = keysNow;
        buttonsBefore = buttonsNow;

        GLFWwindow* handle = window.getGLFWwindow();

        for (std::size_t i = 0; i < keysNow.size(); i++) {
            keysNow[i] = glfwGetKey(handle, static_cast<int>(i)) == GLFW_PRESS;
        }
        for (std::size_t i = 0; i < buttonsNow.size(); i++) {
            buttonsNow[i] = glfwGetMouseButton(handle, static_cast<int>(i)) == GLFW_PRESS;
        }

        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(handle, &x, &y);
        previousMousePosition = currentMousePosition;
        currentMousePosition = {static_cast<float>(x), static_cast<float>(y)};

        // Without this the first frame after capture reports the whole distance
        // from wherever the cursor happened to be, which snaps the camera.
        currentMouseDelta =
            hasMouseReference ? currentMousePosition - previousMousePosition : glm::vec2{0.f};
        hasMouseReference = true;

        currentScrollDelta = pendingScroll;
        pendingScroll = glm::vec2{0.f};
    }

    bool Input::isDown(Key key) const {
        return keysNow[indexOf(key)];
    }

    bool Input::wasPressed(Key key) const {
        const std::size_t index = indexOf(key);
        return keysNow[index] && !keysBefore[index];
    }

    bool Input::wasReleased(Key key) const {
        const std::size_t index = indexOf(key);
        return !keysNow[index] && keysBefore[index];
    }

    bool Input::isDown(MouseButton button) const {
        return buttonsNow[indexOf(button)];
    }

    bool Input::wasPressed(MouseButton button) const {
        const std::size_t index = indexOf(button);
        return buttonsNow[index] && !buttonsBefore[index];
    }

    bool Input::wasReleased(MouseButton button) const {
        const std::size_t index = indexOf(button);
        return !buttonsNow[index] && buttonsBefore[index];
    }

    void Input::setCursorMode(CursorMode mode) {
        if (mode == currentCursorMode) {
            return;
        }
        currentCursorMode = mode;

        int glfwMode = GLFW_CURSOR_NORMAL;
        switch (mode) {
            case CursorMode::Normal:
                glfwMode = GLFW_CURSOR_NORMAL;
                break;
            case CursorMode::Hidden:
                glfwMode = GLFW_CURSOR_HIDDEN;
                break;
            case CursorMode::Captured:
                glfwMode = GLFW_CURSOR_DISABLED;
                break;
        }
        glfwSetInputMode(window.getGLFWwindow(), GLFW_CURSOR, glfwMode);

        // Changing mode teleports the cursor, so drop the reference point and
        // let the next frame re-establish it rather than reporting the jump.
        hasMouseReference = false;
    }

    void Input::bindAction(std::string name, Key key) {
        actions[std::move(name)].keys.push_back(key);
    }

    void Input::bindAction(std::string name, MouseButton button) {
        actions[std::move(name)].buttons.push_back(button);
    }

    const Input::Binding* Input::findBinding(std::string_view name) const {
        // Heterogeneous lookup on unordered_map needs C++20, so construct.
        const auto found = actions.find(std::string{name});
        return found == actions.end() ? nullptr : &found->second;
    }

    bool Input::isActionDown(std::string_view name) const {
        const Binding* binding = findBinding(name);
        if (binding == nullptr) {
            return false;
        }
        const bool key = std::any_of(
            binding->keys.begin(), binding->keys.end(), [this](Key k) { return isDown(k); });
        return key ||
               std::any_of(binding->buttons.begin(), binding->buttons.end(), [this](MouseButton b) {
                   return isDown(b);
               });
    }

    bool Input::wasActionPressed(std::string_view name) const {
        const Binding* binding = findBinding(name);
        if (binding == nullptr) {
            return false;
        }
        const bool key = std::any_of(
            binding->keys.begin(), binding->keys.end(), [this](Key k) { return wasPressed(k); });
        return key ||
               std::any_of(binding->buttons.begin(), binding->buttons.end(), [this](MouseButton b) {
                   return wasPressed(b);
               });
    }

    float Input::axis(std::string_view negative, std::string_view positive) const {
        const float value =
            (isActionDown(positive) ? 1.f : 0.f) - (isActionDown(negative) ? 1.f : 0.f);
        return value;
    }

}  // namespace ege
