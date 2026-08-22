#define VMA_IMPLEMENTATION
#include "rhi/Device.hpp"

#include "core/Log.hpp"

// std headers
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <unordered_set>

namespace ege {

    // local callback functions
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT /*messageSeverity*/,
        VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* /*pUserData*/) {
        EGE_ERROR("validation layer: {}", pCallbackData->pMessage);

        return VK_FALSE;
    }

    VkResult CreateDebugUtilsMessengerEXT(
        VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDebugUtilsMessengerEXT* pDebugMessenger) {
        auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (func != nullptr) {
            return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
        } else {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    void DestroyDebugUtilsMessengerEXT(
        VkInstance instance,
        VkDebugUtilsMessengerEXT debugMessenger,
        const VkAllocationCallbacks* pAllocator) {
        auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (func != nullptr) {
            func(instance, debugMessenger, pAllocator);
        }
    }

    // class member functions
    Device::Device(Window& windowRef) : window{windowRef} {
        createInstance();
        setupDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createAllocator();
        createPipelineCache();
    }

    Device::~Device() {
        for (const auto& [threadId, pool] : commandPools) {
            vkDestroyCommandPool(device_, pool, nullptr);
        }
        commandPools.clear();
        savePipelineCache();
        vkDestroyPipelineCache(device_, cache, nullptr);
        vmaDestroyAllocator(vmaAllocator);
        vkDestroyDevice(device_, nullptr);

        if (enableValidationLayers) {
            DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        }

        vkDestroySurfaceKHR(instance, surface_, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    void Device::createInstance() {
        if (enableValidationLayers && !checkValidationLayerSupport()) {
            throw std::runtime_error("validation layers requested, but not available!");
        }

        // The loader must support 1.3 before a 1.3 instance may be created.
        // vkEnumerateInstanceVersion exists from 1.1, and a loader old enough
        // to lack it could not run this engine anyway.
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        vkEnumerateInstanceVersion(&loaderVersion);
        if (loaderVersion < VK_API_VERSION_1_3) {
            throw std::runtime_error("the Vulkan loader does not support Vulkan 1.3");
        }

        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Enchanted";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "Enchanted";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        auto extensions = getRequiredExtensions();

        // MoltenVK is a non-conformant implementation and is hidden by the
        // loader unless portability enumeration is asked for. Only asked for
        // when available, because asking on a loader without it is an error.
        if (isInstanceExtensionAvailable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }

        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo;
        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();

            populateDebugMessengerCreateInfo(debugCreateInfo);
            createInfo.pNext = &debugCreateInfo;
        } else {
            createInfo.enabledLayerCount = 0;
            createInfo.pNext = nullptr;
        }

        if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
            throw std::runtime_error("failed to create instance!");
        }

        hasGflwRequiredInstanceExtensions();
    }

    void Device::pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            throw std::runtime_error("failed to find GPUs with Vulkan support!");
        }
        EGE_INFO("Physical devices found: {}", deviceCount);
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for (const auto& device : devices) {
            if (isDeviceSuitable(device)) {
                physicalDevice = device;
                break;
            }
        }

        if (physicalDevice == VK_NULL_HANDLE) {
            // Every device was rejected, and isDeviceSuitable has already
            // warned which of them failed on what. Naming the requirement
            // here anyway, because this is the message that reaches someone
            // who has a perfectly good GPU and a driver a version behind -
            // most often on macOS, where MoltenVK reached Vulkan 1.3 later
            // than the desktop drivers did.
            throw std::runtime_error(
                "no device supports what the engine needs; Vulkan 1.3 is required");
        }

        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        EGE_INFO("Using physical device: {}", properties.deviceName);
    }

    void Device::createLogicalDevice() {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily, indices.presentFamily};

        float queuePriority = 1.0f;
        for (uint32_t queueFamily : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo = {};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        // Dynamic rendering replaces render passes and framebuffers wholesale;
        // synchronization2 is the barrier interface the frame graph emits.
        // Both are core in 1.3, so this is a feature enable rather than an
        // extension dance.
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        VkPhysicalDeviceFeatures2 deviceFeatures{};
        deviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        deviceFeatures.features.samplerAnisotropy = VK_TRUE;
        // Point-light shadows are a cube per light, sampled as one cube array
        // rather than as a binding each. Core since Vulkan 1.0 and universal
        // on anything that reaches 1.3, but it is still opt-in.
        deviceFeatures.features.imageCubeArray = VK_TRUE;
        // GPU-driven culling seeds one indirect command per batch, each
        // pointing at its own window of the instance buffer - which is a
        // non-zero firstInstance read from a buffer, and that is precisely
        // what this feature gates. Enabled when the device has it rather
        // than required, because the renderer has an honest life without it:
        // culling falls back to the CPU frustum and the draws go out direct.
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &supported);
        indirectFirstInstance = supported.drawIndirectFirstInstance == VK_TRUE;
        deviceFeatures.features.drawIndirectFirstInstance =
            indirectFirstInstance ? VK_TRUE : VK_FALSE;
        deviceFeatures.pNext = &features13;

        std::vector<const char*> enabledExtensions = deviceExtensions;

        // A portability (MoltenVK) device requires the subset extension to be
        // enabled whenever the device advertises it.
        if (isDeviceExtensionAvailable(physicalDevice, "VK_KHR_portability_subset")) {
            enabledExtensions.push_back("VK_KHR_portability_subset");
        }

        VkDeviceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &deviceFeatures;

        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();

        createInfo.pEnabledFeatures = nullptr;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        createInfo.ppEnabledExtensionNames = enabledExtensions.data();

        // might not really be necessary anymore because device specific validation layers
        // have been deprecated
        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
        } else {
            createInfo.enabledLayerCount = 0;
        }

        if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device_) != VK_SUCCESS) {
            throw std::runtime_error("failed to create logical device!");
        }

        vkGetDeviceQueue(device_, indices.graphicsFamily, 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, indices.presentFamily, 0, &presentQueue_);
    }

    void Device::waitIdle() {
        const std::unique_lock<std::mutex> lock = lockGraphicsQueue();
        vkDeviceWaitIdle(device_);
    }

    VkCommandPool Device::commandPool() {
        const std::thread::id self = std::this_thread::get_id();

        {
            const std::lock_guard<std::mutex> lock{poolMutex};
            const auto existing = commandPools.find(self);
            if (existing != commandPools.end()) {
                return existing->second;
            }
        }

        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = findPhysicalQueueFamilies().graphicsFamily;
        poolInfo.flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VkCommandPool pool = VK_NULL_HANDLE;
        if (vkCreateCommandPool(device_, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create command pool!");
        }

        const std::lock_guard<std::mutex> lock{poolMutex};
        // Another thread cannot have raced this one to the same key - the key
        // is this thread - but the map it is going into is shared.
        commandPools.emplace(self, pool);
        EGE_TRACE("created a command pool for thread {}", commandPools.size());
        return pool;
    }

    void Device::createSurface() {
        window.createWindowSurface(instance, &surface_);
    }

    bool Device::isDeviceSuitable(VkPhysicalDevice device) {
        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(device, &deviceProperties);
        if (deviceProperties.apiVersion < VK_API_VERSION_1_3) {
            EGE_WARN(
                "Skipping {}: driver only supports Vulkan {}.{}",
                deviceProperties.deviceName,
                VK_API_VERSION_MAJOR(deviceProperties.apiVersion),
                VK_API_VERSION_MINOR(deviceProperties.apiVersion));
            return false;
        }

        QueueFamilyIndices indices = findQueueFamilies(device);

        bool extensionsSupported = checkDeviceExtensionSupport(device);

        bool swapChainAdequate = false;
        if (extensionsSupported) {
            SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
            swapChainAdequate =
                !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
        }

        VkPhysicalDeviceVulkan13Features supported13{};
        supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 supportedFeatures{};
        supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        supportedFeatures.pNext = &supported13;
        vkGetPhysicalDeviceFeatures2(device, &supportedFeatures);

        return indices.isComplete() && extensionsSupported && swapChainAdequate &&
               supportedFeatures.features.samplerAnisotropy &&
               supportedFeatures.features.imageCubeArray && supported13.dynamicRendering &&
               supported13.synchronization2;
    }

    VkSampleCountFlagBits Device::maxUsableSampleCount() const {
        const VkSampleCountFlags counts = properties.limits.framebufferColorSampleCounts &
                                          properties.limits.framebufferDepthSampleCounts;

        if (counts & VK_SAMPLE_COUNT_4_BIT) {
            return VK_SAMPLE_COUNT_4_BIT;
        }
        if (counts & VK_SAMPLE_COUNT_2_BIT) {
            return VK_SAMPLE_COUNT_2_BIT;
        }
        return VK_SAMPLE_COUNT_1_BIT;
    }

    void Device::populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
        createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
        createInfo.pUserData = nullptr;  // Optional
    }

    void Device::setupDebugMessenger() {
        if (!enableValidationLayers)
            return;
        VkDebugUtilsMessengerCreateInfoEXT createInfo;
        populateDebugMessengerCreateInfo(createInfo);
        if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) !=
            VK_SUCCESS) {
            throw std::runtime_error("failed to set up debug messenger!");
        }
    }

    bool Device::checkValidationLayerSupport() {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        for (const char* layerName : validationLayers) {
            bool layerFound = false;

            for (const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    layerFound = true;
                    break;
                }
            }

            if (!layerFound) {
                return false;
            }
        }

        return true;
    }

    std::vector<const char*> Device::getRequiredExtensions() {
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

        if (enableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        return extensions;
    }

    void Device::hasGflwRequiredInstanceExtensions() {
        uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());

        EGE_TRACE("Available instance extensions:");
        std::unordered_set<std::string> available;
        for (const auto& extension : extensions) {
            EGE_TRACE("  {}", extension.extensionName);
            available.insert(extension.extensionName);
        }

        EGE_TRACE("Required instance extensions:");
        auto requiredExtensions = getRequiredExtensions();
        for (const auto& required : requiredExtensions) {
            EGE_TRACE("  {}", required);
            if (available.find(required) == available.end()) {
                throw std::runtime_error("Missing required glfw extension");
            }
        }
    }

    bool Device::checkDeviceExtensionSupport(VkPhysicalDevice device) {
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(
            device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

        for (const auto& extension : availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    bool Device::isInstanceExtensionAvailable(const char* name) {
        uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());

        for (const auto& extension : extensions) {
            if (strcmp(extension.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    bool Device::isDeviceExtensionAvailable(VkPhysicalDevice device, const char* name) {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());

        for (const auto& extension : extensions) {
            if (strcmp(extension.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    QueueFamilyIndices Device::findQueueFamilies(VkPhysicalDevice device) {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        uint32_t i = 0;
        for (const auto& queueFamily : queueFamilies) {
            // Compute as well as graphics, because light culling dispatches
            // on this same queue. The spec does not promise every graphics
            // family also does compute, but it does promise at least one
            // family somewhere supports both - so requiring it here narrows
            // which family is picked rather than which devices qualify.
            constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if (queueFamily.queueCount > 0 && (queueFamily.queueFlags & required) == required) {
                indices.graphicsFamily = i;
                indices.graphicsFamilyHasValue = true;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
            if (queueFamily.queueCount > 0 && presentSupport) {
                indices.presentFamily = i;
                indices.presentFamilyHasValue = true;
            }
            if (indices.isComplete()) {
                break;
            }

            i++;
        }

        return indices;
    }

    SwapChainSupportDetails Device::querySwapChainSupport(VkPhysicalDevice device) {
        SwapChainSupportDetails details;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);

        if (formatCount != 0) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                device, surface_, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);

        if (presentModeCount != 0) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                device, surface_, &presentModeCount, details.presentModes.data());
        }
        return details;
    }

    VkFormat Device::findSupportedFormat(
        const std::vector<VkFormat>& candidates,
        VkImageTiling tiling,
        VkFormatFeatureFlags features) {
        for (VkFormat format : candidates) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

            if (tiling == VK_IMAGE_TILING_LINEAR &&
                (props.linearTilingFeatures & features) == features) {
                return format;
            } else if (
                tiling == VK_IMAGE_TILING_OPTIMAL &&
                (props.optimalTilingFeatures & features) == features) {
                return format;
            }
        }
        throw std::runtime_error("failed to find supported format!");
    }

    uint32_t Device::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags memoryProperties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags &
                                            memoryProperties) == memoryProperties) {
                return i;
            }
        }

        throw std::runtime_error("failed to find suitable memory type!");
    }

    void Device::createAllocator() {
        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.physicalDevice = physicalDevice;
        allocatorInfo.device = device_;
        allocatorInfo.instance = instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;

        if (vmaCreateAllocator(&allocatorInfo, &vmaAllocator) != VK_SUCCESS) {
            throw std::runtime_error("failed to create the memory allocator");
        }

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        EGE_INFO(
            "Memory allocator ready: {} heap(s), {} type(s), device allocation limit {}",
            memoryProperties.memoryHeapCount,
            memoryProperties.memoryTypeCount,
            properties.limits.maxMemoryAllocationCount);
    }

    namespace {

        // Beside the executable rather than in the source tree: it is a
        // per-driver, per-device artifact, not something to share or commit.
        const char* pipelineCachePath() {
            return "pipeline_cache.bin";
        }

    }  // namespace

    void Device::createPipelineCache() {
        std::vector<char> initialData;

        if (std::ifstream file{pipelineCachePath(), std::ios::binary | std::ios::ate}) {
            const auto size = static_cast<std::streamsize>(file.tellg());
            if (size > 0) {
                initialData.resize(static_cast<std::size_t>(size));
                file.seekg(0);
                file.read(initialData.data(), size);
            }
        }

        VkPipelineCacheCreateInfo cacheInfo{};
        cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        cacheInfo.initialDataSize = initialData.size();
        cacheInfo.pInitialData = initialData.empty() ? nullptr : initialData.data();

        // The driver validates the blob's header against itself and silently
        // ignores one written by a different device or driver version, so a
        // stale file costs a recompile rather than corrupting anything. That is
        // why no version checking is needed here.
        if (vkCreatePipelineCache(device_, &cacheInfo, nullptr, &cache) != VK_SUCCESS) {
            // Not fatal: without a cache every pipeline simply compiles from
            // scratch, which is the behaviour before this existed.
            EGE_WARN("failed to create the pipeline cache; pipelines will compile uncached");
            cache = VK_NULL_HANDLE;
            return;
        }

        if (initialData.empty()) {
            EGE_INFO("Pipeline cache created empty");
        } else {
            EGE_INFO("Pipeline cache loaded, {} bytes", initialData.size());
        }
    }

    void Device::savePipelineCache() const {
        if (cache == VK_NULL_HANDLE) {
            return;
        }

        std::size_t size = 0;
        if (vkGetPipelineCacheData(device_, cache, &size, nullptr) != VK_SUCCESS || size == 0) {
            return;
        }

        std::vector<char> data(size);
        if (vkGetPipelineCacheData(device_, cache, &size, data.data()) != VK_SUCCESS) {
            return;
        }

        if (std::ofstream file{pipelineCachePath(), std::ios::binary}) {
            file.write(data.data(), static_cast<std::streamsize>(size));
        }
    }

    void Device::createBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags memoryProperties,
        VkBuffer& buffer,
        VmaAllocation& allocation) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // requiredFlags rather than a usage hint, so the existing call sites -
        // which ask for HOST_VISIBLE or DEVICE_LOCAL explicitly - keep their
        // exact meaning.
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_UNKNOWN;
        allocInfo.requiredFlags = memoryProperties;

        if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &buffer, &allocation, nullptr) !=
            VK_SUCCESS) {
            throw std::runtime_error("failed to create vertex buffer!");
        }
    }

    void Device::destroyBuffer(VkBuffer buffer, VmaAllocation allocation) {
        vmaDestroyBuffer(vmaAllocator, buffer, allocation);
    }

    VkCommandBuffer Device::beginSingleTimeCommands() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        // This thread's pool, and the same one endSingleTimeCommands will
        // free from - which is why the two have to be called on one thread.
        allocInfo.commandPool = commandPool();
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        return commandBuffer;
    }

    void Device::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
        vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        // A fence rather than vkQueueWaitIdle, which waits for everything the
        // queue holds rather than for this. On one thread the difference was
        // only wasted time; with uploads arriving from workers it is the
        // difference between waiting for your own copy and waiting for the
        // frame somebody else just submitted.
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(device_, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            throw std::runtime_error("failed to create an upload fence!");
        }

        {
            // Only the submission itself needs the queue. Holding the lock
            // across the wait as well would serialise every upload behind
            // whatever is already running.
            const std::unique_lock<std::mutex> lock = lockGraphicsQueue();
            if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, fence) != VK_SUCCESS) {
                vkDestroyFence(device_, fence, nullptr);
                throw std::runtime_error("failed to submit a single-time command buffer!");
            }
        }

        vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, fence, nullptr);

        vkFreeCommandBuffers(device_, commandPool(), 1, &commandBuffer);
    }

    void Device::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = 0;  // Optional
        copyRegion.dstOffset = 0;  // Optional
        copyRegion.size = size;
        vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

        endSingleTimeCommands(commandBuffer);
    }

    void Device::copyBufferToImage(
        VkBuffer buffer, VkImage image, uint32_t width, uint32_t height, uint32_t layerCount) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;

        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = layerCount;

        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};

        vkCmdCopyBufferToImage(
            commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        endSingleTimeCommands(commandBuffer);
    }

    void Device::createImageWithInfo(
        const VkImageCreateInfo& imageInfo,
        VkMemoryPropertyFlags memoryProperties,
        VkImage& image,
        VmaAllocation& allocation) {
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_UNKNOWN;
        allocInfo.requiredFlags = memoryProperties;

        if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &image, &allocation, nullptr) !=
            VK_SUCCESS) {
            throw std::runtime_error("failed to create image!");
        }
    }

    void Device::destroyImage(VkImage image, VmaAllocation allocation) {
        vmaDestroyImage(vmaAllocator, image, allocation);
    }

}  // namespace ege