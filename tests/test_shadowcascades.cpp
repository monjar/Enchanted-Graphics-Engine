// Shadow cascade fitting.
//
// All of it is matrix arithmetic with no Vulkan in it, which is the reason
// the fitting lives in its own file: the properties that make cascades look
// right - every visible surface falls inside some cascade, the maps do not
// resize as the camera turns, the edges do not crawl as it moves - are
// checkable on a machine with no GPU, and each of them fails quietly on one.

#include "render/ShadowCascades.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <doctest/doctest.h>

#include <array>

using ege::CascadeSettings;
using ege::cascadeSplitDistances;
using ege::fitShadowCascades;
using ege::maxShadowCascades;
using ege::ShadowCascadeSet;

namespace {

    constexpr float nearPlane = 0.1f;
    constexpr float farPlane = 100.f;

    glm::mat4 cameraViewProjection(glm::vec3 eye, glm::vec3 target) {
        glm::mat4 projection = glm::perspective(glm::radians(50.f), 4.f / 3.f, nearPlane, farPlane);
        return projection * glm::lookAt(eye, target, glm::vec3{0.f, 1.f, 0.f});
    }

    glm::mat4 inverseCamera(glm::vec3 eye, glm::vec3 target) {
        return glm::inverse(cameraViewProjection(eye, target));
    }

    // Where a world point lands in a cascade's clip space. Inside the map is
    // xy within [-1, 1] and z within [0, 1] - Vulkan's clip volume.
    glm::vec3 project(const glm::mat4& viewProjection, glm::vec3 world) {
        const glm::vec4 clip = viewProjection * glm::vec4{world, 1.f};
        return glm::vec3{clip} / clip.w;
    }

    bool insideMap(glm::vec3 ndc) {
        return ndc.x >= -1.f && ndc.x <= 1.f && ndc.y >= -1.f && ndc.y <= 1.f && ndc.z >= 0.f &&
               ndc.z <= 1.f;
    }

    // The ortho half-width the cascade covers. Recovered from the length of
    // the matrix's first row rather than from its [0][0]: the combined
    // view-projection folds the light's rotation into every element, and a
    // single element therefore measures the basis as much as the scale. The
    // rotation is orthonormal, so the row's length is the scale alone.
    float mapHalfWidth(const glm::mat4& viewProjection) {
        const glm::vec3 row0{viewProjection[0][0], viewProjection[1][0], viewProjection[2][0]};
        return 1.f / glm::length(row0);
    }

    CascadeSettings defaultSettings() {
        CascadeSettings settings{};
        settings.count = 4;
        settings.resolution = 2048;
        return settings;
    }

}  // namespace

TEST_CASE("splits increase and end exactly at the far plane") {
    const auto splits = cascadeSplitDistances(nearPlane, farPlane, 4, 0.7f);

    CHECK(splits[0] > nearPlane);
    for (std::size_t i = 1; i < 4; i++) {
        CHECK(splits[i] > splits[i - 1]);
    }
    // Exactly, not approximately: everything beyond the last split is
    // unshadowed, and that boundary belongs where the caller put it.
    CHECK(splits[3] == farPlane);
}

TEST_CASE("the blend sits between a uniform and a logarithmic split") {
    const auto uniform = cascadeSplitDistances(nearPlane, farPlane, 4, 0.f);
    const auto logarithmic = cascadeSplitDistances(nearPlane, farPlane, 4, 1.f);
    const auto blended = cascadeSplitDistances(nearPlane, farPlane, 4, 0.7f);

    // Logarithmic puts the near splits closer in; the blend lands between the
    // two, which is the whole point of having a lambda.
    for (std::size_t i = 0; i < 3; i++) {
        CHECK(logarithmic[i] < uniform[i]);
        CHECK(blended[i] <= uniform[i]);
        CHECK(blended[i] >= logarithmic[i]);
    }
}

