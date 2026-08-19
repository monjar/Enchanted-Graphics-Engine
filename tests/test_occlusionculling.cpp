// Occlusion culling against a hierarchical depth buffer.
//
// The pyramid arrives from the GPU as an array of floats, so everything that
// decides whether an object is drawn can be exercised here: the pyramid is
// built by hand, the boxes are projected through the engine's own Camera, and
// what is checked is that an occluder hides what is behind it, that it hides
// nothing else, and that every uncertainty comes out as "draw it".

#include "render/Camera.hpp"
#include "render/OcclusionCulling.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using ege::Aabb;
using ege::Camera;
using ege::DepthPyramid;
using ege::occludedByPyramid;
using ege::OcclusionSnapshot;
using ege::projectBounds;
using ege::ScreenBounds;

namespace {

    Camera lookingForward() {
        Camera camera;
        camera.setPerspectiveProjection(glm::radians(50.f), 4.f / 3.f, 0.1f, 100.f);
        // This engine's camera looks down +Z. A test written for the more
        // common -Z convention would put every one of these boxes behind the
        // camera, where nothing is ever culled and every check below passes
        // for the wrong reason.
        camera.setViewDirection(glm::vec3{0.f}, glm::vec3{0.f, 0.f, 1.f});
        return camera;
    }

    glm::mat4 viewProjectionOf(const Camera& camera) {
        return camera.getProjection() * camera.getView();
    }

    Aabb boxAt(glm::vec3 centre, glm::vec3 halfSize) {
        Aabb box{};
        box.min = centre - halfSize;
        box.max = centre + halfSize;
        return box;
    }

    // A pyramid whose every texel says the same thing: something was drawn
    // this far away, everywhere on screen.
    DepthPyramid flatPyramid(uint32_t width, uint32_t height, float depth) {
        const std::vector<float> level(static_cast<std::size_t>(width) * height, depth);
        DepthPyramid pyramid;
        pyramid.build(level.data(), width, height);
        return pyramid;
    }

    // The depth the buffer would hold for a point this far down the camera's
    // forward axis - taken from the engine's own projection rather than from a
    // formula written out here, which would only be the same mistake twice.
    float depthAt(const Camera& camera, float forward) {
        const glm::vec4 clip = viewProjectionOf(camera) * glm::vec4{0.f, 0.f, forward, 1.f};
        return clip.z / clip.w;
    }

}  // namespace

TEST_CASE("a pyramid level records the farthest depth beneath it") {
    // Four texels, one of them further away than the others: the level above
    // has to report that one, because it is the level above's job to say what
    // the worst case anywhere beneath it is.
    const std::vector<float> level0{0.2f, 0.4f, 0.9f, 0.3f};

    DepthPyramid pyramid;
    pyramid.build(level0.data(), 2, 2);

    REQUIRE(pyramid.levelCount() == 2);
    CHECK(pyramid.width(0) == 2);
    CHECK(pyramid.height(0) == 2);
    CHECK(pyramid.width(1) == 1);
    CHECK(pyramid.height(1) == 1);
    CHECK(pyramid.at(1, 0, 0) == doctest::Approx(0.9f));
}

TEST_CASE("an odd-sized level does not lose its last row") {
    // Three texels across: two parents, and the second has to take in the
    // third texel as well as its own two. Halving by simple pairs would drop
    // it, and a parent that has not seen a texel reports a nearer maximum than
    // the truth - which culls things that are visible.
    const std::vector<float> level0{0.1f, 0.2f, 0.95f};

    DepthPyramid pyramid;
    pyramid.build(level0.data(), 3, 1);

    REQUIRE(pyramid.levelCount() >= 2);
    CHECK(pyramid.width(1) == 2);

    float coarsest = 0.f;
    const uint32_t top = pyramid.levelCount() - 1;
    for (uint32_t y = 0; y < pyramid.height(top); y++) {
        for (uint32_t x = 0; x < pyramid.width(top); x++) {
            coarsest = std::max(coarsest, pyramid.at(top, x, y));
        }
    }
    CHECK(coarsest == doctest::Approx(0.95f));
}

