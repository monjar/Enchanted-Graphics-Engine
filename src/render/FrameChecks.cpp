#include "render/FrameChecks.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace ege {

    namespace {

        // Rec. 709 luma weights, applied to the stored values. See the header
        // for why this is not converted to linear first.
        float luminanceOf(uint8_t r, uint8_t g, uint8_t b) {
            return (0.2126f * static_cast<float>(r) + 0.7152f * static_cast<float>(g) +
                    0.0722f * static_cast<float>(b)) /
                   255.f;
        }

        std::string fixed(float value) {
            std::ostringstream out;
            out << std::fixed << std::setprecision(4) << value;
            return out.str();
        }

        // "mean luminance 0.0123, needs at least 0.0200" and the like. Built
        // by concatenation rather than with a runtime format string, which the
        // warning set rejects for the good reason that nothing can check one.
        std::string describe(
            const std::string& measure, float value, const std::string& bound, float limit) {
            return measure + " " + fixed(value) + ", " + bound + " " + fixed(limit);
        }

    }  // namespace

    FrameStats measureFrame(const FramePixels& frame) {
        FrameStats stats{};
        if (!frame.valid()) {
            return stats;
        }

        const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;

        // One flag per quantised colour. Five bits a channel is 32768 buckets,
        // fine enough that a gradient does not collapse into one of them and
        // coarse enough that dither and noise do not each get their own.
        std::vector<bool> seen(32768, false);

        double luminanceSum = 0.0;
        float minimum = 1.f;
        float maximum = 0.f;
        std::size_t black = 0;
        std::size_t white = 0;

        for (std::size_t i = 0; i < pixels; i++) {
            const uint8_t r = frame.rgba[i * 4 + 0];
            const uint8_t g = frame.rgba[i * 4 + 1];
            const uint8_t b = frame.rgba[i * 4 + 2];

            const float luminance = luminanceOf(r, g, b);
            luminanceSum += static_cast<double>(luminance);
            minimum = std::min(minimum, luminance);
            maximum = std::max(maximum, luminance);

            if (r <= 2 && g <= 2 && b <= 2) {
                black++;
            }
            if (r >= 253 && g >= 253 && b >= 253) {
                white++;
            }

            seen[static_cast<std::size_t>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))] = true;
        }

        stats.meanLuminance = static_cast<float>(luminanceSum / static_cast<double>(pixels));
        stats.minLuminance = minimum;
        stats.maxLuminance = maximum;
        stats.blackFraction = static_cast<float>(black) / static_cast<float>(pixels);
        stats.whiteFraction = static_cast<float>(white) / static_cast<float>(pixels);
        stats.distinctColors = static_cast<uint32_t>(std::count(seen.begin(), seen.end(), true));
        return stats;
    }

    std::vector<FrameCheck> checkFrame(const FramePixels& frame, const FrameCheckLimits& limits) {
        std::vector<FrameCheck> checks;

        if (!frame.valid()) {
            checks.push_back({"frame decodes to pixels", false, "the frame is empty or malformed"});
            return checks;
        }
        checks.push_back({"frame decodes to pixels", true, ""});

        const FrameStats stats = measureFrame(frame);
        const float spread = stats.maxLuminance - stats.minLuminance;

        checks.push_back(
            {"frame is not crushed to black",
             stats.meanLuminance >= limits.minMeanLuminance,
             describe(
                 "mean luminance",
                 stats.meanLuminance,
                 "needs at least",
                 limits.minMeanLuminance)});

        checks.push_back(
            {"frame is not blown out to white",
             stats.meanLuminance <= limits.maxMeanLuminance,
             describe(
                 "mean luminance",
                 stats.meanLuminance,
                 "allowed at most",
                 limits.maxMeanLuminance)});

        checks.push_back(
            {"frame is not one flat colour",
             spread >= limits.minLuminanceSpread,
             describe("luminance spread", spread, "needs at least", limits.minLuminanceSpread)});

        checks.push_back(
            {"frame is not mostly black pixels",
             stats.blackFraction <= limits.maxBlackFraction,
             describe(
                 "black fraction",
                 stats.blackFraction,
                 "allowed at most",
                 limits.maxBlackFraction)});

        checks.push_back(
            {"frame is not mostly white pixels",
             stats.whiteFraction <= limits.maxWhiteFraction,
             describe(
                 "white fraction",
                 stats.whiteFraction,
                 "allowed at most",
                 limits.maxWhiteFraction)});

        checks.push_back(
            {"frame has more than a handful of colours",
             stats.distinctColors >= limits.minDistinctColors,
             describe(
                 "distinct colours",
                 static_cast<float>(stats.distinctColors),
                 "needs at least",
                 static_cast<float>(limits.minDistinctColors))});

        return checks;
    }

    bool allPassed(const std::vector<FrameCheck>& checks) {
        return std::all_of(
            checks.begin(), checks.end(), [](const FrameCheck& check) { return check.passed; });
    }

}  // namespace ege
