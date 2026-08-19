// Screen-space ambient occlusion.
//
// The part worth testing without a GPU is the part the shader keeps getting
// wrong in published code: turning a depth-buffer sample back into a point in
// view space. Every SSAO tutorial assumes OpenGL's conventions - a depth range
// of [-1, 1] and a camera looking down -Z - and this engine has neither. So
// rather than checking the reconstruction against a formula copied from the
// same place the code was, these tests project points through the engine's own
// Camera and check that the reconstruction lands back where they started.

#include "render/Camera.hpp"
#include "render/Ssao.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using ege::Camera;
using ege::ndcFromScreen;
using ege::ssaoKernel;
using ege::ssaoNoise;
using ege::viewPositionFromDepth;

namespace {

    constexpr float tolerance = 1e-3f;

    Camera demoCamera() {
        Camera camera;
        camera.setPerspectiveProjection(glm::radians(50.f), 4.f / 3.f, 0.1f, 100.f);
        return camera;
    }

    // Where a view-space point ends up on screen, done the long way: through
    // the projection the engine itself builds, then the perspective divide.
    // This is the ground truth the reconstruction has to invert.
    glm::vec3 projectToNdc(const Camera& camera, glm::vec3 viewPosition) {
        const glm::vec4 clip = camera.getProjection() * glm::vec4{viewPosition, 1.f};
        REQUIRE(std::abs(clip.w) > 1e-6f);
        return glm::vec3{clip} / clip.w;
    }

}  // namespace

TEST_CASE("a depth sample reconstructs to the point it was projected from") {
    const Camera camera = demoCamera();
    const glm::mat4 inverseProjection = glm::inverse(camera.getProjection());

    // Spread through the frustum: near and far, left and right, up and down.
    // This engine's view space looks down +Z, so every one of these has a
    // positive z - a set of test points in front of a -Z camera would all be
    // behind this one, and the round trip would pass on points nothing draws.
    const std::vector<glm::vec3> points{
        {0.f, 0.f, 1.f},
        {0.f, 0.f, 40.f},
        {0.5f, -0.25f, 2.f},
        {-1.5f, 0.75f, 6.f},
        {3.f, 2.f, 20.f},
        {-4.f, -3.f, 55.f},
    };

    for (const glm::vec3& expected : points) {
        const glm::vec3 ndc = projectToNdc(camera, expected);
        const glm::vec3 recovered = viewPositionFromDepth(inverseProjection, ndc);

        CHECK(recovered.x == doctest::Approx(expected.x).epsilon(tolerance));
        CHECK(recovered.y == doctest::Approx(expected.y).epsilon(tolerance));
        CHECK(recovered.z == doctest::Approx(expected.z).epsilon(tolerance));
    }
}

TEST_CASE("the stored depth grows with distance from the camera") {
    // The whole occlusion test is one comparison: is the geometry the depth
    // buffer records nearer than the point being sampled? That comparison is
    // made in view space, so what has to hold is that a larger view z means a
    // larger stored depth, and that reconstruction preserves the order.
    const Camera camera = demoCamera();
    const glm::mat4 inverseProjection = glm::inverse(camera.getProjection());

    float previousDepth = -1.f;
    float previousViewZ = -1.f;

    for (float z = 0.5f; z < 60.f; z *= 1.5f) {
        const glm::vec3 ndc = projectToNdc(camera, glm::vec3{0.2f, -0.1f, z});

        // Vulkan's depth range, which is what makes the reconstruction use
        // ndc.z unscaled.
        CHECK(ndc.z >= 0.f);
        CHECK(ndc.z <= 1.f);
        CHECK(ndc.z > previousDepth);

        const float viewZ = viewPositionFromDepth(inverseProjection, ndc).z;
        CHECK(viewZ > previousViewZ);
        CHECK(viewZ == doctest::Approx(z).epsilon(tolerance));

        previousDepth = ndc.z;
        previousViewZ = viewZ;
    }
}

