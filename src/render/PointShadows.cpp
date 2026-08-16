#include "render/PointShadows.hpp"

#include "core/Assert.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace ege {

    namespace {

        // The conventional cube-map basis: for each face, the direction it
        // looks along and the up vector that makes its image come out the way
        // the hardware reads it. These are not free choices - a different but
        // equally "reasonable" up vector rotates or mirrors that face's image,
        // and only that face's, so the scene ends up with shadows that are
        // right in four directions and wrong in two.
        struct FaceBasis {
            glm::vec3 forward;
            glm::vec3 up;
        };

        constexpr std::array<FaceBasis, cubeFaceCount> faceBases{
            FaceBasis{{1.f, 0.f, 0.f}, {0.f, -1.f, 0.f}},   // +X
            FaceBasis{{-1.f, 0.f, 0.f}, {0.f, -1.f, 0.f}},  // -X
            FaceBasis{{0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}},    // +Y
            FaceBasis{{0.f, -1.f, 0.f}, {0.f, 0.f, -1.f}},  // -Y
            FaceBasis{{0.f, 0.f, 1.f}, {0.f, -1.f, 0.f}},   // +Z
            FaceBasis{{0.f, 0.f, -1.f}, {0.f, -1.f, 0.f}},  // -Z
        };

    }  // namespace

    glm::mat4 pointShadowFaceMatrix(glm::vec3 position, uint32_t face, float farPlane) {
        EGE_VERIFY(face < cubeFaceCount, "cube face index out of range");

        const FaceBasis& basis = faceBases[face];
        // Ninety degrees square: six of them meeting at the light is exactly
        // the whole sphere, with no overlap to waste and no gap to leak
        // through.
        const glm::mat4 projection =
            glm::perspective(glm::radians(90.f), 1.f, pointShadowNearPlane, farPlane);
        const glm::mat4 view = glm::lookAt(position, position + basis.forward, basis.up);
        return projection * view;
    }

    std::array<glm::mat4, cubeFaceCount> pointShadowFaceMatrices(
        glm::vec3 position, float farPlane) {
        std::array<glm::mat4, cubeFaceCount> matrices{};
        for (uint32_t face = 0; face < cubeFaceCount; face++) {
            matrices[face] = pointShadowFaceMatrix(position, face, farPlane);
        }
        return matrices;
    }

    uint32_t pointShadowFaceFor(glm::vec3 direction) {
        const glm::vec3 magnitude = glm::abs(direction);

        if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
            return direction.x >= 0.f ? 0u : 1u;
        }
        if (magnitude.y >= magnitude.z) {
            return direction.y >= 0.f ? 2u : 3u;
        }
        return direction.z >= 0.f ? 4u : 5u;
    }

    float pointShadowReferenceDepth(float axisDistance, float nearPlane, float farPlane) {
        // What glm::perspective with a zero-to-one depth range writes for a
        // point this far along the view axis, worked through by hand so the
        // shader can compute it without a matrix multiply:
        //
        //   ndc.z = far * (axis - near) / ((far - near) * axis)
        if (axisDistance <= 0.f) {
            return 0.f;
        }
        return farPlane * (axisDistance - nearPlane) / ((farPlane - nearPlane) * axisDistance);
    }

}  // namespace ege
