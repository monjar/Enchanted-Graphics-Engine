#include "render/OcclusionCulling.hpp"

#include "core/Assert.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ege {

    namespace {

        // Half a size, rounded up: a level with an odd size still has to have
        // a parent, and that parent covers the odd texel out.
        uint32_t halved(uint32_t size) {
            return std::max(1u, (size + 1u) / 2u);
        }

    }  // namespace

    void DepthPyramid::clear() {
        levels.clear();
    }

    void DepthPyramid::build(const float* level0, uint32_t width, uint32_t height) {
        levels.clear();
        if (level0 == nullptr || width == 0 || height == 0) {
            return;
        }

        Level base{};
        base.width = width;
        base.height = height;
        base.texels.assign(level0, level0 + static_cast<std::size_t>(width) * height);
        levels.push_back(std::move(base));

        while (levels.back().width > 1 || levels.back().height > 1) {
            const Level& source = levels.back();
            Level next{};
            next.width = halved(source.width);
            next.height = halved(source.height);
            next.texels.assign(static_cast<std::size_t>(next.width) * next.height, 0.f);

            // Three texels rather than two along an axis whose source size is
            // odd. Two would leave the last row or column out of every parent,
            // and a parent that has not seen a texel can report a maximum
            // nearer than the one that is really there - which is the one
            // direction this must never err in.
            const uint32_t spanX = (source.width & 1u) != 0u ? 3u : 2u;
            const uint32_t spanY = (source.height & 1u) != 0u ? 3u : 2u;

            for (uint32_t y = 0; y < next.height; y++) {
                for (uint32_t x = 0; x < next.width; x++) {
                    float farthest = 0.f;
                    for (uint32_t dy = 0; dy < spanY; dy++) {
                        const uint32_t sy = std::min(y * 2u + dy, source.height - 1u);
                        for (uint32_t dx = 0; dx < spanX; dx++) {
                            const uint32_t sx = std::min(x * 2u + dx, source.width - 1u);
                            farthest = std::max(
                                farthest,
                                source.texels[static_cast<std::size_t>(sy) * source.width + sx]);
                        }
                    }
                    next.texels[static_cast<std::size_t>(y) * next.width + x] = farthest;
                }
            }

            levels.push_back(std::move(next));
        }
    }

    float DepthPyramid::at(uint32_t level, uint32_t x, uint32_t y) const {
        EGE_ASSERT(level < levels.size(), "depth pyramid level out of range");
        const Level& chosen = levels[level];
        const uint32_t clampedX = std::min(x, chosen.width - 1u);
        const uint32_t clampedY = std::min(y, chosen.height - 1u);
        return chosen.texels[static_cast<std::size_t>(clampedY) * chosen.width + clampedX];
    }

    float DepthPyramid::maxDepth(glm::vec2 minTexel, glm::vec2 maxTexel) const {
        if (levels.empty()) {
            // No pyramid is the same answer as "everything is at the far
            // plane": nothing can be shown to be hidden.
            return 1.f;
        }

        const Level& base = levels[0];
        const float lowX = std::max(minTexel.x, 0.f);
        const float lowY = std::max(minTexel.y, 0.f);
        const float highX = std::min(maxTexel.x, static_cast<float>(base.width) - 1.f);
        const float highY = std::min(maxTexel.y, static_cast<float>(base.height) - 1.f);
        if (highX < lowX || highY < lowY) {
            return 1.f;  // entirely off the edge of what was captured
        }

        // The finest level at which the rectangle still fits inside the tap
        // budget. Finest, not coarsest: a coarse texel answers for more screen
        // than was asked about, and every extra bit of screen it takes in can
        // only push the answer further away and prevent a cull. The budget is
        // what keeps this bounded however much of the screen the box covers.
        const float span = std::max(highX - lowX, highY - lowY);
        const float perLevel = static_cast<float>(occlusionMaxTapsPerAxis) - 1.f;
        uint32_t level = 0;
        if (span > perLevel) {
            level = static_cast<uint32_t>(std::ceil(std::log2(span / perLevel)));
        }
        level = std::min(level, static_cast<uint32_t>(levels.size()) - 1u);

        const float texelScale = 1.f / static_cast<float>(1u << level);
        const uint32_t beginX = static_cast<uint32_t>(std::floor(lowX * texelScale));
        const uint32_t beginY = static_cast<uint32_t>(std::floor(lowY * texelScale));
        const uint32_t endX = static_cast<uint32_t>(std::floor(highX * texelScale));
        const uint32_t endY = static_cast<uint32_t>(std::floor(highY * texelScale));

        float farthest = 0.f;
        for (uint32_t y = beginY; y <= endY; y++) {
            for (uint32_t x = beginX; x <= endX; x++) {
                farthest = std::max(farthest, at(level, x, y));
            }
        }
        return farthest;
    }

    ScreenBounds projectBounds(const glm::mat4& viewProjection, const Aabb& box) {
        ScreenBounds bounds{};
        if (!box.valid()) {
            return bounds;
        }

        glm::vec2 ndcMin{std::numeric_limits<float>::max()};
        glm::vec2 ndcMax{std::numeric_limits<float>::lowest()};
        float nearest = std::numeric_limits<float>::max();

        for (int corner = 0; corner < 8; corner++) {
            const glm::vec3 point{
                (corner & 1) != 0 ? box.max.x : box.min.x,
                (corner & 2) != 0 ? box.max.y : box.min.y,
                (corner & 4) != 0 ? box.max.z : box.min.z};

            const glm::vec4 clip = viewProjection * glm::vec4{point, 1.f};

            // A corner at or behind the eye has no place on screen, and a box
            // with one is a box the camera is inside or clipping through.
            // Projecting it anyway produces a rectangle in the wrong half of
            // the frame, which is how an object standing right in front of the
            // camera ends up culled against the wall behind it.
            if (clip.w <= 1e-5f) {
                return bounds;
            }

            const glm::vec3 ndc = glm::vec3{clip} / clip.w;
            ndcMin = glm::min(ndcMin, glm::vec2{ndc});
            ndcMax = glm::max(ndcMax, glm::vec2{ndc});
            nearest = std::min(nearest, ndc.z);
        }

        // x and y from [-1, 1] to [0, 1]. Vulkan's y already points down the
        // screen, so this is a rescale and not a flip - the same mapping the
        // occlusion pass uses, and for the same reason.
        bounds.min = glm::clamp(ndcMin * 0.5f + 0.5f, glm::vec2{0.f}, glm::vec2{1.f});
        bounds.max = glm::clamp(ndcMax * 0.5f + 0.5f, glm::vec2{0.f}, glm::vec2{1.f});
        // Depth is already in [0, 1] under Vulkan's convention, so unlike x and
        // y it is used as it comes.
        bounds.nearestDepth = std::clamp(nearest, 0.f, 1.f);
        bounds.testable = true;
        return bounds;
    }

    bool occludedByPyramid(const DepthPyramid& pyramid, const ScreenBounds& bounds) {
        if (!bounds.testable || pyramid.empty()) {
            return false;
        }

        // The rectangle in level-0 texels. The pyramid covers the frame, so a
        // fraction of the frame is a fraction of the pyramid.
        const glm::vec2 size{
            static_cast<float>(pyramid.width(0)), static_cast<float>(pyramid.height(0))};
        const glm::vec2 minTexel = bounds.min * size;
        const glm::vec2 maxTexel = bounds.max * size;

        const float farthestDrawn = pyramid.maxDepth(minTexel, maxTexel);

        // Everything already drawn across the rectangle is nearer than the
        // box's own nearest point, so every pixel the box could reach is
        // already covered by something in front of it.
        //
        // The comparison is strict, so a box lying exactly on the recorded
        // depth - which is what an object being tested against a pyramid its
        // own depth went into looks like - is not culled.
        return farthestDrawn < bounds.nearestDepth;
    }

    bool OcclusionSnapshot::hides(const Aabb& box) const {
        if (!valid || pyramid.empty() || !box.valid()) {
            return false;
        }

        // Grown a little before it is tested. The pyramid is a couple of frames
        // old, and a box that only just fitted behind an occluder then may be
        // peering round it now; a margin costs a few draws that would have been
        // skipped and buys back the objects that would otherwise blink.
        const glm::vec3 margin = box.extents() * occlusionBoundsMargin;
        Aabb grown{};
        grown.min = box.min - margin;
        grown.max = box.max + margin;

        return occludedByPyramid(pyramid, projectBounds(viewProjection, grown));
    }

}  // namespace ege