TEST_CASE("every corner of a slice falls inside that cascade's map") {
    // The property that decides whether anything is shadowed at all: if a
    // slice's own geometry projects outside its map, the shadow test samples
    // the border and everything reads as lit.
    const glm::mat4 inverse = inverseCamera({4.f, 3.f, 4.f}, {0.f, 0.f, 0.f});
    const ShadowCascadeSet set = fitShadowCascades(
        inverse,
        glm::normalize(glm::vec3{0.5f, -1.f, 0.3f}),
        nearPlane,
        farPlane,
        defaultSettings());

    REQUIRE(set.count == 4);

    const auto splits = cascadeSplitDistances(nearPlane, farPlane, 4, 0.7f);
    float sliceStart = nearPlane;

    for (uint32_t cascade = 0; cascade < set.count; cascade++) {
        const float sliceEnd = splits[cascade];
        const float startFraction = (sliceStart - nearPlane) / (farPlane - nearPlane);
        const float endFraction = (sliceEnd - nearPlane) / (farPlane - nearPlane);

        // Rebuild the slice's corners the way the fit sees them.
        std::array<glm::vec3, 8> worldCorners{};
        constexpr std::array<glm::vec3, 4> nearClip{
            glm::vec3{-1.f, -1.f, 0.f},
            glm::vec3{1.f, -1.f, 0.f},
            glm::vec3{-1.f, 1.f, 0.f},
            glm::vec3{1.f, 1.f, 0.f}};

        for (std::size_t i = 0; i < 4; i++) {
            const glm::vec4 nearPoint = inverse * glm::vec4{nearClip[i], 1.f};
            const glm::vec4 farPoint = inverse * glm::vec4{nearClip[i].x, nearClip[i].y, 1.f, 1.f};
            const glm::vec3 nearWorld = glm::vec3{nearPoint} / nearPoint.w;
            const glm::vec3 farWorld = glm::vec3{farPoint} / farPoint.w;
            const glm::vec3 edge = farWorld - nearWorld;
            worldCorners[i] = nearWorld + edge * startFraction;
            worldCorners[i + 4] = nearWorld + edge * endFraction;
        }

        for (const glm::vec3& corner : worldCorners) {
            CHECK(insideMap(project(set.cascades[cascade].viewProjection, corner)));
        }

        sliceStart = sliceEnd;
    }
}

TEST_CASE("a caster above the slice still renders into the map") {
    // The extrusion behind the light exists for exactly this: something
    // between the sun and the visible ground is outside the camera's frustum
    // and must still cast into it.
    CascadeSettings settings = defaultSettings();
    settings.casterExtrusion = 30.f;

    const glm::vec3 light = glm::normalize(glm::vec3{0.f, -1.f, 0.f});
    const ShadowCascadeSet set = fitShadowCascades(
        inverseCamera({0.f, 2.f, 6.f}, {0.f, 0.f, 0.f}), light, nearPlane, farPlane, settings);

    // A point twenty units straight up from the origin - well behind the
    // camera's near slice, directly between the sun and the ground.
    const glm::vec3 highCaster{0.f, 20.f, 0.f};
    CHECK(insideMap(project(set.cascades[set.count - 1].viewProjection, highCaster)));
}

TEST_CASE("turning the camera does not resize a cascade") {
    // A box fitted to the frustum corners grows and shrinks as the camera
    // rotates, and every shadow in the scene swims with it. The sphere fit is
    // what makes the map's extent depend on the slice's size alone.
    const glm::vec3 eye{0.f, 3.f, 0.f};
    const glm::vec3 light = glm::normalize(glm::vec3{0.4f, -1.f, 0.2f});

    auto mapExtent = [&](glm::vec3 target) {
        const ShadowCascadeSet set = fitShadowCascades(
            inverseCamera(eye, target), light, nearPlane, farPlane, defaultSettings());
        return mapHalfWidth(set.cascades[0].viewProjection);
    };

    const float facingNorth = mapExtent({0.f, 3.f, -10.f});
    const float facingEast = mapExtent({10.f, 3.f, 0.f});
    const float facingDiagonally = mapExtent({7.f, 3.f, -7.f});

    // Same slice, same size, whichever way the camera looks.
    CHECK(facingEast == doctest::Approx(facingNorth).epsilon(0.02f));
    CHECK(facingDiagonally == doctest::Approx(facingNorth).epsilon(0.02f));
}

