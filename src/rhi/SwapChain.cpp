#include "rhi/SwapChain.hpp"

#include "core/Log.hpp"

// std
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace ege {

    SwapChain::SwapChain(Device& deviceRef, VkExtent2D extent)
        : device{deviceRef}, windowExtent{extent} {
        init();
    }

    SwapChain::SwapChain(
        Device& deviceRef, VkExtent2D extent, std::shared_ptr<SwapChain> previousChain)
        : device{deviceRef}, windowExtent{extent}, oldSwapChain{std::move(previousChain)} {
        init();
        oldSwapChain = nullptr;
    }

    void SwapChain::init() {
        createSwapChain();
        createImageViews();
        swapChainDepthFormat = findDepthFormat();
        createSyncObjects();
    }

    SwapChain::~SwapChain() {
        for (auto imageView : swapChainImageViews) {
            vkDestroyImageView(device.device(), imageView, nullptr);
        }
        swapChainImageViews.clear();

        if (swapChain != nullptr) {
            vkDestroySwapchainKHR(device.device(), swapChain, nullptr);
            swapChain = nullptr;
        }

        // cleanup synchronization objects
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(device.device(), renderFinishedSemaphores[i], nullptr);
            vkDestroySemaphore(device.device(), imageAvailableSemaphores[i], nullptr);
            vkDestroyFence(device.device(), inFlightFences[i], nullptr);
        }
    }

    VkResult SwapChain::acquireNextImage(uint32_t* imageIndex) {
        vkWaitForFences(
            device.device(),
            1,
            &inFlightFences[currentFrame],
            VK_TRUE,
            std::numeric_limits<uint64_t>::max());

        VkResult result = vkAcquireNextImageKHR(
            device.device(),
            swapChain,
            std::numeric_limits<uint64_t>::max(),
            imageAvailableSemaphores[currentFrame],  // must be a not signaled semaphore
            VK_NULL_HANDLE,
            imageIndex);

        return result;
    }

    VkResult SwapChain::submitCommandBuffers(const VkCommandBuffer* buffers, uint32_t* imageIndex) {
        if (imagesInFlight[*imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(device.device(), 1, &imagesInFlight[*imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight[*imageIndex] = inFlightFences[currentFrame];

        // synchronization2 submission: semaphores carry their own stage masks
        // instead of the parallel pWaitDstStageMask array.
        VkSemaphoreSubmitInfo waitSemaphoreInfo{};
        waitSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitSemaphoreInfo.semaphore = imageAvailableSemaphores[currentFrame];
        waitSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalSemaphoreInfo{};
        signalSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalSemaphoreInfo.semaphore = renderFinishedSemaphores[currentFrame];
        signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo commandBufferInfo{};
        commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commandBufferInfo.commandBuffer = *buffers;

        VkSubmitInfo2 submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitSemaphoreInfo;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandBufferInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalSemaphoreInfo;

        vkResetFences(device.device(), 1, &inFlightFences[currentFrame]);
        if (vkQueueSubmit2(device.graphicsQueue(), 1, &submitInfo, inFlightFences[currentFrame]) !=
            VK_SUCCESS) {
            throw std::runtime_error("failed to submit draw command buffer!");
        }

        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinishedSemaphores[currentFrame];

        VkSwapchainKHR swapChains[] = {swapChain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;

        presentInfo.pImageIndices = imageIndex;

        auto result = vkQueuePresentKHR(device.presentQueue(), &presentInfo);

        currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;

        return result;
    }

    void SwapChain::createSwapChain() {
        SwapChainSupportDetails swapChainSupport = device.getSwapChainSupport();

        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
        VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
        VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);

        uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
        if (swapChainSupport.capabilities.maxImageCount > 0 &&
            imageCount > swapChainSupport.capabilities.maxImageCount) {
            imageCount = swapChainSupport.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = device.surface();

        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        // Asked for so a frame can be read back and written to disk - what
        // recording and, eventually, golden-image regression tests need.
        // Every driver worth the name offers it, but it is optional in the
        // specification, so the capability is checked rather than assumed and
        // its absence costs only the ability to capture.
        if ((swapChainSupport.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) !=
            0) {
            createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            transferSourceSupported = true;
        } else {
            EGE_WARN("swapchain images cannot be copied from; frame capture is unavailable");
        }

        QueueFamilyIndices indices = device.findPhysicalQueueFamilies();
        uint32_t queueFamilyIndices[] = {indices.graphicsFamily, indices.presentFamily};

        if (indices.graphicsFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0;      // Optional
            createInfo.pQueueFamilyIndices = nullptr;  // Optional
        }

        createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;

        createInfo.oldSwapchain =
            oldSwapChain == nullptr ? VK_NULL_HANDLE : oldSwapChain->swapChain;

        if (vkCreateSwapchainKHR(device.device(), &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
            throw std::runtime_error("failed to create swap chain!");
        }

        // we only specified a minimum number of images in the swap chain, so the implementation is
        // allowed to create a swap chain with more. That's why we'll first query the final number
        // of images with vkGetSwapchainImagesKHR, then resize the container and finally call it
        // again to retrieve the handles.
        vkGetSwapchainImagesKHR(device.device(), swapChain, &imageCount, nullptr);
        swapChainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device.device(), swapChain, &imageCount, swapChainImages.data());

        swapChainImageFormat = surfaceFormat.format;
        swapChainExtent = extent;
    }

    void SwapChain::createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());
        for (size_t i = 0; i < swapChainImages.size(); i++) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = swapChainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = swapChainImageFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(device.device(), &viewInfo, nullptr, &swapChainImageViews[i]) !=
                VK_SUCCESS) {
                throw std::runtime_error("failed to create texture image view!");
            }
        }
    }

    void SwapChain::createSyncObjects() {
        imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
        imagesInFlight.resize(imageCount(), VK_NULL_HANDLE);

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(
                    device.device(), &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) !=
                    VK_SUCCESS ||
                vkCreateSemaphore(
                    device.device(), &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) !=
                    VK_SUCCESS ||
                vkCreateFence(device.device(), &fenceInfo, nullptr, &inFlightFences[i]) !=
                    VK_SUCCESS) {
                throw std::runtime_error("failed to create synchronization objects for a frame!");
            }
        }
    }

    VkSurfaceFormatKHR SwapChain::chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        // An sRGB format is not just a preference: the tonemap shader writes
        // linear values and counts on the hardware transfer-curve encode.
        for (const auto& availableFormat : availableFormats) {
            if ((availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB ||
                 availableFormat.format == VK_FORMAT_R8G8B8A8_SRGB) &&
                availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }

        EGE_WARN("no sRGB swapchain format available; output will look too dark");
        return availableFormats[0];
    }

    VkPresentModeKHR SwapChain::chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& availablePresentModes) {
        for (const auto& availablePresentMode : availablePresentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                EGE_INFO("Present mode: Mailbox");
                return availablePresentMode;
            }
        }

        // for (const auto &availablePresentMode : availablePresentModes) {
        //   if (availablePresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
        //     EGE_INFO("Present mode: Immediate");
        //     return availablePresentMode;
        //   }
        // }

        EGE_INFO("Present mode: V-Sync");
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D SwapChain::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        } else {
            VkExtent2D actualExtent = windowExtent;
            actualExtent.width = std::max(
                capabilities.minImageExtent.width,
                std::min(capabilities.maxImageExtent.width, actualExtent.width));
            actualExtent.height = std::max(
                capabilities.minImageExtent.height,
                std::min(capabilities.maxImageExtent.height, actualExtent.height));

            return actualExtent;
        }
    }

    VkFormat SwapChain::findDepthFormat() {
        return device.findSupportedFormat(
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

}  // namespace ege
