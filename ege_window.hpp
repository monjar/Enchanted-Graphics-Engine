#pragma once

#define GLFW_INCLUDE_VULKAN
#include<GLFW/glfw3.h>
#include<string>
namespace ege {

	class EgeWindow {

	public:
		EgeWindow(int w, int h, std::string name);
		~EgeWindow();

		//Delete copy constructor and operator because we want the relation between EgeWindow and glfWindow to be 1 to 1
		EgeWindow(const EgeWindow& other) = delete;
		EgeWindow &operator = (const EgeWindow &) = delete;

		bool shouldClose() { return glfwWindowShouldClose(window); }

		VkExtent2D getExtent() { return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) }; }

		void createWindowSurface(VkInstance instance, VkSurfaceKHR* surface);

		bool wasWindowResized() { return wasFrameBufferResized; }
		void resetWindowResizedFlag() { wasFrameBufferResized = false; }

	private:

		static void frameBufferResizeCallBack(GLFWwindow* window, int width, int height);
		void initWindow();

		int width;
		int height;
		bool wasFrameBufferResized = false;



		std::string windowName;
		GLFWwindow* window;
	};


}