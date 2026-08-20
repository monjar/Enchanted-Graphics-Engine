#include "render/GpuCulling.hpp"

#include <algorithm>
#include <cmath>

namespace ege {

    glm::vec4 boundingSphere(const Aabb& box) {
        const glm::vec3 centre = (box.min + box.max) * 0.5f;
        const float radius = glm::length(box.max - centre);
        return glm::vec4{centre, radius};
    }

    SphereScreenBounds projectSphere(
        const glm::mat4& view, const glm::mat4& projection, glm::vec4 sphere, float nearPlane) {
        SphereScreenBounds bounds{};

        const glm::vec3 centre = glm::vec3{view * glm::vec4{glm::vec3{sphere}, 1.f}};
        const float radius = sphere.w;

        // View space looks down +Z here. A sphere whose nearest point reaches
        // the near plane cannot be projected honestly - part of it has no
        // place on screen at all - so it is not tested. The margin of the
        // comparison is the plane itself: at exactly the plane the depth
        // below would be exactly zero, which no stored depth is nearer than.
        if (centre.z - radius <= nearPlane) {
            return bounds;
        }

        // Depth under this projection is a function of view-space z alone:
        // ndcDepth(z) = P[2][2] + P[3][2] / z. That is what makes the
        // sphere's nearest depth one formula rather than a corner search.
        const float depthScale = projection[2][2];
        const float depthOffset = projection[3][2];
        bounds.nearestDepth = std::clamp(depthScale + depthOffset / (centre.z - radius), 0.f, 1.f);

        // The rectangle: project the eight corners of the sphere's view-space
        // bounding box. Every corner has z >= centre.z - radius > near, so
        // each projects somewhere meaningful. The box contains the sphere, so
        // its rectangle contains the sphere's - and a rectangle too large
        // only ever prevents a cull.
        glm::vec2 minNdc{std::numeric_limits<float>::max()};
        glm::vec2 maxNdc{std::numeric_limits<float>::lowest()};
        for (int corner = 0; corner < 8; corner++) {
            const glm::vec3 at{
                centre.x + (((corner & 1) != 0) ? radius : -radius),
                centre.y + (((corner & 2) != 0) ? radius : -radius),
                centre.z + (((corner & 4) != 0) ? radius : -radius)};
            const glm::vec4 clip = projection * glm::vec4{at, 1.f};
            const glm::vec2 ndc = glm::vec2{clip} / clip.w;
            minNdc = glm::min(minNdc, ndc);
            maxNdc = glm::max(maxNdc, ndc);
        }

        // NDC to the pyramid's texel space: y already points down in Vulkan
        // NDC, so this is a scale and shift and nothing flips.
        bounds.minUv = glm::clamp(minNdc * 0.5f + 0.5f, 0.f, 1.f);
        bounds.maxUv = glm::clamp(maxNdc * 0.5f + 0.5f, 0.f, 1.f);
        bounds.testable = true;
        return bounds;
    }

    int chooseLevel(
        glm::vec2 minUv,
        glm::vec2 maxUv,
        const glm::uvec2* levelExtents,
        uint32_t levelCount,
        uint32_t maxSpanTexels) {
        for (uint32_t level = 0; level < levelCount; level++) {
            const glm::vec2 span{
                (maxUv.x - minUv.x) * static_cast<float>(levelExtents[level].x),
                (maxUv.y - minUv.y) * static_cast<float>(levelExtents[level].y)};
            if (span.x <= static_cast<float>(maxSpanTexels) &&
                span.y <= static_cast<float>(maxSpanTexels)) {
                return static_cast<int>(level);
            }
        }
        return -1;
    }

    bool occludedAtLevel(
        const float* texels,
        glm::uvec2 extent,
        glm::vec2 minUv,
        glm::vec2 maxUv,
        float nearestDepth) {
        // The texels the rectangle touches. A texel covers [i/w, (i+1)/w), so
        // the last touched column is floor(maxU * w) - clamped, because a
        // rectangle reaching exactly 1.0 would otherwise name a column past
        // the edge.
        const auto lastIndex = [](float uv, uint32_t size) {
            const auto index = static_cast<uint32_t>(std::floor(uv * static_cast<float>(size)));
            return std::min(index, size - 1);
        };
        const auto firstIndex = [&lastIndex](float uv, uint32_t size) {
            return std::min(
                static_cast<uint32_t>(std::floor(uv * static_cast<float>(size))),
                lastIndex(uv, size));
        };

        const uint32_t x0 = firstIndex(minUv.x, extent.x);
        const uint32_t x1 = lastIndex(maxUv.x, extent.x);
        const uint32_t y0 = firstIndex(minUv.y, extent.y);
        const uint32_t y1 = lastIndex(maxUv.y, extent.y);

        float farthest = 0.f;
        for (uint32_t y = y0; y <= y1; y++) {
            for (uint32_t x = x0; x <= x1; x++) {
                farthest = std::max(farthest, texels[y * extent.x + x]);
            }
        }

        // Occluded only if everything already drawn across the whole
        // rectangle is strictly nearer than the sphere's own nearest point.
        // The margin keeps an object from being culled against its own depth
        // by floating-point luck; any real occluder clears it easily.
        return farthest < nearestDepth - gpuCullDepthMargin;
    }

    std::vector<uint32_t> seedDrawCommands(
        const std::vector<SeedBatch>& batches, uint32_t maxInstances) {
        const std::size_t batchCount = batches.size();
        std::vector<uint32_t> words(batchCount * 2 * drawCommandWords, 0);

        for (std::size_t b = 0; b < batchCount; b++) {
            const SeedBatch& batch = batches[b];
            // The early command draws into the batch's own window; the late
            // one into the same window shifted a whole buffer along, so
            // neither pass needs the other's count to know where to write.
            const std::size_t early = b * drawCommandWords;
            const std::size_t late = (batchCount + b) * drawCommandWords;

            if (batch.indexed) {
                // VkDrawIndexedIndirectCommand:
                //   indexCount, instanceCount, firstIndex, vertexOffset,
                //   firstInstance
                words[early + 0] = batch.drawCount;
                words[early + 4] = batch.firstInstance;
                words[late + 0] = batch.drawCount;
                words[late + 4] = maxInstances + batch.firstInstance;
            } else {
                // VkDrawIndirectCommand:
                //   vertexCount, instanceCount, firstVertex, firstInstance
                words[early + 0] = batch.drawCount;
                words[early + 3] = batch.firstInstance;
                words[late + 0] = batch.drawCount;
                words[late + 3] = maxInstances + batch.firstInstance;
            }
        }
        return words;
    }

}  // namespace ege
