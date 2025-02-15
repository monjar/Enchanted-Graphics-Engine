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

	private:
		GLFWwindow* window;

		void initWindow();
		const int width;
		const int height;
		std::string windowName;
	};


}