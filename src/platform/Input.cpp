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

        std::size_t indexOf(GamepadButton button) {
            const auto raw = static_cast<std::int32_t>(button);
            EGE_ASSERT(
                raw >= 0 && raw < static_cast<std::int32_t>(GamepadButton::Count),
                "gamepad button {} is outside the tracked range",
                raw);
            return static_cast<std::size_t>(raw);
        }

        std::size_t indexOf(GamepadAxis axis) {
            const auto raw = static_cast<std::int32_t>(axis);
            EGE_ASSERT(
                raw >= 0 && raw < static_cast<std::int32_t>(GamepadAxis::Count),
                "gamepad axis {} is outside the tracked range",
                raw);
            return static_cast<std::size_t>(raw);
        }

        bool padInRange(int pad) {
            return pad >= 0 && pad < Input::maxGamepads;
        }

    }  // namespace

    glm::vec2 applyStickDeadzone(glm::vec2 stick, float deadzone) {
        const float magnitude = glm::length(stick);
        if (magnitude <= deadzone || magnitude <= 0.f) {
            return glm::vec2{0.f};
        }
        const float usable = std::max(1.f - deadzone, 1e-6f);
        const float scaled = std::min((magnitude - deadzone) / usable, 1.f);
        return stick * (scaled / magnitude);
    }

    float gamepadAxisValue(float raw, float scale, float threshold) {
        const float value = std::clamp(raw * scale, 0.f, 1.f);
        return value >= threshold ? value : 0.f;
    }

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

        pollGamepads();
    }

    void Input::pollGamepads() {
        padsBefore = padsNow;

        for (int pad = 0; pad < maxGamepads; pad++) {
            GamepadState& state = padsNow[static_cast<std::size_t>(pad)];
            state = GamepadState{};

            const int joystick = GLFW_JOYSTICK_1 + pad;
            GLFWgamepadstate raw{};
            // Both checks: a joystick GLFW has no mapping for is not a
            // gamepad, and asking for its state would report an order nobody
            // can interpret.
            if (glfwJoystickIsGamepad(joystick) == GLFW_FALSE ||
                glfwGetGamepadState(joystick, &raw) == GLFW_FALSE) {
                continue;
            }

            state.connected = true;
            for (std::size_t i = 0; i < state.buttons.size(); i++) {
                state.buttons[i] = raw.buttons[i] == GLFW_PRESS;
            }
            for (std::size_t i = 0; i < state.axes.size(); i++) {
                state.axes[i] = raw.axes[i];
            }
        }
    }

    bool Input::gamepadConnected(int pad) const {
        return padInRange(pad) && padsNow[static_cast<std::size_t>(pad)].connected;
    }

    int Input::gamepadCount() const {
        return static_cast<int>(
            std::count_if(padsNow.begin(), padsNow.end(), [](const GamepadState& state) {
                return state.connected;
            }));
    }

    bool Input::isDown(GamepadButton button, int pad) const {
        return padInRange(pad) && padsNow[static_cast<std::size_t>(pad)].buttons[indexOf(button)];
    }

    bool Input::wasPressed(GamepadButton button, int pad) const {
        if (!padInRange(pad)) {
            return false;
        }
        const std::size_t index = indexOf(button);
        const auto slot = static_cast<std::size_t>(pad);
        return padsNow[slot].buttons[index] && !padsBefore[slot].buttons[index];
    }

    bool Input::wasReleased(GamepadButton button, int pad) const {
        if (!padInRange(pad)) {
            return false;
        }
        const std::size_t index = indexOf(button);
        const auto slot = static_cast<std::size_t>(pad);
        return !padsNow[slot].buttons[index] && padsBefore[slot].buttons[index];
    }

    float Input::gamepadAxis(GamepadAxis axis, int pad) const {
        return padInRange(pad) ? padsNow[static_cast<std::size_t>(pad)].axes[indexOf(axis)] : 0.f;
    }

    glm::vec2 Input::stickOf(GamepadAxis x, GamepadAxis y, int pad) const {
        // Y is negated here and nowhere else: GLFW reports -1 at the top of a
        // stick, and every caller in the engine means forward when it says
        // positive.
        return applyStickDeadzone({gamepadAxis(x, pad), -gamepadAxis(y, pad)}, stickDeadzone);
    }

    glm::vec2 Input::leftStick(int pad) const {
        return stickOf(GamepadAxis::LeftX, GamepadAxis::LeftY, pad);
    }

    glm::vec2 Input::rightStick(int pad) const {
        return stickOf(GamepadAxis::RightX, GamepadAxis::RightY, pad);
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

    void Input::bindAction(std::string name, GamepadButton button) {
        actions[std::move(name)].padButtons.push_back(button);
    }

    void Input::bindAction(std::string name, GamepadAxis axis, float scale, float threshold) {
        actions[std::move(name)].padAxes.push_back(AxisBinding{axis, scale, threshold});
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
        if (std::any_of(
                binding->keys.begin(), binding->keys.end(), [this](Key k) { return isDown(k); })) {
            return true;
        }
        if (std::any_of(binding->buttons.begin(), binding->buttons.end(), [this](MouseButton b) {
                return isDown(b);
            })) {
            return true;
        }
        // Any connected pad, so a scene that binds Jump once is playable by
        // whoever picks up a controller.
        for (int pad = 0; pad < maxGamepads; pad++) {
            if (!gamepadConnected(pad)) {
                continue;
            }
            if (std::any_of(
                    binding->padButtons.begin(),
                    binding->padButtons.end(),
                    [this, pad](GamepadButton b) { return isDown(b, pad); })) {
                return true;
            }
            if (std::any_of(
                    binding->padAxes.begin(),
                    binding->padAxes.end(),
                    [this, pad](const AxisBinding& axisBinding) {
                        return gamepadAxisValue(
                                   gamepadAxis(axisBinding.axis, pad),
                                   axisBinding.scale,
                                   axisBinding.threshold) > 0.f;
                    })) {
                return true;
            }
        }
        return false;
    }

    bool Input::wasActionPressed(std::string_view name) const {
        const Binding* binding = findBinding(name);
        if (binding == nullptr) {
            return false;
        }
        if (std::any_of(binding->keys.begin(), binding->keys.end(), [this](Key k) {
                return wasPressed(k);
            })) {
            return true;
        }
        if (std::any_of(binding->buttons.begin(), binding->buttons.end(), [this](MouseButton b) {
                return wasPressed(b);
            })) {
            return true;
        }
        for (int pad = 0; pad < maxGamepads; pad++) {
            if (!gamepadConnected(pad)) {
                continue;
            }
            if (std::any_of(
                    binding->padButtons.begin(),
                    binding->padButtons.end(),
                    [this, pad](GamepadButton b) { return wasPressed(b, pad); })) {
                return true;
            }
            // A trigger crossing its threshold is a press, which is what
            // makes "jump on the right trigger" work at all. Compared
            // against the same threshold on the previous frame rather than
            // against zero, or a trigger held halfway would press every
            // frame it wandered by a hundredth.
            for (const AxisBinding& axisBinding : binding->padAxes) {
                const auto slot = static_cast<std::size_t>(pad);
                const float now = gamepadAxisValue(
                    padsNow[slot].axes[indexOf(axisBinding.axis)],
                    axisBinding.scale,
                    axisBinding.threshold);
                const float before = gamepadAxisValue(
                    padsBefore[slot].axes[indexOf(axisBinding.axis)],
                    axisBinding.scale,
                    axisBinding.threshold);
                if (now > 0.f && before <= 0.f) {
                    return true;
                }
            }
        }
        return false;
    }

    float Input::actionValue(std::string_view name) const {
        const Binding* binding = findBinding(name);
        if (binding == nullptr) {
            return 0.f;
        }
        if (std::any_of(
                binding->keys.begin(), binding->keys.end(), [this](Key k) { return isDown(k); })) {
            return 1.f;
        }
        if (std::any_of(binding->buttons.begin(), binding->buttons.end(), [this](MouseButton b) {
                return isDown(b);
            })) {
            return 1.f;
        }

        float value = 0.f;
        for (int pad = 0; pad < maxGamepads; pad++) {
            if (!gamepadConnected(pad)) {
                continue;
            }
            for (const GamepadButton button : binding->padButtons) {
                if (isDown(button, pad)) {
                    return 1.f;
                }
            }
            for (const AxisBinding& axisBinding : binding->padAxes) {
                // The largest wins rather than the sum: a hand on the stick
                // and a hand on the keyboard is one player asking for one
                // thing, not two asking for twice as much.
                value = std::max(
                    value,
                    gamepadAxisValue(
                        gamepadAxis(axisBinding.axis, pad),
                        axisBinding.scale,
                        axisBinding.threshold));
            }
        }
        return value;
    }

    float Input::axis(std::string_view negative, std::string_view positive) const {
        return actionValue(positive) - actionValue(negative);
    }

}  // namespace ege
