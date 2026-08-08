// Transform maths.
//
// The normal-matrix case pins down an identity the renderer relies on: for a
// strict T * R * S composition, scaling the rotation columns by 1/scale is not
// an approximation of transpose(inverse(M)), it is exactly equal to it. If the
// transform ever gains shear or general parent matrices that stops being true,
// and this test is what should fail.

#include "scene/GameObject.hpp"

#include <glm/gtc/constants.hpp>

#include <doctest/doctest.h>

#include <random>

using ege::TransformComponent;

namespace {

    void checkMatricesClose(const glm::mat3& a, const glm::mat3& b, float tolerance) {
        for (int column = 0; column < 3; column++) {
            for (int row = 0; row < 3; row++) {
                REQUIRE(a[column][row] == doctest::Approx(b[column][row]).epsilon(tolerance));
            }
        }
    }

}  // namespace

TEST_CASE("identity transform produces the identity matrix") {
    TransformComponent transform{};

    const glm::mat4 matrix = transform.mat4();
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            const float expected = (column == row) ? 1.f : 0.f;
            CHECK(matrix[column][row] == doctest::Approx(expected));
        }
    }
}

TEST_CASE("translation lands in the fourth column") {
    TransformComponent transform{};
    transform.translation = {1.f, -2.f, 3.5f};

    const glm::mat4 matrix = transform.mat4();
    CHECK(matrix[3][0] == doctest::Approx(1.f));
    CHECK(matrix[3][1] == doctest::Approx(-2.f));
    CHECK(matrix[3][2] == doctest::Approx(3.5f));
    CHECK(matrix[3][3] == doctest::Approx(1.f));
}

TEST_CASE("transform maps points as translate(rotate(scale(p)))") {
    TransformComponent transform{};
    transform.translation = {2.f, 0.f, 0.f};
    transform.scale = {2.f, 2.f, 2.f};
    transform.rotation = {0.f, glm::half_pi<float>(), 0.f};

    // A quarter turn about Y maps +X to -Z, then the translation shifts it.
    const glm::vec4 mapped = transform.mat4() * glm::vec4{1.f, 0.f, 0.f, 1.f};
    CHECK(mapped.x == doctest::Approx(2.f).epsilon(1e-5));
    CHECK(mapped.y == doctest::Approx(0.f).epsilon(1e-5));
    CHECK(mapped.z == doctest::Approx(-2.f).epsilon(1e-5));
}

TEST_CASE("normal matrix equals transpose(inverse(M)) for T*R*S transforms") {
    std::mt19937 rng{0xC0FFEE};
    std::uniform_real_distribution<float> angle{-glm::pi<float>(), glm::pi<float>()};
    std::uniform_real_distribution<float> scale{0.05f, 8.f};

    for (int trial = 0; trial < 2000; trial++) {
        TransformComponent transform{};
        transform.translation = {angle(rng), angle(rng), angle(rng)};
        transform.rotation = {angle(rng), angle(rng), angle(rng)};
        transform.scale = {scale(rng), scale(rng), scale(rng)};

        const glm::mat3 shortcut = transform.normalMatrix();
        const glm::mat3 reference = glm::transpose(glm::inverse(glm::mat3(transform.mat4())));

        CAPTURE(trial);
        checkMatricesClose(shortcut, reference, 1e-3f);
    }
}

TEST_CASE("normal matrix keeps normals perpendicular under non-uniform scale") {
    // The case a plain rotation matrix gets wrong: squash one axis hard and a
    // surface normal must still come out perpendicular to the surface.
    TransformComponent transform{};
    transform.scale = {1.f, 0.1f, 1.f};

    const glm::mat4 model = transform.mat4();
    const glm::mat3 normalMatrix = transform.normalMatrix();

    // Two tangents of a plane tilted in the XY sense, and its normal.
    const glm::vec3 tangentA{1.f, 1.f, 0.f};
    const glm::vec3 tangentB{0.f, 0.f, 1.f};
    const glm::vec3 normal = glm::normalize(glm::cross(tangentA, tangentB));

    const glm::vec3 transformedA = glm::mat3(model) * tangentA;
    const glm::vec3 transformedB = glm::mat3(model) * tangentB;
    const glm::vec3 transformedNormal = glm::normalize(normalMatrix * normal);

    CHECK(
        glm::dot(transformedNormal, glm::normalize(transformedA)) ==
        doctest::Approx(0.f).epsilon(1e-4).scale(1.f));
    CHECK(
        glm::dot(transformedNormal, glm::normalize(transformedB)) ==
        doctest::Approx(0.f).epsilon(1e-4).scale(1.f));
}
