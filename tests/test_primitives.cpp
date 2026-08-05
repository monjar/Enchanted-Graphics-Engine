// Geometry checks for the procedural primitive builders.
//
// The winding assertion here is the one that matters: it caught the plane and
// the sphere both being generated backwards, which backface culling would then
// have made invisible rather than merely wrong.

#include "render/ege_model.hpp"

#include <doctest/doctest.h>

#include <cmath>

using ege::EgeModel;

namespace {

struct MeshStats {
    int degenerate = 0;
    int backwards = 0;
};

// Compares each triangle's geometric normal, from the winding, against the
// interpolated shading normal. They agree only if the triangle is wound
// counter-clockwise seen from outside.
MeshStats analyse(const EgeModel::Builder& builder) {
    MeshStats stats{};

    for (size_t i = 0; i + 2 < builder.indices.size(); i += 3) {
        const glm::vec3& p0 = builder.vertices[builder.indices[i + 0]].position;
        const glm::vec3& p1 = builder.vertices[builder.indices[i + 1]].position;
        const glm::vec3& p2 = builder.vertices[builder.indices[i + 2]].position;

        glm::vec3 geometric = glm::cross(p1 - p0, p2 - p0);
        const float area = glm::length(geometric);
        if (area < 1e-9f) {
            stats.degenerate++;
            continue;
        }
        geometric /= area;

        const glm::vec3 shading = glm::normalize(
            builder.vertices[builder.indices[i + 0]].normal +
            builder.vertices[builder.indices[i + 1]].normal +
            builder.vertices[builder.indices[i + 2]].normal);

        if (glm::dot(geometric, shading) <= 0.f) {
            stats.backwards++;
        }
    }

    return stats;
}

void requireWellFormed(const EgeModel::Builder& builder) {
    REQUIRE(builder.indices.size() % 3 == 0);
    REQUIRE_FALSE(builder.vertices.empty());

    for (uint32_t index : builder.indices) {
        REQUIRE(index < builder.vertices.size());
    }

    for (const auto& vertex : builder.vertices) {
        CHECK(glm::length(vertex.normal) == doctest::Approx(1.f).epsilon(1e-4));
    }

    const MeshStats stats = analyse(builder);
    CHECK(stats.degenerate == 0);
    CHECK(stats.backwards == 0);
}

}  // namespace

TEST_CASE("box is a well-formed unit cube") {
    const auto box = EgeModel::Builder::box();

    // Six faces, four vertices each: faces cannot share vertices because each
    // one carries its own normal.
    CHECK(box.vertices.size() == 24);
    CHECK(box.indices.size() == 36);

    requireWellFormed(box);

    for (const auto& vertex : box.vertices) {
        CHECK(std::fabs(vertex.position.x) <= doctest::Approx(0.5f));
        CHECK(std::fabs(vertex.position.y) <= doctest::Approx(0.5f));
        CHECK(std::fabs(vertex.position.z) <= doctest::Approx(0.5f));
    }
}

TEST_CASE("plane is a single quad facing +Y") {
    const auto plane = EgeModel::Builder::plane();

    CHECK(plane.vertices.size() == 4);
    CHECK(plane.indices.size() == 6);

    requireWellFormed(plane);

    for (const auto& vertex : plane.vertices) {
        CHECK(vertex.position.y == doctest::Approx(0.f));
        CHECK(vertex.normal.y == doctest::Approx(1.f));
    }
}

TEST_CASE("sphere is well-formed at a range of tessellations") {
    // Includes the minimum the builder accepts, where every triangle touches a
    // pole and the degenerate-skipping logic is doing all the work.
    for (uint32_t latitude : {2u, 3u, 8u, 16u}) {
        for (uint32_t longitude : {3u, 4u, 16u, 32u}) {
            CAPTURE(latitude);
            CAPTURE(longitude);

            const auto sphere = EgeModel::Builder::sphere(latitude, longitude);
            requireWellFormed(sphere);

            // Radius 0.5 so the sphere occupies the same unit extent as the box.
            for (const auto& vertex : sphere.vertices) {
                CHECK(glm::length(vertex.position) == doctest::Approx(0.5f).epsilon(1e-4));
            }
        }
    }
}

TEST_CASE("sphere texture coordinates span the full range") {
    const auto sphere = EgeModel::Builder::sphere(8, 16);

    float minU = 1.f, maxU = 0.f, minV = 1.f, maxV = 0.f;
    for (const auto& vertex : sphere.vertices) {
        minU = std::fmin(minU, vertex.uv.x);
        maxU = std::fmax(maxU, vertex.uv.x);
        minV = std::fmin(minV, vertex.uv.y);
        maxV = std::fmax(maxV, vertex.uv.y);
    }

    CHECK(minU == doctest::Approx(0.f));
    CHECK(maxU == doctest::Approx(1.f));
    CHECK(minV == doctest::Approx(0.f));
    CHECK(maxV == doctest::Approx(1.f));
}
