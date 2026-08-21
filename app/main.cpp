#include "core/Application.hpp"
#include "core/Log.hpp"

#include <cstdlib>
#include <exception>
#include <string_view>

namespace {

    void printUsage() {
        EGE_INFO("usage: EnchantedEngine [--demo] [--exit-after SECONDS]");
        EGE_INFO("  --demo               run the scripted camera tour and close");
        EGE_INFO("  --exit-after SECONDS close after this long regardless");
        EGE_INFO("  --record DIR         write every frame there as a PNG");
        EGE_INFO("  --record-fps N       seconds per frame while recording (default 30)");
        EGE_INFO("  --script-module PATH load this module instead of the default sandbox");
        EGE_INFO("                       ('none' loads no behaviours at all)");
    }

}  // namespace

int main(int argc, char** argv) {
    ege::Application::Options options{};

    for (int index = 1; index < argc; index++) {
        const std::string_view argument{argv[index]};
        if (argument == "--demo") {
            options.demo = true;
        } else if (argument == "--exit-after" && index + 1 < argc) {
            options.exitAfterSeconds = std::strtof(argv[++index], nullptr);
        } else if (argument == "--record" && index + 1 < argc) {
            options.recordDirectory = argv[++index];
        } else if (argument == "--record-fps" && index + 1 < argc) {
            options.recordFrameRate = std::strtof(argv[++index], nullptr);
        } else if (argument == "--script-module" && index + 1 < argc) {
            options.scriptModule = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            printUsage();
            return EXIT_SUCCESS;
        } else {
            EGE_ERROR("unrecognised argument '{}'", argument);
            printUsage();
            return EXIT_FAILURE;
        }
    }

    // Construction is inside the try: Application's constructor creates the
    // descriptor pool and loads the scene, both of which can throw, and
    // previously did so outside any handler.
    try {
        ege::Application application{options};
        application.run();
    } catch (const std::exception& e) {
        EGE_CRITICAL("Fatal: {}", e.what());
        ege::Log::flush();
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
