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

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ege {

    class Application {
    public:
        static constexpr int WIDTH = 800, HEIGHT = 600;

        // How the application was asked to run.
        struct Options {
            // Runs the scripted camera tour with the editor hidden and the
            // scene playing, then closes. What `--demo` selects, and what the
            // recording in the documentation is made from.
            bool demo = false;
            // Closes after this many seconds regardless. Zero means run until
            // the window is closed.
            float exitAfterSeconds = 0.f;
            // Writes every rendered frame here as a PNG. Empty records
            // nothing.
            std::string recordDirectory;
            // While recording, time advances by exactly this much per frame
            // rather than by the clock, so the result is the same however
            // fast the machine renders it.
            float recordFrameRate = 30.f;
            // The script module to load, if it is not the one built beside
            // the executable. Empty takes the default; "none" loads nothing,
            // which is how the demo is run without it to show the difference.
            std::string scriptModule;
        };

        Application() : Application(Options{}) {}

        explicit Application(Options optionsRef);

        ~Application();

        // Delete copy constructor and operator1
        Application(const Application& other) = delete;
        Application& operator=(const Application&) = delete;

        void run();

    private:
        // Where the project's assets live. EGE_ASSET_ROOT points at the source
        // tree during development; a shipped build looks beside itself.
        static std::filesystem::path assetRoot();

        // Where script modules are built. Same arrangement as assetRoot: a
        // compile definition during development, beside the binary otherwise.
        static std::filesystem::path moduleRoot();

        // Loads the project's script module, if there is one. Behaviours it
        // registers are available to the scene from the moment this returns,
        // which is why it happens before the scene is built.
        void loadScriptModule();

        // Reloads it when the file changes, and rebuilds every live behaviour
        // from what the new one registered.
        void reloadChangedScripts(class FileWatcher& watcher, class ScriptSystem& scripts);

        void loadScene();
        // Acts on whatever the project directory changed since the last look.
        void reloadChangedAssets(class FileWatcher& watcher);

        // Imports every .gltf/.glb under assets/models, if the directory
        // exists. The demo ships none - dropping a file there is how content
        // gets in until the asset database gives imports a real home.
        void importGltfModels();

        // Round-trips the scene through the serializer at start-up, which keeps
        // save and load exercised by every run rather than only by the tests.
        void verifySceneRoundTrip();

        Options options{};
        Window window{WIDTH, HEIGHT, "Enchanted Engine"};
        Device device{window};
        Renderer renderer{window, device};

        std::unique_ptr<DescriptorPool> globalPool{};
        // Materials allocate combined image samplers, which the global pool
        // does not provide, and there are far more of them than frames.
        std::unique_ptr<DescriptorPool> materialPool{};
        std::unique_ptr<DescriptorSetLayout> materialSetLayout{};
        JobSystem jobs{};
        World world;

        // Kept for the process's lifetime, and every reload adds another -
        // see the note on ScriptModule about why one is never let go of.
        std::vector<std::unique_ptr<class ScriptModule>> scriptModules;
    };
}  // namespace ege