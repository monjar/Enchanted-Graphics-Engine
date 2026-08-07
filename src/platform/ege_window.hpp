#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <string>

namespace ege {

    class Window {
    public:
        Window(int w, int h, std::string name);
        ~Window();

        // Delete copy constructor and operator because we want the relation between Window and
        // glfWindow to be 1 to 1
        Window(const Window& other) = delete;
        Window& operator=(const Window&) = delete;

        bool shouldClose() { return glfwWindowShouldClose(window); }

        VkExtent2D getExtent() {
            return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        }

        void createWindowSurface(VkInstance instance, VkSurfaceKHR* surface);

        bool wasWindowResized() { return wasFrameBufferResized; }

        void resetWindowResizedFlag() { wasFrameBufferResized = false; }

        GLFWwindow* getGLFWwindow() const { return window; }

    private:
        static void frameBufferResizeCallBack(GLFWwindow* window, int width, int height);
        void initWindow();

        int width;
        int height;
        bool wasFrameBufferResized = false;

        std::string windowName;
        GLFWwindow* window;
    };

}  // namespace ege