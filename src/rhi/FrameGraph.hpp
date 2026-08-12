#pragma once

#include "rhi/Device.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ege {

    // The frame graph: render passes declare what they read and write, and the
    // graph derives everything that used to be hand-written per pass - barriers,
    // image layout transitions, load ops, transient image allocation and the
    // vkCmdBeginRendering call itself.
    //
    // The frame is rebuilt every frame: declaration is cheap, and it means a
    // pass can come and go (a debug view, a disabled effect) without any
    // persistent state to migrate. Physical GPU images persist across frames in
    // a cache keyed by description, so steady-state execution allocates nothing.
    //
    // Split deliberately into a device-free front half (declare, compile) and a
    // device-using back half (execute). Everything the graph decides - pass
    // culling, execution order, barriers, load ops - is decided by compile() and
    // is inspectable, which is what makes the scheduling testable on a machine
    // with no GPU at all.

    // Handle to a declared resource. Only meaningful within the frame that
    // declared it.
    struct FrameGraphResource {
        static constexpr uint32_t invalidIndex = UINT32_MAX;
        uint32_t index = invalidIndex;

        bool valid() const { return index != invalidIndex; }
    };

    // How a pass touches a resource. The graph maps these to layouts, stages
    // and access masks; passes never spell Vulkan synchronization themselves.
    enum class ResourceAccess {
        colorWrite,  // rendered to as a color attachment
        depthWrite,  // rendered to as the depth attachment
        sampled,     // read in a fragment shader through a sampler
    };

    // A GPU image that lives for (at most) one frame. Extent {0,0} means
    // "match the frame's output extent", which is what almost every full-screen
    // target wants and what keeps resize handling out of the passes.
    struct TransientImageDesc {
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkExtent2D extent{0, 0};
        VkClearValue clearValue{};
    };

    class FrameGraph;

    // Resolved physical resources, handed to a pass's execute callback.
    class FrameGraphResources {
    public:
        VkImageView view(FrameGraphResource resource) const;
        VkExtent2D extent(FrameGraphResource resource) const;

    private:
        friend class FrameGraph;

        explicit FrameGraphResources(const FrameGraph& graphRef) : graph{graphRef} {}

        const FrameGraph& graph;
    };

    class FrameGraph {
    public:
        using ExecuteCallback = std::function<void(VkCommandBuffer, const FrameGraphResources&)>;

        // ---- declaration ---------------------------------------------------

        // Starts a new frame. outputExtent is what {0,0} transient extents
        // resolve to - in practice the swapchain extent.
        void beginFrame(VkExtent2D outputExtent);

        FrameGraphResource createTransient(std::string name, const TransientImageDesc& desc);

        // Brings an externally owned image (the swapchain image) into the
        // graph. Its content on entry is discardable; after the last pass that
        // touches it, the graph transitions it to finalLayout. srcStage is
        // what the first barrier must chain after - for a swapchain image, the
        // stage the acquire semaphore is waited at.
        FrameGraphResource importImage(
            std::string name,
            VkImage image,
            VkImageView view,
            VkFormat format,
            VkExtent2D extent,
            VkClearValue clearValue,
            VkPipelineStageFlags2 srcStage,
            VkImageLayout finalLayout);

        class PassBuilder {
        public:
            void read(FrameGraphResource resource, ResourceAccess access);
            void write(FrameGraphResource resource, ResourceAccess access);

        private:
            friend class FrameGraph;

            PassBuilder(FrameGraph& graphRef, uint32_t passIndexRef)
                : graph{graphRef}, passIndex{passIndexRef} {}

            FrameGraph& graph;
            uint32_t passIndex;
        };

        void addPass(
            std::string name,
            const std::function<void(PassBuilder&)>& setup,
            ExecuteCallback execute);

        // ---- compilation (pure logic, no device) ---------------------------

        // A single planned layout transition / memory dependency.
        struct PlannedBarrier {
            uint32_t resourceIndex = 0;
            VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_NONE;
            VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
            VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_NONE;
            VkAccessFlags2 dstAccess = VK_ACCESS_2_NONE;
            // First touch of a transient this frame. Execution replaces the
            // source scope with what actually happened to the reused physical
            // image at the end of the previous frame.
            bool firstUse = false;
        };

        struct PlannedAttachment {
            uint32_t resourceIndex = 0;
            VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            // STORE only when something will read the result - a later pass,
            // or whoever the image was imported from. A depth buffer nobody
            // samples is discarded at the end of its pass.
            VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkClearValue clearValue{};
        };

        struct CompiledPass {
            uint32_t passIndex = 0;
            std::vector<PlannedBarrier> barriers;
            std::vector<PlannedAttachment> colorAttachments;
            std::optional<PlannedAttachment> depthAttachment;
        };

        // Culls passes that contribute nothing to an imported output, orders
        // the survivors, and plans every barrier and load op. Throws
        // std::logic_error on a malformed graph - reading a resource no pass
        // has written, or a graph with no imported output.
        void compile();

        const std::vector<CompiledPass>& compiledPasses() const { return compiled; }

        // Barriers that run after the last pass: imported images moving to
        // their declared final layout.
        const std::vector<PlannedBarrier>& finalBarriers() const { return finishingBarriers; }

        const std::string& passName(uint32_t passIndex) const { return passes[passIndex].name; }

        // ---- execution -----------------------------------------------------

        // Records the compiled frame: allocates or reuses physical images,
        // emits the planned barriers, wraps raster passes in
        // vkCmdBeginRendering / vkCmdEndRendering (with viewport and scissor
        // set to the render area) and invokes each pass callback.
        void execute(Device& deviceRef, VkCommandBuffer commandBuffer);

        ~FrameGraph();

        FrameGraph() = default;
        FrameGraph(const FrameGraph&) = delete;
        FrameGraph& operator=(const FrameGraph&) = delete;

    private:
        friend class FrameGraphResources;

        struct ResourceState {
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
            VkAccessFlags2 access = VK_ACCESS_2_NONE;
        };

        struct Resource {
            std::string name;
            TransientImageDesc desc;
            bool imported = false;
            bool written = false;

            // Imported-only fields.
            VkImage importedImage = VK_NULL_HANDLE;
            VkImageView importedView = VK_NULL_HANDLE;
            VkPipelineStageFlags2 importSrcStage = VK_PIPELINE_STAGE_2_NONE;
            VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            // Filled during compile / execute.
            ResourceState state{};
            VkImageUsageFlags usage = 0;
            uint32_t physicalIndex = UINT32_MAX;
        };

        struct PassAccess {
            FrameGraphResource resource;
            ResourceAccess access;
            bool isWrite = false;
        };

        struct Pass {
            std::string name;
            std::vector<PassAccess> accesses;
            ExecuteCallback execute;
        };

        // A cached GPU image, reused frame to frame as long as some transient
        // keeps asking for its description.
        struct PhysicalImage {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkExtent2D extent{0, 0};
            VkImageUsageFlags usage = 0;
            uint64_t lastFrameUsed = 0;
            bool usedThisFrame = false;
            // What the previous frame left the image doing; the source scope
            // for this frame's first-use barrier.
            VkPipelineStageFlags2 lastStage = VK_PIPELINE_STAGE_2_NONE;
            VkAccessFlags2 lastWriteAccess = VK_ACCESS_2_NONE;
        };

        VkExtent2D resolveExtent(const Resource& resource) const;
        uint32_t acquirePhysicalImage(Device& deviceRef, Resource& resource);
        void destroyPhysicalImage(PhysicalImage& physical);

        std::vector<Resource> resources;
        std::vector<Pass> passes;
        std::vector<CompiledPass> compiled;
        std::vector<PlannedBarrier> finishingBarriers;
        std::vector<bool> passLive;

        std::vector<PhysicalImage> physicalImages;
        VkExtent2D outputExtent{0, 0};
        uint64_t frameCounter = 0;
        bool compiledThisFrame = false;

        Device* device = nullptr;
    };

}  // namespace ege
