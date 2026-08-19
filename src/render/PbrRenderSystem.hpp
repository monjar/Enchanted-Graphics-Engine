#pragma once

#include "render/Bounds.hpp"
#include "render/FrameInfo.hpp"
#include "render/Material.hpp"
#include "render/Model.hpp"
#include "render/OcclusionCulling.hpp"
#include "rhi/Buffer.hpp"
#include "rhi/Descriptors.hpp"
#include "rhi/Pipeline.hpp"

#include <memory>
#include <vector>

namespace ege {

    // One drawn object's transform, as the vertex shaders read it.
    //
    // Mirrors Instance in shaders/model_instances.glsl. std430, where a mat4
    // is four vec4 columns and needs no padding either side of it.
    struct GpuInstance {
        glm::mat4 modelMatrix{1.f};
        glm::mat4 normalMatrix{1.f};
    };

    // How many objects one frame can draw. A bound on storage rather than on
    // work: nothing loops over this, and a scene with more objects than this
    // draws the first of them rather than failing.
    inline constexpr uint32_t maxDrawInstances = 4096;

    // Draws the scene with the metallic-roughness PBR shader.
    //
    // Replaces SimpleRenderSystem, whose shader had a single hardcoded point
    // light, no textures, Lambert diffuse and no specular term at all.
    //
    // Draws in two passes over the same list. The first writes depth and
    // nothing else; the second shades with depth writes off and the test set
    // to EQUAL, so a fragment is shaded only where it is the one that ends up
    // visible. Clustered forward shading makes that worth doing: its fragment
    // shader samples four environment maps, walks the cascades and loops the
    // fragment's whole cluster, and a scene drawn back to front pays all of
    // that once per layer of geometry over the pixel rather than once.
    class PbrRenderSystem {
    public:
        PbrRenderSystem(
            Device& device,
            VkFormat colorFormat,
            VkFormat depthFormat,
            VkDescriptorSetLayout globalSetLayout,
            VkDescriptorSetLayout materialSetLayout,
            VkSampleCountFlagBits samples,
            uint32_t framesInFlight);
        ~PbrRenderSystem();

        PbrRenderSystem(const PbrRenderSystem&) = delete;
        PbrRenderSystem& operator=(const PbrRenderSystem&) = delete;

        // Gathers the frame's visible objects into one list, sorted for
        // submission. Separate from the passes that draw it because both draw
        // the same list, and gathering twice would risk them disagreeing about
        // what is visible - which for an EQUAL depth test means geometry that
        // shades against depth nothing wrote.
        //
        // The snapshot is the depth pyramid of a frame a little while ago; an
        // object it says was hidden then is left out. An empty one leaves
        // everything in.
        //
        // `instanceBuffer` is where the gathered transforms are written, in
        // submission order, for both passes to index by instance.
        void prepare(
            FrameInfo& frameInfo, const OcclusionSnapshot& occlusion, Buffer& instanceBuffer);

        // Depth only, from the same list, with the same vertex transform. What
        // this writes is what the shading pass tests EQUAL against.
        //
        // Takes the uniform block directly rather than through the renderer's
        // global descriptor set, because it owns a set of its own - see the
        // note on depthSetLayout below.
        void renderDepthPrePass(
            FrameInfo& frameInfo,
            const VkDescriptorBufferInfo& globalUbo,
            const VkDescriptorBufferInfo& instances);

        void render(FrameInfo& frameInfo);

        // Draw statistics for the frame just submitted.
        struct Stats {
            std::size_t candidates = 0;
            std::size_t culled = 0;
            // Inside the frustum, but standing behind something that was
            // already covering every pixel of them.
            std::size_t occluded = 0;
            // Objects submitted, and the draw calls that carried them.
            // Consecutive objects sharing a mesh and a material go out as one
            // instanced draw, so the second number is what the GPU was
            // actually asked to do and the gap between them is what
            // instancing saved.
            std::size_t drawn = 0;
            std::size_t batches = 0;
            std::size_t materialBinds = 0;
        };

        const Stats& stats() const { return frameStats; }

        // Culling can be turned off to check that it is not wrongly discarding
        // something - the first thing to try when geometry goes missing.
        bool cullingEnabled = true;
        // The same switch for the occlusion test, which unlike the frustum
        // test is answering with information a couple of frames old and is
        // therefore the more likely of the two to be wrong.
        bool occlusionCullingEnabled = true;

    private:
        void createPipelineLayout(
            VkDescriptorSetLayout globalSetLayout, VkDescriptorSetLayout materialSetLayout);
        void createPipeline(
            VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples);
        void createDepthPipeline(
            VkFormat depthFormat, VkSampleCountFlagBits samples, uint32_t framesInFlight);

        Device& device;

        // One entry per visible object, sorted before submission.
        struct DrawItem {
            VkDescriptorSet materialSet = VK_NULL_HANDLE;
            const Material* material = nullptr;
            const Model* model = nullptr;
            glm::mat4 modelMatrix{1.f};
            glm::mat4 normalMatrix{1.f};
        };

        // A run of consecutive draw items sharing a mesh and a material, which
        // is what one instanced draw call covers. The sort that groups them is
        // the one that was already there for the sake of descriptor set binds;
        // this is what finally spends it.
        struct Batch {
            VkDescriptorSet materialSet = VK_NULL_HANDLE;
            const Material* material = nullptr;
            const Model* model = nullptr;
            uint32_t firstInstance = 0;
            uint32_t instanceCount = 0;
        };

        void buildBatches();

        std::vector<DrawItem> drawList;
        std::vector<Batch> batches;
        Stats frameStats{};
        // Guards the order the three calls above have to happen in. Drawing
        // from a list gathered for a different frame is the kind of mistake
        // that shows up as flicker on one machine and nothing on another.
        bool gathered = false;

        std::unique_ptr<Pipeline> pipeline;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

        // The pre-pass reads the uniform block and nothing else, and reads it
        // through a set of its own rather than through the global one. Not
        // tidiness: the scene pass writes this frame's shadow maps into the
        // global set, and updating a descriptor set that an earlier pass has
        // already bound invalidates the whole command buffer. The light
        // culling pass keeps its own set for exactly the same reason.
        std::unique_ptr<DescriptorSetLayout> depthSetLayout;
        std::unique_ptr<DescriptorPool> depthPool;
        std::vector<VkDescriptorSet> depthDescriptorSets;
        std::unique_ptr<Pipeline> depthPipeline;
        VkPipelineLayout depthPipelineLayout = VK_NULL_HANDLE;
    };

}  // namespace ege
