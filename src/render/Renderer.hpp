#pragma once

#include "platform/Window.hpp"
#include "render/Model.hpp"
#include "rhi/SwapChain.hpp"

#include <memory>
#include <vector>

namespace ege {

    class Renderer {
    public:
        Renderer(Window& window, Device& device);
        ~Renderer();

        // Delete copy constructor and operator1
        Renderer(const Renderer& other) = delete;
        Renderer& operator=(const Renderer&) = delete;

        VkFormat getSwapChainColorFormat() const { return egeSwapChain->getSwapChainImageFormat(); }

        VkFormat getSwapChainDepthFormat() const { return egeSwapChain->getSwapChainDepthFormat(); }

        VkExtent2D getSwapChainExtent() const { return egeSwapChain->getSwapChainExtent(); }

        float getAspectRatio() const { return egeSwapChain->extentAspectRatio(); }

        // The image acquired by beginFrame, for the frame graph to import as
        // this frame's backbuffer.
        VkImage currentSwapChainImage() const {
            assert(isFrameStarted && "no image is acquired outside a frame");
            return egeSwapChain->getImage(currentImageIndex);
        }

        VkImageView currentSwapChainImageView() const {
            assert(isFrameStarted && "no image is acquired outside a frame");
            return egeSwapChain->getImageView(currentImageIndex);
        }

        bool isFrameInProgress() const { return isFrameStarted; };

        VkCommandBuffer getCurrentCommandBuffer() const {
            assert(isFrameStarted && "Cannot get command buffer when frame not in progress");
            return commandBuffers[currentFrameIndex];
        }

        uint32_t getFrameIndex() const {
            assert(isFrameStarted && "Cannot get frame Index When not in progress");
            return currentFrameIndex;
        }

        VkCommandBuffer beginFrame();
        void endFrame();

    private:
        void createCommandBuffers();
        void freeCommandBuffers();
        void recreateSwapChain();

        Window& window;
        Device& device;
        std::unique_ptr<SwapChain> egeSwapChain;
        std::vector<VkCommandBuffer> commandBuffers;
        uint32_t currentImageIndex;
        uint32_t currentFrameIndex{0};
        bool isFrameStarted{false};
    };
}  // namespace ege