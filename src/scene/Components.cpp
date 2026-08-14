#include "scene/Components.hpp"

#include <glm/gtx/euler_angles.hpp>

namespace ege {

    glm::mat4 Transform::mat4() const {
        const float c3 = glm::cos(rotation.z);
        const float s3 = glm::sin(rotation.z);
        const float c2 = glm::cos(rotation.x);
        const float s2 = glm::sin(rotation.x);
        const float c1 = glm::cos(rotation.y);
        const float s1 = glm::sin(rotation.y);
        return glm::mat4{
            {
                scale.x * (c1 * c3 + s1 * s2 * s3),
                scale.x * (c2 * s3),
                scale.x * (c1 * s2 * s3 - c3 * s1),
                0.0f,
            },
            {
                scale.y * (c3 * s1 * s2 - c1 * s3),
                scale.y * (c2 * c3),
                scale.y * (c1 * c3 * s2 + s1 * s3),
                0.0f,
            },
            {
                scale.z * (c2 * s1),
                scale.z * (-s2),
                scale.z * (c1 * c2),
                0.0f,
            },
            {translation.x, translation.y, translation.z, 1.0f}};
    }

    Transform Transform::fromMatrix(const glm::mat4& matrix) {
        Transform out{};
        out.translation = glm::vec3{matrix[3]};
        out.scale = glm::vec3{
            glm::length(glm::vec3{matrix[0]}),
            glm::length(glm::vec3{matrix[1]}),
            glm::length(glm::vec3{matrix[2]})};

        // Dividing each basis vector by its length leaves the rotation. A
        // degenerate axis has no direction to recover, so it keeps identity
        // rather than producing NaNs that would spread through the scene.
        glm::mat4 rotation{1.f};
        for (int column = 0; column < 3; column++) {
            const float length = out.scale[column];
            if (length > 1e-8f) {
                rotation[column] = matrix[column] / length;
            }
        }

        // extractEulerAngleYXZ decomposes M = Ry * Rx * Rz, which is exactly
        // the composition mat4() builds.
        glm::extractEulerAngleYXZ(rotation, out.rotation.y, out.rotation.x, out.rotation.z);
        return out;
    }

    // The upper 3x3 of mat4() is M = R * S, with R orthonormal and S a positive
    // diagonal scale. For that form the usual transpose(inverse(M)) simplifies
    // exactly:
    //
    //     transpose(inverse(R * S)) = transpose(inverse(S) * transpose(R))
    //                               = R * inverse(S)
    //
    // because inverse(S) is diagonal and therefore its own transpose. Scaling
    // each rotation column by 1/scale is not an approximation here, it is the
    // exact normal matrix. Verified numerically over 2000 randomised transforms
    // in the test suite.
    //
    // This holds only while the transform stays a pure T * R * S composition.
    glm::mat3 Transform::normalMatrix() const {
        const float c3 = glm::cos(rotation.z);
        const float s3 = glm::sin(rotation.z);
        const float c2 = glm::cos(rotation.x);
        const float s2 = glm::sin(rotation.x);
        const float c1 = glm::cos(rotation.y);
        const float s1 = glm::sin(rotation.y);
        const glm::vec3 invScale = 1.0f / scale;

        return glm::mat3{
            {
                invScale.x * (c1 * c3 + s1 * s2 * s3),
                invScale.x * (c2 * s3),
                invScale.x * (c1 * s2 * s3 - c3 * s1),
            },
            {
                invScale.y * (c3 * s1 * s2 - c1 * s3),
                invScale.y * (c2 * c3),
                invScale.y * (c1 * c3 * s2 + s1 * s3),
            },
            {
                invScale.z * (c2 * s1),
                invScale.z * (-s2),
                invScale.z * (c1 * c2),
            },
        };
    }

}  // namespace ege
