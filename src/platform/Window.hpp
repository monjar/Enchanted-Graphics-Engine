#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <memory>
#include <string>

namespace ege {

    class Input;

    class Window {
    public:
        Window(int w, int h, std::string name);
        // Defined out of line: Input is incomplete here.
        ~Window();

        // Delete copy constructor and operator because we want the relation between Window and
        // glfWindow to be 1 to 1
        Window(const Window& other) = delete;
        Window& operator=(const Window&) = delete;

        bool shouldClose() { return glfwWindowShouldClose(window); }

        // Drains the platform event queue. Call once per frame before reading
        // input.
        static void pollEvents() { glfwPollEvents(); }

        // Blocks until an event arrives. Used while minimised, so a zero-sized
        // window does not spin the CPU redrawing nothing.
        static void waitForEvents() { glfwWaitEvents(); }

        VkExtent2D getExtent() {
            return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        }

        void createWindowSurface(VkInstance instance, VkSurfaceKHR* surface);

        bool wasWindowResized() { return wasFrameBufferResized; }

        void resetWindowResizedFlag() { wasFrameBufferResized = false; }

        GLFWwindow* getGLFWwindow() const { return window; }

        // Input is per-window, and the window owns the GLFW user pointer that
        // the callbacks need, so it owns the input state too.
        Input& input() { return *inputState; }

    private:
        static void frameBufferResizeCallBack(GLFWwindow* window, int width, int height);
        static void scrollCallBack(GLFWwindow* window, double x, double y);
        void initWindow();

        int width;
        int height;
        bool wasFrameBufferResized = false;

        std::string windowName;
        GLFWwindow* window;
        std::unique_ptr<Input> inputState;
    };

}  // namespace ege