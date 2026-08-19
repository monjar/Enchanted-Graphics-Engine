#include "render/Ssao.hpp"

#include <cmath>
#include <random>

namespace ege {

    std::vector<glm::vec4> ssaoKernel(uint32_t count, uint32_t seed) {
        std::mt19937 rng{seed};
        std::uniform_real_distribution<float> symmetric{-1.f, 1.f};
        std::uniform_real_distribution<float> unitRange{0.f, 1.f};

        std::vector<glm::vec4> kernel;
        kernel.reserve(count);

        for (uint32_t i = 0; i < count; i++) {
            glm::vec3 direction{symmetric(rng), symmetric(rng), unitRange(rng)};
            const float length = glm::length(direction);
            // Only reachable if all three components come out zero, which is
            // vanishingly unlikely and still not a reason to emit a sample
            // with no direction at all.
            direction = length < 1e-6f ? glm::vec3{0.f, 0.f, 1.f} : direction / length;

            // Quadratic in the sample's index, so the first samples sit close
            // to the surface and the last reach the full radius. Multiplied by
            // a random factor as well, so that samples of neighbouring indices
            // do not all land on the same shell.
            const float alongIndex = static_cast<float>(i) / static_cast<float>(count);
            const float scale =
                (0.1f + 0.9f * alongIndex * alongIndex) * (0.25f + 0.75f * unitRange(rng));

            // w is padding: the shader reads these out of a std140 block,
            // where a vec3 occupies a vec4's worth of space regardless.
            kernel.push_back(glm::vec4{direction * scale, 0.f});
        }

        return kernel;
    }

    std::vector<glm::vec4> ssaoNoise(uint32_t count, uint32_t seed) {
        std::mt19937 rng{seed};
        std::uniform_real_distribution<float> symmetric{-1.f, 1.f};

        std::vector<glm::vec4> noise;
        noise.reserve(count);

        for (uint32_t i = 0; i < count; i++) {
            glm::vec2 rotation{symmetric(rng), symmetric(rng)};
            const float length = glm::length(rotation);
            // Normalized so the encoding into an 8-bit texture uses its whole
            // range, and so a vector that comes out near zero cannot leave the
            // shader's tangent frame degenerate.
            rotation = length < 1e-4f ? glm::vec2{1.f, 0.f} : rotation / length;
            noise.push_back(glm::vec4{rotation, 0.f, 0.f});
        }

        return noise;
    }

    glm::vec3 viewPositionFromDepth(const glm::mat4& inverseProjection, glm::vec3 ndc) {
        const glm::vec4 position = inverseProjection * glm::vec4{ndc, 1.f};
        // The perspective divide, undone. A projection matrix that puts
        // something other than 1 in w is the entire reason this is not just
        // the matrix product.
        return glm::vec3{position} / position.w;
    }

    glm::vec3 ndcFromScreen(glm::vec2 uv, float depth) {
        return glm::vec3{uv * 2.f - 1.f, depth};
    }

}  // namespace ege
