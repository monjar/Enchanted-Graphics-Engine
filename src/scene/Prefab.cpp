#include "scene/Prefab.hpp"

#include "core/Log.hpp"
#include "scene/EntitySerialization.hpp"
#include "scene/Hierarchy.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace ege::prefab {

    namespace {

        // Bumped when the on-disk shape changes incompatibly, like a scene's.
        constexpr int prefabFormatVersion = 1;

        // The fragment, root first and every parent before its children.
        //
        // The order is not merely tidy: position zero is the contract that
        // makes `spawn` able to hand back a root without searching for one.
        void collect(World& world, EntityId entity, std::vector<EntityId>& out) {
            out.push_back(entity);
            for (const EntityId child : hierarchy::childrenOf(world, entity)) {
                collect(world, child, out);
            }
        }

    }  // namespace

    std::string write(World& world, EntityId root) {
        std::vector<EntityId> fragment;
        collect(world, root, fragment);

        serialization::Positions positions;
        positions.reserve(fragment.size());
        for (std::size_t index = 0; index < fragment.size(); index++) {
            positions.emplace(fragment[index], index);
        }

        nlohmann::json entities = nlohmann::json::array();
        for (const EntityId entity : fragment) {
            entities.push_back(serialization::entityToJson(world, entity, positions));
        }

        nlohmann::json document = nlohmann::json::object();
        document["version"] = prefabFormatVersion;
        document["entities"] = std::move(entities);
        return document.dump(2);
    }

    void save(World& world, EntityId root, const std::filesystem::path& path) {
        std::ofstream file{path};
        if (!file) {
            throw std::runtime_error{"cannot open prefab for writing: " + path.string()};
        }
        file << write(world, root);
        if (!file) {
            throw std::runtime_error{"failed while writing prefab: " + path.string()};
        }
        EGE_INFO("Saved prefab to {}", path.string());
    }

    Entity spawn(World& world, const std::string& document) {
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(document);
        } catch (const std::exception& error) {
            EGE_ERROR("prefab is not valid JSON: {}", error.what());
            return Entity{};
        }

        const int version = parsed.value("version", 0);
        if (version != prefabFormatVersion) {
            EGE_WARN(
                "prefab written in format version {}, loader expects {}",
                version,
                prefabFormatVersion);
        }

        const auto entities = parsed.find("entities");
        if (entities == parsed.end() || !entities->is_array() || entities->empty()) {
            EGE_WARN("prefab has no entities");
            return Entity{};
        }

        std::vector<EntityId> spawned;
        spawned.reserve(entities->size());
        for (const nlohmann::json& record : *entities) {
            spawned.push_back(serialization::spawnEntityFromJson(world, record));
        }
        serialization::linkParents(world, spawned, *entities);

        // Position zero by construction - see `collect`.
        return world.lookup(spawned.front());
    }

    Entity spawn(World& world, const PrefabRef& reference) {
        if (!reference.resolved()) {
            // The same answer a MeshRenderer gives for an unresolved mesh:
            // nothing happens, and the reference still knows what it wanted.
            return Entity{};
        }
        return spawn(world, reference.get()->document);
    }

}  // namespace ege::prefab
