#include "render/DynamicMesh.hpp"

#include <glm/glm.hpp>

namespace ege {

    void DynamicMesh::recalculateNormals() {
        for (Model::Vertex& vertex : vertices) {
            vertex.normal = glm::vec3{0.f};
        }

        for (std::size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3) {
            Model::Vertex& a = vertices[indices[triangle]];
            Model::Vertex& b = vertices[indices[triangle + 1]];
            Model::Vertex& c = vertices[indices[triangle + 2]];

            const glm::vec3 faceNormal =
                glm::cross(b.position - a.position, c.position - a.position);
            a.normal += faceNormal;
            b.normal += faceNormal;
            c.normal += faceNormal;
        }

        for (Model::Vertex& vertex : vertices) {
            // A degenerate triangle contributes a zero cross product, and a
            // vertex surrounded only by those has no normal to recover.
            // Leaving it at zero beats normalising into NaN.
            if (glm::dot(vertex.normal, vertex.normal) > 1e-12f) {
                vertex.normal = glm::normalize(vertex.normal);
            }
        }
    }

    DynamicMesh DynamicMesh::grid(std::uint32_t segments) {
        DynamicMesh mesh{};
        const std::uint32_t side = segments + 1;
        const float step = 1.f / static_cast<float>(segments);

        mesh.vertices.reserve(static_cast<std::size_t>(side) * side);
        for (std::uint32_t z = 0; z < side; z++) {
            for (std::uint32_t x = 0; x < side; x++) {
                const float u = static_cast<float>(x) * step;
                const float v = static_cast<float>(z) * step;

                Model::Vertex vertex{};
                vertex.position = {u - 0.5f, 0.f, v - 0.5f};
                // +Y, matching Builder::plane() and the winding below - the
                // engine's scenes treat -Y as up, so a caller that wants this
                // facing upwards rotates it, exactly as the demo floor does.
                vertex.normal = {0.f, 1.f, 0.f};
                vertex.uv = {u, v};
                vertex.color = {1.f, 1.f, 1.f};
                mesh.vertices.push_back(vertex);
            }
        }

        mesh.indices.reserve(static_cast<std::size_t>(segments) * segments * 6u);
        for (std::uint32_t z = 0; z < segments; z++) {
            for (std::uint32_t x = 0; x < segments; x++) {
                const std::uint32_t topLeft = z * side + x;
                const std::uint32_t topRight = topLeft + 1;
                const std::uint32_t bottomLeft = topLeft + side;
                const std::uint32_t bottomRight = bottomLeft + 1;

                // Counter-clockwise seen from +Y, which is the front face the
                // pipeline keeps and the direction the normals point.
                mesh.indices.push_back(topLeft);
                mesh.indices.push_back(bottomLeft);
                mesh.indices.push_back(topRight);
                mesh.indices.push_back(topRight);
                mesh.indices.push_back(bottomLeft);
                mesh.indices.push_back(bottomRight);
            }
        }

        return mesh;
    }

}  // namespace ege
