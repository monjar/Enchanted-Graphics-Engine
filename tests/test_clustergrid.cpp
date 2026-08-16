// The clustered-shading grid.
//
// The whole point of clustering is that a fragment loops over the lights that
// reach it rather than every light in the scene. That is only true if the grid
// actually covers the frustum, if a light lands in every cell it touches, and
// if a fragment's own depth resolves to the cell its position falls inside.
// Each of those fails silently on a GPU - the picture just goes subtly wrong -
// so they are pinned here, where there is no GPU at all.

#include "render/ClusterGrid.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using ege::ClusterBounds;
using ege::clusterBounds;
using ege::ClusterGrid;
using ege::clusterIndex;
using ege::clusterSliceDepth;
using ege::clusterSliceForDepth;
using ege::clusterSliceScaleBias;
using ege::sphereTouchesCluster;

namespace {

    constexpr float nearPlane = 0.1f;
    constexpr float farPlane = 100.f;

    ClusterGrid defaultGrid() {
        ClusterGrid grid{};
        grid.nearPlane = nearPlane;
        grid.farPlane = farPlane;
        return grid;
    }

    glm::mat4 projection() {
        return glm::perspective(glm::radians(50.f), 16.f / 9.f, nearPlane, farPlane);
    }

    glm::mat4 inverseProjection() {
        return glm::inverse(projection());
    }

    // Where a view-space point lands in the grid's screen tiles, computed the
    // long way round through the projection - which is what the shader does
    // with gl_FragCoord.
    glm::vec2 viewToScreen(const glm::mat4& proj, glm::vec3 viewPoint) {
        const glm::vec4 clip = proj * glm::vec4{viewPoint, 1.f};
        const glm::vec3 ndc = glm::vec3{clip} / clip.w;
        return glm::vec2{ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f};
    }

    bool insideBounds(const ClusterBounds& bounds, glm::vec3 point) {
        constexpr float slack = 1e-4f;
        return point.x >= bounds.minPoint.x - slack && point.x <= bounds.maxPoint.x + slack &&
               point.y >= bounds.minPoint.y - slack && point.y <= bounds.maxPoint.y + slack &&
               point.z >= bounds.minPoint.z - slack && point.z <= bounds.maxPoint.z + slack;
    }

}  // namespace

TEST_CASE("the slices span exactly the clustering range") {
    const ClusterGrid grid = defaultGrid();

    CHECK(clusterSliceDepth(grid, 0) == doctest::Approx(nearPlane));
    CHECK(clusterSliceDepth(grid, grid.z) == doctest::Approx(farPlane));
}

TEST_CASE("slices grow with distance rather than dividing evenly") {
    const ClusterGrid grid = defaultGrid();

    float previousThickness = 0.f;
    for (uint32_t slice = 0; slice < grid.z; slice++) {
        const float thickness = clusterSliceDepth(grid, slice + 1) - clusterSliceDepth(grid, slice);
        CHECK(thickness > 0.f);
        CHECK(thickness > previousThickness);
        previousThickness = thickness;
    }

    // The point of the exponential spacing: the near slice is orders of
    // magnitude tighter than the far one, matching how perspective compresses
    // distance. An even split would make these equal.
    const float nearest = clusterSliceDepth(grid, 1) - clusterSliceDepth(grid, 0);
    const float furthest = clusterSliceDepth(grid, grid.z) - clusterSliceDepth(grid, grid.z - 1);
    CHECK(furthest > nearest * 100.f);
}

TEST_CASE("a depth resolves to the slice that contains it") {
    const ClusterGrid grid = defaultGrid();

    for (uint32_t slice = 0; slice < grid.z; slice++) {
        // Sampled inside the slice rather than at its edge, where floating
        // point could legitimately land either side.
        const float begin = clusterSliceDepth(grid, slice);
        const float end = clusterSliceDepth(grid, slice + 1);
        const float middle = begin + (end - begin) * 0.5f;

        CHECK(clusterSliceForDepth(grid, middle) == slice);
    }
}

TEST_CASE("depths outside the range clamp instead of indexing past the grid") {
    const ClusterGrid grid = defaultGrid();

    CHECK(clusterSliceForDepth(grid, 0.f) == 0);
    CHECK(clusterSliceForDepth(grid, -5.f) == 0);
    CHECK(clusterSliceForDepth(grid, nearPlane * 0.5f) == 0);
    CHECK(clusterSliceForDepth(grid, farPlane) == grid.z - 1);
    CHECK(clusterSliceForDepth(grid, farPlane * 10.f) == grid.z - 1);
}

TEST_CASE("the shader's cheap slice formula agrees with the exact one") {
    // The shader cannot afford a division and two logarithms per fragment, so
    // it uses a precomputed scale and bias. If the two ever disagree, lights
    // are looked up from a cell the fragment is not in - which reads as
    // lighting that pops as the camera moves rather than as an obvious break.
    const ClusterGrid grid = defaultGrid();
    const glm::vec2 scaleBias = clusterSliceScaleBias(grid);

    for (int step = 0; step <= 200; step++) {
        const float t = static_cast<float>(step) / 200.f;
        const float depth = nearPlane * std::pow(farPlane / nearPlane, t);

        const auto cheap = static_cast<uint32_t>(std::min(
            std::max(std::log2(depth) * scaleBias.x + scaleBias.y, 0.f),
            static_cast<float>(grid.z - 1)));
        CHECK(cheap == clusterSliceForDepth(grid, depth));
    }
}

