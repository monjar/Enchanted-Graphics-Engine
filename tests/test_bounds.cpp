// Bounding volumes and frustum culling.
//
// Culling errors come in two directions and need different tests. Discarding
// too much makes geometry vanish, and is caught by asserting that visible
// things survive. Keeping too much is invisible at runtime and survives every
// such assertion, so it has to be caught by pinning where each plane actually
// sits - which is what the near-plane case below does, and why it exists
// separately.

#include "render/Bounds.hpp"
#include "render/Camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <doctest/doctest.h>

using ege::Aabb;
using ege::Camera;
using ege::Frustum;
using ege::Sphere;

namespace {

    Aabb boxAt(glm::vec3 center, float halfSize) {
        Aabb box{};
        box.expand(center - glm::vec3{halfSize});
        box.expand(center + glm::vec3{halfSize});
        return box;
    }

    // A camera at the origin looking down +Z, matching the engine's convention.
    glm::mat4 defaultViewProjection() {
        Camera camera{};
        camera.setPerspectiveProjection(glm::radians(50.f), 1.6f, 0.1f, 100.f);
        camera.setViewDirection(
            glm::vec3{0.f}, glm::vec3{0.f, 0.f, 1.f}, glm::vec3{0.f, -1.f, 0.f});
        return camera.getProjection() * camera.getView();
    }

}  // namespace

TEST_CASE("a default AABB is invalid until something is added") {
    Aabb box{};
    CHECK_FALSE(box.valid());

    box.expand({1.f, 2.f, 3.f});
    CHECK(box.valid());
    CHECK(box.center().x == doctest::Approx(1.f));
    CHECK(box.extents().x == doctest::Approx(0.f));
}

TEST_CASE("an AABB grows to contain every point") {
    Aabb box{};
    box.expand({-1.f, -2.f, -3.f});
    box.expand({4.f, 5.f, 6.f});

    CHECK(box.min == glm::vec3{-1.f, -2.f, -3.f});
    CHECK(box.max == glm::vec3{4.f, 5.f, 6.f});
    CHECK(box.center() == glm::vec3{1.5f, 1.5f, 1.5f});
}

TEST_CASE("translating an AABB moves it without resizing") {
    const Aabb box = boxAt({0.f, 0.f, 0.f}, 1.f);
    const Aabb moved = box.transformed(glm::translate(glm::mat4{1.f}, glm::vec3{10.f, 0.f, 0.f}));

    CHECK(moved.center().x == doctest::Approx(10.f));
    CHECK(moved.extents().x == doctest::Approx(1.f));
    CHECK(moved.extents().y == doctest::Approx(1.f));
}

TEST_CASE("scaling an AABB scales its extents") {
    const Aabb box = boxAt({0.f, 0.f, 0.f}, 1.f);
    const Aabb scaled = box.transformed(glm::scale(glm::mat4{1.f}, glm::vec3{2.f, 3.f, 4.f}));

    CHECK(scaled.extents().x == doctest::Approx(2.f));
    CHECK(scaled.extents().y == doctest::Approx(3.f));
    CHECK(scaled.extents().z == doctest::Approx(4.f));
}

TEST_CASE("rotating an AABB produces a box containing the rotated original") {
    // A unit cube turned 45 degrees about Z needs a wider axis-aligned box to
    // contain it - half-extent sqrt(2) rather than 1. A transform that returned
    // the original extents would be silently clipping geometry at the corners.
    const Aabb box = boxAt({0.f, 0.f, 0.f}, 1.f);
    const Aabb rotated =
        box.transformed(glm::rotate(glm::mat4{1.f}, glm::radians(45.f), glm::vec3{0.f, 0.f, 1.f}));

    CHECK(rotated.extents().x == doctest::Approx(std::sqrt(2.f)).epsilon(1e-4));
    CHECK(rotated.extents().y == doctest::Approx(std::sqrt(2.f)).epsilon(1e-4));
    // The rotation axis is untouched.
    CHECK(rotated.extents().z == doctest::Approx(1.f));
}

TEST_CASE("a sphere encloses the box it was built from") {
    const Aabb box = boxAt({1.f, 2.f, 3.f}, 2.f);
    const Sphere sphere = Sphere::fromAabb(box);

    CHECK(sphere.center == box.center());
    // Must reach the corner, not just the face.
    CHECK(sphere.radius >= 2.f);
    CHECK(sphere.radius == doctest::Approx(glm::length(glm::vec3{2.f})));
}

TEST_CASE("geometry in front of the camera survives culling") {
    const Frustum frustum = Frustum::fromViewProjection(defaultViewProjection());

    CHECK(frustum.intersects(boxAt({0.f, 0.f, 5.f}, 1.f)));
    CHECK(frustum.intersects(Sphere{{0.f, 0.f, 5.f}, 1.f}));
}

TEST_CASE("geometry close to the camera is not wrongly discarded") {
    const Frustum frustum = Frustum::fromViewProjection(defaultViewProjection());

    CHECK(frustum.intersects(boxAt({0.f, 0.f, 0.5f}, 0.2f)));
    CHECK(frustum.intersects(boxAt({0.f, 0.f, 0.2f}, 0.05f)));
    // Straddling the camera, so partly behind it, must still be kept.
    CHECK(frustum.intersects(boxAt({0.f, 0.f, 0.f}, 1.f)));
}

