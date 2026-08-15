#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace ege {

    // Fitting the sun's shadow maps to the camera's view.
    //
    // One shadow map stretched over the whole visible range spends most of its
    // texels where nothing looks at them: the far distance covers acres per
    // texel while the ground under the camera - the only place anyone can see
    // a shadow's edge - gets a handful. Cascades split the view frustum by
    // depth and give each slice its own map, so texel density follows the
    // camera instead of the scene's bounding box. That is what replaces the
    // fixed box this engine shipped first, which was sized to the demo floor
    // and would have shadowed nothing in a scene any larger.
    //
    // Everything here is arithmetic on matrices and no Vulkan whatsoever, so
    // the fitting can be - and is - unit-tested on a machine with no GPU.

    inline constexpr uint32_t maxShadowCascades = 4;

    struct CascadeSettings {
        uint32_t count = 4;
        // Blend between an even split of the depth range and an even split of
        // its logarithm. Pure logarithmic matches how perspective compresses
        // distance and is right in principle; pure uniform gives the near
        // cascade too little. The practical scheme mixes them, and 0.7 is the
        // value most engines land on.
        float splitLambda = 0.7f;
        // The shadow map's side length. Needed for texel snapping, which is
        // what stops shadow edges crawling as the camera moves.
        uint32_t resolution = 2048;
        // How far behind the fitted slice the light starts rendering, so that
        // geometry between the light and the slice still casts into it. A
        // caster outside the camera's view still shadows what is inside it.
        float casterExtrusion = 30.f;
    };

    // What the renderer needs per cascade: the matrix to render and test
    // through, and where the cascade ends in view depth.
    struct ShadowCascade {
        glm::mat4 viewProjection{1.f};
        // Distance from the camera along its forward axis where this cascade
        // stops and the next begins.
        float splitDepth = 0.f;
    };

    struct ShadowCascadeSet {
        std::array<ShadowCascade, maxShadowCascades> cascades{};
        uint32_t count = 0;
    };

    // Fits `settings.count` cascades to the camera's frustum.
    //
    // `inverseViewProjection` unprojects clip space back to world space, which
    // is how the frustum corners are found without re-deriving them from the
    // camera's fields - it works for any projection the camera can hold.
    // `lightDirection` is the direction the light travels.
    //
    // Two properties are deliberate and both are pinned by tests. Each slice
    // is bounded by a *sphere* rather than by its corners, because a box
    // fitted to the corners changes size as the camera turns and the shadows
    // visibly swim; a sphere has the same radius from every angle. And the
    // light-space origin is snapped to whole texels, because a sub-texel shift
    // between frames re-rasterises every edge slightly differently, which
    // reads as crawling along every shadow boundary.
    ShadowCascadeSet fitShadowCascades(
        const glm::mat4& inverseViewProjection,
        glm::vec3 lightDirection,
        float nearPlane,
        float farPlane,
        const CascadeSettings& settings);

    // The split distances the fit uses, exposed because the shader needs the
    // same numbers to choose a cascade and a test needs to check them without
    // building matrices.
    std::array<float, maxShadowCascades> cascadeSplitDistances(
        float nearPlane, float farPlane, uint32_t count, float lambda);

}  // namespace ege
