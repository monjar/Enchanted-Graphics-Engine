#include "render/SpotShadows.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace ege {

    namespace {

        // A vector not parallel to the spot, to build its view basis against.
        // Straight up fails for a spot pointing straight up or down, which a
        // ceiling light is.
        glm::vec3 upFor(glm::vec3 direction) {
            return std::abs(direction.y) > 0.99f ? glm::vec3{0.f, 0.f, 1.f}
                                                 : glm::vec3{0.f, 1.f, 0.f};
        }

    }  // namespace

    float spotConeAttenuation(
        glm::vec3 spotDirection, glm::vec3 toFragment, float cosOuter, float cosInner) {
        const float lengthSquared = glm::dot(toFragment, toFragment);
        if (lengthSquared < 1e-12f) {
            return 1.f;  // at the light itself; the cone has no meaning here
        }
        const float cosAngle =
            glm::dot(spotDirection, toFragment * glm::inversesqrt(lengthSquared));

        // A larger angle is a smaller cosine, so inner is the upper bound. An
        // authored pair whose inner angle exceeds its outer would invert the
        // range and produce a cone bright at the rim and dark in the middle;
        // the guard makes that case a hard edge instead, which is merely
        // plain rather than wrong.
        const float span = cosInner - cosOuter;
        if (span <= 1e-5f) {
            return cosAngle >= cosOuter ? 1.f : 0.f;
        }
        return std::clamp((cosAngle - cosOuter) / span, 0.f, 1.f);
    }

    glm::mat4 spotShadowMatrix(
        glm::vec3 position, glm::vec3 direction, float outerAngle, float range) {
        // A projection needs a field of view strictly inside a half turn, and
        // twice the outer angle is what covers the cone - so the half-angle
        // has to stay under a right angle by a margin the matrix can express.
        constexpr float maxHalfAngle = 1.5f;  // just under pi/2
        const float halfAngle = std::clamp(outerAngle, 0.01f, maxHalfAngle);
        const float farPlane = std::max(range, spotShadowNearPlane * 2.f);

        const glm::vec3 forward = glm::normalize(direction);
        const glm::mat4 projection =
            glm::perspective(halfAngle * 2.f, 1.f, spotShadowNearPlane, farPlane);
        const glm::mat4 view = glm::lookAt(position, position + forward, upFor(forward));
        return projection * view;
    }

}  // namespace ege
