#pragma once

#include "scene/World.hpp"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>

namespace ege::serialization {

    // One entity, as JSON, and back.
    //
    // Shared by the scene serializer and the prefab one, because a prefab is
    // a scene fragment and the only difference between them is which entities
    // are written and whether loading replaces the world or adds to it. The
    // per-entity part - name, parent, and every reflected component - is one
    // piece of code so that the two cannot drift into writing subtly
    // different entities.
    //
    // Parenting is recorded as a position in the document's own entity array,
    // never as an EntityId. A handle is an index into a running world's slot
    // table and means nothing in the next process, or even in the same
    // process after a despawn; an array position is the only identity a
    // document can honestly claim - and it is also what makes a prefab
    // instantiable twice, since each stamping resolves those positions to a
    // different set of fresh entities.
    using Positions = std::unordered_map<EntityId, std::size_t>;

    nlohmann::json entityToJson(World& world, EntityId entity, const Positions& positions);

    // Spawns the entity and attaches its components. Parenting is deliberately
    // not done here: a child may be written before its parent, so linking is a
    // second pass over the whole document.
    EntityId spawnEntityFromJson(World& world, const nlohmann::json& record);

    // The second pass. `spawned` is indexed by the same positions the document
    // used.
    void linkParents(
        World& world, const std::vector<EntityId>& spawned, const nlohmann::json& entities);

}  // namespace ege::serialization
