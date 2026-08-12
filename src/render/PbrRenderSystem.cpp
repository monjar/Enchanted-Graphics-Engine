#include "render/PbrRenderSystem.hpp"

#include "core/Assert.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"

#include <algorithm>
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
        VkFormat colorFormat,
        VkFormat depthFormat,
        VkDescriptorSetLayout globalSetLayout,
        VkDescriptorSetLayout materialSetLayout)
        : device{deviceRef} {
        createPipelineLayout(globalSetLayout, materialSetLayout);
        createPipeline(colorFormat, depthFormat);
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

    void PbrRenderSystem::createPipeline(VkFormat colorFormat, VkFormat depthFormat) {
        EGE_ASSERT(pipelineLayout != VK_NULL_HANDLE, "pipeline layout must exist first");

        PipelineConfigInfo pipelineConfig{};
        Pipeline::defaultPipelineConfigInfo(pipelineConfig);
        pipelineConfig.colorAttachmentFormats = {colorFormat};
        pipelineConfig.depthAttachmentFormat = depthFormat;
        pipelineConfig.pipelineLayout = pipelineLayout;

        pipeline =
            std::make_unique<Pipeline>(device, "pbr.vert.spv", "pbr.frag.spv", pipelineConfig);
    }

    void PbrRenderSystem::render(FrameInfo& frameInfo) {
        frameStats = Stats{};
        drawList.clear();

        const Frustum frustum = Frustum::fromViewProjection(
            frameInfo.camera.getProjection() * frameInfo.camera.getView());

        // Gather and cull first, submit second. Separating them is what makes
        // sorting possible at all, and it keeps the Vulkan calls out of the
        // query callback where an early return would be easy to get wrong.
        frameInfo.world.each<Transform, MeshRenderer>(
            Without<Hidden>{}, [&](Entity entity, Transform& transform, MeshRenderer& renderer) {
                if (!renderer.visible || renderer.model == nullptr ||
                    renderer.material == nullptr) {
                    return;
                }

                frameStats.candidates++;

                DrawItem item{};

                if (frameInfo.world.has<Hierarchy>(entity.id())) {
                    // Under a hierarchy the model matrix is the composed world
                    // matrix, and the inverse-scale shortcut for the normal
                    // matrix no longer holds: a non-uniform parent scale
                    // combined with a child rotation introduces shear, which is
                    // exactly the case that shortcut excludes. Fall back to the
                    // general transpose(inverse(M)).
                    item.modelMatrix = hierarchy::worldMatrix(frameInfo.world, entity.id());
                    item.normalMatrix =
                        glm::mat4{glm::transpose(glm::inverse(glm::mat3{item.modelMatrix}))};
                } else {
                    item.modelMatrix = transform.mat4();
                    item.normalMatrix = glm::mat4{transform.normalMatrix()};
                }

                if (cullingEnabled) {
                    const Aabb worldBounds = renderer.model->bounds().transformed(item.modelMatrix);
                    if (!frustum.intersects(worldBounds)) {
                        frameStats.culled++;
                        return;
                    }
                }

                item.material = renderer.material.get();
                item.materialSet = renderer.material->descriptorSet();
                item.model = renderer.model.get();
                drawList.push_back(item);
            });

        // Sorted by material, then by mesh. Descriptor set binds are the
        // expensive part of a draw, and pool iteration order has no reason to
        // group objects that share one - so without this a scene alternating
        // between two materials rebinds on every single object.
        std::sort(drawList.begin(), drawList.end(), [](const DrawItem& a, const DrawItem& b) {
            if (a.material != b.material) {
                return a.material < b.material;
            }
            return a.model < b.model;
        });

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

        for (const DrawItem& item : drawList) {
            if (item.materialSet != boundMaterial) {
                vkCmdBindDescriptorSets(
                    frameInfo.commandBuffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout,
                    1,
                    1,
                    &item.materialSet,
                    0,
                    nullptr);
                boundMaterial = item.materialSet;
                frameStats.materialBinds++;
            }

            const MaterialProperties& properties = item.material->properties;

            PushConstants push{};
            push.modelMatrix = item.modelMatrix;
            push.normalMatrix = item.normalMatrix;
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

            const_cast<Model*>(item.model)->bind(frameInfo.commandBuffer);
            const_cast<Model*>(item.model)->draw(frameInfo.commandBuffer);
            frameStats.drawn++;
        }
    }

}  // namespace ege
