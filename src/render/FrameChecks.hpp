#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ege {

    // Looking at the picture, automatically.
    //
    // The headless CI run proves the engine starts, renders for a while and
    // upsets no validation layer. It does not prove anything came out: a frame
    // that is uniformly black passes all three, and so would a scene pass that
    // silently produced nothing. Every renderer change in this project has
    // been checked by a person opening a PNG, which works right up until
    // nobody does it.
    //
    // These are the checks a person makes in the first half-second of looking:
    // is there an image, is it not one flat colour, is it not crushed to black
    // or blown out to white. They catch catastrophic regressions - a dead
    // pass, a lost descriptor, a shader that fails to bind - and they are
    // deliberately not trying to catch subtle ones. Whether a shadow lands in
    // the right place is what the device-free unit tests are for, because a
    // pixel comparison strict enough to answer it would fail on a driver
    // update instead.
    //
    // No Vulkan and no image decoding here: this takes pixels and returns
    // verdicts, so the thresholds themselves are unit-tested against images
    // built in memory.

    // A decoded frame: 8-bit RGBA, row-major, top row first - what a recorded
    // PNG turns into.
    struct FramePixels {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;

        bool valid() const {
            return width > 0 && height > 0 &&
                   rgba.size() == static_cast<std::size_t>(width) * height * 4;
        }
    };

    struct FrameStats {
        // All in [0, 1], measured on the stored (display-encoded) values
        // rather than in linear light. That is the right space for these
        // questions: "does this look black" is a question about what reaches
        // the screen, not about the radiance behind it.
        float meanLuminance = 0.f;
        float minLuminance = 0.f;
        float maxLuminance = 0.f;
        // Fractions of the frame pinned at the ends of the range.
        float blackFraction = 0.f;
        float whiteFraction = 0.f;
        // Distinct colours after quantising each channel to five bits. An
        // image with a handful is a flat fill or a broken gradient; a rendered
        // frame has thousands.
        uint32_t distinctColors = 0;
    };

    FrameStats measureFrame(const FramePixels& frame);

    // What counts as a picture. Deliberately loose - these are the bounds a
    // frame has to be outside for something to be badly wrong, not a
    // description of a good frame.
    struct FrameCheckLimits {
        float minMeanLuminance = 0.02f;
        float maxMeanLuminance = 0.97f;
        // Between the darkest and brightest pixel. A frame with no spread is
        // one flat colour however bright it is.
        float minLuminanceSpread = 0.15f;
        float maxBlackFraction = 0.90f;
        float maxWhiteFraction = 0.60f;
        uint32_t minDistinctColors = 64;
    };

    struct FrameCheck {
        std::string name;
        bool passed = false;
        std::string detail;
    };

    // Every check, passed or not, so a caller can report the lot rather than
    // stopping at the first failure - when a frame is wrong it is usually
    // wrong in several of these at once, and the combination says more than
    // the first one alone.
    std::vector<FrameCheck> checkFrame(
        const FramePixels& frame, const FrameCheckLimits& limits = {});

    bool allPassed(const std::vector<FrameCheck>& checks);

}  // namespace ege
