#include "render/Renderer.hpp"

#include <array>
#include <stdexcept>

namespace ege {

    Renderer::Renderer(Window& windowRef, Device& deviceRef)
        : window{windowRef}, device{deviceRef} {
        recreateSwapChain();
        createCommandBuffers();
    }

    Renderer::~Renderer() {
        freeCommandBuffers();
    }

    void Renderer::recreateSwapChain() {
        auto extent = window.getExtent();
        while (extent.width == 0 || extent.height == 0) {
            extent = window.getExtent();
            Window::waitForEvents();
        }
        vkDeviceWaitIdle(device.device());
        if (egeSwapChain == nullptr) {
            egeSwapChain = std::make_unique<SwapChain>(device, extent);
        } else {
            std::shared_ptr<SwapChain> oldSwapChain = std::move(egeSwapChain);
            egeSwapChain = std::make_unique<SwapChain>(device, extent, oldSwapChain);

            if (!oldSwapChain->compareSwapFormats(*egeSwapChain.get())) {
                throw std::runtime_error("Swap chain image(or depth) format has changed!");
            }
        }
    }

    void Renderer::freeCommandBuffers() {
        vkFreeCommandBuffers(
            device.device(),
            device.getCommandPool(),
            static_cast<uint32_t>(commandBuffers.size()),
            commandBuffers.data());
        commandBuffers.clear();
    }

    void Renderer::createCommandBuffers() {
        commandBuffers.resize(SwapChain::MAX_FRAMES_IN_FLIGHT);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = device.getCommandPool();
        allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

        if (vkAllocateCommandBuffers(device.device(), &allocInfo, commandBuffers.data()) !=
            VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate command buffer!");
        }
    }

    VkCommandBuffer Renderer::beginFrame() {
        assert(!isFrameStarted && "Cant begin frame while it's already started");

        auto result = egeSwapChain->acquireNextImage(&currentImageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapChain();
            return nullptr;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("failed to acquire swap chain image!");
        }
        isFrameStarted = true;

        auto commandBuffer = getCurrentCommandBuffer();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("failed to begin recording command buffer!");
        }

        return commandBuffer;
    }

    void Renderer::endFrame() {
        assert(isFrameStarted && "Can't call endFrame while frame is not in progress");
        auto commandBuffer = getCurrentCommandBuffer();
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to record command buffer!");
        }

        auto result = egeSwapChain->submitCommandBuffers(&commandBuffer, &currentImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
            window.wasWindowResized()) {
            window.resetWindowResizedFlag();
            recreateSwapChain();
        } else if (result != VK_SUCCESS) {
            throw std::runtime_error("failed to present swap chain image!");
        }

        isFrameStarted = false;
        currentFrameIndex = (currentFrameIndex + 1) % SwapChain::MAX_FRAMES_IN_FLIGHT;
    }

    void Renderer::beginSwapChainRendering(VkCommandBuffer commandBuffer) {
        assert(isFrameStarted && "Cant begin rendering while frame is not in progress");

        assert(
            commandBuffer == getCurrentCommandBuffer() &&
            "Can't begin rendering on cmd buffer from a different frame.");

        // What the render pass used to do implicitly, spelled out: move the
        // acquired image into color-attachment layout and the depth buffer into
        // depth-attachment layout. Both start from UNDEFINED because neither
        // carries content across frames; the source stages still matter, since
        // the swapchain transition must chain after the acquire semaphore and
        // the depth transition after the previous frame's depth writes.
        std::array<VkImageMemoryBarrier2, 2> barriers{};

        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_NONE;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[0].image = egeSwapChain->getImage(currentImageIndex);
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barriers[1].image = egeSwapChain->getDepthImage(currentImageIndex);
        // A transition of a combined depth/stencil format must cover both
        // aspects, even though nothing here uses the stencil half.
        VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        const VkFormat depthFormat = egeSwapChain->getSwapChainDepthFormat();
        if (depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
            depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
            depthFormat == VK_FORMAT_D16_UNORM_S8_UINT) {
            depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        barriers[1].subresourceRange = {depthAspect, 0, 1, 0, 1};

        VkDependencyInfo dependencyInfo{};
        dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependencyInfo.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = egeSwapChain->getImageView(currentImageIndex);
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{0.01f, 0.01f, 0.01f, 1.0f}};

        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = egeSwapChain->getDepthImageView(currentImageIndex);
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, egeSwapChain->getSwapChainExtent()};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(commandBuffer, &renderingInfo);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(egeSwapChain->getSwapChainExtent().width);
        viewport.height = static_cast<float>(egeSwapChain->getSwapChainExtent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, egeSwapChain->getSwapChainExtent()};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    void Renderer::endSwapChainRendering(VkCommandBuffer commandBuffer) {
        assert(isFrameStarted && "Cant end rendering while frame is not in progress");

        assert(
            commandBuffer == getCurrentCommandBuffer() &&
            "Can't end rendering on cmd buffer from a different frame.");

        vkCmdEndRendering(commandBuffer);

        // The other half of the render pass's implicit work: hand the image to
        // the presentation engine. No destination access - the semaphore
        // signalled at submit is what orders presentation.
        VkImageMemoryBarrier2 presentBarrier{};
        presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        presentBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        presentBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        presentBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        presentBarrier.dstAccessMask = VK_ACCESS_2_NONE;
        presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presentBarrier.image = egeSwapChain->getImage(currentImageIndex);
        presentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dependencyInfo{};
        dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependencyInfo.imageMemoryBarrierCount = 1;
        dependencyInfo.pImageMemoryBarriers = &presentBarrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

}  // namespace ege