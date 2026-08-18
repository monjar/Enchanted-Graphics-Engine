#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace ege {

    // Spot lights: the cone they shine through, and the map they cast into.
    //
    // A spot is the cheapest light to shadow of the three. The sun needs a
    // cascade per depth slice because it lights everything the camera can see;
    // a point light needs six faces because it lights every direction. A spot
    // already has exactly one direction and one bounded angle, so one ordinary
    // perspective map covers it - which is why this file is so much smaller
    // than the two beside it.
    //
    // All arithmetic, no Vulkan, so both halves are unit-tested with no GPU.

    // How many spot lights cast shadows at once. One depth pass each, so this
    // is a sixth of what a point light costs and the cap can be looser.
    inline constexpr uint32_t maxShadowedSpotLights = 4;

    // The near plane of a spot's shadow map. Small, because a spot is usually
    // mounted close to what it lights; the far plane is the light's range.
    inline constexpr float spotShadowNearPlane = 0.05f;

    // How much of a spot reaches a point, from the cone alone: 1 inside the
    // inner angle, 0 outside the outer one, and a smooth falloff between.
    //
    // Both angles arrive as cosines because that is what a dot product gives
    // and converting back would cost an inverse cosine per fragment. Note the
    // ordering that follows from that: a *larger* angle is a *smaller* cosine,
    // so the outer cosine is the lower bound.
    //
    // `toFragment` need not be normalised; `spotDirection` must be.
    float spotConeAttenuation(
        glm::vec3 spotDirection, glm::vec3 toFragment, float cosOuter, float cosInner);

    // The matrix a spot's shadow map is rendered and tested through.
    //
    // The field of view is twice the outer half-angle, so the map covers
    // exactly the cone and not a degree more - every texel spent outside it
    // would be a texel the lit region does not get. `outerAngle` is in
    // radians and is clamped to something a perspective projection can
    // express: an angle at or past a right angle has no finite frustum.
    glm::mat4 spotShadowMatrix(
        glm::vec3 position, glm::vec3 direction, float outerAngle, float range);

}  // namespace ege
