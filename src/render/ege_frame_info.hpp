#pragma once

#include "render/ege_camera.hpp"
#include "scene/ege_game_object.hpp"
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