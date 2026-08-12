#include "scene/World.hpp"

#include "core/Log.hpp"

#include <algorithm>

namespace ege {

    namespace {

        const std::string emptyName{};

    }  // namespace

    Entity World::spawn(std::string name) {
        EntityId::Storage index = 0;

        if (!freeList.empty()) {
            // Reuse a despawned slot so the index space stays compact, which
            // keeps every component pool's sparse array small.
            index = freeList.back();
            freeList.pop_back();
        } else {
            EGE_VERIFY(
                slots.size() < EntityId::maxEntities,
                "entity limit of {} reached",
                EntityId::maxEntities);
            index = static_cast<EntityId::Storage>(slots.size());
            slots.emplace_back();
        }

        EntitySlot& slot = slots[index];
        slot.alive = true;
        slot.name = std::move(name);
        liveCount++;

        return Entity{*this, EntityId{index, slot.generation}};
    }

    void World::despawn(EntityId entity) {
        if (!alive(entity)) {
            return;
        }

        // Every pool, not just the ones this entity is known to use: the world
        // does not track which components an entity holds, and asking each pool
        // is cheaper than maintaining that bookkeeping on every attach.
        for (auto& [type, pool] : componentPools) {
            pool->remove(entity);
        }

        EntitySlot& slot = slots[entity.index()];
        slot.alive = false;
        slot.name.clear();

        // Bumping the generation is what makes an old handle detectably stale.
        // On wrap the slot is retired rather than reused, because a recycled
        // slot at the same generation would make a stale handle silently valid
        // again - a bug that would surface as one object mysteriously
        // becoming another.
        if (slot.generation == EntityId::maxGeneration) {
            EGE_DEBUG("entity slot {} retired after exhausting its generations", entity.index());
        } else {
            slot.generation++;
            freeList.push_back(entity.index());
        }

        liveCount--;
    }

    bool World::alive(EntityId entity) const {
        if (entity.isNull()) {
            return false;
        }
        const auto index = entity.index();
        return index < slots.size() && slots[index].alive &&
               slots[index].generation == entity.generation();
    }

    std::vector<Entity> World::all() {
        std::vector<Entity> result;
        result.reserve(liveCount);
        for (std::size_t index = 0; index < slots.size(); index++) {
            if (slots[index].alive) {
                result.emplace_back(
                    *this,
                    EntityId{static_cast<EntityId::Storage>(index), slots[index].generation});
            }
        }
        return result;
    }

    Entity World::findByName(std::string_view name) {
        for (std::size_t index = 0; index < slots.size(); index++) {
            if (slots[index].alive && slots[index].name == name) {
                return Entity{
                    *this,
                    EntityId{static_cast<EntityId::Storage>(index), slots[index].generation}};
            }
        }
        return Entity{};
    }

    const std::string& World::nameOf(EntityId entity) const {
        return alive(entity) ? slots[entity.index()].name : emptyName;
    }

    void World::setName(EntityId entity, std::string value) {
        if (alive(entity)) {
            slots[entity.index()].name = std::move(value);
        }
    }

}  // namespace ege
