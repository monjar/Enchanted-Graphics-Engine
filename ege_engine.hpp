#pragma once

#include "ege_window.hpp"

namespace ege {

	class EnchantedEngine {
	public:
		static constexpr int WIDTH = 800, HEIGHT = 600;
		void run();
	private:
		EgeWindow egeWindow{ WIDTH, HEIGHT, "Hello World!" };

	};
}