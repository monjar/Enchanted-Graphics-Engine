#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace ege {

    // Screen-space ambient occlusion: how much of its surroundings a point can
    // actually see.
    //
    // Image-based ambient lighting hands every fragment the same irradiance
    // whatever is standing next to it, so the inside of a corner is lit as
    // brightly as an open field. Contact between two surfaces stops reading as
    // contact, and everything looks as though it is floating a little. The
    // shadow maps do not help: they answer for one light each, and the ambient
    // term is the sum of every direction there is no light for.
    //
    // The estimate is made from the depth buffer alone. Around each fragment,
    // points are sampled in the hemisphere over its surface; a sample that
    // turns out to sit behind the geometry the depth buffer already records is
    // one whose direction is blocked. The fraction blocked is the occlusion.
    // It is an estimate of the scene as seen from the camera and nothing more -
    // what the camera cannot see cannot occlude - which is the standing cost of
    // doing this in screen space rather than in the world.
    //
    // Everything in this header is arithmetic with no Vulkan in sight, which is
    // what lets it be tested on a machine with no GPU. ssao.frag implements the
    // same definitions: viewPositionFromDepth in particular is the
    // specification the shader's reconstruction is written against, and the
    // tests are what pin it to this engine's camera rather than to the
    // convention most published SSAO code assumes.

    // How many points in the hemisphere one fragment tests. The estimate is an
    // average over these, so its noise falls as the square root of the count -
    // which is why the blur below matters more than raising this does.
    inline constexpr uint32_t ssaoSampleCount = 32;

    // The kernel is uploaded as a fixed-size array, so the shader's block has
    // a size that does not depend on how many samples are in use. Anything up
    // to this is legal; the count in use travels beside it.
    inline constexpr uint32_t ssaoMaxSamples = 64;

    // The rotation texture is this many texels on a side, and the blur that
    // follows is the same width. That is not a coincidence: turning the kernel
    // by a different angle per pixel trades banding for noise, and a box blur
    // exactly as wide as the pattern is what averages the noise back out
    // without smearing the occlusion across an edge.
    inline constexpr uint32_t ssaoNoiseSize = 4;

    // How far from a point the estimate reaches, in world units. The demo's
    // spheres are about a unit across and sit on a floor, so half a unit is
    // roughly the scale at which one object shades its own contact with
    // another. Too large and every surface darkens against the whole room;
    // too small and the effect disappears into the depth buffer's precision.
    inline constexpr float ssaoRadius = 0.5f;

    // How much nearer than the sample point the recorded geometry has to be
    // before it counts as blocking it. Without it a flat surface occludes
    // itself: samples land within a hair of the depth already stored, the
    // comparison goes either way per pixel, and the floor comes out speckled.
    inline constexpr float ssaoBias = 0.02f;

    // The exponent the result is raised to. Above one it deepens the dark
    // parts and leaves the open ones alone, which is what keeps the effect
    // reading as contact rather than as a grey wash over everything.
    inline constexpr float ssaoPower = 1.6f;

    // The hemisphere the samples are taken from, in tangent space: +Z is the
    // surface normal, which is a convention of this kernel and not of the
    // engine's view space. Whichever way view space points, the shader builds
    // a frame from the surface normal and the kernel is expressed in that.
    //
    // Samples are packed towards the origin rather than spread evenly through
    // the hemisphere, because occlusion is a local effect: what sits within a
    // few centimetres of a point decides how dark its corner is, and a sample
    // out at the radius mostly reports on geometry that was going to be
    // visible anyway.
    //
    // Deterministic for a given seed - the kernel is generated once at startup
    // and uploaded, so nothing is gained by it differing between runs, and a
    // reproducible one can be tested.
    std::vector<glm::vec4> ssaoKernel(uint32_t count, uint32_t seed = 1u);

    // Angles to turn the kernel by, one per texel of a small tile repeated
    // across the screen. Each is a unit vector in the surface's tangent plane -
    // a cosine and a sine - and the shader spins its samples about the normal
    // by it, so neighbouring pixels sample different directions and the
    // estimate's error comes out as noise rather than as banding. The blur
    // that follows is exactly as wide as the tile, which is what turns that
    // noise back into an average.
    //
    // The third component is zero and stays zero. A rotation with any
    // component along the normal would tilt the hemisphere off the surface
    // instead of spinning it in place, and the samples that tilted below would
    // report the surface itself as their occluder.
    std::vector<glm::vec4> ssaoNoise(uint32_t count, uint32_t seed = 2u);

    // Where a depth-buffer sample sits in view space.
    //
    // `ndc` is a point in normalized device coordinates: x and y in [-1, 1],
    // and z the depth exactly as the buffer stores it. Vulkan's depth range is
    // [0, 1], not OpenGL's [-1, 1], so the z that comes out of the buffer is
    // used as it is - rescaling it, which most published SSAO does, puts every
    // reconstructed point in the wrong place.
    glm::vec3 viewPositionFromDepth(const glm::mat4& inverseProjection, glm::vec3 ndc);

    // The point on screen a fragment sits at, as normalized device
    // coordinates. `uv` is the fraction across the render target, origin at the
    // top left - which is where both gl_FragCoord and the fullscreen
    // triangle's own coordinates start. Vulkan's NDC y also points down, so
    // the two agree and this is a straight rescale with no flip in it.
    glm::vec3 ndcFromScreen(glm::vec2 uv, float depth);

}  // namespace ege
