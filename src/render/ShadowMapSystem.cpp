#include "render/ShadowMapSystem.hpp"

#include "render/Model.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"

#include <stdexcept>

namespace ege {

    namespace {

        struct ShadowPush {
            glm::mat4 lightMvp{1.f};
        };

    }  // namespace

    ShadowMapSystem::ShadowMapSystem(Device& deviceRef, VkFormat depthFormat) : device{deviceRef} {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(ShadowPush);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(
                device.device(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the shadow pipeline layout"};
        }

        PipelineConfigInfo config{};
        Pipeline::defaultPipelineConfigInfo(config);
        config.bindingDescriptions = Model::Vertex::getBindingDescriptions();
        // Only position is consumed; the other attributes stay in the buffer
        // but are not declared, so the same vertex buffers bind unchanged.
        config.attributeDescriptions = Model::Vertex::getAttributeDescriptions();
        config.attributeDescriptions.resize(1);
        // No color attachment at all - depth is the product.
        config.colorAttachmentFormats.clear();
        config.colorBlendInfo.attachmentCount = 0;
        config.colorBlendInfo.pAttachments = nullptr;
        config.depthAttachmentFormat = depthFormat;
        // A polygon-offset bias at raster time, so the stored depth is pushed
        // away from the light and a surface does not shadow itself. Constant
        // and slope terms both matter: acne on slopes is a slope problem.
        config.rasterizationInfo.depthBiasEnable = VK_TRUE;
        config.rasterizationInfo.depthBiasConstantFactor = 1.25f;
        config.rasterizationInfo.depthBiasSlopeFactor = 2.f;
        config.pipelineLayout = pipelineLayout;

        pipeline = std::make_unique<Pipeline>(device, "shadow.vert.spv", "shadow.frag.spv", config);

        // The comparison sampler the shadow test samples through: LINEAR with
        // compare enabled gives 2x2 hardware PCF per tap. Border color is
        // opaque white so anything outside the map compares as lit.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samplerInfo.compareEnable = VK_TRUE;
        samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &shadowSampler) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the shadow comparison sampler"};
        }
    }

    ShadowMapSystem::~ShadowMapSystem() {
        vkDestroySampler(device.device(), shadowSampler, nullptr);
        vkDestroyPipelineLayout(device.device(), pipelineLayout, nullptr);
    }

    void ShadowMapSystem::render(
        VkCommandBuffer commandBuffer, World& world, const glm::mat4& lightViewProj) {
        pipeline->bind(commandBuffer);

        world.each<Transform, MeshRenderer>(
            Without<Hidden>{}, [&](Entity entity, Transform& transform, MeshRenderer& renderer) {
                if (!renderer.visible || renderer.model == nullptr) {
                    return;
                }

                const glm::mat4 modelMatrix = world.has<Hierarchy>(entity.id())
                                                  ? hierarchy::worldMatrix(world, entity.id())
                                                  : transform.mat4();

                ShadowPush push{};
                push.lightMvp = lightViewProj * modelMatrix;
                vkCmdPushConstants(
                    commandBuffer,
                    pipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT,
                    0,
                    sizeof(ShadowPush),
                    &push);

                renderer.model->bind(commandBuffer);
                renderer.model->draw(commandBuffer);
            });
    }

}  // namespace ege