TEST_CASE("a rectangle's maximum is never reported nearer than it is") {
    // The reading may be pessimistic - a coarse texel covers more than was
    // asked about - but it may never be optimistic, because an optimistic
    // reading is what culls something that can be seen.
    std::vector<float> level0(64 * 48, 0.f);
    for (std::size_t i = 0; i < level0.size(); i++) {
        level0[i] = static_cast<float>((i * 37u) % 101u) / 100.f;
    }

    DepthPyramid pyramid;
    pyramid.build(level0.data(), 64, 48);

    const std::vector<glm::ivec4> rectangles{
        {0, 0, 0, 0},
        {3, 4, 3, 4},
        {0, 0, 63, 47},
        {10, 10, 11, 11},
        {20, 5, 45, 30},
        {60, 44, 63, 47},
    };

    for (const glm::ivec4& rect : rectangles) {
        float trueMax = 0.f;
        for (int y = rect.y; y <= rect.w; y++) {
            for (int x = rect.x; x <= rect.z; x++) {
                const std::size_t index =
                    static_cast<std::size_t>(y) * 64u + static_cast<std::size_t>(x);
                trueMax = std::max(trueMax, level0[index]);
            }
        }

        const float reported = pyramid.maxDepth(
            glm::vec2{static_cast<float>(rect.x), static_cast<float>(rect.y)},
            glm::vec2{static_cast<float>(rect.z), static_cast<float>(rect.w)});

        CHECK(reported >= trueMax - 1e-5f);
    }

    // A single texel is answered exactly - there is no coarser level to be
    // pessimistic with.
    CHECK(
        pyramid.maxDepth(glm::vec2{7.f, 9.f}, glm::vec2{7.f, 9.f}) ==
        doctest::Approx(level0[9 * 64 + 7]));
}

TEST_CASE("a box in front of the camera lands inside the frame") {
    const Camera camera = lookingForward();

    const ScreenBounds bounds =
        projectBounds(viewProjectionOf(camera), boxAt(glm::vec3{0.f, 0.f, 10.f}, glm::vec3{1.f}));

    REQUIRE(bounds.testable);
    CHECK(bounds.min.x >= 0.f);
    CHECK(bounds.min.y >= 0.f);
    CHECK(bounds.max.x <= 1.f);
    CHECK(bounds.max.y <= 1.f);
    CHECK(bounds.min.x < bounds.max.x);
    CHECK(bounds.min.y < bounds.max.y);

    // Centred on the camera's axis, so centred on the frame.
    CHECK((bounds.min.x + bounds.max.x) * 0.5f == doctest::Approx(0.5f).epsilon(1e-3f));
    CHECK((bounds.min.y + bounds.max.y) * 0.5f == doctest::Approx(0.5f).epsilon(1e-3f));

    // And its nearest corner is nearer than a box further away.
    const ScreenBounds further =
        projectBounds(viewProjectionOf(camera), boxAt(glm::vec3{0.f, 0.f, 30.f}, glm::vec3{1.f}));
    REQUIRE(further.testable);
    CHECK(bounds.nearestDepth < further.nearestDepth);
}

TEST_CASE("a box the camera is inside is never tested") {
    const Camera camera = lookingForward();

    // Straddling the eye: some corners are behind it, where a projection puts
    // them in the opposite half of the frame. Rather than test a rectangle
    // that means nothing, this reports untestable and the object is drawn.
    const ScreenBounds straddling =
        projectBounds(viewProjectionOf(camera), boxAt(glm::vec3{0.f}, glm::vec3{2.f}));
    CHECK_FALSE(straddling.testable);

    const ScreenBounds behind =
        projectBounds(viewProjectionOf(camera), boxAt(glm::vec3{0.f, 0.f, -10.f}, glm::vec3{1.f}));
    CHECK_FALSE(behind.testable);

    // An untestable box is never culled, whatever the pyramid says.
    const DepthPyramid wall = flatPyramid(32, 24, 0.001f);
    CHECK_FALSE(occludedByPyramid(wall, straddling));
    CHECK_FALSE(occludedByPyramid(wall, behind));
}

