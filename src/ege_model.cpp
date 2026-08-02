#include "ege_model.hpp"

#include "ege_utils.hpp"
// libs
#define TINYOBJLOADER_IMPLEMENTATION
#include <glm/gtc/constants.hpp>
#include <glm/gtx/hash.hpp>
#include <tiny_obj_loader.h>

#include <cassert>
#include <cstring>
#include <unordered_map>

// Root the engine resolves relative asset paths against. Defined by CMake;
// the fallback only covers running with the repository root as the cwd.
#ifndef EGE_ASSET_ROOT
#define EGE_ASSET_ROOT "assets"
#endif

namespace std {
    template<>
    struct hash<ege::EgeModel::Vertex> {
        size_t operator()(ege::EgeModel::Vertex const& vertex) const {
            size_t seed = 0;
            ege::hashCombine(seed, vertex.position, vertex.color, vertex.normal, vertex.uv);
            return seed;
        }
    };
}  // namespace std

namespace ege {

    EgeModel::EgeModel(EgeDevice& device, const EgeModel::Builder& builder) : egeDevice{device} {
        createVertexBuffers(builder.vertices);
        createIndexBuffers(builder.indices);
    }

    EgeModel::~EgeModel() {}

    std::unique_ptr<EgeModel> EgeModel::createModelFromFile(
        EgeDevice& device, const std::string& filepath) {
        Builder builder{};
        builder.loadModel(std::string{EGE_ASSET_ROOT} + "/" + filepath);
        return std::make_unique<EgeModel>(device, builder);
    }

    void EgeModel::createVertexBuffers(const std::vector<Vertex>& vertices) {
        vertexCount = static_cast<uint32_t>(vertices.size());
        assert(vertexCount >= 3 && "Vertex count must be at least 3");
        VkDeviceSize bufferSize = sizeof(vertices[0]) * vertexCount;
        uint32_t vertexSize = sizeof(vertices[0]);

        EgeBuffer stagingBuffer{
            egeDevice,
            vertexSize,
            vertexCount,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        };

        stagingBuffer.map();
        stagingBuffer.writeToBuffer(const_cast<void*>(static_cast<const void*>(vertices.data())));

        vertexBuffer = std::make_unique<EgeBuffer>(
            egeDevice,
            vertexSize,
            vertexCount,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        egeDevice.copyBuffer(stagingBuffer.getBuffer(), vertexBuffer->getBuffer(), bufferSize);
    }

    void EgeModel::createIndexBuffers(const std::vector<uint32_t>& indices) {
        indexCount = static_cast<uint32_t>(indices.size());
        hasIndexBuffer = indexCount > 0;

        if (!hasIndexBuffer) {
            return;
        }

        VkDeviceSize bufferSize = sizeof(indices[0]) * indexCount;

        uint32_t indexSize = sizeof(indices[0]);

        EgeBuffer stagingBuffer{
            egeDevice,
            indexSize,
            indexCount,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        };

        stagingBuffer.map();
        stagingBuffer.writeToBuffer(const_cast<void*>(static_cast<const void*>(indices.data())));

        indexBuffer = std::make_unique<EgeBuffer>(
            egeDevice,
            indexSize,
            indexCount,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        egeDevice.copyBuffer(stagingBuffer.getBuffer(), indexBuffer->getBuffer(), bufferSize);
    }

    void EgeModel::draw(VkCommandBuffer commandBuffer) {
        if (hasIndexBuffer) {
            vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, 0, 0);
        } else {
            vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);
        }
    }

    void EgeModel::bind(VkCommandBuffer commandBuffer) {
        VkBuffer buffers[] = {vertexBuffer->getBuffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);

        if (hasIndexBuffer) {
            vkCmdBindIndexBuffer(commandBuffer, indexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);
        }
    }

