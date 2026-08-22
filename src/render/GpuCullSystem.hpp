#pragma once

#include "render/GpuCulling.hpp"
#include "rhi/Buffer.hpp"
#include "rhi/Descriptors.hpp"
#include "rhi/Pipeline.hpp"

#include <array>
#include <memory>
#include <vector>

namespace ege {

    // Owns the two culling dispatches and every buffer they touch.
    //
    // The frame this serves is two-phase: the early pass compacts what was
    // visible last frame into the first depth pass's draws, the pyramid is
    // built from that partial depth, and the late pass rules on everything
    // against it - appending the newly visible to a second set of draws and
    // leaving its verdicts for next frame's early pass. The scene pass draws
    // both sets. Nothing waits on a copy coming back, and nothing pops in:
    // anything wrongly left out of the early set is caught by the late one
    // against real depth in the same frame.
    //
    // Everything here is per frame in flight except the visibility buffer,
    // which is the one deliberate exception: it is the channel between one
    // frame's late pass and the next frame's early pass, and the queue's
    // ordering is what keeps the handoff safe.
    class GpuCullSystem {
    public:
        GpuCullSystem(Device& device, uint32_t framesInFlight);
        ~GpuCullSystem();

        GpuCullSystem(const GpuCullSystem&) = delete;
        GpuCullSystem& operator=(const GpuCullSystem&) = delete;

        // Writes everything the CPU knows for this frame: the candidates'
        // spheres, the seeded draw commands, and the uniform block the
        // shaders read - in the exact layout the shaders will read it, which
        // is why the parameter is the wire struct rather than something
        // friendlier. Also collects what the GPU counted the last time this
        // frame index ran; the fence that recycled the index is what makes
        // that read safe.
        void feed(
            uint32_t frameIndex,
            const std::vector<GpuCullInput>& candidates,
            const std::vector<SeedBatch>& batches,
            const glm::mat4& view,
            const glm::mat4& projection,
            bool occlusionEnabled);

        // The early dispatch: compact last frame's visible set into the first
        // depth pass's draws. `candidates` is the buffer prepare() wrote the
        // frame's instances into, in draw-list order.
        void recordEarly(
            VkCommandBuffer commandBuffer,
            uint32_t frameIndex,
            const VkDescriptorBufferInfo& candidates);

        // The late dispatch: rule on everything against the pyramid, append
        // the newly visible, store the verdicts. The four views are the
        // pyramid's coarsest levels, finest first, with their extents in the
        // order the uniform block carries them.
        void recordLate(
            VkCommandBuffer commandBuffer,
            uint32_t frameIndex,
            const VkDescriptorBufferInfo& candidates,
            const std::array<VkImageView, gpuCullBoundLevels>& levels);

        // What the late pass counted, two frames ago - the most recent run
        // whose fence has been waited on. The panel labels it as belonging to
        // a previous frame, because it does.
        const GpuCullStatsData& lastStats() const { return collected; }

        // The buffers the rest of the frame reads, for descriptor writes and
        // frame-graph imports. The compacted instances and the commands are
        // per frame in flight; visibility is the single cross-frame channel.
        Buffer& instances(uint32_t frameIndex) { return *compacted[frameIndex]; }

        Buffer& commands(uint32_t frameIndex) { return *drawCommands[frameIndex]; }

        Buffer& visibilityBuffer() { return *visibility; }

        Buffer& statsBuffer(uint32_t frameIndex) { return *stats[frameIndex]; }

        uint32_t candidateCount(uint32_t frameIndex) const { return counts[frameIndex]; }

        uint32_t batchCount(uint32_t frameIndex) const { return batchCounts[frameIndex]; }

        // The level extents feed() stored, for callers that need to know
        // which reduce-chain outputs to hand recordLate().
        void setLevelExtents(const std::array<glm::uvec2, gpuCullBoundLevels>& extents);

    private:
        void createBuffers(uint32_t framesInFlight);
        void createDescriptors(uint32_t framesInFlight);
        void createPipelines();

        // The std140 block both shaders read. Mirrors CullUbo in
        // shaders/gpu_cull_common.glsl.
        struct CullUniform {
            glm::mat4 view{1.f};
            glm::mat4 projection{1.f};
            glm::vec4 depthTerms{0.f};
            glm::uvec4 counts{0};
            std::array<glm::ivec4, gpuCullBoundLevels> levelExtents{};
        };

        Device& device;

        // Per frame in flight, written by the CPU.
        std::vector<std::unique_ptr<Buffer>> uniforms;
        std::vector<std::unique_ptr<Buffer>> cullInputs;
        std::vector<std::unique_ptr<Buffer>> drawCommands;
        std::vector<std::unique_ptr<Buffer>> stats;
        // Per frame in flight, written only by the GPU.
        std::vector<std::unique_ptr<Buffer>> compacted;
        // The one cross-frame buffer: verdicts out of the late pass, into the
        // next early pass. Cleared to "nothing was visible" once at creation,
        // which makes the very first frame draw everything through the late
        // pass - correct, and gone by the second frame.
        std::unique_ptr<Buffer> visibility;

        std::vector<uint32_t> counts;
        std::vector<uint32_t> batchCounts;
        std::array<glm::uvec2, gpuCullBoundLevels> boundExtents{};
        GpuCullStatsData collected{};

        VkSampler levelSampler = VK_NULL_HANDLE;

        std::unique_ptr<DescriptorSetLayout> earlyLayout;
        std::unique_ptr<DescriptorSetLayout> lateLayout;
        std::unique_ptr<DescriptorPool> pool;
        std::vector<VkDescriptorSet> earlySets;
        std::vector<VkDescriptorSet> lateSets;

        VkPipelineLayout earlyPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout latePipelineLayout = VK_NULL_HANDLE;
        std::unique_ptr<ComputePipeline> earlyPipeline;
        std::unique_ptr<ComputePipeline> latePipeline;
    };

}  // namespace ege
