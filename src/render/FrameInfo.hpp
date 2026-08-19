#pragma once

#include "render/Camera.hpp"
#include "scene/World.hpp"
// lib
#include <vulkan/vulkan.h>

namespace ege {
    struct FrameInfo {
        uint32_t frameIndex;
        float frameTime;
        // How far this frame sits between the last completed fixed step and
        // the next, in [0, 1). What the renderer draws simulated things at.
        float fixedAlpha;
        VkCommandBuffer commandBuffer;
        Camera& camera;
        VkDescriptorSet globalDescriptorSet;
        World& world;
    };
}  // namespace ege