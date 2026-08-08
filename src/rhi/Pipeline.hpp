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
        VkRenderPass renderPass = nullptr;
        uint32_t subpass = 0;
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
        static std::vector<char> readFile(const std::string& filePath);

        void createGraphicsPipeline(
            const std::string& vertFilePath,
            const std::string& fragFilePath,
            const PipelineConfigInfo& configInfo);

        void createShaderModule(const std::vector<char>& code, VkShaderModule* shaderModule);

        // Pipline fundementally needs a device to exist, so no risk of dangling (Aggregation)
        Device& device;

        VkPipeline graphicsPipeline;

        VkShaderModule vertShaderModule;
        VkShaderModule fragShaderModule;
    };
}  // namespace ege