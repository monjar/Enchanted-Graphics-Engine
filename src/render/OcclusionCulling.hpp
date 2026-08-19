#pragma once

#include "render/Bounds.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace ege {

    // Occlusion culling against a hierarchical depth buffer.
    //
    // Frustum culling drops what is outside the camera; it says nothing about
    // what is inside it and standing behind a wall. Those objects are drawn in
    // full, rasterized in full, and then thrown away one fragment at a time by
    // the depth test. The depth pre-pass makes them cost less than they used
    // to, but they still cost.
    //
    // A hierarchical depth buffer answers the question the depth test answers,
    // only for a whole object at once and before the object is submitted. It is
    // a pyramid over the frame's own depth: each texel of each level records the
    // farthest depth anywhere in the region it covers, so a single lookup at a
    // coarse level bounds a large piece of the screen. If everything already
    // drawn across an object's screen rectangle is nearer than the object's own
    // nearest point, nothing of it can be seen and the draw can be skipped.
    //
    // The pyramid is built on the GPU down to a level small enough to be worth
    // copying back - a few tens of kilobytes - and the test itself runs here,
    // on the CPU, because that is where the draws are issued. There is nothing
    // else this engine could do with the answer: it submits one draw per object
    // from a loop in C++, so a verdict that never leaves the GPU could not skip
    // anything.
    //
    // The cost of that is latency. The copy is read a couple of frames after
    // the frame it describes, so what is tested is whether an object was hidden
    // then, not whether it is hidden now. An object that becomes visible is
    // drawn again as soon as the next result arrives, so the artifact is a
    // fraction of a second of pop-in after a fast camera move, and never a
    // missing object that stays missing. Everything here errs the same way: any
    // uncertainty means "draw it".
    //
    // Device-free by design. The pyramid arrives as an array of floats and the
    // test is arithmetic on it, so the whole decision can be exercised without
    // a GPU - which is what the tests do, building pyramids by hand and
    // checking that an occluder hides what is behind it and nothing else.

    // How large the level copied back from the GPU is allowed to be, on its
    // longer axis. The pyramid is halved on the GPU until it fits, and the
    // rest of the levels are built here - they are small enough that doing so
    // costs less than another pass would.
    inline constexpr uint32_t occlusionPyramidMaxSize = 256;

    // How many texels of one level a single test is allowed to read along each
    // axis. It bounds the work: whatever fraction of the screen a box covers,
    // there is a level coarse enough to bound it in this many lookups. Larger
    // is a finer level for the same box and so a sharper answer, and since all
    // of this runs on a pyramid of a few tens of thousands of floats, sixty
    // four lookups is not a number worth economising on. Two, which is what a
    // GPU implementation would use, would throw away most of the resolution
    // that was copied back.
    inline constexpr uint32_t occlusionMaxTapsPerAxis = 8;

    // Grown by this fraction of its own size before an object is tested, so
    // that a box which is only just hidden is not culled on the strength of a
    // reading that is already a couple of frames old.
    inline constexpr float occlusionBoundsMargin = 0.05f;

    // The depth pyramid, once it is back on the CPU.
    //
    // Level 0 is what the GPU produced; each level above it is half the size,
    // rounded up, with every texel the maximum of the texels it covers. The
    // rounding matters: when a level's size is odd, a parent has to take in
    // three texels along that axis rather than two, or the last row falls out
    // of the pyramid and the level above claims a nearer maximum than the
    // truth. Claiming a nearer maximum is exactly the error that culls
    // something visible.
    class DepthPyramid {
    public:
        // Takes the level the GPU produced - width * height floats, each
        // already the farthest depth over its own footprint - and builds the
        // rest.
        void build(const float* level0, uint32_t width, uint32_t height);

        void clear();

        bool empty() const { return levels.empty(); }

        uint32_t levelCount() const { return static_cast<uint32_t>(levels.size()); }

        uint32_t width(uint32_t level) const { return levels[level].width; }

        uint32_t height(uint32_t level) const { return levels[level].height; }

        float at(uint32_t level, uint32_t x, uint32_t y) const;

        // The farthest depth anywhere in a rectangle given in level-0 texels.
        // Reads the coarsest level whose texels are still small enough to
        // bound the rectangle closely, which is what keeps this a handful of
        // lookups however large the rectangle is.
        //
        // The answer is an upper bound on the true maximum and may exceed it,
        // because a coarse texel covers more than was asked for. An upper
        // bound only ever prevents a cull.
        float maxDepth(glm::vec2 minTexel, glm::vec2 maxTexel) const;

    private:
        struct Level {
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<float> texels;
        };

        std::vector<Level> levels;
    };

    // A world-space box as it lands on screen.
    struct ScreenBounds {
        // False when the box cannot be tested at all - it reaches behind the
        // camera, where a projection says nothing useful. Those are drawn.
        bool testable = false;
        // The box's rectangle as a fraction of the frame, origin top left.
        glm::vec2 min{0.f};
        glm::vec2 max{0.f};
        // The depth of the box's nearest corner, in the units the depth buffer
        // stores - which for Vulkan means zero at the near plane and one at the
        // far one.
        float nearestDepth = 0.f;
    };

    // Where a world-space box lands on screen, seen through a combined
    // view-projection matrix.
    ScreenBounds projectBounds(const glm::mat4& viewProjection, const Aabb& box);

    // Whether the pyramid says that nothing of this box can be seen: every
    // pixel of its rectangle already has something nearer drawn into it than
    // the box's own nearest point.
    bool occludedByPyramid(const DepthPyramid& pyramid, const ScreenBounds& bounds);

    // A pyramid together with the camera it was captured through. The test has
    // to use that camera and not the current one: the pyramid records what was
    // in front of what from where the camera stood then, and re-projecting
    // today's boxes through today's camera onto it would compare two different
    // views of the scene.
    struct OcclusionSnapshot {
        DepthPyramid pyramid;
        glm::mat4 viewProjection{1.f};
        bool valid = false;

        // Whether this box was hidden in the frame the snapshot came from.
        // False whenever there is any doubt, including when there is no
        // snapshot at all.
        bool hides(const Aabb& box) const;
    };

}  // namespace ege
