#pragma once

#include "ege_camera.hpp"

// lib
#include <vulkan/vulkan.h>

namespace ege {
	struct FrameInfo {
		int frameIndex;
		float frameTime;
		VkCommandBuffer commandBuffer;
		EgeCamera& camera;
	};
}  // namespace lve