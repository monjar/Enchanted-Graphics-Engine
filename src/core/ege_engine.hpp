#pragma once

#include "platform/ege_window.hpp"
#include "render/ege_model.hpp"
#include "render/ege_renderer.hpp"
#include "rhi/ege_descriptors.hpp"
#include "scene/ege_game_object.hpp"

#include <memory>
#include <vector>

namespace ege {

    class Application {
    public:
        static constexpr int WIDTH = 800, HEIGHT = 600;

        Application();
        ~Application();

        // Delete copy constructor and operator1
        Application(const Application& other) = delete;
        Application& operator=(const Application&) = delete;

        void run();

    private:
        void loadGameObjects();

        Window window{WIDTH, HEIGHT, "Hello World!"};
        Device device{window};
        Renderer renderer{window, device};

        std::unique_ptr<DescriptorPool> globalPool{};
        GameObject::Map gameObjects;
    };
}  // namespace ege