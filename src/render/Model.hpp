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
        void draw(VkCommandBuffer commandBuffer);

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
        uint32_t vertexCount;
        bool dynamicVertices = false;
        bool hasIndexBuffer = false;
        std::unique_ptr<Buffer> indexBuffer;
        uint32_t indexCount;

        void createVertexBuffers(const std::vector<Vertex>& vertices, bool dynamic);
        void createIndexBuffers(const std::vector<uint32_t>& indices);
    };
}  // namespace ege