    std::vector<VkVertexInputBindingDescription> EgeModel::Vertex::getBindingDescriptions() {
        std::vector<VkVertexInputBindingDescription> bindingDescriptions(1);
        bindingDescriptions[0].binding = 0;
        bindingDescriptions[0].stride = sizeof(Vertex);
        bindingDescriptions[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescriptions;
    }

    std::vector<VkVertexInputAttributeDescription> EgeModel::Vertex::getAttributeDescriptions() {
        std::vector<VkVertexInputAttributeDescription> attributeDescriptions{};

        attributeDescriptions.push_back(
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)});
        attributeDescriptions.push_back(
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)});
        attributeDescriptions.push_back(
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)});
        attributeDescriptions.push_back({3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)});

        return attributeDescriptions;
    }

    void EgeModel::Builder::loadModel(const std::string& filepath) {
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string warn, err;

        if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filepath.c_str())) {
            throw std::runtime_error(warn + err);
        }

        vertices.clear();
        indices.clear();

        std::unordered_map<Vertex, uint32_t> uniqueVertices{};
        for (const auto& shape : shapes) {
            for (const auto& index : shape.mesh.indices) {
                Vertex vertex{};

                if (index.vertex_index >= 0) {
                    vertex.position = {
                        attrib.vertices[3 * static_cast<size_t>(index.vertex_index) + 0],
                        attrib.vertices[3 * static_cast<size_t>(index.vertex_index) + 1],
                        attrib.vertices[3 * static_cast<size_t>(index.vertex_index) + 2],
                    };

                    vertex.color = {
                        attrib.colors[3 * static_cast<size_t>(index.vertex_index) + 0],
                        attrib.colors[3 * static_cast<size_t>(index.vertex_index) + 1],
                        attrib.colors[3 * static_cast<size_t>(index.vertex_index) + 2],
                    };
                }

                if (index.normal_index >= 0) {
                    vertex.normal = {
                        attrib.normals[3 * static_cast<size_t>(index.normal_index) + 0],
                        attrib.normals[3 * static_cast<size_t>(index.normal_index) + 1],
                        attrib.normals[3 * static_cast<size_t>(index.normal_index) + 2],
                    };
                }

                if (index.texcoord_index >= 0) {
                    vertex.uv = {
                        attrib.texcoords[2 * static_cast<size_t>(index.texcoord_index) + 0],
                        attrib.texcoords[2 * static_cast<size_t>(index.texcoord_index) + 1],
                    };
                }

                if (uniqueVertices.count(vertex) == 0) {
                    uniqueVertices[vertex] = static_cast<uint32_t>(vertices.size());
                    vertices.push_back(vertex);
                }
                indices.push_back(uniqueVertices[vertex]);
            }
        }
    }

    EgeModel::Builder EgeModel::Builder::box() {
        Builder builder{};

        // One quad per face. Faces do not share vertices because each carries its
        // own normal.
        const glm::vec3 faceNormals[6] = {
            {1.f, 0.f, 0.f},
            {-1.f, 0.f, 0.f},
            {0.f, 1.f, 0.f},
            {0.f, -1.f, 0.f},
            {0.f, 0.f, 1.f},
            {0.f, 0.f, -1.f},
        };
        // Tangent/bitangent pairs spanning each face, chosen so that
        // cross(tangent, bitangent) points along the face normal.
        const glm::vec3 faceTangents[6] = {
            {0.f, 0.f, -1.f},
            {0.f, 0.f, 1.f},
            {1.f, 0.f, 0.f},
            {1.f, 0.f, 0.f},
            {1.f, 0.f, 0.f},
            {-1.f, 0.f, 0.f},
        };
        const glm::vec3 faceBitangents[6] = {
            {0.f, 1.f, 0.f},
            {0.f, 1.f, 0.f},
            {0.f, 0.f, -1.f},
            {0.f, 0.f, 1.f},
            {0.f, 1.f, 0.f},
            {0.f, 1.f, 0.f},
        };

        for (uint32_t face = 0; face < 6; face++) {
            const glm::vec3 n = faceNormals[face];
            const glm::vec3 t = faceTangents[face];
            const glm::vec3 b = faceBitangents[face];
            const uint32_t base = static_cast<uint32_t>(builder.vertices.size());

            // Corners in (u, v) order: (0,0) (1,0) (1,1) (0,1)
            const glm::vec2 corners[4] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
            for (const glm::vec2& c : corners) {
                Vertex vertex{};
                vertex.position = 0.5f * (n + t * (2.f * c.x - 1.f) + b * (2.f * c.y - 1.f));
                vertex.normal = n;
                vertex.uv = c;
                vertex.color = glm::vec3{1.f};
                builder.vertices.push_back(vertex);
            }

            builder.indices.insert(
                builder.indices.end(),
                {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});
        }

        return builder;
    }

    EgeModel::Builder EgeModel::Builder::plane() {
        Builder builder{};

        // Unit quad in the XZ plane, facing +Y.
        const glm::vec3 positions[4] = {
            {-0.5f, 0.f, -0.5f}, {0.5f, 0.f, -0.5f}, {0.5f, 0.f, 0.5f}, {-0.5f, 0.f, 0.5f}};
        const glm::vec2 uvs[4] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};

        for (uint32_t i = 0; i < 4; i++) {
            Vertex vertex{};
            vertex.position = positions[i];
            vertex.normal = {0.f, 1.f, 0.f};
            vertex.uv = uvs[i];
            vertex.color = glm::vec3{1.f};
            builder.vertices.push_back(vertex);
        }

        // Wound so that cross(p1 - p0, p2 - p0) points along +Y, matching the
        // stored normal.
        builder.indices = {0, 3, 2, 2, 1, 0};
        return builder;
    }

    EgeModel::Builder EgeModel::Builder::sphere(
        uint32_t latitudeSegments, uint32_t longitudeSegments) {
        Builder builder{};

        assert(latitudeSegments >= 2 && "sphere needs at least 2 latitude segments");
        assert(longitudeSegments >= 3 && "sphere needs at least 3 longitude segments");

        // UV sphere. Poles are duplicated per longitude so that each ring shares a
        // consistent u coordinate, which also gives the seam column distinct uvs.
        for (uint32_t lat = 0; lat <= latitudeSegments; lat++) {
            const float v = static_cast<float>(lat) / static_cast<float>(latitudeSegments);
            const float theta = v * glm::pi<float>();
            const float sinTheta = glm::sin(theta);
            const float cosTheta = glm::cos(theta);

            for (uint32_t lon = 0; lon <= longitudeSegments; lon++) {
                const float u = static_cast<float>(lon) / static_cast<float>(longitudeSegments);
                const float phi = u * glm::two_pi<float>();

                Vertex vertex{};
                // Unit radius 0.5 so the sphere matches the box's unit extent.
                vertex.normal = {sinTheta * glm::cos(phi), cosTheta, sinTheta * glm::sin(phi)};
                vertex.position = 0.5f * vertex.normal;
                vertex.uv = {u, v};
                vertex.color = glm::vec3{1.f};
                builder.vertices.push_back(vertex);
            }
        }

        const uint32_t stride = longitudeSegments + 1;
        for (uint32_t lat = 0; lat < latitudeSegments; lat++) {
            for (uint32_t lon = 0; lon < longitudeSegments; lon++) {
                const uint32_t topLeft = lat * stride + lon;
                const uint32_t bottomLeft = topLeft + stride;

                // Skip the degenerate triangle at each pole, where both of the
                // ring's vertices collapse onto the same point.
                if (lat != 0) {
                    builder.indices.insert(
                        builder.indices.end(), {topLeft, topLeft + 1, bottomLeft});
                }
                if (lat != latitudeSegments - 1) {
                    builder.indices.insert(
                        builder.indices.end(), {topLeft + 1, bottomLeft + 1, bottomLeft});
                }
            }
        }

        return builder;
    }
}  // namespace ege