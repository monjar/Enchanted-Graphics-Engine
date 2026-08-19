#pragma once

#include "platform/Window.hpp"

#include <vk_mem_alloc.h>

// std lib headers
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ege {

    struct SwapChainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    struct QueueFamilyIndices {
        uint32_t graphicsFamily;
        uint32_t presentFamily;
        bool graphicsFamilyHasValue = false;
        bool presentFamilyHasValue = false;

        bool isComplete() { return graphicsFamilyHasValue && presentFamilyHasValue; }
    };

    class Device {
    public:
#ifdef NDEBUG
        const bool enableValidationLayers = false;
#else
        const bool enableValidationLayers = true;
#endif

        Device(Window& window);
        ~Device();

        // Not copyable or movable
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;
        Device(Device&&) = delete;
        Device& operator=(Device&&) = delete;

        // The command pool belonging to the calling thread, created the first
        // time that thread asks for one.
        //
        // A VkCommandPool is externally synchronized: allocating from one on
        // two threads at once is undefined, and so is freeing while another
        // thread allocates. One pool per thread is the arrangement Vulkan
        // expects, and it is what lets an asset be uploaded from a worker
        // while the main thread is recording the frame.
        VkCommandPool commandPool();

        // Held for the duration of any submission to the graphics queue.
        //
        // A VkQueue is externally synchronized too, and unlike the pools it
        // cannot be duplicated: the frame's submit and an upload's submit are
        // the same queue and have to take turns. Every caller of
        // vkQueueSubmit, vkQueuePresentKHR or vkQueueWaitIdle takes this.
        [[nodiscard]] std::unique_lock<std::mutex> lockGraphicsQueue() {
            return std::unique_lock<std::mutex>{queueMutex};
        }

        // Waits for everything the device is doing. Host synchronization on
        // this is the same as on a submission - it is one for every queue at
        // once - so it goes through the same lock rather than around it.
        void waitIdle();

        VkDevice device() const { return device_; }

        VkSurfaceKHR surface() { return surface_; }

        VkQueue graphicsQueue() { return graphicsQueue_; }

        VkQueue presentQueue() { return presentQueue_; }

        SwapChainSupportDetails getSwapChainSupport() {
            return querySwapChainSupport(physicalDevice);
        }

        uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags memoryProperties);

        QueueFamilyIndices findPhysicalQueueFamilies() { return findQueueFamilies(physicalDevice); }

        VkFormat findSupportedFormat(
            const std::vector<VkFormat>& candidates,
            VkImageTiling tiling,
            VkFormatFeatureFlags features);

        // The allocator every GPU allocation goes through.
        VmaAllocator allocator() const { return vmaAllocator; }

        // Needed to query format capabilities, such as whether a format
        // supports the linear filtering that mip generation blits require.
        VkPhysicalDevice physicalDeviceHandle() const { return physicalDevice; }

        // Needed by libraries that drive Vulkan themselves - the ImGui
        // backend initialises against the instance and queue family directly.
        VkInstance instanceHandle() const { return instance; }

        uint32_t graphicsQueueFamily() { return findPhysicalQueueFamilies().graphicsFamily; }

        // Shared pipeline cache, persisted to disk between runs.
        //
        // Compiling a pipeline means the driver compiling SPIR-V to its own ISA,
        // which is the slowest part of start-up and is repaid on every launch
        // without this. The cache is a hint: a stale or rejected one costs a
        // recompile, never a wrong result.
        VkPipelineCache pipelineCache() const { return cache; }

        // Writes the cache to disk. Called from the destructor, but also worth
        // calling explicitly once pipelines have been created: a process that
        // is killed rather than closed never runs the destructor, and an engine
        // is killed fairly often during development.
        void savePipelineCache() const;

        // Buffer Helper Functions
        //
        // Allocation is suballocated from VMA's pools rather than taken from
        // vkAllocateMemory directly. Drivers cap the number of live device
        // allocations - maxMemoryAllocationCount is commonly 4096 - and one
        // allocation per buffer reaches that at a few thousand meshes.
        void createBuffer(
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkMemoryPropertyFlags memoryProperties,
            VkBuffer& buffer,
            VmaAllocation& allocation);

        void destroyBuffer(VkBuffer buffer, VmaAllocation allocation);
        VkCommandBuffer beginSingleTimeCommands();
        void endSingleTimeCommands(VkCommandBuffer commandBuffer);
        void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
        void copyBufferToImage(
            VkBuffer buffer, VkImage image, uint32_t width, uint32_t height, uint32_t layerCount);

        void createImageWithInfo(
            const VkImageCreateInfo& imageInfo,
            VkMemoryPropertyFlags memoryProperties,
            VkImage& image,
            VmaAllocation& allocation);

        void destroyImage(VkImage image, VmaAllocation allocation);

        // The best multisample count this device supports for both colour
        // and depth, capped at 4x.
        //
        // Both matter: an implementation may allow more samples on colour
        // than on depth, and an attachment pair has to agree. The cap is a
        // judgement rather than a limit - 8x costs twice the bandwidth of 4x
        // for a difference that is hard to see at ordinary resolutions.
        // Returns VK_SAMPLE_COUNT_1_BIT when nothing better is available,
        // which switches multisampling off rather than failing.
        VkSampleCountFlagBits maxUsableSampleCount() const;

        VkPhysicalDeviceProperties properties;

    private:
        void createInstance();
        void setupDebugMessenger();
        void createSurface();
        void pickPhysicalDevice();
        void createLogicalDevice();
        void createAllocator();
        void createPipelineCache();

        // helper functions
        bool isDeviceSuitable(VkPhysicalDevice device);
        std::vector<const char*> getRequiredExtensions();
        static bool isInstanceExtensionAvailable(const char* name);
        static bool isDeviceExtensionAvailable(VkPhysicalDevice device, const char* name);
        bool checkValidationLayerSupport();
        QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
        void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);
        void hasGflwRequiredInstanceExtensions();
        bool checkDeviceExtensionSupport(VkPhysicalDevice device);
        SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);

        VkInstance instance;
        VkDebugUtilsMessengerEXT debugMessenger;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        Window& window;

        // One pool per thread that has asked for one, made on demand and kept
        // until the device goes away. Guarded because the map itself is shared
        // even though what it hands out is not.
        std::mutex poolMutex;
        std::unordered_map<std::thread::id, VkCommandPool> commandPools;
        std::mutex queueMutex;

        VkDevice device_;
        VmaAllocator vmaAllocator = VK_NULL_HANDLE;
        VkPipelineCache cache = VK_NULL_HANDLE;
        VkSurfaceKHR surface_;
        VkQueue graphicsQueue_;
        VkQueue presentQueue_;

        const std::vector<const char*> validationLayers = {"VK_LAYER_KHRONOS_validation"};
        const std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    };

}  // namespace ege