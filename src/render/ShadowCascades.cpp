#include "render/ShadowCascades.hpp"

#include "core/Assert.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace ege {

    namespace {

        // The eight corners of the unit clip cube, unprojected into world
        // space. Vulkan's clip space is z in [0, 1], which is what
        // GLM_FORCE_DEPTH_ZERO_TO_ONE gives the projection matrices here.
        std::array<glm::vec3, 8> frustumCorners(const glm::mat4& inverseViewProjection) {
            constexpr std::array<glm::vec3, 8> clipCorners{
                glm::vec3{-1.f, -1.f, 0.f},
                glm::vec3{1.f, -1.f, 0.f},
                glm::vec3{-1.f, 1.f, 0.f},
                glm::vec3{1.f, 1.f, 0.f},
                glm::vec3{-1.f, -1.f, 1.f},
                glm::vec3{1.f, -1.f, 1.f},
                glm::vec3{-1.f, 1.f, 1.f},
                glm::vec3{1.f, 1.f, 1.f}};

            std::array<glm::vec3, 8> corners{};
            for (std::size_t i = 0; i < clipCorners.size(); i++) {
                const glm::vec4 unprojected =
                    inverseViewProjection * glm::vec4{clipCorners[i], 1.f};
                corners[i] = glm::vec3{unprojected} / unprojected.w;
            }
            return corners;
        }

        // A vector not parallel to the light, to build the light's view basis
        // against. Straight up fails for a light pointing straight up or down,
        // which a sun at noon is.
        glm::vec3 upFor(glm::vec3 direction) {
            return std::abs(direction.y) > 0.99f ? glm::vec3{0.f, 0.f, 1.f}
                                                 : glm::vec3{0.f, 1.f, 0.f};
        }

    }  // namespace

    std::array<float, maxShadowCascades> cascadeSplitDistances(
        float nearPlane, float farPlane, uint32_t count, float lambda) {
        EGE_VERIFY(count >= 1 && count <= maxShadowCascades, "cascade count out of range");

        std::array<float, maxShadowCascades> splits{};
        const float range = farPlane - nearPlane;
        const float ratio = farPlane / nearPlane;

        for (uint32_t i = 0; i < count; i++) {
            const float p = static_cast<float>(i + 1) / static_cast<float>(count);
            // Logarithmic follows perspective's own compression of distance;
            // uniform keeps the far cascades from collapsing into nothing.
            const float logSplit = nearPlane * std::pow(ratio, p);
            const float uniformSplit = nearPlane + range * p;
            splits[i] = glm::mix(uniformSplit, logSplit, lambda);
        }
        // The last cascade always ends exactly at the far plane, whatever the
        // blend did with rounding: anything beyond it is unshadowed, and that
        // boundary should be where the caller said it was.
        splits[count - 1] = farPlane;
        return splits;
    }

    ShadowCascadeSet fitShadowCascades(
        const glm::mat4& inverseViewProjection,
        glm::vec3 lightDirection,
        float nearPlane,
        float farPlane,
        const CascadeSettings& settings) {
        ShadowCascadeSet result{};
        result.count = std::clamp(settings.count, 1u, maxShadowCascades);

        const std::array<float, maxShadowCascades> splits =
            cascadeSplitDistances(nearPlane, farPlane, result.count, settings.splitLambda);
        const std::array<glm::vec3, 8> corners = frustumCorners(inverseViewProjection);
        const glm::vec3 light = glm::normalize(lightDirection);

        const float depthRange = farPlane - nearPlane;
        float sliceStart = nearPlane;

        for (uint32_t cascade = 0; cascade < result.count; cascade++) {
            const float sliceEnd = splits[cascade];

            // The slice's own eight corners, found by walking each of the four
            // frustum edges from the near corner to the far one. Fractions of
            // the whole depth range, because that is what the edge spans.
            const float startFraction = (sliceStart - nearPlane) / depthRange;
            const float endFraction = (sliceEnd - nearPlane) / depthRange;

            std::array<glm::vec3, 8> sliceCorners{};
            for (std::size_t i = 0; i < 4; i++) {
                const glm::vec3 edge = corners[i + 4] - corners[i];
                sliceCorners[i] = corners[i] + edge * startFraction;
                sliceCorners[i + 4] = corners[i] + edge * endFraction;
            }

            // Bound the slice with a sphere, not a box. A box fitted to the
            // corners changes size as the camera turns - the diagonal of a
            // frustum is longer than its side - and every shadow in the scene
            // swims as the texel density changes underneath it. A sphere has
            // one radius from every angle, so turning the camera cannot
            // resize the map.
            glm::vec3 center{0.f};
            for (const glm::vec3& corner : sliceCorners) {
                center += corner;
            }
            center /= static_cast<float>(sliceCorners.size());

            float radius = 0.f;
            for (const glm::vec3& corner : sliceCorners) {
                radius = std::max(radius, glm::length(corner - center));
            }
            // Quantised upwards, so that a radius which drifts by a hair
            // between frames - floating point over eight corner distances -
            // does not rescale the whole map and undo the texel snapping
            // below. Sixteenths of a world unit is far finer than any visible
            // change in coverage and far coarser than the drift.
            radius = std::ceil(radius * 16.f) / 16.f;

            // Snap the centre to whole texels, so that the map is rasterised
            // from the same world-space lattice every frame. Without it a
            // sub-texel shift re-rasterises every edge slightly differently
            // and all the shadows crawl, which is far more visible than the
            // half-texel of accuracy snapping costs.
            //
            // The lattice has to be anchored to something that does not move
            // with the camera, which is why the basis here is a rotation
            // about the origin rather than a look-at aimed at the slice: a
            // look-at puts its target at the origin of light space *by
            // construction*, so snapping the centre in that space would round
            // zero to zero and quietly do nothing at all.
            const glm::mat4 lightRotation = glm::lookAt(glm::vec3{0.f}, light, upFor(light));
            const float texelSize = (radius * 2.f) / static_cast<float>(settings.resolution);

            const glm::vec3 centerInLight = glm::vec3{lightRotation * glm::vec4{center, 1.f}};
            const glm::vec3 snappedInLight{
                std::floor(centerInLight.x / texelSize) * texelSize,
                std::floor(centerInLight.y / texelSize) * texelSize,
                centerInLight.z};
            const glm::vec3 snappedCenter =
                glm::vec3{glm::inverse(lightRotation) * glm::vec4{snappedInLight, 1.f}};

            // Look at the slice from far enough back that everything between
            // the light and the slice is still rendered: a caster behind the
            // camera casts into what the camera can see.
            const glm::vec3 eye = snappedCenter - light * (radius + settings.casterExtrusion);
            const glm::mat4 lightView = glm::lookAt(eye, snappedCenter, upFor(light));

            // The slice sits at distance (radius + extrusion) from the eye,
            // so the box spans the sphere with the extrusion in front of it.
            const glm::mat4 lightProjection = glm::ortho(
                -radius, radius, -radius, radius, 0.f, radius * 2.f + settings.casterExtrusion);

            result.cascades[cascade].viewProjection = lightProjection * lightView;
            result.cascades[cascade].splitDepth = sliceEnd;

            sliceStart = sliceEnd;
        }

        return result;
    }

}  // namespace ege
