// Spot lights: the cone and the map.
//
// The cone is the part that looks wrong in a way that is hard to name - a
// falloff that runs the wrong way gives a light bright at its rim and dark in
// its middle, which reads as "the material is odd" rather than as a bug in the
// light. So the direction of the falloff is pinned here, along with the
// property that the shadow map covers the cone and nothing outside it.

#include "render/SpotShadows.hpp"
#include "scene/Components.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <doctest/doctest.h>

#include <cmath>

using ege::spotConeAttenuation;
using ege::spotShadowMatrix;
using ege::spotShadowNearPlane;

namespace {

    constexpr float range = 20.f;

    // A direction at a given angle from the spot's axis, in the plane the
    // axis and a chosen perpendicular span. The reference vector has to be
    // picked rather than fixed: a hardcoded one is parallel to some axis a
    // test will use, and the cross product then yields a zero vector whose
    // normalisation is NaN - which shows up as a failing assertion about the
    // cone rather than as an obviously broken helper.
    glm::vec3 atAngle(glm::vec3 axis, float angle) {
        const glm::vec3 reference =
            std::abs(axis.z) > 0.9f ? glm::vec3{1.f, 0.f, 0.f} : glm::vec3{0.f, 0.f, 1.f};
        const glm::vec3 side = glm::normalize(glm::cross(axis, reference));
        return glm::normalize(axis * std::cos(angle) + side * std::sin(angle));
    }

    glm::vec3 project(const glm::mat4& viewProjection, glm::vec3 world) {
        const glm::vec4 clip = viewProjection * glm::vec4{world, 1.f};
        return glm::vec3{clip} / clip.w;
    }

    bool insideMap(glm::vec3 ndc) {
        return ndc.x >= -1.f && ndc.x <= 1.f && ndc.y >= -1.f && ndc.y <= 1.f && ndc.z >= 0.f &&
               ndc.z <= 1.f;
    }

}  // namespace

TEST_CASE("the cone is bright in the middle and dark outside") {
    const glm::vec3 axis{1.f, 0.f, 0.f};
    const float inner = 0.3f;
    const float outer = 0.5f;
    const float cosInner = std::cos(inner);
    const float cosOuter = std::cos(outer);

    // Straight down the axis: full brightness.
    CHECK(spotConeAttenuation(axis, axis, cosOuter, cosInner) == doctest::Approx(1.f));
    // Inside the inner angle: still full.
    CHECK(
        spotConeAttenuation(axis, atAngle(axis, inner * 0.5f), cosOuter, cosInner) ==
        doctest::Approx(1.f));
    // Past the outer angle: nothing.
    CHECK(
        spotConeAttenuation(axis, atAngle(axis, outer * 1.2f), cosOuter, cosInner) ==
        doctest::Approx(0.f));
    // Behind the light entirely: nothing.
    CHECK(spotConeAttenuation(axis, -axis, cosOuter, cosInner) == doctest::Approx(0.f));
}

TEST_CASE("the falloff runs from bright to dark, not the other way") {
    // The failure this catches does not look like a bug in the light. It looks
    // like a ring: dark in the middle of the cone and bright at its edge.
    const glm::vec3 axis = glm::normalize(glm::vec3{0.3f, -1.f, 0.2f});
    const float inner = 0.2f;
    const float outer = 0.6f;
    const float cosInner = std::cos(inner);
    const float cosOuter = std::cos(outer);

    float previous = 2.f;
    for (int step = 0; step <= 40; step++) {
        const float angle = outer * 1.1f * static_cast<float>(step) / 40.f;
        const float attenuation =
            spotConeAttenuation(axis, atAngle(axis, angle), cosOuter, cosInner);

        CHECK(attenuation <= previous + 1e-5f);
        CHECK(attenuation >= 0.f);
        CHECK(attenuation <= 1.f);
        previous = attenuation;
    }
}

TEST_CASE("an inner angle wider than the outer gives a hard edge, not a ring") {
    // Authored angles can cross over. Inverting the range would light the rim
    // and darken the core; a hard-edged cone is merely plain.
    const glm::vec3 axis{0.f, 0.f, 1.f};
    const float cosOuter = std::cos(0.3f);
    const float cosInner = std::cos(0.6f);  // wider than the outer, on purpose

    CHECK(spotConeAttenuation(axis, axis, cosOuter, cosInner) == doctest::Approx(1.f));
    CHECK(
        spotConeAttenuation(axis, atAngle(axis, 0.2f), cosOuter, cosInner) == doctest::Approx(1.f));
    CHECK(
        spotConeAttenuation(axis, atAngle(axis, 0.5f), cosOuter, cosInner) == doctest::Approx(0.f));
}

