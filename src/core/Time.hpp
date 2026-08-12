#pragma once

#include <cstdint>

namespace ege {

    // Frame timing and the fixed-timestep accumulator.
    //
    // The loop previously measured a raw delta and fed it straight to
    // everything, which means simulation behaviour depends on frame rate: the
    // same scene behaves differently on a fast machine than a slow one, and a
    // single long frame - a breakpoint, a window drag, a shader recompile -
    // can advance the world far enough to tunnel straight through geometry.
    //
    // Time separates the two clocks an engine needs. Rendering runs on the
    // variable delta, because it should draw as often as it can. Simulation
    // runs on a fixed step, because it has to be reproducible. Phase 8 hangs
    // Jolt off exactly this.
    class Time {
    public:
        explicit Time(float fixedStepSeconds = 1.f / 60.f);

        // Call once at the top of each frame. Reads the clock and advances by
        // the measured delta.
        void beginFrame();

        // Advances by an explicit delta, doing everything beginFrame does apart
        // from reading the clock. Splitting the clock out from the accumulator
        // is what makes the stepping behaviour testable without sleeping, and
        // it is also what a replay or a deterministic server tick needs.
        void advance(float deltaSeconds);

        // Seconds since the previous frame, after clamping.
        float delta() const { return frameDelta; }

        // Unclamped, for diagnostics that want the truth.
        float rawDelta() const { return rawFrameDelta; }

        // Seconds since the first beginFrame().
        double elapsed() const { return totalElapsed; }

        std::uint64_t frameCount() const { return frames; }

        // Frames per second, smoothed over roughly the last half second so the
        // number is readable rather than flickering.
        float framesPerSecond() const { return smoothedFps; }

        // Drives the fixed-step loop:
        //
        //     while (time.consumeFixedStep()) {
        //         world.fixedTick(time.fixedStep());
        //     }
        //
        // Returns true while a whole step remains in the accumulator, and
        // deducts it. Never returns true more than maxStepsPerFrame times in
        // one frame.
        bool consumeFixedStep();

        float fixedStep() const { return fixedStepSize; }

        // How far the current frame sits between the last completed fixed step
        // and the next, in [0, 1). Render transforms are interpolated by this
        // so that a 60 Hz simulation does not look like 60 Hz on a 144 Hz
        // display.
        float fixedAlpha() const;

        // A frame longer than this is treated as this long. Covers debugger
        // pauses and window drags, where the wall clock advanced but the
        // simulation should not leap.
        float maxDelta = 0.25f;

        // Ceiling on fixed steps per frame. Without it, a machine that cannot
        // simulate as fast as real time accumulates an ever-growing debt and
        // the frame rate spirals to zero.
        int maxStepsPerFrame = 5;

    private:
        double lastFrameTime = 0.0;
        double totalElapsed = 0.0;
        float frameDelta = 0.f;
        float rawFrameDelta = 0.f;
        std::uint64_t frames = 0;
        bool started = false;

        float fixedStepSize;
        float accumulator = 0.f;
        int stepsThisFrame = 0;

        float fpsAccumulator = 0.f;
        int fpsFrames = 0;
        float smoothedFps = 0.f;
    };

}  // namespace ege
