// Runs the frame checks over a directory of recorded frames.
//
// The engine records its own frames as PNGs; this reads them back and asks
// whether a picture came out. It exists as a separate program rather than as
// part of the test suite because the test suite deliberately never touches a
// GPU, and these frames only exist once one has rendered them - so this runs
// in CI's headless render step, after the engine has produced its own input.
//
// Exits non-zero if any frame fails, naming every failure rather than the
// first: when a frame is wrong it is usually wrong in several ways at once.

#include "render/FrameChecks.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

    ege::FramePixels loadFrame(const std::filesystem::path& path) {
        int width = 0;
        int height = 0;
        int channels = 0;
        // Four channels whatever the file holds, so the stride is known.
        unsigned char* data = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
        if (data == nullptr) {
            return {};
        }

        ege::FramePixels frame{};
        frame.width = static_cast<uint32_t>(width);
        frame.height = static_cast<uint32_t>(height);
        const std::size_t bytes = static_cast<std::size_t>(frame.width) * frame.height * 4;
        frame.rgba.assign(data, data + bytes);
        stbi_image_free(data);
        return frame;
    }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: EnchantedFrameChecks <directory of recorded frames>\n";
        return 2;
    }

    const std::filesystem::path directory{argv[1]};
    std::error_code errorCode;
    if (!std::filesystem::is_directory(directory, errorCode)) {
        std::cerr << "not a directory: " << directory << "\n";
        return 2;
    }

    std::vector<std::filesystem::path> frames;
    for (const auto& entry : std::filesystem::directory_iterator{directory, errorCode}) {
        if (entry.is_regular_file() && entry.path().extension() == ".png") {
            frames.push_back(entry.path());
        }
    }
    // Sorted, so a failure names the same frame on every machine.
    std::sort(frames.begin(), frames.end());

    if (frames.empty()) {
        std::cerr << "no frames were recorded in " << directory
                  << " - the engine produced nothing to check\n";
        return 1;
    }

    std::size_t failed = 0;
    for (const std::filesystem::path& path : frames) {
        const ege::FramePixels frame = loadFrame(path);
        const std::vector<ege::FrameCheck> checks = ege::checkFrame(frame);
        if (ege::allPassed(checks)) {
            continue;
        }

        failed++;
        std::cerr << path.filename().string() << ":\n";
        for (const ege::FrameCheck& check : checks) {
            if (!check.passed) {
                std::cerr << "  FAILED " << check.name << " - " << check.detail << "\n";
            }
        }
    }

    if (failed > 0) {
        std::cerr << failed << " of " << frames.size() << " frames did not look like a picture\n";
        return 1;
    }

    std::cout << "All " << frames.size() << " frames look like a picture.\n";
    return 0;
}
