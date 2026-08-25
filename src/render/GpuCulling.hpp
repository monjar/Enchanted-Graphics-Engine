#pragma once

#include "render/Bounds.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace ege {

    // The arithmetic of GPU-driven occlusion culling, kept device-free.
    //
    // The verdict moved onto the GPU because that is the only place it can be
    // both current and useful: a compute pass tests every candidate against a
    // depth pyramid built this frame and writes the instance counts the draws
    // consume, so nothing waits two frames for a copy to come back and nothing
    // pops in after a camera move. See shaders/gpu_cull_common.glsl for the
    // running copy of this logic; every function here mirrors it line for
    // line, and the tests pin this one because it is the one a test can reach.
    //
    // The shape of the frame it serves - draw last frame's visible set, build
    // the pyramid from that partial depth, test everything against it, then
    // draw whatever turned out to be newly visible - is documented in
    // docs/ROADMAP.md §10.6, along with why it needs no merged geometry
    // buffer: each batch keeps its own draw call, and only the instance count
    // moves onto the GPU.

    // How many of the pyramid's coarsest levels the culling shader binds. A
    // fixed number rather than the whole chain, because a descriptor per
    // level would make the set layout depend on the window size. Four spans
    // footprints from a few texels to most of the screen; an object too large
    // even for the coarsest bound level is drawn without a test, which for
    // something covering half the frame is almost always the right answer
    // anyway.
    inline constexpr uint32_t gpuCullBoundLevels = 4;

    // The widest an object's footprint may be, in texels of the level chosen
    // for it, before the test moves to a coarser level. Four texels means at
    // most five taps per axis once the rectangle's edges are included.
    inline constexpr uint32_t gpuCullMaxSpanTexels = 4;

    // Subtracted from an object's nearest depth before the comparison, so
    // that an object exactly coplanar with what the pyramid recorded - itself,
    // usually - is never culled by floating-point luck.
    inline constexpr float gpuCullDepthMargin = 1e-5f;

    // One VkDrawIndexedIndirectCommand is five 32-bit words; the non-indexed
    // VkDrawIndirectCommand is four. Commands are stored in five-word slots
    // either way, so the shader can find any command by index without knowing
    // which kind it is - and the one field the shader touches, the instance
    // count, sits at the same word in both layouts.
    inline constexpr uint32_t drawCommandWords = 5;
    inline constexpr uint32_t drawCommandInstanceCountWord = 1;

    // What the culling shader knows about one candidate, in the std430 layout
    // the shader reads. The sphere is world-space centre and radius; batch is
    // which draw command the object belongs to, and batchFirst is where that
    // batch's window in the compacted instance buffer begins - carried here so
    // the shader never has to read a command's firstInstance field, whose
    // position depends on whether the batch is indexed.
    struct GpuCullInput {
        glm::vec4 sphere{0.f};
        uint32_t batch = 0;
        uint32_t batchFirst = 0;
        uint32_t pad0 = 0;
        uint32_t pad1 = 0;
    };

    static_assert(sizeof(GpuCullInput) == 32, "must match the std430 struct the shader reads");

    // What the late culling pass counts, in the layout the shader writes.
    struct GpuCullStatsData {
        uint32_t occluded = 0;
        uint32_t drawnEarly = 0;
        uint32_t drawnLate = 0;
        uint32_t pad = 0;
    };

    static_assert(sizeof(GpuCullStatsData) == 16, "must match the std430 struct the shader writes");

    // The tightest sphere around a world-space box. Conservative by
    // construction - the box already grew when the local bounds were
    // transformed - and a conservative bound only ever prevents a cull.
    glm::vec4 boundingSphere(const Aabb& box);

    // A world-space sphere as it lands on screen.
    struct SphereScreenBounds {
        // False when the sphere reaches the near plane or behind it, where
        // the projection stops meaning anything. Those are drawn.
        bool testable = false;
        // The screen rectangle bounding the sphere, as a fraction of the
        // frame, origin top left - the same space the pyramid's texels are
        // addressed in.
        glm::vec2 minUv{0.f};
        glm::vec2 maxUv{0.f};
        // The depth of the sphere's nearest point, in the depth buffer's own
        // units. Depth under this projection is a function of view-space z
        // alone, so the nearest point is simply the centre brought forward by
        // the radius.
        float nearestDepth = 0.f;
    };

    // Where a sphere lands on screen, through a view and projection given
    // separately - the depth formula needs the projection's own terms, not
    // the product. The rectangle is the projection of the sphere's view-space
    // bounding box, which contains the sphere's true silhouette; a rectangle
    // too large only ever prevents a cull, because occlusion demands that
    // every texel of it be covered.
    SphereScreenBounds projectSphere(
        const glm::mat4& view, const glm::mat4& projection, glm::vec4 sphere, float nearPlane);

    // Which of the bound levels to test against: the finest one whose texels
    // are still large enough to bound the rectangle in gpuCullMaxSpanTexels
    // of them per axis. Extents are ordered finest first. Returns -1 when
    // even the coarsest is too fine, which means the object is enormous on
    // screen and is drawn without a test.
    int chooseLevel(
        glm::vec2 minUv,
        glm::vec2 maxUv,
        const glm::uvec2* levelExtents,
        uint32_t levelCount,
        uint32_t maxSpanTexels = gpuCullMaxSpanTexels);

    // Whether one level of the pyramid says nothing of this rectangle can be
    // seen: every texel it touches already holds a depth nearer than the
    // sphere's own nearest point. `texels` is the level's data, row-major.
    bool occludedAtLevel(
        const float* texels,
        glm::uvec2 extent,
        glm::vec2 minUv,
        glm::vec2 maxUv,
        float nearestDepth);

    // One batch, as command seeding needs to see it.
    struct SeedBatch {
        // Index count for an indexed mesh, vertex count otherwise.
        uint32_t drawCount = 0;
        uint32_t firstInstance = 0;
        bool indexed = true;
    };

    // The frame's draw commands, seeded on the CPU with everything the CPU
    // already knows - what to draw and where each batch's instances begin -
    // and with every instance count zero, which is the half the culling
    // passes fill in.
    //
    // Two commands per batch: slot b for the early pass, drawing what was
    // visible last frame, and slot batchCount + b for the late pass, drawing
    // what the pyramid test newly admitted. The late command's window in the
    // instance buffer starts maxInstances past the batch's own, so the two
    // passes can append independently without either knowing the other's
    // count.
    std::vector<uint32_t> seedDrawCommands(
        const std::vector<SeedBatch>& batches, uint32_t maxInstances);

}  // namespace ege
