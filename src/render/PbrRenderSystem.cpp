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
        VkDescriptorSetLayout materialSetLayout,
        VkSampleCountFlagBits samples,
        uint32_t framesInFlight)
        : device{deviceRef} {
        createPipelineLayout(globalSetLayout, materialSetLayout);
        createPipeline(colorFormat, depthFormat, samples);
        createDepthPipeline(depthFormat, samples, framesInFlight);
    }

    PbrRenderSystem::~PbrRenderSystem() {
        vkDestroyPipelineLayout(device.device(), depthPipelineLayout, nullptr);
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

    void PbrRenderSystem::createPipeline(
        VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples) {
        EGE_ASSERT(pipelineLayout != VK_NULL_HANDLE, "pipeline layout must exist first");

        PipelineConfigInfo pipelineConfig{};
        Pipeline::defaultPipelineConfigInfo(pipelineConfig);
        // Has to match the attachments this pipeline renders into. A pipeline
        // whose sample count disagrees with its attachment is invalid, which
        // is why the count travels from the frame graph's target all the way
        // down here rather than being decided locally.
        pipelineConfig.multisampleInfo.rasterizationSamples = samples;
        // The depth pre-pass has already written the depth of whatever ends up
        // visible, so this pass keeps only the fragments that match it exactly
        // and writes none of its own. EQUAL rather than LESS_OR_EQUAL because
        // a fragment that is merely in front of the recorded depth is one the
        // pre-pass did not see, which means the two passes disagree about the
        // scene - better to lose it visibly than to shade it twice quietly.
        pipelineConfig.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_EQUAL;
        pipelineConfig.depthStencilInfo.depthWriteEnable = VK_FALSE;
        pipelineConfig.bindingDescriptions = Model::Vertex::getBindingDescriptions();
        pipelineConfig.attributeDescriptions = Model::Vertex::getAttributeDescriptions();
        pipelineConfig.colorAttachmentFormats = {colorFormat};
        pipelineConfig.depthAttachmentFormat = depthFormat;
        pipelineConfig.pipelineLayout = pipelineLayout;

        pipeline =
            std::make_unique<Pipeline>(device, "pbr.vert.spv", "pbr.frag.spv", pipelineConfig);
    }

    void PbrRenderSystem::createDepthPipeline(
        VkFormat depthFormat, VkSampleCountFlagBits samples, uint32_t framesInFlight) {
        // Binding 0 is the uniform block, the same number the global set uses,
        // so the pre-pass's vertex shader includes the same declaration of it
        // as everything else.
        depthSetLayout =
            DescriptorSetLayout::Builder(device)
                .addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
                .build();

        depthPool = DescriptorPool::Builder(device)
                        .setMaxSets(framesInFlight)
                        .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight)
                        .build();

        depthDescriptorSets.resize(framesInFlight);
        for (VkDescriptorSet& set : depthDescriptorSets) {
            if (!depthPool->allocateDescriptor(depthSetLayout->getDescriptorSetLayout(), set)) {
                throw std::runtime_error{"failed to allocate a depth pre-pass descriptor set"};
            }
        }

        // The same push constant block as the shading pass, so the two vertex
        // shaders can share its declaration; only the vertex stage reads it
        // here, because the pre-pass has no fragment shader worth the name.
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(PushConstants);

        const VkDescriptorSetLayout rawLayout = depthSetLayout->getDescriptorSetLayout();
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &rawLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &depthPipelineLayout) !=
            VK_SUCCESS) {
            throw std::runtime_error{"failed to create the depth pre-pass pipeline layout"};
        }

        PipelineConfigInfo config{};
        Pipeline::defaultPipelineConfigInfo(config);
        // Everything that decides where a triangle lands has to match the
        // shading pipeline: the same sample count, the same cull mode, the
        // same winding. A pre-pass that rasterizes even slightly differently
        // writes depth the shading pass cannot match.
        config.multisampleInfo.rasterizationSamples = samples;
        config.bindingDescriptions = Model::Vertex::getBindingDescriptions();
        // The whole vertex is bound, but only position is declared - the same
        // arrangement the shadow pass uses, and for the same reason.
        config.attributeDescriptions = Model::Vertex::getAttributeDescriptions();
        config.attributeDescriptions.resize(1);
        // No colour attachment at all: depth is the entire product.
        config.colorAttachmentFormats.clear();
        config.colorBlendInfo.attachmentCount = 0;
        config.colorBlendInfo.pAttachments = nullptr;
        config.depthAttachmentFormat = depthFormat;
        config.pipelineLayout = depthPipelineLayout;

        // No depth bias here, unlike the shadow pass. A shadow map is compared
        // against from a different point of view and needs slack; this depth
        // is compared against from the point of view that wrote it, and any
        // offset at all would make every EQUAL test fail.
        depthPipeline =
            std::make_unique<Pipeline>(device, "depth_prepass.vert.spv", "shadow.frag.spv", config);
    }

    void PbrRenderSystem::prepare(FrameInfo& frameInfo, const OcclusionSnapshot& occlusion) {
        frameStats = Stats{};
        drawList.clear();

        const Frustum frustum = Frustum::fromViewProjection(
            frameInfo.camera.getProjection() * frameInfo.camera.getView());

        // Gather and cull first, submit second. Separating them is what makes
        // sorting possible at all, and it keeps the Vulkan calls out of the
        // query callback where an early return would be easy to get wrong.
        frameInfo.world.each<Transform, MeshRenderer>(
            Without<Hidden>{}, [&](Entity entity, Transform& transform, MeshRenderer& renderer) {
                if (!renderer.visible || !renderer.mesh.resolved() ||
                    !renderer.material.resolved()) {
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

                if (cullingEnabled || occlusionCullingEnabled) {
                    const Aabb worldBounds =
                        renderer.mesh.get()->bounds().transformed(item.modelMatrix);

                    if (cullingEnabled && !frustum.intersects(worldBounds)) {
                        frameStats.culled++;
                        return;
                    }

                    // Inside the frustum and still not worth drawing, because
                    // the last pyramid to come back says something was covering
                    // every pixel it could have reached. Second, because it is
                    // the more expensive test and the frustum has already
                    // thrown out most of what it would have looked at.
                    if (occlusionCullingEnabled && occlusion.hides(worldBounds)) {
                        frameStats.occluded++;
                        return;
                    }
                }

                item.material = renderer.material.get();
                item.materialSet = renderer.material.get()->descriptorSet();
                item.model = renderer.mesh.get();
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

        gathered = true;
    }

    void PbrRenderSystem::renderDepthPrePass(
        FrameInfo& frameInfo, const VkDescriptorBufferInfo& globalUbo) {
        EGE_ASSERT(gathered, "prepare must run before the depth pre-pass");
        EGE_ASSERT(frameInfo.frameIndex < depthDescriptorSets.size(), "frame index out of range");

        // Written before it is bound and not touched again this frame, on the
        // same terms as the light culling pass's set.
        VkDescriptorSet& set = depthDescriptorSets[frameInfo.frameIndex];
        DescriptorWriter(*depthSetLayout, *depthPool)
            .writeBuffer(0, const_cast<VkDescriptorBufferInfo*>(&globalUbo))
            .overwrite(set);

        depthPipeline->bind(frameInfo.commandBuffer);

        vkCmdBindDescriptorSets(
            frameInfo.commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            depthPipelineLayout,
            0,
            1,
            &set,
            0,
            nullptr);

        // No material binds: the pre-pass reads no textures, so the whole list
        // draws with one pipeline and one descriptor set. The push constants
        // still go out in full, because the range is shared with the shading
        // pass and a partial update would leave the rest of it stale.
        for (const DrawItem& item : drawList) {
            PushConstants push{};
            push.modelMatrix = item.modelMatrix;
            push.normalMatrix = item.normalMatrix;

            vkCmdPushConstants(
                frameInfo.commandBuffer,
                depthPipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(PushConstants),
                &push);

            const_cast<Model*>(item.model)->bind(frameInfo.commandBuffer);
            const_cast<Model*>(item.model)->draw(frameInfo.commandBuffer);
        }
    }

    void PbrRenderSystem::render(FrameInfo& frameInfo) {
        EGE_ASSERT(gathered, "prepare must run before the scene pass");

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

        gathered = false;
    }

}  // namespace ege
