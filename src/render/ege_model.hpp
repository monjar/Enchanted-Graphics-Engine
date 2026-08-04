#pragma once

#include "rhi/ege_buffer.hpp"
#include "rhi/ege_engine_device.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace ege {

    class EgeModel {
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

        EgeModel(EgeDevice& device, const EgeModel::Builder& builder);
        ~EgeModel();

        // Delete copy constructor and operator
        EgeModel(const EgeModel& other) = delete;
        EgeModel& operator=(const EgeModel&) = delete;

        static std::unique_ptr<EgeModel> createModelFromFile(
            EgeDevice& device, const std::string& filepath);

        void bind(VkCommandBuffer commandBuffer);
        void draw(VkCommandBuffer commandBuffer);

    private:
        EgeDevice& egeDevice;
        std::unique_ptr<EgeBuffer> vertexBuffer;
        uint32_t vertexCount;
        bool hasIndexBuffer = false;
        std::unique_ptr<EgeBuffer> indexBuffer;
        uint32_t indexCount;

        void createVertexBuffers(const std::vector<Vertex>& vertices);
        void createIndexBuffers(const std::vector<uint32_t>& indices);
    };
}  // namespace ege