TEST_CASE("a box behind what has already been drawn is culled") {
    const Camera camera = lookingForward();
    const glm::mat4 viewProjection = viewProjectionOf(camera);

    // Something was drawn six units out, across the whole frame.
    const DepthPyramid wall = flatPyramid(32, 24, depthAt(camera, 6.f));

    const ScreenBounds hidden =
        projectBounds(viewProjection, boxAt(glm::vec3{0.f, 0.f, 20.f}, glm::vec3{1.f}));
    REQUIRE(hidden.testable);
    CHECK(occludedByPyramid(wall, hidden));

    // The same box in front of it is not.
    const ScreenBounds infront =
        projectBounds(viewProjection, boxAt(glm::vec3{0.f, 0.f, 3.f}, glm::vec3{1.f}));
    REQUIRE(infront.testable);
    CHECK_FALSE(occludedByPyramid(wall, infront));

    // Nor is a box that reaches through it: its nearest corner is in front,
    // even though most of it is behind.
    const ScreenBounds through =
        projectBounds(viewProjection, boxAt(glm::vec3{0.f, 0.f, 8.f}, glm::vec3{0.f, 0.f, 4.f}));
    REQUIRE(through.testable);
    CHECK_FALSE(occludedByPyramid(wall, through));
}

TEST_CASE("a box is not culled by an occluder that does not cover it") {
    const Camera camera = lookingForward();
    const glm::mat4 viewProjection = viewProjectionOf(camera);

    const float nearDepth = depthAt(camera, 6.f);

    // A wall over the left half of the frame and open sky over the right.
    std::vector<float> level0(32 * 24, 1.f);
    for (uint32_t y = 0; y < 24; y++) {
        for (uint32_t x = 0; x < 16; x++) {
            level0[static_cast<std::size_t>(y) * 32 + x] = nearDepth;
        }
    }
    DepthPyramid pyramid;
    pyramid.build(level0.data(), 32, 24);

    // Far away and off to the left, behind the wall.
    const ScreenBounds behindWall =
        projectBounds(viewProjection, boxAt(glm::vec3{-4.f, 0.f, 20.f}, glm::vec3{0.5f}));
    REQUIRE(behindWall.testable);
    CHECK(occludedByPyramid(pyramid, behindWall));

    // The same distance out but on the open side.
    const ScreenBounds inTheOpen =
        projectBounds(viewProjection, boxAt(glm::vec3{4.f, 0.f, 20.f}, glm::vec3{0.5f}));
    REQUIRE(inTheOpen.testable);
    CHECK_FALSE(occludedByPyramid(pyramid, inTheOpen));

    // And one straddling the edge of the wall is drawn: part of it is over
    // open sky, so it cannot be shown to be hidden.
    const ScreenBounds straddling =
        projectBounds(viewProjection, boxAt(glm::vec3{0.f, 0.f, 20.f}, glm::vec3{2.f}));
    REQUIRE(straddling.testable);
    CHECK_FALSE(occludedByPyramid(pyramid, straddling));
}

TEST_CASE("a snapshot that has not arrived hides nothing") {
    const Camera camera = lookingForward();

    OcclusionSnapshot snapshot{};
    snapshot.viewProjection = viewProjectionOf(camera);
    // valid stays false: no frame has been read back yet.
    CHECK_FALSE(snapshot.hides(boxAt(glm::vec3{0.f, 0.f, 20.f}, glm::vec3{1.f})));

    // Marked valid but with no pyramid in it, which is the state after a
    // resize discards one - still nothing to say.
    snapshot.valid = true;
    CHECK_FALSE(snapshot.hides(boxAt(glm::vec3{0.f, 0.f, 20.f}, glm::vec3{1.f})));
}

TEST_CASE("a snapshot grows a box before testing it") {
    const Camera camera = lookingForward();

    OcclusionSnapshot snapshot{};
    snapshot.viewProjection = viewProjectionOf(camera);
    snapshot.valid = true;

    const float nearDepth = depthAt(camera, 6.f);

    // A solid wall with one texel of open sky in it. What the margin can do
    // is turn a cull into a draw, never the reverse, so a box clear of the
    // hole is still hidden and one large enough to reach it is not.
    std::vector<float> level0(64 * 48, nearDepth);
    level0[24 * 64 + 32] = 1.f;

    snapshot.pyramid.build(level0.data(), 64, 48);

    const Aabb hidden = boxAt(glm::vec3{-6.f, 3.f, 20.f}, glm::vec3{0.4f});
    CHECK(snapshot.hides(hidden));

    // Whatever the margin does, it can only ever make the answer "draw it":
    // the grown box covers everything the original did.
    const Aabb enormous = boxAt(glm::vec3{0.f, 0.f, 20.f}, glm::vec3{20.f});
    CHECK_FALSE(snapshot.hides(enormous));
}