TEST_CASE("the shadow map covers the cone and little else") {
    const glm::vec3 position{1.f, -2.f, 0.5f};
    const glm::vec3 direction = glm::normalize(glm::vec3{0.2f, 1.f, 0.3f});
    const float outer = 0.45f;
    const glm::mat4 matrix = spotShadowMatrix(position, direction, outer, range);

    // Anything inside the cone and in front of the light lands on the map.
    for (int step = 0; step <= 20; step++) {
        const float angle = outer * 0.98f * static_cast<float>(step) / 20.f;
        for (float distance : {0.5f, 3.f, 15.f}) {
            const glm::vec3 world = position + atAngle(direction, angle) * distance;
            CHECK(insideMap(project(matrix, world)));
        }
    }

    // Well outside it does not - the map would be spending texels on darkness.
    const glm::vec3 outside = position + atAngle(direction, outer * 2.f) * 5.f;
    CHECK_FALSE(insideMap(project(matrix, outside)));
}

TEST_CASE("the map spans the light's own range in depth") {
    const glm::vec3 position{0.f, 0.f, 0.f};
    const glm::vec3 direction{0.f, 0.f, 1.f};
    const glm::mat4 matrix = spotShadowMatrix(position, direction, 0.4f, range);

    CHECK(project(matrix, direction * spotShadowNearPlane).z == doctest::Approx(0.f).epsilon(1e-3));
    CHECK(project(matrix, direction * range).z == doctest::Approx(1.f).epsilon(1e-3));
    // Beyond the range is off the end of the map rather than clamped into it.
    CHECK(project(matrix, direction * (range * 1.5f)).z > 1.f);
}

TEST_CASE("a spot pointing straight down still gets a usable basis") {
    // The degenerate case for a look-at: an up vector parallel to the
    // direction produces a matrix full of NaNs, and a ceiling light is
    // exactly that case.
    for (const glm::vec3& direction : {glm::vec3{0.f, 1.f, 0.f}, glm::vec3{0.f, -1.f, 0.f}}) {
        const glm::mat4 matrix = spotShadowMatrix(glm::vec3{0.f}, direction, 0.4f, range);
        const glm::vec3 ndc = project(matrix, direction * 5.f);

        CHECK(std::isfinite(ndc.x));
        CHECK(std::isfinite(ndc.y));
        CHECK(std::isfinite(ndc.z));
        CHECK(insideMap(ndc));
    }
}

TEST_CASE("an absurd cone angle still produces a finite matrix") {
    // A half-angle at or past a right angle has no finite frustum. Clamping
    // costs a wrong-looking cone; not clamping costs every value in the map.
    const glm::mat4 matrix =
        spotShadowMatrix(glm::vec3{0.f}, glm::vec3{0.f, 0.f, 1.f}, 3.0f, range);
    const glm::vec3 ndc = project(matrix, glm::vec3{0.f, 0.f, 5.f});

    CHECK(std::isfinite(ndc.x));
    CHECK(std::isfinite(ndc.y));
    CHECK(std::isfinite(ndc.z));
}

// ---- how a spot is aimed ---------------------------------------------------

TEST_CASE("a spot shines down its transform's forward axis") {
    // A spot is aimed by rotating the entity rather than by carrying a
    // direction of its own, so what "forward" means for a Transform is part
    // of the light's contract. This scene treats -Y as up, so a spot mounted
    // above the floor and pointing at it has a forward with positive Y - and
    // getting the sign wrong aims a ceiling light at the sky, which shows up
    // as a light that appears to do nothing at all.
    const auto forwardFor = [](glm::vec3 rotation) {
        ege::Transform transform{};
        transform.rotation = rotation;
        return glm::normalize(glm::vec3{transform.mat4() * glm::vec4{0.f, 0.f, 1.f, 0.f}});
    };

    // Unrotated, a spot points along +Z, away from the default camera.
    const glm::vec3 unrotated = forwardFor(glm::vec3{0.f});
    CHECK(unrotated.z == doctest::Approx(1.f));

    // Pitched by a quarter turn it points along one of the vertical axes;
    // which one is what the demo's ceiling spot depends on.
    // Named by axis rather than by "up" and "down" on purpose: this scene's
    // up is -Y, so the two words point opposite ways depending on which you
    // mean, and picking the wrong sign here aims a ceiling light at the sky.
    const glm::vec3 negativePitch = forwardFor(glm::vec3{-glm::half_pi<float>(), 0.f, 0.f});
    const glm::vec3 positivePitch = forwardFor(glm::vec3{glm::half_pi<float>(), 0.f, 0.f});
    CHECK(negativePitch.y == doctest::Approx(1.f));   // +Y, which is downwards here
    CHECK(positivePitch.y == doctest::Approx(-1.f));  // -Y, which is upwards here
}
