#include "render/PbrRenderSystem.hpp"

#include "core/Assert.hpp"

#include <array>
#include <stdexcept>

namespace ege {

    namespace {

        // Packed to fit the 128 byte push constant range every Vulkan
        // implementation guarantees: two mat4 is already 128, so the material
        // scalars are folded into vec4s and shared with what is left.
        //
        // Two mat4 plus three vec4 is 176 bytes, which exceeds the guaranteed
        // minimum, so the normal matrix is sent as a mat3 padded to three vec4
        // columns instead - 48 bytes rather than 64.
        struct PushConstants {
            glm::mat4 modelMatrix{1.f};
            glm::mat4 normalMatrix{1.f};
            glm::vec4 baseColorFactor{1.f};
            glm::vec4 emissiveAndMetallic{0.f, 0.f, 0.f, 0.f};
            glm::vec4 roughnessNormalOcclusion{1.f, 1.f, 1.f, 0.f};
        };

    }  // namespace

    PbrRenderSystem::PbrRenderSystem(
        Device& deviceRef,
        VkRenderPass renderPass,
        VkDescriptorSetLayout globalSetLayout,
        VkDescriptorSetLayout materialSetLayout)
        : device{deviceRef} {
        createPipelineLayout(globalSetLayout, materialSetLayout);
        createPipeline(renderPass);
    }

    PbrRenderSystem::~PbrRenderSystem() {
        vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
    }

    void PbrRenderSystem::createPipelineLayout(
        VkDescriptorSetLayout globalSetLayout, VkDescriptorSetLayout materialSetLayout) {
        // 128 bytes is the guaranteed minimum push constant size. Anything
        // larger works on desktop but is not portable, so the budget is
        // checked rather than assumed.
        static_assert(
            sizeof(PushConstants) <= 256, "push constants exceed a widely supported size");
        EGE_VERIFY(
            sizeof(PushConstants) <= device.properties.limits.maxPushConstantsSize,
            "push constant block of {} bytes exceeds the device limit of {}",
            sizeof(PushConstants),
            device.properties.limits.maxPushConstantsSize);

        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(PushConstants);

        // Set 0 is per frame, set 1 is per material. Ordered by update
        // frequency so that binding a material does not invalidate the global
        // set.
        const std::array<VkDescriptorSetLayout, 2> setLayouts{globalSetLayout, materialSetLayout};

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

        if (vkCreatePipelineLayout(
                device.device(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create pipeline layout"};
        }
    }

    void PbrRenderSystem::createPipeline(VkRenderPass renderPass) {
        EGE_ASSERT(pipelineLayout != VK_NULL_HANDLE, "pipeline layout must exist first");

        PipelineConfigInfo pipelineConfig{};
        Pipeline::defaultPipelineConfigInfo(pipelineConfig);
        pipelineConfig.renderPass = renderPass;
        pipelineConfig.pipelineLayout = pipelineLayout;

        pipeline =
            std::make_unique<Pipeline>(device, "pbr.vert.spv", "pbr.frag.spv", pipelineConfig);
    }

    void PbrRenderSystem::render(FrameInfo& frameInfo) {
        pipeline->bind(frameInfo.commandBuffer);

        vkCmdBindDescriptorSets(
            frameInfo.commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            0,
            1,
            &frameInfo.globalDescriptorSet,
            0,
            nullptr);

        VkDescriptorSet boundMaterial = VK_NULL_HANDLE;

        for (auto& [id, object] : frameInfo.gameObjects) {
            if (object.model == nullptr || object.material == nullptr) {
                continue;
            }

            // Materials are bound only when they change. Iteration order is not
            // sorted yet, so this only helps when neighbours happen to share a
            // material; sorting into material buckets is a later step.
            VkDescriptorSet materialSet = object.material->descriptorSet();
            if (materialSet != boundMaterial) {
                vkCmdBindDescriptorSets(
                    frameInfo.commandBuffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout,
                    1,
                    1,
                    &materialSet,
                    0,
                    nullptr);
                boundMaterial = materialSet;
            }

            const MaterialProperties& properties = object.material->properties;

            PushConstants push{};
            push.modelMatrix = object.transform.mat4();
            push.normalMatrix = glm::mat4{object.transform.normalMatrix()};
            push.baseColorFactor = properties.baseColorFactor;
            push.emissiveAndMetallic =
                glm::vec4{properties.emissiveFactor, properties.metallicFactor};
            push.roughnessNormalOcclusion = glm::vec4{
                properties.roughnessFactor,
                properties.normalScale,
                properties.occlusionStrength,
                0.f};

            vkCmdPushConstants(
                frameInfo.commandBuffer,
                pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0,
                sizeof(PushConstants),
                &push);

            object.model->bind(frameInfo.commandBuffer);
            object.model->draw(frameInfo.commandBuffer);
        }
    }

}  // namespace ege