TEST_CASE("screen coordinates and clip space agree about which way is up") {
    // The engine's scenes treat -Y as up, and Vulkan's NDC y points down the
    // screen. Both of those have to line up for the occlusion pass to sample
    // the depth buffer at the pixel it is shading rather than at its mirror
    // image - and a vertical flip is the kind of mistake that still produces
    // a plausible-looking picture.
    const Camera camera = demoCamera();

    // A point above the camera's axis, in this engine's sense of above.
    const glm::vec3 ndcAbove = projectToNdc(camera, glm::vec3{0.f, -2.f, 10.f});
    CHECK(ndcAbove.y < 0.f);

    // Which has to be the top half of the image: y zero is the top row of the
    // render target, and the fullscreen triangle's own uv starts there too.
    const glm::vec2 uvAbove{0.5f, (ndcAbove.y + 1.f) * 0.5f};
    CHECK(uvAbove.y < 0.5f);

    // And the mapping back is the plain rescale that implies, with no flip.
    const glm::vec3 roundTrip = ndcFromScreen(uvAbove, ndcAbove.z);
    CHECK(roundTrip.y == doctest::Approx(ndcAbove.y));
    CHECK(roundTrip.z == doctest::Approx(ndcAbove.z));

    CHECK(ndcFromScreen(glm::vec2{0.f, 0.f}, 0.f).x == doctest::Approx(-1.f));
    CHECK(ndcFromScreen(glm::vec2{0.f, 0.f}, 0.f).y == doctest::Approx(-1.f));
    CHECK(ndcFromScreen(glm::vec2{1.f, 1.f}, 1.f).x == doctest::Approx(1.f));
    CHECK(ndcFromScreen(glm::vec2{1.f, 1.f}, 1.f).y == doctest::Approx(1.f));
}

TEST_CASE("every kernel sample sits in the hemisphere over the surface") {
    const std::vector<glm::vec4> kernel = ssaoKernel(ege::ssaoSampleCount);
    REQUIRE(kernel.size() == ege::ssaoSampleCount);

    for (const glm::vec4& sample : kernel) {
        const glm::vec3 offset{sample};

        // +Z is the surface normal in the space this kernel is expressed in.
        // A sample with negative z is one below the surface, which would
        // report the geometry the fragment is standing on as an occluder and
        // darken every flat floor in the scene.
        CHECK(offset.z >= 0.f);

        // Inside the unit hemisphere: the shader scales these by the world
        // radius, so a sample longer than one would reach further than the
        // radius says it does.
        const float length = glm::length(offset);
        CHECK(length > 0.f);
        CHECK(length <= 1.f);

        // Padding, not a fourth dimension.
        CHECK(sample.w == doctest::Approx(0.f));
    }
}

TEST_CASE("kernel samples crowd towards the surface") {
    // What makes the estimate local: occlusion is decided by what is within a
    // few centimetres, and an evenly spread hemisphere spends most of its
    // samples out where nothing is blocked.
    const std::vector<glm::vec4> kernel = ssaoKernel(64);

    auto meanLength = [&](std::size_t begin, std::size_t end) {
        float total = 0.f;
        for (std::size_t i = begin; i < end; i++) {
            total += glm::length(glm::vec3{kernel[i]});
        }
        return total / static_cast<float>(end - begin);
    };

    CHECK(meanLength(0, 16) < meanLength(48, 64));
}

TEST_CASE("the kernel is the same every run for a given seed") {
    // Generated once at startup and uploaded, so nothing is gained by it
    // differing between runs - and a kernel that differs is one no test can
    // say anything about.
    const std::vector<glm::vec4> first = ssaoKernel(16, 7u);
    const std::vector<glm::vec4> again = ssaoKernel(16, 7u);
    REQUIRE(first.size() == again.size());
    for (std::size_t i = 0; i < first.size(); i++) {
        CHECK(first[i].x == doctest::Approx(again[i].x));
        CHECK(first[i].y == doctest::Approx(again[i].y));
        CHECK(first[i].z == doctest::Approx(again[i].z));
    }

    const std::vector<glm::vec4> different = ssaoKernel(16, 8u);
    bool anyDifference = false;
    for (std::size_t i = 0; i < first.size(); i++) {
        anyDifference = anyDifference || glm::length(glm::vec3{first[i] - different[i]}) > 1e-6f;
    }
    CHECK(anyDifference);
}

TEST_CASE("rotation vectors lie in the surface's tangent plane") {
    const std::vector<glm::vec4> noise = ssaoNoise(ege::ssaoNoiseSize * ege::ssaoNoiseSize);
    REQUIRE(noise.size() == ege::ssaoNoiseSize * ege::ssaoNoiseSize);

    for (const glm::vec4& rotation : noise) {
        // Zero along the normal, so what the shader does with this is turn the
        // kernel about the normal - not tilt it off the surface, which would
        // push half the samples inside the geometry.
        CHECK(rotation.z == doctest::Approx(0.f));
        CHECK(rotation.w == doctest::Approx(0.f));

        // Unit length, so the encoding into an eight-bit texture uses its
        // whole range and the shader's tangent frame cannot come out
        // degenerate.
        CHECK(glm::length(glm::vec2{rotation}) == doctest::Approx(1.f).epsilon(tolerance));
    }

    // And they point in different directions, which is the entire point of
    // there being sixteen of them.
    bool anyDifference = false;
    for (std::size_t i = 1; i < noise.size(); i++) {
        anyDifference = anyDifference || glm::length(glm::vec2{noise[i] - noise[0]}) > 1e-3f;
    }
    CHECK(anyDifference);
}
