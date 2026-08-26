#include "scene/EntitySerialization.hpp"

#include "core/Log.hpp"
#include "reflect/Serialization.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Hierarchy.hpp"

namespace ege::serialization {

    nlohmann::json entityToJson(World& world, EntityId entity, const Positions& positions) {
        const ComponentRegistry& registry = ComponentRegistry::instance();
        const Serializer& serializer = Serializer::instance();

        nlohmann::json record = nlohmann::json::object();
        record["name"] = world.lookup(entity).name();

        const EntityId parent = hierarchy::parentOf(world, entity);
        // A parent outside the document is simply not written: for a scene
        // there is no such thing, and for a prefab it is the fragment's root
        // being lifted out of whatever it was sitting under.
        if (const auto parentPosition = positions.find(parent); parentPosition != positions.end()) {
            record["parent"] = parentPosition->second;
        }

        nlohmann::json components = nlohmann::json::object();
        for (const ComponentRegistry::Entry& component : registry.all()) {
            if (!component.has(world, entity)) {
                continue;
            }
            void* instance = component.find(world, entity);
            components[component.name] = serializer.write(*component.type, instance);
        }
        record["components"] = std::move(components);
        return record;
    }

    EntityId spawnEntityFromJson(World& world, const nlohmann::json& record) {
        const ComponentRegistry& registry = ComponentRegistry::instance();
        const Serializer& serializer = Serializer::instance();

        Entity entity = world.spawn(record.value("name", std::string{}));

        const auto components = record.find("components");
        if (components == record.end() || !components->is_object()) {
            return entity.id();
        }

        for (const auto& [name, value] : components->items()) {
            const ComponentRegistry::Entry* component = registry.find(name);
            if (component == nullptr) {
                // Skipping rather than failing: a document written by a build
                // with extra components should still open, minus those.
                EGE_WARN("skipping unknown component '{}'", name);
                continue;
            }
            void* instance = component->attach(world, entity.id());
            serializer.read(*component->type, instance, value);
        }
        return entity.id();
    }

    void linkParents(
        World& world, const std::vector<EntityId>& spawned, const nlohmann::json& entities) {
        for (std::size_t index = 0; index < spawned.size() && index < entities.size(); index++) {
            const nlohmann::json& record = entities[index];
            const auto parent = record.find("parent");
            if (parent == record.end() || !parent->is_number_unsigned()) {
                continue;
            }
            const auto parentIndex = parent->get<std::size_t>();
            if (parentIndex >= spawned.size()) {
                EGE_WARN("a document names a parent outside itself; leaving the entity at root");
                continue;
            }
            hierarchy::setParent(world, spawned[index], spawned[parentIndex]);
        }
    }

}  // namespace ege::serialization
