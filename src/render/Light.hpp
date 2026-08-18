#pragma once

#include "reflect/BuiltinTypes.hpp"

#include <glm/glm.hpp>

namespace ege {

    // Which kind of light a GpuLight describes. Both kinds share one buffer
    // and one culling pass rather than getting an array each: a spot light is
    // a point light with a direction and a cone, so everything up to the
    // shading itself is the same work on the same data.
    enum class GpuLightType : int {
        point = 0,
        spot = 1,
    };

    // A light as the GPU sees it, point or spot.
    //
    // Every member is a vec4 even where three components would do, because
    // std430 rounds a vec3 up to a vec4 anyway - writing it as vec3 just makes
    // the padding invisible and the layout easy to get wrong. The spare
    // components earn their keep, and each is documented where it is packed
    // rather than left to be discovered.
    //
    // There used to be a `maxPointLights = 16` beside this, the length of a
    // fixed array in the uniform block that every fragment looped over. The
    // lights now live in a storage buffer and no fragment loops over all of
    // them, so the ceiling is gone rather than raised; what bounds the buffer
    // is maxSceneLights in ClusterGrid.hpp, which costs memory and not time.
    struct GpuLight {
        glm::vec4 position{0.f};  // xyz world position, w cull radius
        glm::vec4 color{1.f};     // rgb colour, w intensity
        // xyz: the direction a spot points, ignored for a point light.
        // w: the cosine of its outer cone half-angle, past which it is dark.
        glm::vec4 direction{0.f, 1.f, 0.f, -1.f};
        // x: which slot of the shadow array holds this light's shadow, or -1
        // for a light that casts none. Point lights index a cube array and
        // spots index a 2D array, so the two never collide.
        // y: the cosine of the inner cone half-angle, inside which a spot is
        //    at full brightness; between the two cosines it falls off.
        // z: the light's type, as a GpuLightType. A float rather than an int
        //    because the rest of the struct is vec4s and mixing the two
        //    invites a layout mistake nothing would report.
        glm::vec4 params{-1.f, 1.f, 0.f, 0.f};
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

    // Authoring-side spot light: a point light that only shines through a
    // cone. Two angles rather than one, because a cone with a single hard
    // edge looks like a cardboard cutout - real light sources have a bright
    // middle and a soft rim, and the gap between the two angles is that rim.
    struct SpotLight {
        glm::vec3 color{1.f};
        float intensity = 1.f;
        // Distance at which the light is considered to contribute nothing:
        // what the cluster culling tests, and the far plane of its shadow map.
        float range = 25.f;
        // Half-angles in radians. Full brightness inside the inner one,
        // nothing outside the outer one, and a smooth falloff between. The
        // inner is clamped below the outer at use, so an authored pair that
        // crosses over dims rather than misbehaving.
        float innerAngle = 0.25f;
        float outerAngle = 0.4f;
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

EGE_REFLECT(ege::SpotLight)
EGE_FIELD(color).asColor();
EGE_FIELD(intensity).range(0.f, 100.f);
EGE_FIELD(range).range(0.f, 1000.f).tooltip("Culling radius, not attenuation falloff");
EGE_FIELD(innerAngle).range(0.f, 1.57f).tooltip("Half-angle of the fully lit core, in radians");
EGE_FIELD(outerAngle).range(0.f, 1.57f).tooltip("Half-angle where the cone goes dark, in radians");
EGE_FIELD(castsShadows).tooltip("One depth pass when on; only the first few lights get it");
EGE_REFLECT_END()

EGE_REFLECT(ege::DirectionalLight)
EGE_FIELD(direction).tooltip("Direction the light travels; normalised at use");
EGE_FIELD(color).asColor();
EGE_FIELD(intensity).range(0.f, 100.f);
EGE_REFLECT_END()