TEST_CASE("the near plane sits where the projection puts it") {
    // Vulkan clip space is z in [0, 1], not OpenGL's [-1, 1], so the near plane
    // is row2 alone rather than row3 + row2.
    //
    // Worth being precise about which way that error goes, because the
    // intuitive guess is wrong: the OpenGL extraction puts the near plane at
    // half the intended distance - 0.05 rather than 0.1 here - so it keeps too
    // much rather than too little. That never makes geometry vanish, which is
    // why it survives every "must not be culled" test. It has to be caught by
    // asserting the plane is where it should be.
    //
    // This box lies entirely between the two candidate planes: culled by the
    // correct one, kept by the OpenGL one.
    const Frustum frustum = Frustum::fromViewProjection(defaultViewProjection());

    CHECK_FALSE(frustum.intersects(boxAt({0.f, 0.f, 0.075f}, 0.02f)));

    // And just beyond the true near plane it survives, so the assertion above
    // is testing the plane's position rather than a blanket rejection.
    CHECK(frustum.intersects(boxAt({0.f, 0.f, 0.15f}, 0.02f)));
}

TEST_CASE("geometry behind the camera is culled") {
    const Frustum frustum = Frustum::fromViewProjection(defaultViewProjection());

    CHECK_FALSE(frustum.intersects(boxAt({0.f, 0.f, -10.f}, 1.f)));
    CHECK_FALSE(frustum.intersects(Sphere{{0.f, 0.f, -10.f}, 1.f}));
}

TEST_CASE("geometry beyond the far plane is culled") {
    const Frustum frustum = Frustum::fromViewProjection(defaultViewProjection());
    CHECK_FALSE(frustum.intersects(boxAt({0.f, 0.f, 500.f}, 1.f)));
}

TEST_CASE("geometry far to the side is culled") {
    const Frustum frustum = Frustum::fromViewProjection(defaultViewProjection());

    CHECK_FALSE(frustum.intersects(boxAt({1000.f, 0.f, 10.f}, 1.f)));
    CHECK_FALSE(frustum.intersects(boxAt({-1000.f, 0.f, 10.f}, 1.f)));
    CHECK_FALSE(frustum.intersects(boxAt({0.f, 1000.f, 10.f}, 1.f)));
}

TEST_CASE("a box large enough to enclose the camera is kept") {
    // A skybox or a room the camera stands inside intersects every plane from
    // the inside; culling it would empty the screen.
    const Frustum frustum = Frustum::fromViewProjection(defaultViewProjection());
    CHECK(frustum.intersects(boxAt({0.f, 0.f, 0.f}, 50.f)));
}

TEST_CASE("an invalid box is culled rather than crashing") {
    const Frustum frustum = Frustum::fromViewProjection(defaultViewProjection());
    CHECK_FALSE(frustum.intersects(Aabb{}));
}

TEST_CASE("frustum planes are normalised") {
    // The sphere test compares a plane distance against a radius, which is only
    // a real distance if the normal is unit length.
    const Frustum frustum = Frustum::fromViewProjection(defaultViewProjection());

    for (int i = 0; i < Frustum::PlaneCount; i++) {
        CAPTURE(i);
        const glm::vec4 plane = frustum.plane(static_cast<Frustum::Plane>(i));
        CHECK(glm::length(glm::vec3{plane}) == doctest::Approx(1.f).epsilon(1e-4));
    }
}

TEST_CASE("culling is conservative across a sweep of positions") {
    // Sweeps a small box across the view volume and checks the answer matches
    // an independent projection-based test: a point is inside if its clip
    // coordinates fall within the Vulkan clip volume. Culling may keep
    // something the point test rejects - it works on boxes, not points - but it
    // must never discard something the point test accepts.
    const glm::mat4 viewProjection = defaultViewProjection();
    const Frustum frustum = Frustum::fromViewProjection(viewProjection);

    int checked = 0;
    for (float x = -20.f; x <= 20.f; x += 2.5f) {
        for (float y = -20.f; y <= 20.f; y += 2.5f) {
            for (float z = -5.f; z <= 60.f; z += 2.5f) {
                const glm::vec3 point{x, y, z};
                const glm::vec4 clip = viewProjection * glm::vec4{point, 1.f};

                const bool insideByProjection = clip.w > 0.f && std::abs(clip.x) <= clip.w &&
                                                std::abs(clip.y) <= clip.w && clip.z >= 0.f &&
                                                clip.z <= clip.w;

                if (insideByProjection) {
                    CAPTURE(x);
                    CAPTURE(y);
                    CAPTURE(z);
                    // A zero-size box at a visible point must never be culled.
                    REQUIRE(frustum.intersects(boxAt(point, 0.f)));
                    checked++;
                }
            }
        }
    }

    // The sweep has to actually cover visible ground for the assertion above to
    // mean anything.
    CHECK(checked > 100);
}
