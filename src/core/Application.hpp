#pragma once

#include "core/JobSystem.hpp"
#include "platform/Window.hpp"
#include "render/Light.hpp"
#include "render/Material.hpp"
#include "render/Model.hpp"
#include "render/Renderer.hpp"
#include "rhi/Descriptors.hpp"
#include "scene/GameObject.hpp"

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
        // Materials allocate combined image samplers, which the global pool
        // does not provide, and there are far more of them than frames.
        std::unique_ptr<DescriptorPool> materialPool{};
        std::unique_ptr<DescriptorSetLayout> materialSetLayout{};
        JobSystem jobs{};
        GameObject::Map gameObjects;
        std::vector<std::shared_ptr<Material>> materials;
        std::vector<GpuPointLight> sceneLights;
    };
}  // namespace ege