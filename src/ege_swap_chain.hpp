#pragma once

#include "ege_engine_device.hpp"

// vulkan headers
#include <vulkan/vulkan.h>

// std lib headers
#include <memory>
#include <string>
#include <vector>

namespace ege {

class EgeSwapChain {
 public:
  static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

  EgeSwapChain(EgeDevice &deviceRef, VkExtent2D windowExtent);
  EgeSwapChain(EgeDevice &deviceRef, VkExtent2D windowExtent, std::shared_ptr<EgeSwapChain> previousChain);
  ~EgeSwapChain();

  EgeSwapChain(const EgeSwapChain &) = delete;
  EgeSwapChain& operator=(const EgeSwapChain &) = delete;

  VkFramebuffer getFrameBuffer(uint32_t index) const { return swapChainFramebuffers[index]; }
  VkRenderPass getRenderPass() const { return renderPass; }
  VkImageView getImageView(uint32_t index) const { return swapChainImageViews[index]; }
  size_t imageCount() const { return swapChainImages.size(); }
  VkFormat getSwapChainImageFormat() const { return swapChainImageFormat; }
  VkExtent2D getSwapChainExtent() const { return swapChainExtent; }
  uint32_t width() { return swapChainExtent.width; }
  uint32_t height() { return swapChainExtent.height; }

  float extentAspectRatio() {
    return static_cast<float>(swapChainExtent.width) / static_cast<float>(swapChainExtent.height);
  }
  VkFormat findDepthFormat();

  VkResult acquireNextImage(uint32_t *imageIndex);
  VkResult submitCommandBuffers(const VkCommandBuffer *buffers, uint32_t *imageIndex);

  bool compareSwapFormats(const EgeSwapChain& other) const {
      return other.swapChainDepthFormat == swapChainDepthFormat && other.swapChainImageFormat == swapChainImageFormat;
  }

 private:
  void init();
  void createSwapChain();
  void createImageViews();
  void createDepthResources();
  void createRenderPass();
  void createFramebuffers();
  void createSyncObjects();

  // Helper functions
  VkSurfaceFormatKHR chooseSwapSurfaceFormat(
      const std::vector<VkSurfaceFormatKHR> &availableFormats);
  VkPresentModeKHR chooseSwapPresentMode(
      const std::vector<VkPresentModeKHR> &availablePresentModes);
  VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities);

  VkFormat swapChainImageFormat;
  VkFormat swapChainDepthFormat;
  VkExtent2D swapChainExtent;

  std::vector<VkFramebuffer> swapChainFramebuffers;
  VkRenderPass renderPass;

  std::vector<VkImage> depthImages;
  std::vector<VkDeviceMemory> depthImageMemorys;
  std::vector<VkImageView> depthImageViews;
  std::vector<VkImage> swapChainImages;
  std::vector<VkImageView> swapChainImageViews;

  EgeDevice&device;
  VkExtent2D windowExtent;

  VkSwapchainKHR swapChain;
  std::shared_ptr<EgeSwapChain> oldSwapChain;


  std::vector<VkSemaphore> imageAvailableSemaphores;
  std::vector<VkSemaphore> renderFinishedSemaphores;
  std::vector<VkFence> inFlightFences;
  std::vector<VkFence> imagesInFlight;
  size_t currentFrame = 0;
};

}  // namespace ege
