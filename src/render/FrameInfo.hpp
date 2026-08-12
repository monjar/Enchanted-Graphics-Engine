#pragma once

#include "render/Camera.hpp"
#include "scene/GameObject.hpp"
// lib
#include <vulkan/vulkan.h>

namespace ege {
    struct FrameInfo {
        uint32_t frameIndex;
        float frameTime;
        VkCommandBuffer commandBuffer;
        Camera& camera;
        VkDescriptorSet globalDescriptorSet;
        GameObject::Map& gameObjects;
    };
}  // namespace ege