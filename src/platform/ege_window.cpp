#include "platform/ege_window.hpp"

#include <iostream>
#include <stdexcept>

namespace ege {

    Window::Window(int w, int h, std::string name) : width{w}, height{h}, windowName{name} {
        initWindow();
    }

    Window::~Window() {
        glfwDestroyWindow(window);
        glfwTerminate();
    }

    void Window::initWindow() {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        window = glfwCreateWindow(width, height, windowName.c_str(), nullptr, nullptr);

        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, frameBufferResizeCallBack);
    }

    void Window::createWindowSurface(VkInstance instance, VkSurfaceKHR* surface) {
        if (glfwCreateWindowSurface(instance, window, nullptr, surface) != VK_SUCCESS) {
            throw std::runtime_error("failed to create window surface");
        }
    }

    void Window::frameBufferResizeCallBack(GLFWwindow* glfwWindow, int width, int height) {
        auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
        self->wasFrameBufferResized = true;
        self->width = width;
        self->height = height;
    }
}  // namespace ege