TEST_CASE("a cluster's box contains the depth range it was cut for") {
    const ClusterGrid grid = defaultGrid();
    const glm::mat4 inverse = inverseProjection();

    for (uint32_t iz = 0; iz < grid.z; iz++) {
        const ClusterBounds bounds = clusterBounds(grid, inverse, 8, 4, iz);

        // View space looks down -Z, so the near face is the larger z.
        CHECK(bounds.maxPoint.z == doctest::Approx(-clusterSliceDepth(grid, iz)).epsilon(1e-3));
        CHECK(bounds.minPoint.z == doctest::Approx(-clusterSliceDepth(grid, iz + 1)).epsilon(1e-3));
        CHECK(bounds.minPoint.x < bounds.maxPoint.x);
        CHECK(bounds.minPoint.y < bounds.maxPoint.y);
    }
}

TEST_CASE("clusters get wider with depth, the way a frustum does") {
    const ClusterGrid grid = defaultGrid();
    const glm::mat4 inverse = inverseProjection();

    const ClusterBounds nearCell = clusterBounds(grid, inverse, 8, 4, 0);
    const ClusterBounds farCell = clusterBounds(grid, inverse, 8, 4, grid.z - 1);

    const float nearWidth = nearCell.maxPoint.x - nearCell.minPoint.x;
    const float farWidth = farCell.maxPoint.x - farCell.minPoint.x;
    CHECK(farWidth > nearWidth);
}

TEST_CASE("the grid tiles the frustum with no gap between neighbours") {
    // A gap is a band of pixels whose lights were assigned to a cell they are
    // not in, and it would show as a dark seam that moves with the camera.
    const ClusterGrid grid = defaultGrid();
    const glm::mat4 inverse = inverseProjection();

    for (uint32_t ix = 0; ix + 1 < grid.x; ix++) {
        const ClusterBounds left = clusterBounds(grid, inverse, ix, 4, 6);
        const ClusterBounds right = clusterBounds(grid, inverse, ix + 1, 4, 6);
        // The axis-aligned boxes of adjacent froxels overlap slightly rather
        // than meeting exactly, because a box bounding a slanted cell is
        // larger than the cell. Overlap is safe; a gap is not.
        CHECK(right.minPoint.x <= left.maxPoint.x + 1e-4f);
    }

    for (uint32_t iz = 0; iz + 1 < grid.z; iz++) {
        const ClusterBounds nearer = clusterBounds(grid, inverse, 8, 4, iz);
        const ClusterBounds further = clusterBounds(grid, inverse, 8, 4, iz + 1);
        CHECK(further.maxPoint.z == doctest::Approx(nearer.minPoint.z).epsilon(1e-3));
    }
}

TEST_CASE("a point lands in the cluster its own screen tile and depth name") {
    // The round trip the whole scheme rests on: take a view-space point, work
    // out which cell the shader would look it up in, and check the point is
    // actually inside that cell's bounds.
    const ClusterGrid grid = defaultGrid();
    const glm::mat4 proj = projection();
    const glm::mat4 inverse = glm::inverse(proj);

    const std::vector<glm::vec3> points = {
        {0.f, 0.f, -0.5f},
        {0.3f, 0.2f, -2.f},
        {-1.5f, 0.8f, -9.f},
        {4.f, -2.f, -30.f},
        {-0.05f, 0.04f, -0.2f},
    };

    for (const glm::vec3& point : points) {
        const float depth = -point.z;
        const glm::vec2 screen = viewToScreen(proj, point);
        REQUIRE(screen.x >= 0.f);
        REQUIRE(screen.x <= 1.f);
        REQUIRE(screen.y >= 0.f);
        REQUIRE(screen.y <= 1.f);

        const auto ix =
            std::min(static_cast<uint32_t>(screen.x * static_cast<float>(grid.x)), grid.x - 1);
        const auto iy =
            std::min(static_cast<uint32_t>(screen.y * static_cast<float>(grid.y)), grid.y - 1);
        const uint32_t iz = clusterSliceForDepth(grid, depth);

        CHECK(insideBounds(clusterBounds(grid, inverse, ix, iy, iz), point));
    }
}

TEST_CASE("a light is found by the cluster it overlaps and not by distant ones") {
    const ClusterGrid grid = defaultGrid();
    const glm::mat4 inverse = inverseProjection();

    const ClusterBounds cell = clusterBounds(grid, inverse, 8, 4, 10);
    const glm::vec3 centre = (cell.minPoint + cell.maxPoint) * 0.5f;

    CHECK(sphereTouchesCluster(cell, centre, 0.01f));

    // Just outside the box, but with reach: still touching.
    const glm::vec3 beyond{cell.maxPoint.x + 1.f, centre.y, centre.z};
    CHECK_FALSE(sphereTouchesCluster(cell, beyond, 0.5f));
    CHECK(sphereTouchesCluster(cell, beyond, 1.5f));

    // Far behind the camera, where nothing in the frustum should find it.
    CHECK_FALSE(sphereTouchesCluster(cell, glm::vec3{0.f, 0.f, 500.f}, 10.f));
}

TEST_CASE("every cluster index is distinct and inside the buffer") {
    const ClusterGrid grid = defaultGrid();
    std::vector<bool> seen(ege::clusterCount, false);

    for (uint32_t iz = 0; iz < grid.z; iz++) {
        for (uint32_t iy = 0; iy < grid.y; iy++) {
            for (uint32_t ix = 0; ix < grid.x; ix++) {
                const uint32_t index = clusterIndex(grid, ix, iy, iz);
                REQUIRE(index < ege::clusterCount);
                REQUIRE_FALSE(seen[index]);
                seen[index] = true;
            }
        }
    }

    for (bool visited : seen) {
        CHECK(visited);
    }
}
