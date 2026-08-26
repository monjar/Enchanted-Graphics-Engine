// The third-person camera's arithmetic.
//
// Where a camera decides to be is a function of numbers; what it sees is not.
// This covers the first, which is where the mistakes are: an angle recovered
// with the wrong sign points the camera at the sky, and smoothing that
// depends on the frame rate is a camera tuned on one machine and lagging on
// another.

#include "platform/FollowCamera.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <doctest/doctest.h>

#include <cmath>

using ege::dampTowards;
using ege::FollowCameraSettings;
using ege::followCameraTarget;
using ege::lookAngles;
using ege::Transform;

namespace {

    // The forward vector the rest of the engine builds from a pitch and a
    // yaw: a Y-then-X rotation applied to +Z.
    glm::vec3 forwardOf(glm::vec3 rotation) {
        const float pitch = rotation.x;
        const float yaw = rotation.y;
        return glm::vec3{
            std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    }

}  // namespace

TEST_CASE("look angles are the inverse of the forward the engine builds") {
    // Round trip: the angles recovered from a direction rebuild it.
    for (const glm::vec3 wanted :
         {glm::vec3{0.f, 0.f, 1.f},
          glm::vec3{1.f, 0.f, 0.f},
          glm::vec3{0.f, 0.f, -1.f},
          glm::normalize(glm::vec3{1.f, -1.f, 1.f}),
          glm::normalize(glm::vec3{-0.3f, 0.8f, -0.5f})}) {
        const glm::vec3 angles = lookAngles(glm::vec3{0.f}, wanted);
        const glm::vec3 rebuilt = forwardOf(angles);
        CHECK(rebuilt.x == doctest::Approx(wanted.x).epsilon(1e-4));
        CHECK(rebuilt.y == doctest::Approx(wanted.y).epsilon(1e-4));
        CHECK(rebuilt.z == doctest::Approx(wanted.z).epsilon(1e-4));
        // A third-person camera that rolls is a bug, not a feature.
        CHECK(angles.z == doctest::Approx(0.f));
    }

    // Nowhere to look is no rotation rather than a division by zero.
    CHECK(glm::length(lookAngles(glm::vec3{1.f}, glm::vec3{1.f})) == doctest::Approx(0.f));
}

TEST_CASE("the camera sits behind and above what it is watching") {
    FollowCameraSettings settings{};
    settings.distance = 3.f;
    settings.height = 1.f;
    settings.aimHeight = 0.5f;

    // Yaw zero is forward along +Z, so behind is -Z. Up is +Y here.
    const Transform pose =
        followCameraTarget(glm::vec3{0.f}, 0.f, glm::vec3{0.f, 1.f, 0.f}, settings);
    CHECK(pose.translation.x == doctest::Approx(0.f));
    CHECK(pose.translation.y == doctest::Approx(1.f));
    CHECK(pose.translation.z == doctest::Approx(-3.f));

    // And it looks back at the point it is aiming for, which is above the
    // subject rather than at its feet.
    const glm::vec3 aim = glm::vec3{0.f, 0.5f, 0.f};
    const glm::vec3 direction = glm::normalize(aim - pose.translation);
    const glm::vec3 looking = forwardOf(pose.rotation);
    CHECK(looking.x == doctest::Approx(direction.x).epsilon(1e-4));
    CHECK(looking.y == doctest::Approx(direction.y).epsilon(1e-4));
    CHECK(looking.z == doctest::Approx(direction.z).epsilon(1e-4));
}

TEST_CASE("a quarter turn puts the camera off to the side") {
    FollowCameraSettings settings{};
    settings.distance = 2.f;
    settings.height = 0.f;
    settings.aimHeight = 0.f;

    // A yaw of a quarter turn faces +X, so behind is -X.
    const Transform pose = followCameraTarget(
        glm::vec3{5.f, 0.f, 5.f}, glm::half_pi<float>(), glm::vec3{0.f, 1.f, 0.f}, settings);
    CHECK(pose.translation.x == doctest::Approx(3.f));
    CHECK(pose.translation.z == doctest::Approx(5.f).epsilon(1e-4));
}

TEST_CASE("up is not assumed to be +Y") {
    FollowCameraSettings settings{};
    settings.distance = 2.f;
    settings.height = 1.f;
    settings.aimHeight = 0.f;

    // The demo scene's frame: -Y is up, so "above the subject" is a smaller y.
    const Transform pose =
        followCameraTarget(glm::vec3{0.f}, 0.f, glm::vec3{0.f, -1.f, 0.f}, settings);
    CHECK(pose.translation.y == doctest::Approx(-1.f));
    CHECK(pose.translation.z == doctest::Approx(-2.f));
    // Looking down at the subject, which in this frame means towards +Y.
    CHECK(forwardOf(pose.rotation).y > 0.f);
}

TEST_CASE("smoothing does not depend on the frame rate") {
    const glm::vec3 start{0.f};
    const glm::vec3 target{10.f, 0.f, 0.f};

    // One second, taken in one step or in a hundred, arrives at the same
    // place. The naive lerp-by-rate-times-dt does not, which is what makes
    // this worth pinning.
    const glm::vec3 oneStep = dampTowards(start, target, 3.f, 1.f);

    glm::vec3 many = start;
    for (int i = 0; i < 100; i++) {
        many = dampTowards(many, target, 3.f, 0.01f);
    }
    CHECK(many.x == doctest::Approx(oneStep.x).epsilon(1e-3));

    // And it is the fraction the exponential says: 1 - e^-3 of the way.
    CHECK(oneStep.x == doctest::Approx(10.f * (1.f - std::exp(-3.f))));
}

TEST_CASE("smoothing approaches without overshooting, and a zero rate cuts") {
    glm::vec3 position{0.f};
    const glm::vec3 target{1.f, 0.f, 0.f};
    for (int i = 0; i < 1000; i++) {
        position = dampTowards(position, target, 5.f, 1.f / 60.f);
        CHECK(position.x <= 1.f);
    }
    CHECK(position.x == doctest::Approx(1.f));

    // No smoothing at all is a cut, not a freeze.
    CHECK(dampTowards(glm::vec3{0.f}, target, 0.f, 0.016f).x == doctest::Approx(1.f));
    // And no time passing leaves it where a cut would put it too, rather than
    // dividing by a zero step.
    CHECK(dampTowards(glm::vec3{0.f}, target, 5.f, 0.f).x == doctest::Approx(1.f));
}

TEST_CASE("the framing scales to the subject it is following") {
    ege::FollowCameraSettings written{};
    // Twice the height the defaults were written for.
    const ege::FollowCameraSettings framed =
        ege::framedFor(written, written.writtenForHeight * 2.f);

    CHECK(framed.distance == doctest::Approx(written.distance * 2.f));
    CHECK(framed.height == doctest::Approx(written.height * 2.f));
    CHECK(framed.aimHeight == doctest::Approx(written.aimHeight * 2.f));
    CHECK(framed.minDistance == doctest::Approx(written.minDistance * 2.f));
    CHECK(framed.wallMargin == doctest::Approx(written.wallMargin * 2.f));

    // Not the lag: how quickly a camera catches up is a matter of feel
    // rather than of size, and a big character followed by a sluggish camera
    // is not what anybody meant.
    CHECK(framed.lag == doctest::Approx(written.lag));

    // And the result says what it is now written for, so framing it again
    // for the same height changes nothing.
    CHECK(framed.writtenForHeight == doctest::Approx(written.writtenForHeight * 2.f));
    const ege::FollowCameraSettings again = ege::framedFor(framed, framed.writtenForHeight);
    CHECK(again.distance == doctest::Approx(framed.distance));
}

TEST_CASE("framing for nothing leaves the framing alone") {
    ege::FollowCameraSettings written{};
    // A subject with no height, or settings that admit to no reference:
    // scaling by either would be a division nobody meant.
    CHECK(ege::framedFor(written, 0.f).distance == doctest::Approx(written.distance));
    CHECK(ege::framedFor(written, -3.f).distance == doctest::Approx(written.distance));

    ege::FollowCameraSettings unreferenced = written;
    unreferenced.writtenForHeight = 0.f;
    CHECK(ege::framedFor(unreferenced, 1.7f).distance == doctest::Approx(written.distance));
}

TEST_CASE("a taller subject is watched from further back") {
    // The whole point, at the level the camera actually works at: the same
    // defaults put the camera a fixed number of subject-heights away, so one
    // set of numbers frames a mouse and a giant.
    const ege::FollowCameraSettings small = ege::framedFor(ege::FollowCameraSettings{}, 0.6f);
    const ege::FollowCameraSettings large = ege::framedFor(ege::FollowCameraSettings{}, 1.7f);

    const ege::Transform watchingSmall =
        ege::followCameraTarget(glm::vec3{0.f}, 0.f, glm::vec3{0.f, 1.f, 0.f}, small);
    const ege::Transform watchingLarge =
        ege::followCameraTarget(glm::vec3{0.f}, 0.f, glm::vec3{0.f, 1.f, 0.f}, large);

    CHECK(glm::length(watchingLarge.translation) > glm::length(watchingSmall.translation));
    // And by the ratio of the two subjects, not by some other amount.
    CHECK(
        glm::length(watchingLarge.translation) ==
        doctest::Approx(glm::length(watchingSmall.translation) * (1.7f / 0.6f)).epsilon(0.001f));
}
