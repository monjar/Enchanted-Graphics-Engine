#pragma once

#include "rhi/Device.hpp"

#include <string>
#include <vector>

namespace ege {

    struct PipelineConfigInfo {
        // Declared so the struct stays default-constructible once the deleted copy
        // constructor below makes it a non-aggregate, which is the rule from C++20
        // onwards. Every member also carries an initializer so the fields are
        // zeroed however the struct is declared, not only when brace-initialized.
        PipelineConfigInfo() = default;
        PipelineConfigInfo(const PipelineConfigInfo&) = delete;
        PipelineConfigInfo& operator=(const PipelineConfigInfo&) = delete;

        VkPipelineViewportStateCreateInfo viewportInfo{};
        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
        VkPipelineRasterizationStateCreateInfo rasterizationInfo{};
        VkPipelineMultisampleStateCreateInfo multisampleInfo{};
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
        VkPipelineDepthStencilStateCreateInfo depthStencilInfo{};
        std::vector<VkDynamicState> dynamicStateEnables{};
        VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
        VkPipelineLayout pipelineLayout = nullptr;

        // Vertex input belongs to whoever owns the vertex data, not to the
        // pipeline machinery - a fullscreen pass has none at all. Empty means
        // exactly that: no vertex buffers, positions from gl_VertexIndex.
        std::vector<VkVertexInputBindingDescription> bindingDescriptions{};
        std::vector<VkVertexInputAttributeDescription> attributeDescriptions{};

        // Dynamic rendering has no render pass object; a pipeline is instead
        // created against the formats of the attachments it will render to.
        std::vector<VkFormat> colorAttachmentFormats{};
        VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    };

    class Pipeline {
    public:
        Pipeline(
            Device& device,
            const std::string& vertFilePath,
            const std::string& fragFilePath,
            const PipelineConfigInfo& configInfo);
        ~Pipeline();

        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;

        void bind(VkCommandBuffer commandBuffer);

        static void defaultPipelineConfigInfo(PipelineConfigInfo& configInfo);

    private:
        void createGraphicsPipeline(
            const std::string& vertFilePath,
            const std::string& fragFilePath,
            const PipelineConfigInfo& configInfo);

        // Pipline fundementally needs a device to exist, so no risk of dangling (Aggregation)
        Device& device;

        VkPipeline graphicsPipeline;

        VkShaderModule vertShaderModule;
        VkShaderModule fragShaderModule;
    };

    // A compute pipeline: one shader stage, no attachments, no fixed-function
    // state at all. It lives beside the graphics pipeline rather than in its
    // own file because the two share how SPIR-V is found on disk and turned
    // into a shader module, and there is no third kind.
    //
    // Compute work is dispatched on the graphics queue rather than on a
    // dedicated compute one. A separate queue would let culling overlap the
    // previous frame's raster, but it would also need ownership transfers and
    // a second timeline to synchronise against - real complexity to buy an
    // overlap this engine has no measurement to justify yet.
    class ComputePipeline {
    public:
        ComputePipeline(
            Device& device, const std::string& compFilePath, VkPipelineLayout pipelineLayout);
        ~ComputePipeline();

        ComputePipeline(const ComputePipeline&) = delete;
        ComputePipeline& operator=(const ComputePipeline&) = delete;

        void bind(VkCommandBuffer commandBuffer);

    private:
        Device& device;

        VkPipeline computePipeline = VK_NULL_HANDLE;
        VkShaderModule shaderModule = VK_NULL_HANDLE;
    };

    // How many workgroups cover `count` items at `groupSize` per group. The
    // rounding-up is what makes the shader's bounds check load-bearing: the
    // last group runs invocations past the end of the work.
    inline uint32_t dispatchGroupCount(uint32_t count, uint32_t groupSize) {
        return groupSize == 0 ? 0 : (count + groupSize - 1) / groupSize;
    }

}  // namespace ege