// Point-light cube shadow maps.
//
// A cube map is the one place in a renderer where the hardware has a strong
// and non-obvious opinion: which face a direction samples, and where on that
// face it lands, are fixed by the specification. Six view matrices that each
// look the right way but disagree with those rules about which way is up will
// still produce a picture - one where shadows are mirrored or rotated on some
// faces and correct on others, which is nearly impossible to debug by eye.
//
// So these tests do not check the matrices against themselves. They compute
// the face and the texture coordinate the way the cube-map specification
// defines them, from the sample direction alone, and check the matrices agree.

#include "render/PointShadows.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/component_wise.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using ege::cubeFaceCount;
using ege::pointShadowFaceFor;
using ege::pointShadowFaceMatrices;
using ege::pointShadowFaceMatrix;
using ege::pointShadowNearPlane;
using ege::pointShadowReferenceDepth;

namespace {

    constexpr float farPlane = 25.f;

    struct FaceUv {
        uint32_t face;
        float u;
        float v;
    };

    // The cube-map lookup rules, transcribed from the specification's table
    // rather than from the engine: the major axis picks the face, and each
    // face names which components become the horizontal and vertical
    // coordinate. This is the independent answer the matrices are checked
    // against.
    FaceUv specCubeLookup(glm::vec3 r) {
        const glm::vec3 magnitude = glm::abs(r);
        float ma = 0.f;
        float sc = 0.f;
        float tc = 0.f;
        uint32_t face = 0;

        if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
            ma = magnitude.x;
            if (r.x >= 0.f) {
                face = 0;
                sc = -r.z;
                tc = -r.y;
            } else {
                face = 1;
                sc = r.z;
                tc = -r.y;
            }
        } else if (magnitude.y >= magnitude.z) {
            ma = magnitude.y;
            if (r.y >= 0.f) {
                face = 2;
                sc = r.x;
                tc = r.z;
            } else {
                face = 3;
                sc = r.x;
                tc = -r.z;
            }
        } else {
            ma = magnitude.z;
            if (r.z >= 0.f) {
                face = 4;
                sc = r.x;
                tc = -r.y;
            } else {
                face = 5;
                sc = -r.x;
                tc = -r.y;
            }
        }

        return FaceUv{face, 0.5f * (sc / ma + 1.f), 0.5f * (tc / ma + 1.f)};
    }

    glm::vec3 project(const glm::mat4& viewProjection, glm::vec3 world) {
        const glm::vec4 clip = viewProjection * glm::vec4{world, 1.f};
        return glm::vec3{clip} / clip.w;
    }

    // A spread of directions that lands on every face, including near the
    // edges where a wrong up vector shows up most clearly.
    std::vector<glm::vec3> sampleDirections() {
        std::vector<glm::vec3> directions;
        for (int i = 0; i < 9; i++) {
            for (int j = 0; j < 9; j++) {
                const float a = -0.9f + 1.8f * static_cast<float>(i) / 8.f;
                const float b = -0.9f + 1.8f * static_cast<float>(j) / 8.f;
                directions.push_back(glm::normalize(glm::vec3{1.f, a, b}));
                directions.push_back(glm::normalize(glm::vec3{-1.f, a, b}));
                directions.push_back(glm::normalize(glm::vec3{a, 1.f, b}));
                directions.push_back(glm::normalize(glm::vec3{a, -1.f, b}));
                directions.push_back(glm::normalize(glm::vec3{a, b, 1.f}));
                directions.push_back(glm::normalize(glm::vec3{a, b, -1.f}));
            }
        }
        return directions;
    }

}  // namespace

TEST_CASE("the face a direction picks matches the cube-map rule") {
    for (const glm::vec3& direction : sampleDirections()) {
        CHECK(pointShadowFaceFor(direction) == specCubeLookup(direction).face);
    }
}

