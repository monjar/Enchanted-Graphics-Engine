#pragma once

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

        void bind(VkCommandBuffer commandBuffer);
        void draw(VkCommandBuffer commandBuffer);

    private:
        Device& device;
        std::unique_ptr<Buffer> vertexBuffer;
        uint32_t vertexCount;
        bool hasIndexBuffer = false;
        std::unique_ptr<Buffer> indexBuffer;
        uint32_t indexCount;

        void createVertexBuffers(const std::vector<Vertex>& vertices);
        void createIndexBuffers(const std::vector<uint32_t>& indices);
    };
}  // namespace ege