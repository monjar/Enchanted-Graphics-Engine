#pragma once

#include "platform/Key.hpp"

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace ege {

    class Window;

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

        // ---- Actions ------------------------------------------------------
        //
        // Named actions rather than raw keys at the call site, so bindings can
        // be changed - eventually by the player - without touching the code
        // that reacts to them.

        void bindAction(std::string name, Key key);
        void bindAction(std::string name, MouseButton button);

        // An action is down if any of its bindings is down.
        bool isActionDown(std::string_view name) const;
        bool wasActionPressed(std::string_view name) const;

        // Signed pair of actions, for movement: +1 when positive is down, -1
        // when negative is, 0 when both or neither are.
        float axis(std::string_view negative, std::string_view positive) const;

    private:
        struct Binding {
            std::vector<Key> keys;
            std::vector<MouseButton> buttons;
        };

        const Binding* findBinding(std::string_view name) const;

        Window& window;

        std::array<bool, static_cast<std::size_t>(Key::Count)> keysNow{};
        std::array<bool, static_cast<std::size_t>(Key::Count)> keysBefore{};
        std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttonsNow{};
        std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttonsBefore{};

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
