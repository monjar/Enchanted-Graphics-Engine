#pragma once

#include "render/Bounds.hpp"
#include "rhi/Buffer.hpp"
#include "rhi/Device.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace ege {

    class Model {
    public:
        struct Vertex {
            glm::vec3 position;
            glm::vec3 color;
            glm::vec3 normal{};
            glm::vec2 uv{};

            static std::vector<VkVertexInputBindingDescription> getBindingDescriptions();
            static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions();

            bool operator==(const Vertex& other) const {
                return position == other.position && color == other.color &&
                       normal == other.normal && uv == other.uv;
            }
        };

        struct Builder {
            std::vector<Vertex> vertices{};
            std::vector<uint32_t> indices{};
            // Skinning attributes, either empty or exactly as long as
            // `vertices`. They live in their own vertex stream rather than
            // inside Vertex, so the rigid majority of meshes pay nothing for
            // their existence - in memory or in the vertex fetch.
            std::vector<glm::uvec4> joints{};
            std::vector<glm::vec4> weights{};
            // Geometry that will be rewritten every frame. The vertex buffer
            // is then host-visible and stays mapped, so an update is a memcpy
            // rather than a staging copy and a queue submission. That is
            // slower to *draw* on a discrete GPU, and exactly the right trade
            // for something a script is changing continuously.
            bool dynamic = false;

            void loadModel(const std::string& filepath);

            // Procedurally generated primitives. All are unit-sized and centred on
            // the origin, wound counter-clockwise when viewed from outside, with
            // per-face normals for the box/plane and per-vertex normals for the
            // sphere.
            static Builder box();
            static Builder plane();
            static Builder sphere(uint32_t latitudeSegments = 16, uint32_t longitudeSegments = 32);
        };

        Model(Device& device, const Model::Builder& builder);
        ~Model();

        // Delete copy constructor and operator
        Model(const Model& other) = delete;
        Model& operator=(const Model&) = delete;

        static std::unique_ptr<Model> createModelFromFile(
            Device& device, const std::string& filepath);

        // Local-space bounds, computed once at construction from the vertex
        // data. Culling transforms these per frame rather than re-deriving
        // them from vertices, which would defeat the point.
        const Aabb& bounds() const { return localBounds; }

        void bind(VkCommandBuffer commandBuffer);

        // `instanceCount` copies, the first of them numbered `firstInstance`.
        // The numbering is what the shader indexes its transform buffer by, so
        // a batch of objects sharing this mesh points at its own run of that
        // buffer without anything per draw having to say where it starts.
        void draw(
            VkCommandBuffer commandBuffer, uint32_t instanceCount = 1, uint32_t firstInstance = 0);

        // The same draw, with its parameters read out of a buffer the GPU
        // wrote. `commands` must hold a VkDrawIndexedIndirectCommand at
        // `offset` for an indexed model and a VkDrawIndirectCommand for a
        // plain one - which of the two is wanted is what indexed() is for.
        void drawIndirect(VkCommandBuffer commandBuffer, VkBuffer commands, VkDeviceSize offset);

        // What one instance of this model draws - index count when indexed,
        // vertex count otherwise - which is what an indirect command is
        // seeded with.
        uint32_t drawCount() const { return hasIndexBuffer ? indexCount : vertexCount; }

        bool indexed() const { return hasIndexBuffer; }

        // Whether this mesh carries joints and weights, and therefore wants
        // the skinned pipelines. Decided at build, never after.
        bool skinned() const { return skinBuffer != nullptr; }

        // The vertex layout of the skinning stream: binding 1, so the
        // skinned pipelines describe both streams and the rigid ones only
        // the first.
        static std::vector<VkVertexInputBindingDescription> skinnedBindingDescriptions();
        static std::vector<VkVertexInputAttributeDescription> skinnedAttributeDescriptions();

        bool isDynamic() const { return dynamicVertices; }

        // Rewrites the vertices of a dynamic model in place, and with them the
        // bounds, since geometry that moves is geometry whose extent moved.
        // The count must not exceed what the model was built with: the buffer
        // is not resized, because a resize is a new allocation and the caller
        // would have no way to know it happened.
        void updateVertices(const std::vector<Vertex>& vertices);

    private:
        Device& device;
        Aabb localBounds{};
        std::unique_ptr<Buffer> vertexBuffer;
        // The second stream, present only for skinned meshes.
        std::unique_ptr<Buffer> skinBuffer;
        uint32_t vertexCount;
        bool dynamicVertices = false;
        bool hasIndexBuffer = false;
        std::unique_ptr<Buffer> indexBuffer;
        uint32_t indexCount;

        void createVertexBuffers(const std::vector<Vertex>& vertices, bool dynamic);
        void createSkinBuffer(const Builder& builder);
        void createIndexBuffers(const std::vector<uint32_t>& indices);
    };
}  // namespace ege