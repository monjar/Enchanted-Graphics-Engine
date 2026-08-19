// The checks that look at a rendered frame.
//
// These exist because the headless CI run proved the engine started, rendered
// and upset no validation layer - all of which a uniformly black frame does
// too. The thresholds are the interesting part and they are tested here
// against images built in memory, so the tool that reads real PNGs has no
// judgement of its own to get wrong.

#include "render/FrameChecks.hpp"

#include <doctest/doctest.h>

#include <cmath>

using ege::allPassed;
using ege::checkFrame;
using ege::FrameCheck;
using ege::FrameCheckLimits;
using ege::FramePixels;
using ege::FrameStats;
using ege::measureFrame;

namespace {

    constexpr uint32_t width = 64;
    constexpr uint32_t height = 48;

    FramePixels filled(uint8_t r, uint8_t g, uint8_t b) {
        FramePixels frame{};
        frame.width = width;
        frame.height = height;
        frame.rgba.assign(static_cast<std::size_t>(width) * height * 4, 255);
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; i++) {
            frame.rgba[i * 4 + 0] = r;
            frame.rgba[i * 4 + 1] = g;
            frame.rgba[i * 4 + 2] = b;
        }
        return frame;
    }

    // Something with the shape of a rendered frame: tones spread across the
    // image in two directions rather than one, none of it pinned at either
    // end. Two directions matters - a gradient that varies only along x + y
    // has barely a hundred distinct colours in it, which is nothing like a
    // rendered frame and trips the colour-count check for the wrong reason.
    FramePixels gradient() {
        FramePixels frame{};
        frame.width = width;
        frame.height = height;
        frame.rgba.assign(static_cast<std::size_t>(width) * height * 4, 255);
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
                frame.rgba[i + 0] = static_cast<uint8_t>(20 + (200 * x) / width);
                frame.rgba[i + 1] = static_cast<uint8_t>(30 + (180 * y) / height);
                frame.rgba[i + 2] = static_cast<uint8_t>(200 - (150 * x) / width);
            }
        }
        return frame;
    }

    bool failedCheck(const std::vector<FrameCheck>& checks, const std::string& name) {
        for (const FrameCheck& check : checks) {
            if (check.name == name) {
                return !check.passed;
            }
        }
        return false;
    }

}  // namespace

TEST_CASE("a frame with a spread of tones passes every check") {
    const std::vector<FrameCheck> checks = checkFrame(gradient());
    CHECK(allPassed(checks));
    // Every check reports, passed or not, so a failure report can show the
    // whole picture rather than the first thing that went wrong.
    CHECK(checks.size() >= 6);
}

TEST_CASE("a black frame is caught") {
    // The exact failure the CI smoke test could not see: a scene pass that
    // produced nothing, rendering, not crashing and upsetting no validation
    // layer the whole time.
    const std::vector<FrameCheck> checks = checkFrame(filled(0, 0, 0));

    CHECK_FALSE(allPassed(checks));
    CHECK(failedCheck(checks, "frame is not crushed to black"));
    CHECK(failedCheck(checks, "frame is not one flat colour"));
    CHECK(failedCheck(checks, "frame is not mostly black pixels"));
}

TEST_CASE("a white frame is caught") {
    const std::vector<FrameCheck> checks = checkFrame(filled(255, 255, 255));

    CHECK_FALSE(allPassed(checks));
    CHECK(failedCheck(checks, "frame is not blown out to white"));
    CHECK(failedCheck(checks, "frame is not mostly white pixels"));
}

TEST_CASE("a flat mid-grey frame is caught even though it is neither dark nor bright") {
    // Mean luminance alone would pass this. A frame of one colour is still a
    // broken frame, which is why the spread and the colour count are separate
    // checks rather than a brightness band.
    const std::vector<FrameCheck> checks = checkFrame(filled(128, 128, 128));

    CHECK_FALSE(allPassed(checks));
    CHECK(failedCheck(checks, "frame is not one flat colour"));
    CHECK(failedCheck(checks, "frame has more than a handful of colours"));
}

TEST_CASE("an empty or malformed frame is caught rather than measured") {
    CHECK_FALSE(allPassed(checkFrame(FramePixels{})));

    FramePixels truncated{};
    truncated.width = width;
    truncated.height = height;
    truncated.rgba.assign(16, 0);  // far too few bytes for those dimensions
    CHECK_FALSE(allPassed(checkFrame(truncated)));
}

TEST_CASE("the measurements say what they mean") {
    const FrameStats black = measureFrame(filled(0, 0, 0));
    CHECK(black.meanLuminance == doctest::Approx(0.f));
    CHECK(black.blackFraction == doctest::Approx(1.f));
    CHECK(black.distinctColors == 1);

    const FrameStats white = measureFrame(filled(255, 255, 255));
    CHECK(white.meanLuminance == doctest::Approx(1.f));
    CHECK(white.whiteFraction == doctest::Approx(1.f));

    const FrameStats varied = measureFrame(gradient());
    CHECK(varied.maxLuminance > varied.minLuminance);
    CHECK(varied.distinctColors > 32);
    CHECK(varied.blackFraction == doctest::Approx(0.f));
    CHECK(varied.whiteFraction == doctest::Approx(0.f));
}

TEST_CASE("the limits are what decide, not the measurements") {
    // A caller can tighten or loosen them; nothing about a threshold is baked
    // into the measuring.
    FrameCheckLimits strict{};
    strict.minMeanLuminance = 0.99f;

    CHECK(allPassed(checkFrame(gradient())));
    CHECK_FALSE(allPassed(checkFrame(gradient(), strict)));
}
