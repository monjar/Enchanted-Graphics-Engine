#pragma once

#include "core/JobSystem.hpp"
#include "platform/Window.hpp"
#include "render/Light.hpp"
#include "render/Material.hpp"
#include "render/Model.hpp"
#include "render/Renderer.hpp"
#include "rhi/Descriptors.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

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
        void loadScene();

        // Imports every .gltf/.glb under assets/models, if the directory
        // exists. The demo ships none - dropping a file there is how content
        // gets in until the asset database gives imports a real home.
        void importGltfModels();

        // Round-trips the scene through the serializer at start-up, which keeps
        // save and load exercised by every run rather than only by the tests.
        void verifySceneRoundTrip();

        Window window{WIDTH, HEIGHT, "Hello World!"};
        Device device{window};
        Renderer renderer{window, device};

        std::unique_ptr<DescriptorPool> globalPool{};
        // Materials allocate combined image samplers, which the global pool
        // does not provide, and there are far more of them than frames.
        std::unique_ptr<DescriptorPool> materialPool{};
        std::unique_ptr<DescriptorSetLayout> materialSetLayout{};
        JobSystem jobs{};
        World world;
        std::vector<std::shared_ptr<Material>> materials;
    };
}  // namespace ege