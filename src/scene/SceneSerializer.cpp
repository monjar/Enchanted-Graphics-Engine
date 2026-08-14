#include "scene/SceneSerializer.hpp"

#include "core/Log.hpp"
#include "reflect/Serialization.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Hierarchy.hpp"

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
            const ComponentRegistry& registry = ComponentRegistry::instance();
            const Serializer& serializer = Serializer::instance();

            // Iterating live entities requires the non-const query surface;
            // nothing is mutated.
            World& mutableWorld = const_cast<World&>(world);

            const std::vector<Entity> live = mutableWorld.all();

            // Parenting is recorded as a position in this file's own entity
            // array, not as an EntityId. Handles are indices into a running
            // world's slot table and mean nothing in the next process; the
            // array position is the only identity a file can honestly claim.
            std::unordered_map<EntityId, std::size_t> position;
            position.reserve(live.size());
            for (std::size_t index = 0; index < live.size(); index++) {
                position.emplace(live[index].id(), index);
            }

            nlohmann::json entities = nlohmann::json::array();
            for (Entity entity : live) {
                nlohmann::json record = nlohmann::json::object();
                record["name"] = entity.name();

                const EntityId parent = hierarchy::parentOf(mutableWorld, entity.id());
                const auto parentPosition = position.find(parent);
                if (parentPosition != position.end()) {
                    record["parent"] = parentPosition->second;
                }

                nlohmann::json components = nlohmann::json::object();
                for (const ComponentRegistry::Entry& component : registry.all()) {
                    if (!component.has(mutableWorld, entity.id())) {
                        continue;
                    }
                    void* instance = component.find(mutableWorld, entity.id());
                    components[component.name] = serializer.write(*component.type, instance);
                }

                record["components"] = std::move(components);
                entities.push_back(std::move(record));
            }

            nlohmann::json scene = nlohmann::json::object();
            scene["version"] = sceneFormatVersion;
            scene["entities"] = std::move(entities);
            return scene;
        }

        void jsonToWorld(World& world, const nlohmann::json& scene) {
            const ComponentRegistry& registry = ComponentRegistry::instance();
            const Serializer& serializer = Serializer::instance();

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
                Entity entity = world.spawn(record.value("name", std::string{}));
                spawned.push_back(entity.id());

                const auto components = record.find("components");
                if (components == record.end() || !components->is_object()) {
                    continue;
                }

                for (const auto& [name, value] : components->items()) {
                    const ComponentRegistry::Entry* component = registry.find(name);
                    if (component == nullptr) {
                        // Skipping rather than failing: a scene saved by a build
                        // with extra components should still open, minus those.
                        EGE_WARN("skipping unknown component '{}'", name);
                        continue;
                    }
                    void* instance = component->attach(world, entity.id());
                    serializer.read(*component->type, instance, value);
                }
            }

            // Parenting in a second pass: a child may be written before its
            // parent, and setParent has to have both ends to link them.
            for (std::size_t index = 0; index < spawned.size(); index++) {
                const nlohmann::json& record = (*entities)[index];
                const auto parent = record.find("parent");
                if (parent == record.end() || !parent->is_number_unsigned()) {
                    continue;
                }
                const auto parentIndex = parent->get<std::size_t>();
                if (parentIndex >= spawned.size()) {
                    EGE_WARN("scene names a parent outside the file; leaving the entity at root");
                    continue;
                }
                hierarchy::setParent(world, spawned[index], spawned[parentIndex]);
            }
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
