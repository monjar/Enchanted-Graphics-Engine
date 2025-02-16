#include"ege_window.hpp"
#include <stdexcept>
#include <iostream>
namespace ege {


	EgeWindow::EgeWindow(int w, int h, std::string name) : width{ w }, height{ h }, windowName{ name } {
		initWindow();
	}

	EgeWindow::~EgeWindow(){
		glfwDestroyWindow(window);
		glfwTerminate();
	}

	void EgeWindow::initWindow() {
		glfwInit();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

		window = glfwCreateWindow(width, height, windowName.c_str(), nullptr, nullptr);

		glfwSetWindowUserPointer(window, this);
		glfwSetFramebufferSizeCallback(window, frameBufferResizeCallBack);
	
	
	}
	void EgeWindow::createWindowSurface(VkInstance instance, VkSurfaceKHR* surface) {

		if (glfwCreateWindowSurface(instance, window, nullptr, surface) != VK_SUCCESS) {
			throw std::runtime_error("failed to create window surface");
		}
	}



	void EgeWindow::frameBufferResizeCallBack(GLFWwindow* window, int width, int height) {
		auto egeWindow = reinterpret_cast<EgeWindow*>(glfwGetWindowUserPointer(window));
		egeWindow->wasFrameBufferResized = true;
		egeWindow->width = width;
		egeWindow->height = height;
	}
}