TEST_CASE("sliding the camera moves the map in whole texels") {
    // Texel snapping: between two nearby camera positions the light-space
    // origin must differ by a whole number of texels, or the map rasterises
    // from a fractionally different grid each frame and every shadow edge
    // crawls.
    const glm::vec3 light = glm::normalize(glm::vec3{0.3f, -1.f, 0.25f});
    CascadeSettings settings = defaultSettings();

    auto originOf = [&](float x) {
        const ShadowCascadeSet set = fitShadowCascades(
            inverseCamera({x, 3.f, 5.f}, {x, 0.f, -5.f}), light, nearPlane, farPlane, settings);
        const glm::mat4& viewProjection = set.cascades[0].viewProjection;
        // Where the world origin lands in the map, in texels.
        const glm::vec3 ndc = project(viewProjection, glm::vec3{0.f});
        const float halfResolution = static_cast<float>(settings.resolution) * 0.5f;
        return glm::vec2{ndc.x * halfResolution, ndc.y * halfResolution};
    };

    const glm::vec2 first = originOf(0.f);
    const glm::vec2 nudged = originOf(0.013f);

    // Whatever the shift is, it is a whole number of texels.
    const glm::vec2 delta = nudged - first;
    CHECK(std::abs(delta.x - std::round(delta.x)) < 0.01f);
    CHECK(std::abs(delta.y - std::round(delta.y)) < 0.01f);
}

TEST_CASE("a sun straight overhead still produces a usable basis") {
    // lookAt with a parallel up vector degenerates; a sun at noon is exactly
    // that case and must not produce a matrix full of NaNs.
    const ShadowCascadeSet set = fitShadowCascades(
        inverseCamera({0.f, 5.f, 5.f}, {0.f, 0.f, 0.f}),
        glm::vec3{0.f, -1.f, 0.f},
        nearPlane,
        farPlane,
        defaultSettings());

    const glm::mat4& viewProjection = set.cascades[0].viewProjection;
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            CHECK(std::isfinite(viewProjection[column][row]));
        }
    }
    CHECK(insideMap(project(viewProjection, glm::vec3{0.f, 0.f, 0.f})));
}

TEST_CASE("cascade count is clamped to what the shader has room for") {
    CascadeSettings settings = defaultSettings();
    settings.count = 99;

    const ShadowCascadeSet set = fitShadowCascades(
        inverseCamera({0.f, 3.f, 5.f}, {0.f, 0.f, 0.f}),
        glm::vec3{0.f, -1.f, 0.f},
        nearPlane,
        farPlane,
        settings);

    CHECK(set.count == maxShadowCascades);
}

TEST_CASE("cascades follow a camera far from the world origin") {
    // What the fixed box this replaced could not do. That box was anchored at
    // the origin with a 24-unit extent, so a camera anywhere else looked at
    // ground with no shadow map over it at all - the scene simply stopped
    // casting. The demo never left the box, which is why it never showed;
    // this is the regression a test has to hold rather than a picture.
    const glm::vec3 faraway{500.f, 3.f, -400.f};
    const ShadowCascadeSet set = fitShadowCascades(
        inverseCamera(faraway, faraway + glm::vec3{0.f, -1.f, -10.f}),
        glm::normalize(glm::vec3{0.4f, -1.f, 0.3f}),
        nearPlane,
        farPlane,
        defaultSettings());

    // The ground just in front of the camera is covered by the near cascade.
    const glm::vec3 groundAhead = faraway + glm::vec3{0.f, -3.f, -2.f};
    CHECK(insideMap(project(set.cascades[0].viewProjection, groundAhead)));

    // And the old fixed box, for the contrast: 24 units at the origin never
    // came near this camera.
    CHECK(glm::length(faraway) > 24.f);
}

TEST_CASE("the near cascade is far tighter than one map over the whole range") {
    // Where the texels go. One map stretched over the shadowed distance gives
    // every part of it the same density; cascades give the near slice a map
    // sized to the near slice, which is most of the point.
    const ShadowCascadeSet set = fitShadowCascades(
        inverseCamera({0.f, 3.f, 6.f}, {0.f, 0.f, 0.f}),
        glm::normalize(glm::vec3{0.4f, -1.f, 0.3f}),
        nearPlane,
        farPlane,
        defaultSettings());

    const float nearHalfWidth = mapHalfWidth(set.cascades[0].viewProjection);
    const float farHalfWidth = mapHalfWidth(set.cascades[set.count - 1].viewProjection);

    CHECK(nearHalfWidth < farHalfWidth);
    // An order of magnitude of texel density, which is the difference between
    // a shadow edge that reads as an edge and one that reads as a staircase.
    CHECK(farHalfWidth > nearHalfWidth * 8.f);
}
