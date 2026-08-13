#include "render/Bounds.hpp"

namespace ege {

    Aabb Aabb::transformed(const glm::mat4& matrix) const {
        if (!valid()) {
            return *this;
        }

        const glm::vec3 newCenter = glm::vec3{matrix * glm::vec4{center(), 1.f}};
        const glm::vec3 oldExtents = extents();

        // The transformed extent along each axis is the extent projected onto
        // the absolute value of the basis. Taking the absolute value is what
        // makes this equivalent to transforming all eight corners and taking
        // their bounds, without doing eight transforms.
        const glm::mat3 basis{matrix};
        const glm::vec3 newExtents{
            std::abs(basis[0].x) * oldExtents.x + std::abs(basis[1].x) * oldExtents.y +
                std::abs(basis[2].x) * oldExtents.z,
            std::abs(basis[0].y) * oldExtents.x + std::abs(basis[1].y) * oldExtents.y +
                std::abs(basis[2].y) * oldExtents.z,
            std::abs(basis[0].z) * oldExtents.x + std::abs(basis[1].z) * oldExtents.y +
                std::abs(basis[2].z) * oldExtents.z};

        Aabb result{};
        result.min = newCenter - newExtents;
        result.max = newCenter + newExtents;
        return result;
    }

    Frustum Frustum::fromViewProjection(const glm::mat4& m) {
        Frustum frustum{};

        // Gribb-Hartmann. glm is column-major, so m[column][row]; row i of the
        // matrix is (m[0][i], m[1][i], m[2][i], m[3][i]).
        const glm::vec4 row0{m[0][0], m[1][0], m[2][0], m[3][0]};
        const glm::vec4 row1{m[0][1], m[1][1], m[2][1], m[3][1]};
        const glm::vec4 row2{m[0][2], m[1][2], m[2][2], m[3][2]};
        const glm::vec4 row3{m[0][3], m[1][3], m[2][3], m[3][3]};

        frustum.planes[Left] = row3 + row0;
        frustum.planes[Right] = row3 - row0;
        frustum.planes[Bottom] = row3 + row1;
        frustum.planes[Top] = row3 - row1;
        // Vulkan clip space is z in [0, 1] rather than OpenGL's [-1, 1], so the
        // near plane is row2 alone rather than row3 + row2. Getting this wrong
        // culls geometry near the camera, which reads as objects vanishing when
        // you walk up to them.
        frustum.planes[Near] = row2;
        frustum.planes[Far] = row3 - row2;

        // Normalising lets the plane equation return a true signed distance,
        // which the sphere test needs to compare against a radius.
        for (glm::vec4& plane : frustum.planes) {
            const float length = glm::length(glm::vec3{plane});
            if (length > 0.f) {
                plane /= length;
            }
        }

        return frustum;
    }

    bool Frustum::intersects(const Sphere& sphere) const {
        for (const glm::vec4& plane : planes) {
            const float distance = glm::dot(glm::vec3{plane}, sphere.center) + plane.w;
            if (distance < -sphere.radius) {
                return false;
            }
        }
        return true;
    }

    bool Frustum::intersects(const Aabb& box) const {
        if (!box.valid()) {
            return false;
        }

        const glm::vec3 center = box.center();
        const glm::vec3 extents = box.extents();

        for (const glm::vec4& plane : planes) {
            const glm::vec3 normal{plane};
            // Projected radius of the box onto the plane normal. The absolute
            // value picks the corner furthest along the normal without testing
            // all eight.
            const float radius = extents.x * std::abs(normal.x) + extents.y * std::abs(normal.y) +
                                 extents.z * std::abs(normal.z);
            if (glm::dot(normal, center) + plane.w < -radius) {
                return false;
            }
        }
        return true;
    }

}  // namespace ege
