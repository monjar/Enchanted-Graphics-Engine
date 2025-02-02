#include "ege_engine.hpp"

namespace ege {

	void EnchantedEngine::run() {
		while (!egeWindow.shouldClose()) {
			glfwPollEvents();
		}

	}

}