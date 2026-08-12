#include "core/Application.hpp"
#include "core/Log.hpp"

#include <cstdlib>
#include <exception>

int main() {
    // Construction is inside the try: Application's constructor creates the
    // descriptor pool and loads the scene, both of which can throw, and
    // previously did so outside any handler.
    try {
        ege::Application application{};
        application.run();
    } catch (const std::exception& e) {
        EGE_CRITICAL("Fatal: {}", e.what());
        ege::Log::flush();
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
