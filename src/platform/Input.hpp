#pragma once

#include "platform/Key.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ege {

    class Window;

    // A radial deadzone that rescales what is left, so the first millimetre
    // past the threshold is a crawl rather than a jump to a fifth of walking
    // speed - and so that full deflection still reaches one, because a
    // deadzone must not also cost the player their top speed.
    //
    // Radial rather than per-axis: a per-axis deadzone squares off the circle
    // a stick actually moves in, and diagonals arrive short.
    //
    // A free function rather than a method because it is arithmetic, and
    // arithmetic can be tested without a window.
    glm::vec2 applyStickDeadzone(glm::vec2 stick, float deadzone);

    // What an axis contributes to an action bound to it: the raw reading
    // scaled - which is how one axis becomes two opposed actions, and how a
    // trigger resting at -1 becomes one that rests at zero - clamped to
    // [0, 1], and refused entirely below `threshold`.
    float gamepadAxisValue(float raw, float scale, float threshold);

    // Input state for the current frame.
    //
    // Replaces polling glfwGetKey directly from gameplay code, which had three
    // problems: GLFW types leaked everywhere, only held-down state was
    // available so nothing could react to a press or a release, and there was
    // no mouse input at all - the camera turned with the arrow keys.
    //
    // State is double-buffered: newFrame() copies current to previous and then
    // pulls fresh values, which is what makes edge detection possible.
    class Input {
    public:
        explicit Input(Window& window);

        // Call once per frame, before anything reads input, and after the
        // platform has been polled.
        void newFrame();

        // Called by Window's scroll callback. Scroll cannot be polled.
        void onScroll(double x, double y);

        // ---- Keys ---------------------------------------------------------

        bool isDown(Key key) const;

        // True only on the frame the key went down or came up.
        bool wasPressed(Key key) const;
        bool wasReleased(Key key) const;

        // ---- Mouse --------------------------------------------------------

        bool isDown(MouseButton button) const;
        bool wasPressed(MouseButton button) const;
        bool wasReleased(MouseButton button) const;

        glm::vec2 mousePosition() const { return currentMousePosition; }

        // Movement since the previous frame, in pixels. Zero on the first frame
        // after the cursor is captured, so that the jump from wherever the
        // cursor happened to be does not fling the camera.
        glm::vec2 mouseDelta() const { return currentMouseDelta; }

        // Accumulated scroll for this frame.
        glm::vec2 scrollDelta() const { return currentScrollDelta; }

        void setCursorMode(CursorMode mode);

        CursorMode cursorMode() const { return currentCursorMode; }

        // ---- Gamepads -----------------------------------------------------
        //
        // Four of them, because four is a couch. GLFW will report sixteen
        // joysticks; the ones past this are not tracked rather than being
        // silently mapped onto a lower slot, which would let a fifth player
        // drive a fourth character.
        //
        // Only pads GLFW recognises as *gamepads* are read. A joystick with
        // no mapping in its database has axes and buttons in an order nobody
        // can guess, and guessing is how a flight stick ends up walking the
        // character sideways.

        static constexpr int maxGamepads = 4;

        bool gamepadConnected(int pad = 0) const;

        // How many of the four slots have a recognised pad in them.
        int gamepadCount() const;

        bool isDown(GamepadButton button, int pad = 0) const;
        bool wasPressed(GamepadButton button, int pad = 0) const;
        bool wasReleased(GamepadButton button, int pad = 0) const;

        // The reading GLFW gives, untouched: sticks -1 at left and at up,
        // triggers -1 at rest. For movement, prefer the stick accessors
        // below, which flip Y and apply the deadzone.
        float gamepadAxis(GamepadAxis axis, int pad = 0) const;

        // A stick as a direction: +x right, +y forward, magnitude at most
        // one, dead below `stickDeadzone`.
        glm::vec2 leftStick(int pad = 0) const;
        glm::vec2 rightStick(int pad = 0) const;

        // How far a stick must move before it counts. Public because the
        // right value is a matter of what pad is in whose hands, and the
        // person holding it is the one who knows.
        float stickDeadzone = 0.2f;

        // ---- Actions ------------------------------------------------------
        //
        // Named actions rather than raw keys at the call site, so bindings can
        // be changed - eventually by the player - without touching the code
        // that reacts to them.

        void bindAction(std::string name, Key key);
        void bindAction(std::string name, MouseButton button);
        void bindAction(std::string name, GamepadButton button);

        // An axis bound as an action. `scale` is how the axis becomes this
        // action - -1 turns a stick's forward half into "MoveForward" while
        // +1 turns its other half into "MoveBackward", and a trigger's +1
        // leaves it as it is - and `threshold` is how far it has to go before
        // the action counts as pressed at all.
        void bindAction(
            std::string name, GamepadAxis axis, float scale = 1.f, float threshold = 0.25f);

        // An action is down if any of its bindings is down. A gamepad
        // binding is down if it is down on *any* connected pad, so a scene
        // that binds Jump once is playable by whoever picks up a controller.
        bool isActionDown(std::string_view name) const;
        bool wasActionPressed(std::string_view name) const;

        // How hard an action is being asked for, from 0 to 1. A key or a
        // button is all or nothing; an axis is as far as it has been pushed.
        // The largest of the action's bindings wins, so a hand on the
        // keyboard and a hand on the stick do not add up to double speed.
        float actionValue(std::string_view name) const;

        // Signed pair of actions, for movement: the difference of their
        // values, so digital bindings still give exactly +1, -1 or 0 and
        // analog ones give what the player is actually asking for.
        float axis(std::string_view negative, std::string_view positive) const;

    private:
        struct AxisBinding {
            GamepadAxis axis = GamepadAxis::LeftX;
            float scale = 1.f;
            float threshold = 0.25f;
        };

        struct Binding {
            std::vector<Key> keys;
            std::vector<MouseButton> buttons;
            std::vector<GamepadButton> padButtons;
            std::vector<AxisBinding> padAxes;
        };

        const Binding* findBinding(std::string_view name) const;

        Window& window;

        std::array<bool, static_cast<std::size_t>(Key::Count)> keysNow{};
        std::array<bool, static_cast<std::size_t>(Key::Count)> keysBefore{};
        std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttonsNow{};
        std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttonsBefore{};

        // One pad's readings for a frame. Disconnected reads as all zero,
        // which also means a pad unplugged mid-frame releases whatever it was
        // holding rather than leaving a character running forever.
        struct GamepadState {
            std::array<bool, static_cast<std::size_t>(GamepadButton::Count)> buttons{};
            std::array<float, static_cast<std::size_t>(GamepadAxis::Count)> axes{};
            bool connected = false;
        };

        void pollGamepads();

        glm::vec2 stickOf(GamepadAxis x, GamepadAxis y, int pad) const;

        std::array<GamepadState, maxGamepads> padsNow{};
        std::array<GamepadState, maxGamepads> padsBefore{};

        glm::vec2 currentMousePosition{0.f};
        glm::vec2 previousMousePosition{0.f};
        glm::vec2 currentMouseDelta{0.f};
        glm::vec2 currentScrollDelta{0.f};
        glm::vec2 pendingScroll{0.f};
        bool hasMouseReference = false;

        CursorMode currentCursorMode = CursorMode::Normal;

        std::unordered_map<std::string, Binding> actions;
    };

}  // namespace ege
