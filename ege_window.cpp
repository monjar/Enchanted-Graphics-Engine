#include"ege_window.hpp"
#include <stdexcept>
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
		//setting the window to not resize
		glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

		window = glfwCreateWindow(width, height, windowName.c_str(), nullptr, nullptr);
	}

	void EgeWindow::createWindowSurface(VkInstance instance, VkSurfaceKHR* surface) {

		if (glfwCreateWindowSurface(instance, window, nullptr, surface) != VK_SUCCESS) {
			throw std::runtime_error("failed to create window surface");
		}
	}
}