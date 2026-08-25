// The arithmetic of GPU-driven occlusion culling.
//
// The running copy of every function here is GLSL, in
// shaders/gpu_cull_common.glsl, where no test can reach it. These pin the
// C++ mirror in src/render/GpuCulling.cpp instead, and the two are kept in
// step by hand - which is the same arrangement the cluster grid, the SSAO
// kernel and the shadow maths already live under.

#include "render/Camera.hpp"
#include "render/GpuCulling.hpp"

#include <doctest/doctest.h>

#include <array>
#include <vector>

using ege::Aabb;
using ege::boundingSphere;
using ege::chooseLevel;
using ege::occludedAtLevel;
using ege::projectSphere;
using ege::SeedBatch;
using ege::seedDrawCommands;
using ege::SphereScreenBounds;

namespace {

    constexpr float nearPlane = 0.1f;
    constexpr float farPlane = 100.f;

    ege::Camera testCamera() {
        ege::Camera camera;
        camera.setPerspectiveProjection(glm::radians(60.f), 4.f / 3.f, nearPlane, farPlane);
        return camera;
    }

    // What the projection turns a view-space z into, straight from its own
    // terms - the same formula projectSphere uses, checked here against a
    // full corner projection so the shortcut cannot drift from the truth.
    float depthOf(const glm::mat4& projection, float z) {
        return projection[2][2] + projection[3][2] / z;
    }

}  // namespace

TEST_CASE("the bounding sphere covers its box exactly") {
    Aabb box{};
    box.min = {-1.f, -2.f, -3.f};
    box.max = {3.f, 2.f, 1.f};

    const glm::vec4 sphere = boundingSphere(box);
    CHECK(sphere.x == doctest::Approx(1.f));
    CHECK(sphere.y == doctest::Approx(0.f));
    CHECK(sphere.z == doctest::Approx(-1.f));
    // Half the diagonal: sqrt(2^2 + 2^2 + 2^2).
    CHECK(sphere.w == doctest::Approx(glm::length(glm::vec3{2.f})));
}

TEST_CASE("a sphere ahead of the camera projects to a centred rectangle") {
    const ege::Camera camera = testCamera();

    // View space is identity: the camera at the origin looking down +Z,
    // which is the convention this engine's view matrices produce.
    const SphereScreenBounds bounds = projectSphere(
        glm::mat4{1.f}, camera.getProjection(), glm::vec4{0.f, 0.f, 10.f, 1.f}, nearPlane);

    REQUIRE(bounds.testable);
    // Centred, so the rectangle straddles 0.5 symmetrically.
    CHECK(bounds.minUv.x == doctest::Approx(1.f - bounds.maxUv.x).epsilon(0.001));
    CHECK(bounds.minUv.y == doctest::Approx(1.f - bounds.maxUv.y).epsilon(0.001));
    CHECK(bounds.minUv.x < 0.5f);
    CHECK(bounds.maxUv.x > 0.5f);
    // The nearest point is the centre brought forward by the radius.
    CHECK(bounds.nearestDepth == doctest::Approx(depthOf(camera.getProjection(), 9.f)));
}

TEST_CASE("the projected rectangle contains the sphere's silhouette") {
    const ege::Camera camera = testCamera();
    const glm::vec4 sphere{1.5f, -0.8f, 6.f, 0.75f};

    const SphereScreenBounds bounds =
        projectSphere(glm::mat4{1.f}, camera.getProjection(), sphere, nearPlane);
    REQUIRE(bounds.testable);

    // Sample points on the sphere's surface and check each lands inside the
    // rectangle. The rectangle may be larger than the silhouette - it is the
    // projection of the bounding box - but never smaller, because occlusion
    // demands every texel of it be covered and a missing sliver would let a
    // visible object be culled.
    for (int i = 0; i < 64; i++) {
        const float a = static_cast<float>(i) * 0.3f;
        const float b = static_cast<float>(i) * 0.7f;
        const glm::vec3 onSphere =
            glm::vec3{sphere} +
            sphere.w * glm::vec3{std::sin(a) * std::cos(b), std::sin(a) * std::sin(b), std::cos(a)};
        const glm::vec4 clip = camera.getProjection() * glm::vec4{onSphere, 1.f};
        const glm::vec2 uv = glm::vec2{clip} / clip.w * 0.5f + 0.5f;
        CHECK(uv.x >= bounds.minUv.x - 0.001f);
        CHECK(uv.x <= bounds.maxUv.x + 0.001f);
        CHECK(uv.y >= bounds.minUv.y - 0.001f);
        CHECK(uv.y <= bounds.maxUv.y + 0.001f);
    }
}

TEST_CASE("a sphere reaching the near plane is not testable") {
    const ege::Camera camera = testCamera();

    // Nearest point behind the plane, then behind the camera entirely.
    CHECK_FALSE(projectSphere(
                    glm::mat4{1.f},
                    camera.getProjection(),
                    glm::vec4{0.f, 0.f, nearPlane + 0.4f, 0.5f},
                    nearPlane)
                    .testable);
    CHECK_FALSE(
        projectSphere(
            glm::mat4{1.f}, camera.getProjection(), glm::vec4{0.f, 0.f, -5.f, 1.f}, nearPlane)
            .testable);
    // Just past the plane is testable again.
    CHECK(projectSphere(
              glm::mat4{1.f},
              camera.getProjection(),
              glm::vec4{0.f, 0.f, nearPlane + 0.51f, 0.5f},
              nearPlane)
              .testable);
}

