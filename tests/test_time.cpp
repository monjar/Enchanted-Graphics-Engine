// Fixed-timestep accumulator.
//
// These are the properties physics depends on in Phase 8: that a given amount
// of elapsed time produces the same number of simulation steps however it was
// divided into frames, and that a machine which cannot keep up degrades by
// falling behind rather than by spiralling.
//
// Time::advance() exists so this can be tested without sleeping - beginFrame()
// is advance() plus a clock read.

#include "core/Time.hpp"

#include <doctest/doctest.h>

#include <cstdlib>

using ege::Time;

namespace {

    int drain(Time& time) {
        int steps = 0;
        while (time.consumeFixedStep()) {
            steps++;
        }
        return steps;
    }

}  // namespace

TEST_CASE("a fresh clock has nothing accumulated") {
    Time time{1.f / 60.f};

    CHECK(time.delta() == doctest::Approx(0.f));
    CHECK(time.elapsed() == doctest::Approx(0.0));
    CHECK(time.fixedStep() == doctest::Approx(1.f / 60.f));
    CHECK_FALSE(time.consumeFixedStep());
}

TEST_CASE("the first frame reports a zero delta") {
    // Whatever start-up cost preceded the first frame is not a frame time, and
    // reporting it as one gives frame zero an enormous delta.
    Time time{};
    time.beginFrame();

    CHECK(time.delta() == doctest::Approx(0.f));
    CHECK(time.frameCount() == 1);
}

TEST_CASE("step count is exact for an exactly representable step") {
    // 1/64 is a power of two and so is exact in binary floating point, which
    // makes the arithmetic here unambiguous. Note that the engine's default of
    // 1.f/60.f is *not* exact - it rounds up - so half a second holds 29 whole
    // steps rather than 30. That is correct behaviour, not drift, and it is
    // why the cross-subdivision test below compares the two runs against each
    // other instead of against a hand-computed constant.
    constexpr float step = 1.f / 64.f;

    Time time{step};
    time.maxStepsPerFrame = 1000;
    time.maxDelta = 10.f;
    time.advance(0.5f);

    CHECK(drain(time) == 32);
    CHECK(time.fixedAlpha() == doctest::Approx(0.f));
}

TEST_CASE("step count is independent of how frames divide the time") {
    constexpr float step = 1.f / 64.f;

    Time oneBigFrame{step};
    oneBigFrame.maxStepsPerFrame = 1000;
    oneBigFrame.maxDelta = 10.f;
    oneBigFrame.advance(0.5f);
    const int bigSteps = drain(oneBigFrame);

    Time manySmallFrames{step};
    manySmallFrames.maxStepsPerFrame = 1000;
    int smallSteps = 0;
    for (int frame = 0; frame < 50; frame++) {
        manySmallFrames.advance(0.01f);
        smallSteps += drain(manySmallFrames);
    }

    // The same elapsed time simulated the same amount, however it was divided.
    // Fifty float additions can leave the accumulator a hair either side of a
    // boundary, so allow one step of slack - what matters is that the two do
    // not diverge.
    CHECK(std::abs(bigSteps - smallSteps) <= 1);
    CHECK(bigSteps == 32);
}

TEST_CASE("a partial step is carried into the next frame, not lost") {
    constexpr float step = 0.1f;
    Time time{step};

    time.advance(0.05f);
    CHECK(drain(time) == 0);  // half a step: nothing to consume yet

    time.advance(0.06f);
    CHECK(drain(time) == 1);  // the carried 0.05 plus 0.06 crosses one step

    // 0.01 remains, so alpha is a tenth of the way to the next step.
    CHECK(time.fixedAlpha() == doctest::Approx(0.1f).epsilon(1e-3));
}

TEST_CASE("fixedAlpha never reaches one") {
    // At alpha 1 a whole step is available and should have been consumed, so a
    // value of 1 would mean the interpolation is extrapolating.
    constexpr float step = 0.1f;
    Time time{step};

    for (int frame = 0; frame < 40; frame++) {
        time.advance(0.037f);
        drain(time);
        CHECK(time.fixedAlpha() >= 0.f);
        CHECK(time.fixedAlpha() < 1.f);
    }
}

TEST_CASE("a long stall is clamped rather than simulated in full") {
    constexpr float step = 1.f / 60.f;
    Time time{step};
    time.maxDelta = 0.25f;
    time.maxStepsPerFrame = 1000;

    // A ten second debugger pause. Simulating it in full would advance the
    // world far enough to tunnel straight through geometry.
    time.advance(10.f);

    CHECK(time.rawDelta() == doctest::Approx(10.f));
    CHECK(time.delta() == doctest::Approx(0.25f));
    // 0.25s at 60 Hz, not 10s at 60 Hz.
    CHECK(drain(time) == 15);
}

TEST_CASE("the step ceiling stops the simulation spiralling") {
    constexpr float step = 1.f / 60.f;
    Time time{step};
    time.maxDelta = 1.f;
    time.maxStepsPerFrame = 3;

    time.advance(1.f);  // 60 steps' worth against a ceiling of 3
    CHECK(drain(time) == 3);

    // The outstanding debt must be dropped, not carried: carrying it would
    // make the next frame ask for even more work than this one.
    time.advance(step);
    CHECK(drain(time) == 1);
}

TEST_CASE("a backwards clock never moves the simulation backwards") {
    Time time{};
    time.advance(-5.f);

    CHECK(time.delta() == doctest::Approx(0.f));
    CHECK(time.elapsed() == doctest::Approx(0.0));
    CHECK_FALSE(time.consumeFixedStep());
}

TEST_CASE("elapsed time accumulates the clamped deltas") {
    Time time{};
    time.maxDelta = 0.25f;

    time.advance(0.1f);
    time.advance(0.1f);
    time.advance(5.f);  // clamped to 0.25

    CHECK(time.elapsed() == doctest::Approx(0.45).epsilon(1e-4));
    CHECK(time.frameCount() == 3);
}
