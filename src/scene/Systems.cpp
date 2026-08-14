#include "scene/Systems.hpp"

#include "assets/AssetDatabase.hpp"
#include "core/Log.hpp"
#include "render/DynamicMesh.hpp"
#include "scene/Components.hpp"

namespace ege::systems {

    void uploadDynamicMeshes(World& world, Device& device) {
        world.each<DynamicMesh, MeshRenderer>(
            [&](Entity, DynamicMesh& mesh, MeshRenderer& renderer) {
                if (!mesh.dirty || mesh.vertices.empty() || mesh.indices.empty()) {
                    return;
                }
                mesh.dirty = false;

                if (mesh.gpu == nullptr) {
                    Model::Builder builder{};
                    builder.vertices = mesh.vertices;
                    builder.indices = mesh.indices;
                    builder.dynamic = true;
                    mesh.gpu = std::make_shared<Model>(device, builder);
                    EGE_DEBUG(
                        "dynamic mesh built: {} vertices, {} indices",
                        mesh.vertices.size(),
                        mesh.indices.size());
                } else {
                    mesh.gpu->updateVertices(mesh.vertices);
                }

                // The renderer draws it like anything else. Its asset id stays
                // null on purpose: this geometry has no file and no identity
                // to share - a scene records that the entity has a dynamic
                // mesh, and the script rebuilds the vertices.
                renderer.mesh.value = mesh.gpu;
            });
    }

    void refreshAssetReferences(World& world) {
        AssetDatabase& assets = AssetDatabase::instance();
        world.each<MeshRenderer>([&](Entity, MeshRenderer& renderer) {
            if (!renderer.mesh.id.isNull()) {
                renderer.mesh.value = assets.mesh(renderer.mesh.id);
            }
            if (!renderer.material.id.isNull()) {
                renderer.material.value = assets.material(renderer.material.id);
            }
        });
    }

}  // namespace ege::systems
