#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace ege {

    // Shadow maps for point lights.
    //
    // A sun casts along one direction and needs one map per cascade. A point
    // light casts in every direction at once, so it needs a map that covers
    // the whole sphere around it - a cube, rendered as six 90-degree views
    // sharing the light's position. The frame graph's layered images are what
    // this is built on: six layers per light, one pass per layer, sampled
    // through a single cube view.
    //
    // Everything here is matrix arithmetic with no Vulkan in it, which is what
    // lets it be tested on a machine with no GPU - and there is a lot worth
    // testing, because the hardware has its own opinion about which direction
    // maps to which face and where on that face it lands. Getting the face
    // orientation wrong still produces a picture; it produces shadows that are
    // mirrored or rotated on some faces and correct on others, which is the
    // kind of thing that is very hard to look at and reason about.

    // How many point lights cast shadows at once. Six depth passes each, so
    // this is a real cost per light, and it is why a light casts only if it
    // is asked to: the demo's forty accent lights would be two hundred and
    // forty passes for shadows nobody would look at.
    inline constexpr uint32_t maxShadowedPointLights = 4;

    inline constexpr uint32_t cubeFaceCount = 6;

    // The cube's near plane. Small, because a point light is usually close to
    // what it lights; the far plane comes from the light's own range.
    inline constexpr float pointShadowNearPlane = 0.05f;

    // The view-projection for one face of the cube around `position`.
    //
    // `face` is the cube-map layer index in the order the hardware expects:
    // +X, -X, +Y, -Y, +Z, -Z. The result projects a world-space point into
    // that face's clip space, so a sample direction is looked up in the same
    // face the hardware would choose for it - which is the property the tests
    // pin, against the cube-map specification's own face and coordinate rules
    // rather than against this code's idea of them.
    glm::mat4 pointShadowFaceMatrix(glm::vec3 position, uint32_t face, float farPlane);

    // All six, in layer order.
    std::array<glm::mat4, cubeFaceCount> pointShadowFaceMatrices(
        glm::vec3 position, float farPlane);

    // Which face the hardware samples for a direction, by the cube-map rule:
    // the largest component of the vector picks the axis and its sign picks
    // the face.
    uint32_t pointShadowFaceFor(glm::vec3 direction);

    // The depth the shadow map holds for a point at this distance from the
    // light, in the same normalised units the map stores.
    //
    // `axisDistance` is the distance along the face's own forward axis - the
    // largest absolute component of the light-to-fragment vector - not the
    // straight-line distance. A perspective depth buffer stores depth along
    // the view axis, so comparing against the straight-line distance would
    // shadow every corner of every face.
    //
    // The shader recomputes this rather than reading it, so the formula is
    // duplicated there. That is what makes the round-trip test below worth
    // having: it checks this against what the face matrices actually produce.
    float pointShadowReferenceDepth(float axisDistance, float nearPlane, float farPlane);

}  // namespace ege
