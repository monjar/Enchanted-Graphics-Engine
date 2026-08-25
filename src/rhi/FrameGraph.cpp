#include "rhi/FrameGraph.hpp"

#include "core/Assert.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <stdexcept>

namespace ege {

    namespace {

        // Every write bit a graph resource can carry. Source access masks are
        // filtered to these: making writes available is what a memory
        // dependency is for, and read bits in a source mask are meaningless.
        // What counts as a write, and therefore as something a later access
        // has to be separated from. The host and memory bits are here for
        // imports: everything the graph itself does is one of the first five,
        // but a buffer handed in may have been filled by the CPU, and an
        // owner describing what it last did in the general MEMORY_WRITE terms
        // should not have that quietly ignored.
        constexpr VkAccessFlags2 allWrites =
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
            VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

        struct AccessInfo {
            VkImageLayout layout;
            VkPipelineStageFlags2 stage;
            VkAccessFlags2 access;
        };

        AccessInfo accessInfo(ResourceAccess access) {
            switch (access) {
                case ResourceAccess::colorWrite:
                    return {
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
                case ResourceAccess::depthWrite:
                    return {
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
                case ResourceAccess::sampled:
                    return {
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
                // The same layout as the fragment read above, which is what
                // makes a compute pass and a raster pass reading the same
                // image cost no transition between them.
                case ResourceAccess::computeSampled:
                    return {
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
                // Buffers have no layout, so these carry UNDEFINED and the
                // barrier logic sees "layout unchanged" for them throughout.
                case ResourceAccess::storageWrite:
                    return {
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                case ResourceAccess::storageRead:
                    return {
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                case ResourceAccess::computeStorageRead:
                    return {
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                // The vertex stage, because that is where a vertex shader
                // pulling its transforms out of a compute-compacted buffer
                // actually reads - a barrier naming the fragment stage would
                // let the vertex fetch race the compaction.
                case ResourceAccess::vertexRead:
                    return {
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                // Read by the command processor rather than by a shader, at a
                // stage of its own that runs before the vertex shader does -
                // which is exactly why it needs naming separately. A barrier
                // that made a compute-written draw count visible to the vertex
                // stage would come too late for the draw that reads it.
                case ResourceAccess::indirectRead:
                    return {
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
            }
            return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
        }

        VkImageUsageFlags usageFor(ResourceAccess access) {
            switch (access) {
                case ResourceAccess::colorWrite:
                    return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                case ResourceAccess::depthWrite:
                    return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                case ResourceAccess::sampled:
                case ResourceAccess::computeSampled:
                    return VK_IMAGE_USAGE_SAMPLED_BIT;
                case ResourceAccess::storageWrite:
                case ResourceAccess::storageRead:
                case ResourceAccess::computeStorageRead:
                case ResourceAccess::vertexRead:
                case ResourceAccess::indirectRead:
                    return 0;
            }
            return 0;
        }

        VkBufferUsageFlags bufferUsageFor(ResourceAccess access) {
            switch (access) {
                case ResourceAccess::storageWrite:
                case ResourceAccess::storageRead:
                case ResourceAccess::computeStorageRead:
                case ResourceAccess::vertexRead:
                    return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                case ResourceAccess::indirectRead:
                    return VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
                case ResourceAccess::colorWrite:
                case ResourceAccess::depthWrite:
                case ResourceAccess::sampled:
                case ResourceAccess::computeSampled:
                    return 0;
            }
            return 0;
        }

        bool isBufferAccess(ResourceAccess access) {
            switch (access) {
                case ResourceAccess::storageWrite:
                case ResourceAccess::storageRead:
                case ResourceAccess::computeStorageRead:
                case ResourceAccess::vertexRead:
                case ResourceAccess::indirectRead:
                    return true;
                case ResourceAccess::colorWrite:
                case ResourceAccess::depthWrite:
                case ResourceAccess::sampled:
                case ResourceAccess::computeSampled:
                    return false;
            }
            return false;
        }

        VkImageAspectFlags aspectFor(VkFormat format) {
            switch (format) {
                case VK_FORMAT_D16_UNORM:
                case VK_FORMAT_X8_D24_UNORM_PACK32:
                case VK_FORMAT_D32_SFLOAT:
                    return VK_IMAGE_ASPECT_DEPTH_BIT;
                case VK_FORMAT_D16_UNORM_S8_UINT:
                case VK_FORMAT_D24_UNORM_S8_UINT:
                case VK_FORMAT_D32_SFLOAT_S8_UINT:
                    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                default:
                    return VK_IMAGE_ASPECT_COLOR_BIT;
            }
        }

    }  // namespace

    // ---- FrameGraphResources ----------------------------------------------

    VkImageView FrameGraphResources::view(FrameGraphResource resource) const {
        const auto& res = graph.resources.at(resource.index);
        if (res.imported) {
            return res.importedView;
        }
        EGE_VERIFY(
            res.physicalIndex != UINT32_MAX, "resource '{}' has no physical image", res.name);
        return graph.physicalImages[res.physicalIndex].view;
    }

    VkExtent2D FrameGraphResources::extent(FrameGraphResource resource) const {
        return graph.resolveExtent(graph.resources.at(resource.index));
    }

    VkBuffer FrameGraphResources::buffer(FrameGraphResource resource) const {
        const auto& res = graph.resources.at(resource.index);
        EGE_VERIFY(
            res.kind == FrameGraph::ResourceKind::buffer,
            "resource '{}' is an image, not a buffer",
            res.name);
        if (res.imported) {
            return res.importedBuffer;
        }
        EGE_VERIFY(
            res.physicalIndex != UINT32_MAX, "resource '{}' has no physical buffer", res.name);
        return graph.physicalBuffers[res.physicalIndex].buffer;
    }

    VkDeviceSize FrameGraphResources::bufferSize(FrameGraphResource resource) const {
        const auto& res = graph.resources.at(resource.index);
        EGE_VERIFY(
            res.kind == FrameGraph::ResourceKind::buffer,
            "resource '{}' is an image, not a buffer",
            res.name);
        return res.bufferDesc.size;
    }

    // ---- declaration -------------------------------------------------------

    void FrameGraph::beginFrame(VkExtent2D outputExtentRef) {
        resources.clear();
        passes.clear();
        compiled.clear();
        finishingBarriers.clear();
        passLive.clear();
        outputExtent = outputExtentRef;
        compiledThisFrame = false;
        frameCounter++;
    }

    FrameGraphResource FrameGraph::createTransient(
        std::string name, const TransientImageDesc& desc) {
        if (desc.cube && (desc.layers == 0 || desc.layers % 6 != 0)) {
            throw std::logic_error{
                "frame graph cube image '" + name + "' needs a multiple of six layers"};
        }
        Resource resource{};
        resource.name = std::move(name);
        resource.desc = desc;
        resources.push_back(std::move(resource));
        return FrameGraphResource{static_cast<uint32_t>(resources.size() - 1)};
    }

    FrameGraphResource FrameGraph::createTransientBuffer(
        std::string name, const TransientBufferDesc& desc) {
        if (desc.size == 0) {
            throw std::logic_error{"frame graph transient buffer '" + name + "' has no size"};
        }
        Resource resource{};
        resource.name = std::move(name);
        resource.kind = ResourceKind::buffer;
        resource.bufferDesc = desc;
        resources.push_back(std::move(resource));
        return FrameGraphResource{static_cast<uint32_t>(resources.size() - 1)};
    }

    FrameGraphResource FrameGraph::importImage(std::string name, const ImportedImageDesc& desc) {
        Resource resource{};
        resource.name = std::move(name);
        resource.desc.format = desc.format;
        resource.desc.extent = desc.extent;
        resource.desc.clearValue = desc.clearValue;
        resource.imported = true;
        resource.importedImage = desc.image;
        resource.importedView = desc.view;
        resource.importSrcStage = desc.srcStage;
        resource.importSrcAccess = desc.srcAccess;
        resource.initialLayout = desc.initialLayout;
        resource.finalLayout = desc.finalLayout;
        resources.push_back(std::move(resource));
        return FrameGraphResource{static_cast<uint32_t>(resources.size() - 1)};
    }

    FrameGraphResource FrameGraph::importBuffer(std::string name, const ImportedBufferDesc& desc) {
        if (desc.buffer == VK_NULL_HANDLE) {
            throw std::logic_error{"frame graph imported buffer '" + name + "' has no buffer"};
        }
        if (desc.size == 0) {
            throw std::logic_error{"frame graph imported buffer '" + name + "' has no size"};
        }
        Resource resource{};
        resource.name = std::move(name);
        resource.kind = ResourceKind::buffer;
        resource.bufferDesc.size = desc.size;
        resource.imported = true;
        resource.importedBuffer = desc.buffer;
        resource.importSrcStage = desc.srcStage;
        resource.importSrcAccess = desc.srcAccess;
        resources.push_back(std::move(resource));
        return FrameGraphResource{static_cast<uint32_t>(resources.size() - 1)};
    }

    void FrameGraph::PassBuilder::read(FrameGraphResource resource, ResourceAccess access) {
        if (!resource.valid() || resource.index >= graph.resources.size()) {
            throw std::logic_error{"pass declared a read of an invalid resource handle"};
        }
        graph.checkAccessKind(resource, access);
        graph.passes[passIndex].accesses.push_back({resource, access, false});
    }

    void FrameGraph::PassBuilder::write(
        FrameGraphResource resource, ResourceAccess access, uint32_t layer) {
        if (!resource.valid() || resource.index >= graph.resources.size()) {
            throw std::logic_error{"pass declared a write of an invalid resource handle"};
        }
        graph.checkAccessKind(resource, access);
        if (graph.resources[resource.index].kind == ResourceKind::image &&
            layer >= graph.resources[resource.index].desc.layers) {
            throw std::logic_error{"pass declared a write of a layer the image does not have"};
        }
        graph.passes[passIndex].accesses.push_back({resource, access, true, false, layer, {}});
    }

    void FrameGraph::PassBuilder::resolve(
        FrameGraphResource multisampled, FrameGraphResource target) {
        if (!multisampled.valid() || multisampled.index >= graph.resources.size() ||
            !target.valid() || target.index >= graph.resources.size()) {
            throw std::logic_error{"pass declared a resolve of an invalid resource handle"};
        }
        const Resource& source = graph.resources[multisampled.index];
        const Resource& destination = graph.resources[target.index];
        if (source.kind != ResourceKind::image || destination.kind != ResourceKind::image) {
            throw std::logic_error{"only images can be resolved"};
        }
        if (source.desc.samples == VK_SAMPLE_COUNT_1_BIT) {
            throw std::logic_error{
                "pass declared a resolve of '" + source.name + "', which is not multisampled"};
        }
        if (destination.desc.samples != VK_SAMPLE_COUNT_1_BIT) {
            throw std::logic_error{
                "pass declared a resolve into '" + destination.name +
                "', which is itself multisampled"};
        }

        // Depth and colour resolve into their own kind of attachment, and
        // the format is what says which: a resolve target is written exactly
        // as the attachment it mirrors, so it wants the same layout, the same
        // usage and the same barriers as one.
        const bool sourceIsDepth = (aspectFor(source.desc.format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        const bool targetIsDepth =
            (aspectFor(destination.desc.format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        if (sourceIsDepth != targetIsDepth) {
            throw std::logic_error{
                "pass declared a resolve between '" + source.name + "' and '" + destination.name +
                "', which are not both depth or both colour"};
        }

        PassAccess access{};
        access.resource = target;
        access.access = sourceIsDepth ? ResourceAccess::depthWrite : ResourceAccess::colorWrite;
        access.isWrite = true;
        access.isResolve = true;
        access.resolveSource = multisampled;
        graph.passes[passIndex].accesses.push_back(access);
    }

    void FrameGraph::checkAccessKind(FrameGraphResource resource, ResourceAccess access) const {
        const Resource& res = resources[resource.index];
        const bool wantsBuffer = isBufferAccess(access);
        if (wantsBuffer != (res.kind == ResourceKind::buffer)) {
            throw std::logic_error{
                "pass declared a " + std::string{wantsBuffer ? "buffer" : "image"} +
                " access of '" + res.name + "', which is a " +
                (res.kind == ResourceKind::buffer ? "buffer" : "image")};
        }
    }

    void FrameGraph::addPass(
        std::string name, const std::function<void(PassBuilder&)>& setup, ExecuteCallback execute) {
        Pass pass{};
        pass.name = std::move(name);
        pass.execute = std::move(execute);
        passes.push_back(std::move(pass));

        PassBuilder builder{*this, static_cast<uint32_t>(passes.size() - 1)};
        setup(builder);
    }

    // ---- compilation -------------------------------------------------------

    void FrameGraph::compile() {
        const bool hasImport =
            std::any_of(resources.begin(), resources.end(), [](const Resource& resource) {
                return resource.imported;
            });
        if (!hasImport) {
            throw std::logic_error{
                "frame graph has no imported output; nothing it renders could ever be seen"};
        }

        // Liveness, walking backwards: a pass matters if it writes an imported
        // image, if it writes something a later live pass reads, or if a later
        // live pass writes the same thing again - because a second write to a
        // layer loads what the first left there rather than clearing it. One
        // reverse sweep suffices because writers always precede their readers
        // in execution order, and a later writer is seen before an earlier one.
        //
        // That last case is not hypothetical: a depth pre-pass produces depth
        // nothing samples, and the pass that consumes it consumes it by
        // testing against it. Without this the pre-pass is culled as
        // contributing nothing, and the scene it was meant to accelerate
        // renders against an empty depth buffer.
        passLive.assign(passes.size(), false);
        std::vector<bool> needed(resources.size(), false);

        // Per resource and layer, the last live pass that writes it. Doubles
        // as the store-op rule below: a writer that is not the last one owes
        // its result to whoever loads it next.
        constexpr uint32_t noWriter = UINT32_MAX;
        std::vector<std::vector<uint32_t>> lastLiveWriter(resources.size());
        for (size_t i = 0; i < resources.size(); i++) {
            lastLiveWriter[i].assign(std::max(resources[i].desc.layers, 1u), noWriter);
        }

        for (size_t i = passes.size(); i-- > 0;) {
            const Pass& pass = passes[i];
            bool live = false;
            for (const PassAccess& access : pass.accesses) {
                if (!access.isWrite) {
                    continue;
                }
                if (resources[access.resource.index].imported || needed[access.resource.index] ||
                    lastLiveWriter[access.resource.index][access.layer] != noWriter) {
                    live = true;
                }
            }
            passLive[i] = live;
            if (live) {
                for (const PassAccess& access : pass.accesses) {
                    if (!access.isWrite) {
                        needed[access.resource.index] = true;
                    } else if (lastLiveWriter[access.resource.index][access.layer] == noWriter) {
                        lastLiveWriter[access.resource.index][access.layer] =
                            static_cast<uint32_t>(i);
                    }
                }
            }
        }

        // Initial states and usage. Usage accumulates only over live passes,
        // so an image asked for solely by culled work is never allocated.
        for (Resource& resource : resources) {
            resource.state = ResourceState{};
            resource.usage = 0;
            resource.bufferUsage = 0;
            resource.written = false;
            resource.firstTouch = true;
            resource.physicalIndex = UINT32_MAX;
            // A preserved import enters already holding something worth
            // keeping, so a pass that writes it loads rather than clears -
            // which is what "written already" means to the load-op rule
            // below. A discardable import (the swapchain) does not.
            const bool preserved =
                resource.imported && resource.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED;
            resource.writtenLayers.assign(resource.desc.layers, preserved);
            if (resource.imported) {
                resource.state.layout = resource.initialLayout;
                resource.state.stage = resource.importSrcStage;
                resource.state.access = resource.importSrcAccess;
            }
        }

        compiled.clear();
        for (uint32_t passIndex = 0; passIndex < passes.size(); passIndex++) {
            if (!passLive[passIndex]) {
                EGE_TRACE("frame graph culled pass '{}'", passes[passIndex].name);
                continue;
            }

            CompiledPass compiledPass{};
            compiledPass.passIndex = passIndex;

            for (const PassAccess& passAccess : passes[passIndex].accesses) {
                Resource& resource = resources[passAccess.resource.index];
                const AccessInfo target = accessInfo(passAccess.access);

                if (!passAccess.isWrite && !resource.written && !resource.imported) {
                    throw std::logic_error{
                        "pass '" + passes[passIndex].name + "' reads '" + resource.name +
                        "', which no earlier pass has written"};
                }

                resource.usage |= usageFor(passAccess.access);
                resource.bufferUsage |= bufferUsageFor(passAccess.access);

                const bool isBuffer = resource.kind == ResourceKind::buffer;

                // A barrier is needed on any layout change, and on any hazard
                // involving a write. The only transition that may be elided is
                // read-after-read in an unchanged layout.
                const bool layoutChanges = !isBuffer && resource.state.layout != target.layout;
                const bool hazard =
                    (resource.state.access & allWrites) != 0 ||
                    ((target.access & allWrites) != 0 && resource.state.access != VK_ACCESS_2_NONE);
                // A buffer has no layout to change, so nothing above fires on
                // its first touch - and its first touch is exactly where the
                // cross-frame hazard lives: this frame's compute write against
                // last frame's fragment read of the same recycled buffer.
                const bool firstBufferUse = isBuffer && !resource.imported && resource.firstTouch;

                if (layoutChanges || hazard || firstBufferUse) {
                    PlannedBarrier barrier{};
                    barrier.resourceIndex = passAccess.resource.index;
                    barrier.oldLayout = resource.state.layout;
                    barrier.newLayout = target.layout;
                    barrier.srcStage = resource.state.stage;
                    barrier.srcAccess = resource.state.access & allWrites;
                    barrier.dstStage = target.stage;
                    barrier.dstAccess = target.access;
                    barrier.firstUse =
                        !resource.imported &&
                        (isBuffer ? resource.firstTouch
                                  : resource.state.layout == VK_IMAGE_LAYOUT_UNDEFINED);
                    compiledPass.barriers.push_back(barrier);
                }
                resource.firstTouch = false;

                if (passAccess.isWrite && isBuffer) {
                    resource.written = true;
                } else if (passAccess.isWrite && passAccess.isResolve) {
                    // The resolve target is not an attachment of its own; it
                    // is named by the multisampled attachment's own entry,
                    // matched up once every attachment exists.
                    resource.written = true;
                    resource.writtenLayers[passAccess.layer] = true;
                } else if (passAccess.isWrite) {
                    PlannedAttachment attachment{};
                    attachment.resourceIndex = passAccess.resource.index;
                    attachment.layer = passAccess.layer;
                    attachment.loadOp = resource.writtenLayers[passAccess.layer]
                                            ? VK_ATTACHMENT_LOAD_OP_LOAD
                                            : VK_ATTACHMENT_LOAD_OP_CLEAR;
                    // Stored when something can still observe it: the image's
                    // owner, a later read, or a later write that will load it
                    // rather than clear it.
                    const bool writtenAgainLater =
                        lastLiveWriter[passAccess.resource.index][passAccess.layer] != passIndex;
                    attachment.storeOp = (resource.imported || needed[passAccess.resource.index] ||
                                          writtenAgainLater)
                                             ? VK_ATTACHMENT_STORE_OP_STORE
                                             : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    attachment.clearValue = resource.desc.clearValue;

                    if (passAccess.access == ResourceAccess::colorWrite) {
                        compiledPass.colorAttachments.push_back(attachment);
                    } else if (passAccess.access == ResourceAccess::depthWrite) {
                        if (compiledPass.depthAttachment.has_value()) {
                            throw std::logic_error{
                                "pass '" + passes[passIndex].name +
                                "' declares two depth attachments"};
                        }
                        compiledPass.depthAttachment = attachment;
                    }
                    resource.written = true;
                    resource.writtenLayers[passAccess.layer] = true;
                }

                resource.state = {target.layout, target.stage, target.access};
            }

            // Now that every attachment exists, point each multisampled one
            // at where it resolves to. Done here rather than inline because
            // the write and the resolve may be declared in either order.
            for (const PassAccess& passAccess : passes[passIndex].accesses) {
                if (!passAccess.isResolve) {
                    continue;
                }
                bool matched = false;
                for (PlannedAttachment& attachment : compiledPass.colorAttachments) {
                    if (attachment.resourceIndex == passAccess.resolveSource.index) {
                        attachment.resolveResourceIndex = passAccess.resource.index;
                        matched = true;
                    }
                }
                if (compiledPass.depthAttachment.has_value() &&
                    compiledPass.depthAttachment->resourceIndex == passAccess.resolveSource.index) {
                    compiledPass.depthAttachment->resolveResourceIndex = passAccess.resource.index;
                    matched = true;
                }
                if (!matched) {
                    throw std::logic_error{
                        "pass '" + passes[passIndex].name + "' resolves '" +
                        resources[passAccess.resolveSource.index].name +
                        "', which it does not render to"};
                }
            }

            compiled.push_back(std::move(compiledPass));
        }

        // Imported images owe their owner a final layout - for a swapchain
        // image, PRESENT_SRC. Emitted even if every pass was culled, because
        // the image will be presented regardless.
        finishingBarriers.clear();
        for (uint32_t resourceIndex = 0; resourceIndex < resources.size(); resourceIndex++) {
            Resource& resource = resources[resourceIndex];
            if (!resource.imported) {
                continue;
            }

            // An imported buffer has no layout to restore, so what it is owed
            // instead is visibility: whatever the graph wrote into it has to
            // be readable by its owner, which is either a transfer recorded
            // after the graph or the host once the fence is waited on. Only
            // when the graph wrote it - a buffer the graph merely read needs
            // nothing said about it.
            if (resource.kind == ResourceKind::buffer) {
                if ((resource.state.access & allWrites) == 0) {
                    continue;
                }
                PlannedBarrier bufferBarrier{};
                bufferBarrier.resourceIndex = resourceIndex;
                bufferBarrier.srcStage = resource.state.stage;
                bufferBarrier.srcAccess = resource.state.access & allWrites;
                bufferBarrier.dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                bufferBarrier.dstAccess = VK_ACCESS_2_MEMORY_READ_BIT;
                finishingBarriers.push_back(bufferBarrier);
                continue;
            }

            if (resource.finalLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
                resource.state.layout == resource.finalLayout) {
                continue;
            }
            PlannedBarrier barrier{};
            barrier.resourceIndex = resourceIndex;
            barrier.oldLayout = resource.state.layout;
            barrier.newLayout = resource.finalLayout;
            barrier.srcStage = resource.state.stage;
            barrier.srcAccess = resource.state.access & allWrites;
            barrier.dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            // A layout transition is itself a write, and whatever the image's
            // owner does next has to be able to see it. For a swapchain image
            // that is the presentation engine; for an image handed over to be
            // copied out of, it is the copy recorded right after the graph
            // finishes, which is a transfer read in the same command buffer.
            // An empty destination access scope makes the transition available
            // to nothing at all.
            barrier.dstAccess = VK_ACCESS_2_MEMORY_READ_BIT;
            finishingBarriers.push_back(barrier);
        }

        compiledThisFrame = true;
    }

    // ---- execution ---------------------------------------------------------

    VkExtent2D FrameGraph::resolveExtent(const Resource& resource) const {
        if (resource.desc.extent.width == 0 || resource.desc.extent.height == 0) {
            return outputExtent;
        }
        return resource.desc.extent;
    }

    uint32_t FrameGraph::acquirePhysicalImage(Device& deviceRef, Resource& resource) {
        const VkExtent2D extent = resolveExtent(resource);

        for (uint32_t i = 0; i < physicalImages.size(); i++) {
            PhysicalImage& physical = physicalImages[i];
            if (!physical.usedThisFrame && physical.format == resource.desc.format &&
                physical.extent.width == extent.width && physical.extent.height == extent.height &&
                physical.layers == resource.desc.layers && physical.cube == resource.desc.cube &&
                physical.samples == resource.desc.samples && physical.usage == resource.usage) {
                physical.usedThisFrame = true;
                physical.lastFrameUsed = frameCounter;
                return i;
            }
        }

        PhysicalImage physical{};
        physical.format = resource.desc.format;
        physical.extent = extent;
        physical.layers = resource.desc.layers;
        physical.cube = resource.desc.cube;
        physical.samples = resource.desc.samples;
        physical.usage = resource.usage;
        physical.usedThisFrame = true;
        physical.lastFrameUsed = frameCounter;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = physical.layers;
        imageInfo.format = resource.desc.format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = resource.usage;
        imageInfo.samples = physical.samples;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // Declared at creation, not at view time: an image can only be viewed
        // as a cube if it was made willing to be.
        if (physical.cube) {
            imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }

        deviceRef.createImageWithInfo(
            imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, physical.image, physical.allocation);

        // The whole-image view, which is what a shader samples through: 2D for
        // one layer, 2D_ARRAY for several, and CUBE or CUBE_ARRAY when the
        // layers are meant to be read as directions instead of indices.
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = physical.image;
        if (physical.cube) {
            viewInfo.viewType =
                physical.layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        } else {
            viewInfo.viewType =
                physical.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        }
        viewInfo.format = resource.desc.format;
        viewInfo.subresourceRange = {aspectFor(resource.desc.format), 0, 1, 0, physical.layers};

        if (vkCreateImageView(deviceRef.device(), &viewInfo, nullptr, &physical.view) !=
            VK_SUCCESS) {
            deviceRef.destroyImage(physical.image, physical.allocation);
            throw std::runtime_error{"failed to create a frame graph image view"};
        }

        // One view per layer, because rendering targets a single layer at a
        // time. Only worth making when there is more than one.
        if (physical.layers > 1) {
            for (uint32_t layer = 0; layer < physical.layers; layer++) {
                VkImageViewCreateInfo layerViewInfo = viewInfo;
                layerViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                layerViewInfo.subresourceRange = {aspectFor(resource.desc.format), 0, 1, layer, 1};

                VkImageView layerView = VK_NULL_HANDLE;
                if (vkCreateImageView(deviceRef.device(), &layerViewInfo, nullptr, &layerView) !=
                    VK_SUCCESS) {
                    for (VkImageView made : physical.layerViews) {
                        vkDestroyImageView(deviceRef.device(), made, nullptr);
                    }
                    vkDestroyImageView(deviceRef.device(), physical.view, nullptr);
                    deviceRef.destroyImage(physical.image, physical.allocation);
                    throw std::runtime_error{"failed to create a frame graph layer view"};
                }
                physical.layerViews.push_back(layerView);
            }
        }

        EGE_DEBUG(
            "frame graph allocated {}x{}x{} transient for '{}'",
            extent.width,
            extent.height,
            physical.layers,
            resource.name);

        physicalImages.push_back(physical);
        return static_cast<uint32_t>(physicalImages.size() - 1);
    }

    uint32_t FrameGraph::acquirePhysicalBuffer(Device& deviceRef, Resource& resource) {
        for (uint32_t i = 0; i < physicalBuffers.size(); i++) {
            PhysicalBuffer& physical = physicalBuffers[i];
            if (!physical.usedThisFrame && physical.size == resource.bufferDesc.size &&
                physical.usage == resource.bufferUsage) {
                physical.usedThisFrame = true;
                physical.lastFrameUsed = frameCounter;
                return i;
            }
        }

        PhysicalBuffer physical{};
        physical.size = resource.bufferDesc.size;
        physical.usage = resource.bufferUsage;
        physical.usedThisFrame = true;
        physical.lastFrameUsed = frameCounter;

        // Device-local and never mapped: the host neither writes nor reads it,
        // so there is nothing to gain from making it visible.
        deviceRef.createBuffer(
            physical.size,
            physical.usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            physical.buffer,
            physical.allocation);

        EGE_DEBUG(
            "frame graph allocated a {}-byte transient buffer for '{}'",
            physical.size,
            resource.name);

        physicalBuffers.push_back(physical);
        return static_cast<uint32_t>(physicalBuffers.size() - 1);
    }

    void FrameGraph::destroyPhysicalBuffer(PhysicalBuffer& physical) {
        device->destroyBuffer(physical.buffer, physical.allocation);
        physical.buffer = VK_NULL_HANDLE;
        physical.allocation = VK_NULL_HANDLE;
    }

    void FrameGraph::destroyPhysicalImage(PhysicalImage& physical) {
        for (VkImageView layerView : physical.layerViews) {
            vkDestroyImageView(device->device(), layerView, nullptr);
        }
        physical.layerViews.clear();
        vkDestroyImageView(device->device(), physical.view, nullptr);
        device->destroyImage(physical.image, physical.allocation);
        physical.view = VK_NULL_HANDLE;
        physical.image = VK_NULL_HANDLE;
        physical.allocation = VK_NULL_HANDLE;
    }

    void FrameGraph::execute(Device& deviceRef, VkCommandBuffer commandBuffer) {
        EGE_VERIFY(compiledThisFrame, "FrameGraph::execute called without compile");
        device = &deviceRef;

        for (PhysicalImage& physical : physicalImages) {
            physical.usedThisFrame = false;
        }
        for (PhysicalBuffer& physical : physicalBuffers) {
            physical.usedThisFrame = false;
        }

        // Give every live transient a physical resource. Liveness is encoded
        // in usage: only accesses from surviving passes accumulated any.
        for (Resource& resource : resources) {
            if (resource.imported) {
                continue;
            }
            if (resource.kind == ResourceKind::buffer) {
                if (resource.bufferUsage != 0) {
                    resource.physicalIndex = acquirePhysicalBuffer(deviceRef, resource);
                }
            } else if (resource.usage != 0) {
                resource.physicalIndex = acquirePhysicalImage(deviceRef, resource);
            }
        }

        const FrameGraphResources resolvedResources{*this};

        auto recordBarriers = [&](const std::vector<PlannedBarrier>& planned) {
            if (planned.empty()) {
                return;
            }
            std::vector<VkImageMemoryBarrier2> barriers;
            std::vector<VkBufferMemoryBarrier2> bufferBarriers;
            barriers.reserve(planned.size());
            for (const PlannedBarrier& plan : planned) {
                const Resource& resource = resources[plan.resourceIndex];

                if (resource.kind == ResourceKind::buffer) {
                    VkBufferMemoryBarrier2 bufferBarrier{};
                    bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                    bufferBarrier.srcStageMask = plan.srcStage;
                    bufferBarrier.srcAccessMask = plan.srcAccess;
                    bufferBarrier.dstStageMask = plan.dstStage;
                    bufferBarrier.dstAccessMask = plan.dstAccess;
                    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bufferBarrier.buffer = resource.imported
                                               ? resource.importedBuffer
                                               : physicalBuffers[resource.physicalIndex].buffer;
                    bufferBarrier.offset = 0;
                    bufferBarrier.size = VK_WHOLE_SIZE;

                    // The same cross-frame chain the images make, for the same
                    // reason: this frame overwrites a buffer the last frame may
                    // still have been reading. Not for an imported one, whose
                    // owner said what came before it when it handed it over.
                    if (plan.firstUse && !resource.imported) {
                        const PhysicalBuffer& physical = physicalBuffers[resource.physicalIndex];
                        bufferBarrier.srcStageMask = physical.lastStage;
                        bufferBarrier.srcAccessMask = physical.lastWriteAccess;
                    }
                    // Nothing has ever touched it, so there is nothing to wait
                    // for; a barrier with no source stage is not legal.
                    if (bufferBarrier.srcStageMask == VK_PIPELINE_STAGE_2_NONE) {
                        continue;
                    }

                    bufferBarriers.push_back(bufferBarrier);
                    continue;
                }

                VkImageMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.srcStageMask = plan.srcStage;
                barrier.srcAccessMask = plan.srcAccess;
                barrier.dstStageMask = plan.dstStage;
                barrier.dstAccessMask = plan.dstAccess;
                barrier.oldLayout = plan.oldLayout;
                barrier.newLayout = plan.newLayout;
                barrier.image = resource.imported ? resource.importedImage
                                                  : physicalImages[resource.physicalIndex].image;
                // Every layer at once. Layout is tracked per image rather than
                // per layer, so a barrier that moved only one layer would
                // leave the tracked state a lie about the others.
                barrier.subresourceRange = {
                    aspectFor(resource.desc.format), 0, 1, 0, resource.desc.layers};

                // A transient's first use discards content from UNDEFINED, but
                // execution still has to wait for whatever the previous frame
                // was doing with the recycled physical image.
                if (plan.firstUse && !resource.imported) {
                    const PhysicalImage& physical = physicalImages[resource.physicalIndex];
                    barrier.srcStageMask = physical.lastStage;
                    barrier.srcAccessMask = physical.lastWriteAccess;
                }

                barriers.push_back(barrier);
            }

            if (barriers.empty() && bufferBarriers.empty()) {
                return;
            }

            VkDependencyInfo dependencyInfo{};
            dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dependencyInfo.pImageMemoryBarriers = barriers.data();
            dependencyInfo.bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size());
            dependencyInfo.pBufferMemoryBarriers = bufferBarriers.data();
            vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
        };

        for (const CompiledPass& compiledPass : compiled) {
            recordBarriers(compiledPass.barriers);

            const bool raster =
                !compiledPass.colorAttachments.empty() || compiledPass.depthAttachment.has_value();

            if (raster) {
                std::vector<VkRenderingAttachmentInfo> colorAttachments;
                colorAttachments.reserve(compiledPass.colorAttachments.size());
                VkExtent2D renderArea = outputExtent;

                // Which view a pass renders through: the image's own for the
                // ordinary case, the selected layer's for an array.
                auto attachmentView = [&](const PlannedAttachment& attachment) {
                    const Resource& resource = resources[attachment.resourceIndex];
                    if (resource.imported) {
                        return resource.importedView;
                    }
                    const PhysicalImage& physical = physicalImages[resource.physicalIndex];
                    return physical.layerViews.empty() ? physical.view
                                                       : physical.layerViews[attachment.layer];
                };

                for (const PlannedAttachment& attachment : compiledPass.colorAttachments) {
                    const Resource& resource = resources[attachment.resourceIndex];
                    renderArea = resolveExtent(resource);

                    VkRenderingAttachmentInfo info{};
                    info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    info.imageView = attachmentView(attachment);
                    info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    info.loadOp = attachment.loadOp;
                    info.storeOp = attachment.storeOp;
                    info.clearValue = attachment.clearValue;

                    // Averaging the samples happens as the attachment is
                    // stored, which is the whole economy of multisampling: no
                    // second pass reads the large image back. Whether the
                    // multisampled image itself is kept is the planner's
                    // decision like any other attachment's - in the ordinary
                    // case nothing reads it after the resolve and it is
                    // discarded.
                    if (attachment.resolveResourceIndex != FrameGraphResource::invalidIndex) {
                        PlannedAttachment resolveTarget{};
                        resolveTarget.resourceIndex = attachment.resolveResourceIndex;
                        info.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                        info.resolveImageView = attachmentView(resolveTarget);
                        info.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    }

                    colorAttachments.push_back(info);
                }

                VkRenderingAttachmentInfo depthAttachment{};
                if (compiledPass.depthAttachment.has_value()) {
                    const PlannedAttachment& attachment = *compiledPass.depthAttachment;
                    const Resource& resource = resources[attachment.resourceIndex];
                    renderArea = resolveExtent(resource);

                    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    depthAttachment.imageView = attachmentView(attachment);
                    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depthAttachment.loadOp = attachment.loadOp;
                    depthAttachment.storeOp = attachment.storeOp;
                    depthAttachment.clearValue = attachment.clearValue;

                    // Depth is resolved by taking one sample rather than by
                    // averaging: the mean of two depths is a surface neither
                    // sample saw, which would put a false occluder halfway
                    // through every silhouette. SAMPLE_ZERO is also the one
                    // mode Vulkan requires every implementation to support.
                    //
                    // Unlike colour, the multisampled depth is normally kept:
                    // the pass that resolves it is the depth pre-pass, and the
                    // scene pass still tests against the full-rate original.
                    if (attachment.resolveResourceIndex != FrameGraphResource::invalidIndex) {
                        PlannedAttachment resolveTarget{};
                        resolveTarget.resourceIndex = attachment.resolveResourceIndex;
                        depthAttachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                        depthAttachment.resolveImageView = attachmentView(resolveTarget);
                        depthAttachment.resolveImageLayout =
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    }
                }

                VkRenderingInfo renderingInfo{};
                renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                renderingInfo.renderArea = {{0, 0}, renderArea};
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
                renderingInfo.pColorAttachments = colorAttachments.data();
                renderingInfo.pDepthAttachment =
                    compiledPass.depthAttachment.has_value() ? &depthAttachment : nullptr;

                vkCmdBeginRendering(commandBuffer, &renderingInfo);

                VkViewport viewport{};
                viewport.width = static_cast<float>(renderArea.width);
                viewport.height = static_cast<float>(renderArea.height);
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                VkRect2D scissor{{0, 0}, renderArea};
                vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
                vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
            }

            passes[compiledPass.passIndex].execute(commandBuffer, resolvedResources);

            if (raster) {
                vkCmdEndRendering(commandBuffer);
            }
        }

        recordBarriers(finishingBarriers);

        // Remember what each physical image was left doing; that becomes the
        // source scope when next frame's first barrier discards its content.
        for (const Resource& resource : resources) {
            if (resource.imported || resource.physicalIndex == UINT32_MAX) {
                continue;
            }
            if (resource.kind == ResourceKind::buffer) {
                PhysicalBuffer& physical = physicalBuffers[resource.physicalIndex];
                physical.lastStage = resource.state.stage;
                physical.lastWriteAccess = resource.state.access & allWrites;
                continue;
            }
            PhysicalImage& physical = physicalImages[resource.physicalIndex];
            physical.lastStage = resource.state.stage;
            physical.lastWriteAccess = resource.state.access & allWrites;
        }

        // Evict images no frame has asked for in a while. The margin is far
        // larger than the frames-in-flight count, so nothing still referenced
        // by an unretired command buffer can be destroyed.
        constexpr uint64_t evictAfterFrames = 8;
        for (auto it = physicalImages.begin(); it != physicalImages.end();) {
            if (!it->usedThisFrame && frameCounter - it->lastFrameUsed > evictAfterFrames) {
                destroyPhysicalImage(*it);
                it = physicalImages.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = physicalBuffers.begin(); it != physicalBuffers.end();) {
            if (!it->usedThisFrame && frameCounter - it->lastFrameUsed > evictAfterFrames) {
                destroyPhysicalBuffer(*it);
                it = physicalBuffers.erase(it);
            } else {
                ++it;
            }
        }
    }

    FrameGraph::~FrameGraph() {
        if (device == nullptr) {
            return;
        }
        for (PhysicalImage& physical : physicalImages) {
            destroyPhysicalImage(physical);
        }
        for (PhysicalBuffer& physical : physicalBuffers) {
            destroyPhysicalBuffer(physical);
        }
    }

}  // namespace ege
