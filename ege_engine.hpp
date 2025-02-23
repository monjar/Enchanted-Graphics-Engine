#pragma once

#include "ege_window.hpp"
#include "ege_model.hpp"
#include "ege_game_object.hpp"
#include "ege_renderer.hpp"


#include <memory>

#include <vector>

namespace ege {

	class EnchantedEngine {
	public:
		static constexpr int WIDTH = 800, HEIGHT = 600;

		EnchantedEngine();
		~EnchantedEngine();



		//Delete copy constructor and operator1
		EnchantedEngine(const EnchantedEngine& other) = delete;
		EnchantedEngine& operator = (const EnchantedEngine&) = delete;


		void run();
	private:

		void loadGameObjects();


		EgeWindow egeWindow{ WIDTH, HEIGHT, "Hello World!" };
		EgeDevice egeDevice{ egeWindow };
		EgeRenderer egeRenderer{ egeWindow , egeDevice};


		std::vector<EgeGameObject> gameObjects;


	};
}