#pragma once

#include "rhi/ege_descriptors.hpp"
#include "scene/ege_game_object.hpp"
#include "render/ege_model.hpp"
#include "render/ege_renderer.hpp"
#include "platform/ege_window.hpp"

#include <memory>
#include <vector>

namespace ege {

    class EnchantedEngine {
    public:
        static constexpr int WIDTH = 800, HEIGHT = 600;

        EnchantedEngine();
        ~EnchantedEngine();

        // Delete copy constructor and operator1
        EnchantedEngine(const EnchantedEngine& other) = delete;
        EnchantedEngine& operator=(const EnchantedEngine&) = delete;

        void run();

    private:
        void loadGameObjects();

        EgeWindow egeWindow{WIDTH, HEIGHT, "Hello World!"};
        EgeDevice egeDevice{egeWindow};
        EgeRenderer egeRenderer{egeWindow, egeDevice};

        std::unique_ptr<EgeDescriptorPool> globalPool{};
        EgeGameObject::Map gameObjects;
    };
}  // namespace ege