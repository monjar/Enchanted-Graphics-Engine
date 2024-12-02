#pragma once

#include "ege_camera.hpp"
#include "ege_game_object.hpp"
// lib
#include <vulkan/vulkan.h>

namespace ege {
	struct FrameInfo {
		int frameIndex;
		float frameTime;
		VkCommandBuffer commandBuffer;
		EgeCamera& camera;
		VkDescriptorSet globalDescriptorSet;
		EgeGameObject::Map& gameObjects;
	};
}  // namespace lve