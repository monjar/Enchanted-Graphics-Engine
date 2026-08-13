#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <limits>

namespace ege {

    // Axis-aligned bounding box in whatever space its owner is expressed in.
    struct Aabb {
        glm::vec3 min{std::numeric_limits<float>::max()};
        glm::vec3 max{std::numeric_limits<float>::lowest()};

        bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

        void expand(const glm::vec3& point) {
            min = glm::min(min, point);
            max = glm::max(max, point);
        }

        glm::vec3 center() const { return (min + max) * 0.5f; }

        glm::vec3 extents() const { return (max - min) * 0.5f; }

        // Transformed AABB, computed by mapping the box's centre and extents
        // rather than its eight corners. Cheaper, and the result is the same
        // tight axis-aligned box in the destination space - see the absolute
        // value of the basis in the implementation.
        Aabb transformed(const glm::mat4& matrix) const;
    };

    // A bounding sphere, used for the cheap first rejection in culling: a
    // sphere-plane test is a dot product and a compare, where a box test is
    // three of each.
    struct Sphere {
        glm::vec3 center{0.f};
        float radius = 0.f;

        static Sphere fromAabb(const Aabb& box) {
            Sphere sphere{};
            sphere.center = box.center();
            sphere.radius = glm::length(box.extents());
            return sphere;
        }
    };

    // View frustum as six inward-facing planes, each stored as (nx, ny, nz, d)
    // with the plane satisfying dot(n, p) + d = 0.
    class Frustum {
    public:
        enum Plane { Left, Right, Bottom, Top, Near, Far, PlaneCount };

        // Extracts the planes from a combined view-projection matrix using the
        // Gribb-Hartmann method: each plane is a sum or difference of two rows
        // of the matrix, which works for any projection including the reversed
        // and infinite ones without special cases.
        static Frustum fromViewProjection(const glm::mat4& viewProjection);

        // False when the volume is entirely outside. May report true for
        // something just outside a corner, which costs a wasted draw but never
        // wrongly discards visible geometry - the direction the error has to go.
        bool intersects(const Sphere& sphere) const;
        bool intersects(const Aabb& box) const;

        const glm::vec4& plane(Plane which) const { return planes[which]; }

    private:
        glm::vec4 planes[PlaneCount]{};
    };

}  // namespace ege