TEST_CASE("every direction falls inside exactly one face's frustum") {
    const glm::vec3 light{2.f, -1.f, 3.f};
    const std::array<glm::mat4, cubeFaceCount> faces = pointShadowFaceMatrices(light, farPlane);

    for (const glm::vec3& direction : sampleDirections()) {
        const glm::vec3 world = light + direction * 4.f;

        int inside = 0;
        for (uint32_t face = 0; face < cubeFaceCount; face++) {
            const glm::vec3 ndc = project(faces[face], world);
            // Strictly inside, so a point exactly on a shared edge - which is
            // in two frusta by construction - does not confuse the count.
            if (ndc.x > -0.999f && ndc.x < 0.999f && ndc.y > -0.999f && ndc.y < 0.999f &&
                ndc.z >= 0.f && ndc.z <= 1.f) {
                inside++;
            }
        }
        CHECK(inside == 1);
    }
}

TEST_CASE("a point lands where the cube-map rule says it lands on its face") {
    // The test that catches a wrong up vector. A face whose image is mirrored
    // or rotated still contains the point, so "inside the frustum" is not
    // enough - the position within the face has to agree too.
    const glm::vec3 light{-1.f, 0.5f, 2.f};
    const std::array<glm::mat4, cubeFaceCount> faces = pointShadowFaceMatrices(light, farPlane);

    for (const glm::vec3& direction : sampleDirections()) {
        const FaceUv expected = specCubeLookup(direction);
        const glm::vec3 world = light + direction * 3.f;
        const glm::vec3 ndc = project(faces[expected.face], world);

        // Vulkan's clip space has +Y downwards, and a cube face's v runs the
        // same way, so both map straight through with no flip.
        CHECK(0.5f * (ndc.x + 1.f) == doctest::Approx(expected.u).epsilon(1e-4));
        CHECK(0.5f * (ndc.y + 1.f) == doctest::Approx(expected.v).epsilon(1e-4));
    }
}

TEST_CASE("the reference depth matches what the face matrix writes") {
    // The shader recomputes the stored depth from the light-to-fragment
    // vector rather than reading a matrix, so the closed form has to agree
    // with the matrices exactly. If it drifts, every surface shadows itself
    // or nothing does, depending on which way.
    const glm::vec3 light{0.5f, -2.f, 1.f};
    const std::array<glm::mat4, cubeFaceCount> faces = pointShadowFaceMatrices(light, farPlane);

    for (const glm::vec3& direction : sampleDirections()) {
        for (float distance : {0.3f, 1.f, 4.f, 12.f}) {
            const glm::vec3 offset = direction * distance;
            const glm::vec3 world = light + offset;

            const uint32_t face = pointShadowFaceFor(offset);
            const float axisDistance = glm::compMax(glm::abs(offset));
            const float expected =
                pointShadowReferenceDepth(axisDistance, pointShadowNearPlane, farPlane);

            CHECK(project(faces[face], world).z == doctest::Approx(expected).epsilon(1e-4));
        }
    }
}

TEST_CASE("depth grows with distance and spans the whole range") {
    CHECK(
        pointShadowReferenceDepth(pointShadowNearPlane, pointShadowNearPlane, farPlane) ==
        doctest::Approx(0.f));
    CHECK(
        pointShadowReferenceDepth(farPlane, pointShadowNearPlane, farPlane) ==
        doctest::Approx(1.f));

    float previous = -1.f;
    for (int i = 1; i <= 50; i++) {
        const float distance =
            pointShadowNearPlane + (farPlane - pointShadowNearPlane) * static_cast<float>(i) / 50.f;
        const float depth = pointShadowReferenceDepth(distance, pointShadowNearPlane, farPlane);
        CHECK(depth > previous);
        previous = depth;
    }
}

TEST_CASE("the axis distance is used, not the straight-line distance") {
    // A point in the corner of a face is further from the light than its
    // distance along the face's axis. Comparing against the straight-line
    // distance would make every corner report itself as further away than the
    // map says, and the whole edge of every face would shadow itself.
    const glm::vec3 corner = glm::normalize(glm::vec3{1.f, 0.9f, 0.9f}) * 5.f;
    const float axisDistance = glm::compMax(glm::abs(corner));
    const float straightLine = glm::length(corner);

    REQUIRE(straightLine > axisDistance);
    CHECK(
        pointShadowReferenceDepth(axisDistance, pointShadowNearPlane, farPlane) <
        pointShadowReferenceDepth(straightLine, pointShadowNearPlane, farPlane));
}
