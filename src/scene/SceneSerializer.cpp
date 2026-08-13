#include "scene/SceneSerializer.hpp"

#include "core/Log.hpp"
#include "reflect/Serialization.hpp"
#include "scene/ComponentRegistry.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

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

            nlohmann::json entities = nlohmann::json::array();
            for (Entity entity : mutableWorld.all()) {
                nlohmann::json record = nlohmann::json::object();
                record["name"] = entity.name();

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

            for (const nlohmann::json& record : *entities) {
                Entity entity = world.spawn(record.value("name", std::string{}));

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
