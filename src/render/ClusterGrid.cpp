#include "render/ClusterGrid.hpp"

#include <algorithm>
#include <cmath>

namespace ege {

    namespace {

        // A point on the ray from the eye through `direction`, moved to the
        // given distance along the view's forward axis.
        //
        // Scaling by |z| rather than by z keeps this correct whichever way the
        // projection points. This engine's own camera looks down +Z, while the
        // GLM constructors everything else is written against look down -Z,
        // and a function that assumed either one would be silently wrong for
        // half its callers - the ray would come out behind the eye and every
        // cluster would bound the wrong region.
        glm::vec3 pointAtDepth(glm::vec3 direction, float depth) {
            if (std::abs(direction.z) < 1e-9f) {
                return direction;
            }
            return direction * (depth / std::abs(direction.z));
        }

        // Clip space back to view space. The clip-space z is irrelevant to the
        // result's direction - every depth along one pixel's ray unprojects
        // onto the same line through the eye - so the far plane is used simply
        // because it is guaranteed to have a non-zero w.
        glm::vec3 unproject(const glm::mat4& inverseProjection, float ndcX, float ndcY) {
            const glm::vec4 view = inverseProjection * glm::vec4{ndcX, ndcY, 1.f, 1.f};
            return glm::vec3{view} / view.w;
        }

    }  // namespace

    float clusterSliceDepth(const ClusterGrid& grid, uint32_t slice) {
        if (grid.z == 0) {
            return grid.nearPlane;
        }
        const float ratio = grid.farPlane / grid.nearPlane;
        const float t = static_cast<float>(slice) / static_cast<float>(grid.z);
        return grid.nearPlane * std::pow(ratio, t);
    }

    glm::vec2 clusterSliceScaleBias(const ClusterGrid& grid) {
        const float span = std::log2(grid.farPlane / grid.nearPlane);
        if (grid.z == 0 || std::abs(span) < 1e-9f) {
            return glm::vec2{0.f};
        }
        const float scale = static_cast<float>(grid.z) / span;
        return glm::vec2{scale, -std::log2(grid.nearPlane) * scale};
    }

    uint32_t clusterSliceForDepth(const ClusterGrid& grid, float viewDepth) {
        if (grid.z == 0) {
            return 0;
        }
        if (viewDepth <= grid.nearPlane) {
            return 0;
        }
        const glm::vec2 scaleBias = clusterSliceScaleBias(grid);
        const float slice = std::log2(viewDepth) * scaleBias.x + scaleBias.y;
        if (slice <= 0.f) {
            return 0;
        }
        // Anything at or beyond the far plane belongs to the last slice rather
        // than to a slice past the end: the grid is the whole clustering
        // range, and a light just outside it still lights what is just inside.
        const auto floored = static_cast<uint32_t>(slice);
        return std::min(floored, grid.z - 1);
    }

    ClusterBounds clusterBounds(
        const ClusterGrid& grid,
        const glm::mat4& inverseProjection,
        uint32_t ix,
        uint32_t iy,
        uint32_t iz) {
        const float minU = static_cast<float>(ix) / static_cast<float>(grid.x);
        const float maxU = static_cast<float>(ix + 1) / static_cast<float>(grid.x);
        const float minV = static_cast<float>(iy) / static_cast<float>(grid.y);
        const float maxV = static_cast<float>(iy + 1) / static_cast<float>(grid.y);

        const float nearDepth = clusterSliceDepth(grid, iz);
        const float farDepth = clusterSliceDepth(grid, iz + 1);

        // All four corners rather than two opposite ones: an off-centre
        // projection has no symmetry to exploit, and the tile's own corners
        // are what bound it whatever the projection does.
        const glm::vec2 corners[4] = {
            {minU, minV},
            {maxU, minV},
            {minU, maxV},
            {maxU, maxV},
        };

        ClusterBounds bounds{};
        bool first = true;
        for (const glm::vec2& corner : corners) {
            const glm::vec3 ray =
                unproject(inverseProjection, corner.x * 2.f - 1.f, corner.y * 2.f - 1.f);
            const glm::vec3 nearPoint = pointAtDepth(ray, nearDepth);
            const glm::vec3 farPoint = pointAtDepth(ray, farDepth);

            for (const glm::vec3& point : {nearPoint, farPoint}) {
                if (first) {
                    bounds.minPoint = point;
                    bounds.maxPoint = point;
                    first = false;
                } else {
                    bounds.minPoint = glm::min(bounds.minPoint, point);
                    bounds.maxPoint = glm::max(bounds.maxPoint, point);
                }
            }
        }
        return bounds;
    }

    bool sphereTouchesCluster(const ClusterBounds& bounds, glm::vec3 centre, float radius) {
        const glm::vec3 closest = glm::clamp(centre, bounds.minPoint, bounds.maxPoint);
        const glm::vec3 offset = centre - closest;
        return glm::dot(offset, offset) <= radius * radius;
    }

}  // namespace ege
