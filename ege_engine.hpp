#pragma once

#include "ege_window.hpp"
#include "ege_pipeline.hpp"
namespace ege {

	class EnchantedEngine {
	public:
		static constexpr int WIDTH = 800, HEIGHT = 600;
		void run();
	private:
		EgeWindow egeWindow{ WIDTH, HEIGHT, "Hello World!" };
		EgeDevice egeDevice{ egeWindow };
		EgePipeline egePipeline{ egeDevice, "CompiledShaders/simple_shader.vert.spv", "CompiledShaders/simple_shader.frag.spv", EgePipeline::defaultPipelineConfigInfo(WIDTH, HEIGHT)};
	};
}