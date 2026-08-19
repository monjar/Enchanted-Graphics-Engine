// Drawing between two fixed steps.
//
// The simulation is reproducible because it runs at a fixed rate; the display
// runs at whatever rate it runs at. Everything here is about the gap between
// those two, and none of it needs a device - which is just as well, because
// the failure it exists to prevent is a smoothness problem that no recorded
// frame can be compared against.

#include "scene/Components.hpp"
#include "scene/TransformInterpolation.hpp"
#include "scene/World.hpp"

#include <glm/gtc/constants.hpp>

#include <doctest/doctest.h>

#include <cmath>

using ege::Entity;
using ege::interpolateAngle;
using ege::interpolateTransform;
using ege::PreviousTransform;
using ege::Transform;
using ege::World;

namespace {

    constexpr float tolerance = 1e-4f;

    PreviousTransform previousOf(glm::vec3 translation, glm::vec3 rotation = glm::vec3{0.f}) {
        PreviousTransform previous{};
        previous.translation = translation;
        previous.rotation = rotation;
        return previous;
    }

    Transform currentOf(glm::vec3 translation, glm::vec3 rotation = glm::vec3{0.f}) {
        Transform current{};
        current.translation = translation;
        current.rotation = rotation;
        return current;
    }

}  // namespace

TEST_CASE("the ends of a step are drawn exactly, not approximately") {
    // Alpha zero is the pose the step started from and alpha one is the pose
    // it produced. Anything else at those two points is a simulation being
    // drawn somewhere it never was.
    const PreviousTransform previous = previousOf({0.f, 0.f, 0.f}, {0.f, 0.5f, 0.f});
    const Transform current = currentOf({2.f, -4.f, 6.f}, {0.f, 1.5f, 0.f});

    const Transform atStart = interpolateTransform(previous, current, 0.f);
    CHECK(atStart.translation.x == doctest::Approx(0.f));
    CHECK(atStart.translation.y == doctest::Approx(0.f));
    CHECK(atStart.rotation.y == doctest::Approx(0.5f));

    const Transform atEnd = interpolateTransform(previous, current, 1.f);
    CHECK(atEnd.translation.x == doctest::Approx(2.f));
    CHECK(atEnd.translation.y == doctest::Approx(-4.f));
    CHECK(atEnd.translation.z == doctest::Approx(6.f));
    CHECK(atEnd.rotation.y == doctest::Approx(1.5f));
}

TEST_CASE("a falling body is drawn along its path") {
    const PreviousTransform previous = previousOf({0.f, 0.f, 0.f});
    const Transform current = currentOf({0.f, 1.f, 0.f});

    float lastHeight = -1.f;
    for (float alpha = 0.f; alpha <= 1.f; alpha += 0.125f) {
        const Transform drawn = interpolateTransform(previous, current, alpha);
        CHECK(drawn.translation.y == doctest::Approx(alpha).epsilon(tolerance));
        CHECK(drawn.translation.y > lastHeight);
        lastHeight = drawn.translation.y;
    }
}

TEST_CASE("a rotation past a full turn goes the short way round") {
    // The one case a plain lerp gets wrong, and it happens once per
    // revolution: something that turned from just under a full turn to just
    // over reads as having spun almost all the way back, and draws a frame
    // going the wrong way.
    const float turn = glm::two_pi<float>();
    const float justUnder = turn - 0.1f;
    const float justOver = turn + 0.1f;

    const float halfway = interpolateAngle(justUnder, justOver, 0.5f);

    // Two tenths of a radian apart, so halfway is one tenth on from the
    // first - which is the full turn itself.
    CHECK(halfway == doctest::Approx(turn).epsilon(tolerance));

    // And it kept going forwards rather than backwards.
    CHECK(halfway > justUnder);

    // The same going the other way: a step that ran backwards past zero.
    const float back = interpolateAngle(justOver, justUnder, 0.5f);
    CHECK(back == doctest::Approx(turn).epsilon(tolerance));
    CHECK(back < justOver);
}

TEST_CASE("the short way is genuinely the shorter one") {
    // Whatever the pair, the drawn angle is never more than half a turn from
    // where it started - which is what "shorter way round" means.
    const float turn = glm::two_pi<float>();
    for (float previous = -8.f; previous < 8.f; previous += 0.37f) {
        for (float current = -8.f; current < 8.f; current += 0.53f) {
            const float drawn = interpolateAngle(previous, current, 1.f);

            CHECK(std::abs(drawn - previous) <= glm::pi<float>() + tolerance);

            // And it lands on the same rotation the step ended at, whatever
            // multiple of a turn it took to express it.
            const float difference = std::fmod(std::abs(drawn - current), turn);
            const float wrapped = std::min(difference, turn - difference);
            CHECK(wrapped == doctest::Approx(0.f).epsilon(tolerance));
        }
    }
}

TEST_CASE("only what opted in is interpolated") {
    // Anything moved on the variable clock - a camera, a script running in
    // tick rather than fixedTick - is already at display rate. Interpolating
    // it would draw it a fraction of a frame behind where it is, which is the
    // very judder this exists to remove.
    World world;

    Entity simulated = world.spawn("Simulated");
    simulated.attach<Transform>(currentOf({10.f, 0.f, 0.f}));
    ege::beginInterpolating(world, simulated.id());

    Entity direct = world.spawn("Direct");
    direct.attach<Transform>(currentOf({10.f, 0.f, 0.f}));

    // Both move by the same amount.
    world.find<Transform>(simulated.id())->translation.x = 20.f;
    world.find<Transform>(direct.id())->translation.x = 20.f;

    // The one that opted in is drawn part of the way; the other is drawn
    // where it is.
    CHECK(
        ege::renderTransform(world, simulated.id(), 0.25f).translation.x == doctest::Approx(12.5f));
    CHECK(ege::renderTransform(world, direct.id(), 0.25f).translation.x == doctest::Approx(20.f));
}

TEST_CASE("recording is what makes the next step interpolate from the right place") {
    // A frame that runs two fixed steps must interpolate from the second, not
    // the first - otherwise the drawn pose lags by a whole step whenever the
    // display falls behind the simulation.
    World world;

    Entity body = world.spawn("Body");
    body.attach<Transform>(currentOf({0.f, 0.f, 0.f}));
    ege::beginInterpolating(world, body.id());

    // First step.
    ege::recordPreviousTransforms(world);
    world.find<Transform>(body.id())->translation.x = 1.f;

    // Second step, in the same frame.
    ege::recordPreviousTransforms(world);
    world.find<Transform>(body.id())->translation.x = 2.f;

    // Halfway through the second step is 1.5, not 1.0.
    CHECK(ege::renderTransform(world, body.id(), 0.5f).translation.x == doctest::Approx(1.5f));
}