TEST_CASE("level choice takes the finest level that still bounds the taps") {
    const std::array<glm::uvec2, 4> extents{
        glm::uvec2{64, 48}, glm::uvec2{32, 24}, glm::uvec2{16, 12}, glm::uvec2{8, 6}};

    // A tiny rectangle fits the finest level.
    CHECK(chooseLevel({0.5f, 0.5f}, {0.55f, 0.55f}, extents.data(), 4) == 0);
    // A tenth of the screen is 6.4 texels at the finest level - too wide -
    // and 3.2 at the next, which fits.
    CHECK(chooseLevel({0.f, 0.f}, {0.1f, 0.1f}, extents.data(), 4) == 1);
    // Half the screen only fits the coarsest.
    CHECK(chooseLevel({0.f, 0.f}, {0.5f, 0.5f}, extents.data(), 4) == 3);
    // The whole screen fits nothing, and is drawn without a test.
    CHECK(chooseLevel({0.f, 0.f}, {1.f, 1.f}, extents.data(), 4) == -1);
}

TEST_CASE("a covered rectangle is occluded and an uncovered one is not") {
    // A 4x4 level where everything drawn so far sits at depth 0.5.
    std::vector<float> level(16, 0.5f);
    const glm::uvec2 extent{4, 4};

    // An object behind that is hidden; one in front is not.
    CHECK(occludedAtLevel(level.data(), extent, {0.3f, 0.3f}, {0.6f, 0.6f}, 0.8f));
    CHECK_FALSE(occludedAtLevel(level.data(), extent, {0.3f, 0.3f}, {0.6f, 0.6f}, 0.3f));

    // One far texel inside the rectangle - a gap in the occluder - and the
    // object might be seen through it.
    level[1 * 4 + 1] = 1.f;
    CHECK_FALSE(occludedAtLevel(level.data(), extent, {0.3f, 0.3f}, {0.6f, 0.6f}, 0.8f));
    // The same gap outside the rectangle changes nothing.
    CHECK(occludedAtLevel(level.data(), extent, {0.6f, 0.6f}, {0.9f, 0.9f}, 0.8f));
}

TEST_CASE("an empty depth buffer occludes nothing") {
    // Cleared depth is 1.0, the far plane; nothing can be behind it.
    std::vector<float> level(16, 1.f);
    CHECK_FALSE(occludedAtLevel(level.data(), {4, 4}, {0.f, 0.f}, {1.f, 1.f}, 0.999f));
}

TEST_CASE("an object is never occluded by its own recorded depth") {
    // The pyramid the late pass tests against includes what the early pass
    // drew - the object itself, if it was visible last frame. Its stored
    // depth can be equal to its own nearest depth, and equal must not cull.
    std::vector<float> level(16, 0.75f);
    CHECK_FALSE(occludedAtLevel(level.data(), {4, 4}, {0.2f, 0.2f}, {0.7f, 0.7f}, 0.75f));
}

TEST_CASE("a rectangle reaching the edge stays inside the level") {
    std::vector<float> level(16, 0.5f);
    // maxUv of exactly 1.0 would name a texel past the edge if the clamp
    // were missing; the answer itself is an ordinary occlusion.
    CHECK(occludedAtLevel(level.data(), {4, 4}, {0.9f, 0.9f}, {1.f, 1.f}, 0.9f));
}

TEST_CASE("seeded commands know everything but the instance counts") {
    std::vector<SeedBatch> batches(2);
    batches[0] = {900, 0, true};
    batches[1] = {36, 5, true};

    const std::vector<uint32_t> words = seedDrawCommands(batches, 4096);
    REQUIRE(words.size() == 2 * 2 * ege::drawCommandWords);

    // Early commands: index count and the batch's own window.
    CHECK(words[0] == 900);
    CHECK(words[4] == 0);
    CHECK(words[5] == 36);
    CHECK(words[9] == 5);

    // Late commands: the same draws, windows a whole buffer along.
    CHECK(words[10] == 900);
    CHECK(words[14] == 4096);
    CHECK(words[15] == 36);
    CHECK(words[19] == 4096 + 5);

    // Every instance count seeded zero - the culling passes own them.
    for (std::size_t c = 0; c < 4; c++) {
        CHECK(words[c * ege::drawCommandWords + ege::drawCommandInstanceCountWord] == 0);
    }
}

TEST_CASE("a non-indexed batch seeds the four-word command layout") {
    std::vector<SeedBatch> batches(1);
    batches[0] = {2401, 7, false};

    const std::vector<uint32_t> words = seedDrawCommands(batches, 4096);
    REQUIRE(words.size() == 2 * ege::drawCommandWords);

    // VkDrawIndirectCommand: vertexCount, instanceCount, firstVertex,
    // firstInstance - the first instance one word earlier than the indexed
    // layout puts it, which is why the shader is never asked to read it.
    CHECK(words[0] == 2401);
    CHECK(words[1] == 0);
    CHECK(words[3] == 7);
    CHECK(words[5 + 3] == 4096 + 7);
    // The instance count sits at the same word in both layouts, which is the
    // one invariant the shader's atomic depends on.
    CHECK(words[ege::drawCommandInstanceCountWord] == 0);
}
