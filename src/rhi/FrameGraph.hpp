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
    //
    // Each value names a stage as well as an operation, because that is what
    // the graph needs to build a dependency and there is no case yet where the
    // same operation happens at two stages. A read at a new stage is a new
    // value here rather than an argument at the call site.
    enum class ResourceAccess {
        colorWrite,    // rendered to as a color attachment
        depthWrite,    // rendered to as the depth attachment
        sampled,       // read in a fragment shader through a sampler
        storageWrite,  // written by a compute shader as a storage buffer
        storageRead,   // read in a fragment shader as a storage buffer
    };

    // A GPU image that lives for (at most) one frame. Extent {0,0} means
    // "match the frame's output extent", which is what almost every full-screen
    // target wants and what keeps resize handling out of the passes.
    struct TransientImageDesc {
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkExtent2D extent{0, 0};
        VkClearValue clearValue{};
        // More than one makes the image a 2D array: sampled through a single
        // array view, rendered into one layer at a time by passes that say
        // which layer they write. Shadow cascades were the first caller;
        // point-light cube shadows are the second.
        uint32_t layers = 1;
        // More than one sample makes the image multisampled: the rasterizer
        // takes several coverage samples per pixel and a resolve averages
        // them, which is what stops geometry edges from staircasing. Nothing
        // samples a multisampled image directly - a pass that renders into
        // one declares a resolve into a single-sample image, and that is what
        // later passes read.
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        // Sample the layers as cube faces rather than as an array. The layers
        // are still rendered one at a time through ordinary 2D views - what
        // changes is only how a shader reads the result: by direction rather
        // than by index, with the hardware choosing the face and filtering
        // across the seams between them. Requires a multiple of six layers,
        // and more than six makes it a cube array.
        bool cube = false;
    };

    // A GPU buffer that lives for (at most) one frame, in device-local memory
    // and never mapped: it is written and read by shaders, not by the host.
    // Light culling is the first caller - a compute pass fills the per-cluster
    // light lists and the scene pass reads them - and anything else that hands
    // shader-computed data to a later pass is the same shape.
    struct TransientBufferDesc {
        VkDeviceSize size = 0;
    };

    class FrameGraph;

    // Resolved physical resources, handed to a pass's execute callback.
    class FrameGraphResources {
    public:
        VkImageView view(FrameGraphResource resource) const;
        VkExtent2D extent(FrameGraphResource resource) const;
        VkBuffer buffer(FrameGraphResource resource) const;
        VkDeviceSize bufferSize(FrameGraphResource resource) const;

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

        FrameGraphResource createTransientBuffer(std::string name, const TransientBufferDesc& desc);

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

            // `layer` selects which layer of an array image this pass renders
            // into; it is ignored for single-layer images. Layout is tracked
            // for the whole image rather than per layer, which is exactly
            // right while the layers of an array are written by consecutive
            // passes and then sampled together - the case cascades are.
            void write(FrameGraphResource resource, ResourceAccess access, uint32_t layer = 0);

            // Averages a multisampled attachment into a single-sample image
            // as the pass ends. This happens while the attachment is being
            // stored rather than as a second pass over the image, which is
            // why multisampling costs so much less than rendering large and
            // scaling down.
            //
            // `multisampled` must also be declared as a write by this pass;
            // `target` is what everything downstream reads, and the graph
            // treats it as written here.
            void resolve(FrameGraphResource multisampled, FrameGraphResource target);

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
            // Where a multisampled attachment is averaged to as it is stored.
            // invalidIndex when the attachment is single-sampled, which is
            // every attachment in a frame with multisampling switched off.
            uint32_t resolveResourceIndex = FrameGraphResource::invalidIndex;
            // Which layer of an array image is attached. Always 0 for the
            // ordinary single-layer case.
            uint32_t layer = 0;
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

        enum class ResourceKind { image, buffer };

        struct Resource {
            std::string name;
            ResourceKind kind = ResourceKind::image;
            TransientImageDesc desc;
            TransientBufferDesc bufferDesc;
            bool imported = false;
            // Whether any pass has touched it yet this frame. A buffer has no
            // layout, so this is the only signal that a barrier is owed to
            // whatever the recycled physical buffer was doing last frame.
            bool firstTouch = true;
            // Any layer written yet, which is what makes a read legal.
            bool written = false;
            // Written per layer, which is what decides clear versus load: the
            // second cascade's layer has never been touched this frame even
            // though the image it lives in has.
            std::vector<bool> writtenLayers;

            // Imported-only fields.
            VkImage importedImage = VK_NULL_HANDLE;
            VkImageView importedView = VK_NULL_HANDLE;
            VkPipelineStageFlags2 importSrcStage = VK_PIPELINE_STAGE_2_NONE;
            VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            // Filled during compile / execute.
            ResourceState state{};
            VkImageUsageFlags usage = 0;
            VkBufferUsageFlags bufferUsage = 0;
            uint32_t physicalIndex = UINT32_MAX;
        };

        struct PassAccess {
            FrameGraphResource resource;
            ResourceAccess access;
            bool isWrite = false;
            bool isResolve = false;
            uint32_t layer = 0;
            // For a resolve access, the multisampled attachment being
            // averaged into this one.
            FrameGraphResource resolveSource{};
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
            // Covers every layer: what a shader samples the image through.
            VkImageView view = VK_NULL_HANDLE;
            // One single-layer view per layer, for attaching. Empty for a
            // single-layer image, which attaches through `view`.
            std::vector<VkImageView> layerViews;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkExtent2D extent{0, 0};
            uint32_t layers = 1;
            bool cube = false;
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            VkImageUsageFlags usage = 0;
            uint64_t lastFrameUsed = 0;
            bool usedThisFrame = false;
            // What the previous frame left the image doing; the source scope
            // for this frame's first-use barrier.
            VkPipelineStageFlags2 lastStage = VK_PIPELINE_STAGE_2_NONE;
            VkAccessFlags2 lastWriteAccess = VK_ACCESS_2_NONE;
        };

        // A cached GPU buffer, reused frame to frame exactly as the images are.
        struct PhysicalBuffer {
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkDeviceSize size = 0;
            VkBufferUsageFlags usage = 0;
            uint64_t lastFrameUsed = 0;
            bool usedThisFrame = false;
            VkPipelineStageFlags2 lastStage = VK_PIPELINE_STAGE_2_NONE;
            VkAccessFlags2 lastWriteAccess = VK_ACCESS_2_NONE;
        };

        // Refuses a buffer access on an image and the reverse, at declaration
        // rather than at execution: getting it wrong otherwise surfaces as a
        // missing barrier, which is the kind of bug that works on one driver.
        void checkAccessKind(FrameGraphResource resource, ResourceAccess access) const;

        VkExtent2D resolveExtent(const Resource& resource) const;
        uint32_t acquirePhysicalImage(Device& deviceRef, Resource& resource);
        void destroyPhysicalImage(PhysicalImage& physical);
        uint32_t acquirePhysicalBuffer(Device& deviceRef, Resource& resource);
        void destroyPhysicalBuffer(PhysicalBuffer& physical);

        std::vector<Resource> resources;
        std::vector<Pass> passes;
        std::vector<CompiledPass> compiled;
        std::vector<PlannedBarrier> finishingBarriers;
        std::vector<bool> passLive;

        std::vector<PhysicalImage> physicalImages;
        std::vector<PhysicalBuffer> physicalBuffers;
        VkExtent2D outputExtent{0, 0};
        uint64_t frameCounter = 0;
        bool compiledThisFrame = false;

        Device* device = nullptr;
    };

}  // namespace ege
