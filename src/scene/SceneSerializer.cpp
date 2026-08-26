#include "scene/SceneSerializer.hpp"

#include "core/Log.hpp"
#include "scene/EntitySerialization.hpp"

#include <cstddef>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace ege {

    namespace {

        // Bumped when the on-disk shape changes incompatibly. Recorded so a
        // future loader can migrate rather than guess.
        constexpr int sceneFormatVersion = 1;

        nlohmann::json worldToJson(const World& world) {
            // Iterating live entities requires the non-const query surface;
            // nothing is mutated.
            World& mutableWorld = const_cast<World&>(world);

            const std::vector<Entity> live = mutableWorld.all();

            serialization::Positions positions;
            positions.reserve(live.size());
            for (std::size_t index = 0; index < live.size(); index++) {
                positions.emplace(live[index].id(), index);
            }

            nlohmann::json entities = nlohmann::json::array();
            for (Entity entity : live) {
                entities.push_back(
                    serialization::entityToJson(mutableWorld, entity.id(), positions));
            }

            nlohmann::json scene = nlohmann::json::object();
            scene["version"] = sceneFormatVersion;
            scene["entities"] = std::move(entities);
            return scene;
        }

        void jsonToWorld(World& world, const nlohmann::json& scene) {
            const int version = scene.value("version", 0);
            if (version != sceneFormatVersion) {
                EGE_WARN(
                    "scene written in format version {}, loader expects {}",
                    version,
                    sceneFormatVersion);
            }

            // Clearing before loading rather than merging: load replaces, and a
            // caller wanting a merge should load into a scratch world.
            for (Entity existing : world.all()) {
                existing.despawn();
            }

            const auto entities = scene.find("entities");
            if (entities == scene.end() || !entities->is_array()) {
                EGE_WARN("scene has no entity array");
                return;
            }

            std::vector<EntityId> spawned;
            spawned.reserve(entities->size());

            for (const nlohmann::json& record : *entities) {
                spawned.push_back(serialization::spawnEntityFromJson(world, record));
            }

            // Parenting in a second pass: a child may be written before its
            // parent, and setParent has to have both ends to link them.
            serialization::linkParents(world, spawned, *entities);
        }

    }  // namespace

    std::string SceneSerializer::toString(const World& world) {
        return worldToJson(world).dump(2);
    }

    void SceneSerializer::save(const World& world, const std::filesystem::path& path) {
        std::ofstream file{path};
        if (!file) {
            throw std::runtime_error{"cannot open scene for writing: " + path.string()};
        }
        file << toString(world);
        if (!file) {
            throw std::runtime_error{"failed while writing scene: " + path.string()};
        }
        EGE_INFO("Saved scene to {}", path.string());
    }

    void SceneSerializer::fromString(World& world, const std::string& json) {
        jsonToWorld(world, nlohmann::json::parse(json));
    }

    void SceneSerializer::load(World& world, const std::filesystem::path& path) {
        std::ifstream file{path};
        if (!file) {
            throw std::runtime_error{"cannot open scene for reading: " + path.string()};
        }
        jsonToWorld(world, nlohmann::json::parse(file));
        EGE_INFO("Loaded scene from {} ({} entities)", path.string(), world.entityCount());
    }

}  // namespace ege
