#pragma once

#include "reflect/BuiltinTypes.hpp"

#include <glm/glm.hpp>

namespace ege {

    // Maximum lights the forward shader loops over.
    //
    // A fixed array in the uniform buffer is the simple forward approach and it
    // does not scale: every fragment iterates every light. Clustered shading in
    // Phase 9 is what removes the ceiling.
    inline constexpr int maxPointLights = 16;

    // Point light as the GPU sees it.
    //
    // Both members are vec4 even though position needs only three components,
    // because std140 rounds a vec3 up to a vec4 anyway - writing it as vec3
    // just makes the padding invisible and the layout easy to get wrong.
    struct GpuPointLight {
        glm::vec4 position{0.f};  // w unused
        glm::vec4 color{1.f};     // w is intensity
    };

    // Authoring-side point light. Becomes a component when the ECS lands.
    struct PointLight {
        glm::vec3 color{1.f};
        float intensity = 1.f;
        // Distance at which the light is considered to contribute nothing.
        // Not used for attenuation, which is inverse-square; this is for
        // culling once there are enough lights to need it.
        float range = 25.f;
    };

}  // namespace ege

EGE_REFLECT(ege::PointLight)
EGE_FIELD(color).asColor();
EGE_FIELD(intensity).range(0.f, 100.f);
EGE_FIELD(range).range(0.f, 1000.f).tooltip("Culling radius, not attenuation falloff");
EGE_REFLECT_END()
