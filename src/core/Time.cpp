#include "core/Time.hpp"

#include "core/Assert.hpp"

#include <chrono>

namespace ege {

    namespace {

        double nowSeconds() {
            using Clock = std::chrono::steady_clock;
            static const Clock::time_point origin = Clock::now();
            return std::chrono::duration<double>(Clock::now() - origin).count();
        }

        constexpr float fpsWindowSeconds = 0.5f;

    }  // namespace

    Time::Time(float fixedStepSeconds) : fixedStepSize{fixedStepSeconds} {
        EGE_VERIFY(fixedStepSeconds > 0.f, "fixed step must be positive");
    }

    void Time::beginFrame() {
        const double now = nowSeconds();

        if (!started) {
            // The first frame has no previous frame to measure against, and
            // whatever start-up cost preceded it is not a frame time.
            started = true;
            lastFrameTime = now;
            frameDelta = 0.f;
            rawFrameDelta = 0.f;
            frames = 1;
            return;
        }

        const auto measured = static_cast<float>(now - lastFrameTime);
        lastFrameTime = now;
        advance(measured);
    }

    void Time::advance(float deltaSeconds) {
        started = true;
        rawFrameDelta = deltaSeconds;

        frameDelta = rawFrameDelta > maxDelta ? maxDelta : rawFrameDelta;
        if (frameDelta < 0.f) {
            // A non-monotonic clock, or a caller passing nonsense. Either way,
            // never move the simulation backwards.
            frameDelta = 0.f;
        }
        totalElapsed += static_cast<double>(frameDelta);
        frames++;

        accumulator += frameDelta;
        stepsThisFrame = 0;

        fpsAccumulator += rawFrameDelta;
        fpsFrames++;
        if (fpsAccumulator >= fpsWindowSeconds) {
            smoothedFps = static_cast<float>(fpsFrames) / fpsAccumulator;
            fpsAccumulator = 0.f;
            fpsFrames = 0;
        }
    }

    bool Time::consumeFixedStep() {
        if (accumulator < fixedStepSize) {
            return false;
        }

        if (stepsThisFrame >= maxStepsPerFrame) {
            // Cannot keep up. Drop the outstanding debt rather than carrying it
            // into the next frame, which would make the next frame slower
            // still and spiral. The simulation falls behind real time, which is
            // the correct trade: a slow simulation beats a frozen one.
            accumulator = 0.f;
            return false;
        }

        accumulator -= fixedStepSize;
        stepsThisFrame++;
        return true;
    }

    float Time::fixedAlpha() const {
        return accumulator / fixedStepSize;
    }

}  // namespace ege
