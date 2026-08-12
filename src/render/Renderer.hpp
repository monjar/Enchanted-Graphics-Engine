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

        float getAspectRatio() const { return egeSwapChain->extentAspectRatio(); }

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
        void beginSwapChainRendering(VkCommandBuffer commandBuffer);
        void endSwapChainRendering(VkCommandBuffer commandBuffer);

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