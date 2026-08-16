#pragma once

#include "reflect/BuiltinTypes.hpp"

#include <glm/glm.hpp>

namespace ege {

    // Point light as the GPU sees it.
    //
    // Both members are vec4 even though position needs only three components,
    // because std430 rounds a vec3 up to a vec4 anyway - writing it as vec3
    // just makes the padding invisible and the layout easy to get wrong. The
    // spare component earns its keep: it carries the cull radius the light
    // culling pass tests clusters against.
    //
    // There used to be a `maxPointLights = 16` beside this, the length of a
    // fixed array in the uniform block that every fragment looped over. The
    // lights now live in a storage buffer and no fragment loops over all of
    // them, so the ceiling is gone rather than raised; what bounds the buffer
    // is maxSceneLights in ClusterGrid.hpp, which costs memory and not time.
    struct GpuPointLight {
        glm::vec4 position{0.f};  // xyz world position, w cull radius
        glm::vec4 color{1.f};     // w is intensity
        // x: which cube of the shadow array holds this light's shadow, or -1
        // for a light that casts none. A float rather than an int because the
        // rest of the struct is vec4s and mixing the two invites a layout
        // mistake nothing would report.
        glm::vec4 shadow{-1.f, 0.f, 0.f, 0.f};
    };

    // Authoring-side point light. Becomes a component when the ECS lands.
    struct PointLight {
        glm::vec3 color{1.f};
        float intensity = 1.f;
        // Distance at which the light is considered to contribute nothing.
        // Not used for attenuation, which is inverse-square; this is what the
        // cluster culling tests against, and the far plane of the light's
        // shadow cube if it casts one.
        float range = 25.f;
        // Whether this light casts shadows. Six depth passes when it does, so
        // it is per-light rather than universal: a scene can afford that for
        // the few lights whose shadows are looked at and not for the decorative
        // ones. Only the first `maxShadowedPointLights` asking for it get it.
        bool castsShadows = true;
    };

    // A sun: parallel rays, no falloff, and the one light that casts a
    // shadow map today. The renderer uses the first one it finds; a second
    // sun is ignored until something needs it.
    struct DirectionalLight {
        // The direction the light travels, scene-ward. Normalised at gather,
        // so authored values need not be.
        glm::vec3 direction{0.f, 1.f, 0.f};
        glm::vec3 color{1.f};
        float intensity = 1.f;
    };

}  // namespace ege

EGE_REFLECT(ege::PointLight)
EGE_FIELD(color).asColor();
EGE_FIELD(intensity).range(0.f, 100.f);
EGE_FIELD(range).range(0.f, 1000.f).tooltip("Culling radius, not attenuation falloff");
EGE_FIELD(castsShadows).tooltip("Six depth passes when on; only the first few lights get it");
EGE_REFLECT_END()

EGE_REFLECT(ege::DirectionalLight)
EGE_FIELD(direction).tooltip("Direction the light travels; normalised at use");
EGE_FIELD(color).asColor();
EGE_FIELD(intensity).range(0.f, 100.f);
EGE_REFLECT_END()
