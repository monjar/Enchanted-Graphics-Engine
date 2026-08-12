// Template definitions for scene/World.hpp.

#include <algorithm>
#include <array>

namespace ege {

    // ---- World: component storage -----------------------------------------

    template <typename T>
    ComponentPool<T>& World::poolFor() {
        const std::type_index key{typeid(T)};
        auto found = componentPools.find(key);
        if (found == componentPools.end()) {
            found = componentPools.emplace(key, std::make_unique<ComponentPool<T>>()).first;
        }
        return static_cast<ComponentPool<T>&>(*found->second);
    }

    template <typename T>
    const ComponentPool<T>* World::poolIfExists() const {
        const auto found = componentPools.find(std::type_index{typeid(T)});
        return found == componentPools.end()
                   ? nullptr
                   : static_cast<const ComponentPool<T>*>(found->second.get());
    }

    template <typename T, typename... Args>
    T& World::attach(EntityId entity, Args&&... args) {
        EGE_VERIFY(alive(entity), "cannot attach a component to a dead entity");
        return poolFor<T>().emplace(entity, std::forward<Args>(args)...);
    }

    template <typename T>
    void World::detach(EntityId entity) {
        if (auto* pool = const_cast<ComponentPool<T>*>(poolIfExists<T>())) {
            pool->remove(entity);
        }
    }

    template <typename T>
    bool World::has(EntityId entity) const {
        const auto* pool = poolIfExists<T>();
        return pool != nullptr && pool->contains(entity);
    }

    template <typename T>
    T& World::fetch(EntityId entity) {
        EGE_VERIFY(alive(entity), "cannot fetch a component from a dead entity");
        return poolFor<T>().get(entity);
    }

    template <typename T>
    T* World::find(EntityId entity) {
        if (!alive(entity)) {
            return nullptr;
        }
        auto* pool = const_cast<ComponentPool<T>*>(poolIfExists<T>());
        return pool == nullptr ? nullptr : pool->find(entity);
    }

    // ---- World: queries ----------------------------------------------------

    template <typename... Components>
    const std::vector<EntityId>* World::smallestPoolEntities() {
        const std::array<const ComponentPoolBase*, sizeof...(Components)> candidates{
            poolIfExists<Components>()...};

        const std::vector<EntityId>* smallest = nullptr;
        for (const ComponentPoolBase* pool : candidates) {
            // A missing pool means nothing has that component, so the whole
            // query is empty.
            if (pool == nullptr) {
                return nullptr;
            }
            if (smallest == nullptr || pool->size() < smallest->size()) {
                smallest = &pool->entities();
            }
        }
        return smallest;
    }

    template <typename... Components, typename Fn>
    void World::each(Fn&& fn) {
        const std::vector<EntityId>* driving = smallestPoolEntities<Components...>();
        if (driving == nullptr) {
            return;
        }

        // Copied because the callback may attach or detach components, which
        // can reorder or reallocate the pool being walked. Correctness before
        // speed; a deferred-command buffer is the eventual answer.
        const std::vector<EntityId> snapshot = *driving;

        for (EntityId entity : snapshot) {
            if (!alive(entity)) {
                continue;
            }
            if (!(has<Components>(entity) && ...)) {
                continue;
            }
            fn(Entity{*this, entity}, fetch<Components>(entity)...);
        }
    }

    template <typename... Components, typename... Excluded, typename Fn>
    void World::each(Without<Excluded...>, Fn&& fn) {
        each<Components...>([&](Entity entity, Components&... components) {
            if ((has<Excluded>(entity.id()) || ...)) {
                return;
            }
            fn(entity, components...);
        });
    }

    template <typename... Components, typename... Required, typename Fn>
    void World::each(With<Required...>, Fn&& fn) {
        each<Components...>([&](Entity entity, Components&... components) {
            if (!(has<Required>(entity.id()) && ...)) {
                return;
            }
            fn(entity, components...);
        });
    }

    template <typename... Components>
    std::size_t World::count() {
        std::size_t total = 0;
        each<Components...>([&total](Entity, Components&...) { total++; });
        return total;
    }

    // ---- Entity facade -----------------------------------------------------

    inline bool Entity::alive() const {
        return owner != nullptr && owner->alive(handle);
    }

    template <typename T, typename... Args>
    T& Entity::attach(Args&&... args) {
        EGE_VERIFY(owner != nullptr, "entity is not bound to a world");
        return owner->attach<T>(handle, std::forward<Args>(args)...);
    }

    template <typename T>
    void Entity::detach() {
        if (owner != nullptr) {
            owner->detach<T>(handle);
        }
    }

    template <typename T>
    bool Entity::has() const {
        return owner != nullptr && owner->has<T>(handle);
    }

    template <typename T>
    T& Entity::fetch() const {
        EGE_VERIFY(owner != nullptr, "entity is not bound to a world");
        return owner->fetch<T>(handle);
    }

    template <typename T>
    T* Entity::find() const {
        return owner == nullptr ? nullptr : owner->find<T>(handle);
    }

    inline const std::string& Entity::name() const {
        EGE_VERIFY(owner != nullptr, "entity is not bound to a world");
        return owner->nameOf(handle);
    }

    inline void Entity::setName(std::string value) {
        EGE_VERIFY(owner != nullptr, "entity is not bound to a world");
        owner->setName(handle, std::move(value));
    }

    inline void Entity::despawn() {
        if (owner != nullptr) {
            owner->despawn(handle);
        }
    }

}  // namespace ege
