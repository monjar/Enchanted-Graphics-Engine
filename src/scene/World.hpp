#pragma once

#include "core/Assert.hpp"
#include "scene/ComponentPool.hpp"
#include "scene/Entity.hpp"

#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ege {

    class World;

    // Filters for queries. `With` requires a component the callback does not
    // receive; `Without` excludes one.
    template<typename... Ts>
    struct With {};

    template<typename... Ts>
    struct Without {};

    // Convenience handle: an EntityId bound to the world that owns it, so the
    // common operations read as verbs on the entity rather than calls on the
    // world with the entity threaded through.
    //
    //     Entity player = world.spawn("Player");
    //     player.attach<Transform>();
    //     player.fetch<Transform>().translation.y += 1.f;
    //
    // Cheap to copy - a pointer and a 32-bit handle - and safe to keep, because
    // the generation in the handle detects a despawn.
    class Entity {
    public:
        Entity() = default;

        Entity(World& world, EntityId id) : owner{&world}, handle{id} {}

        EntityId id() const { return handle; }

        // False for a default-constructed Entity, and for one whose entity has
        // been despawned.
        bool alive() const;

        explicit operator bool() const { return alive(); }

        template<typename T, typename... Args>
        T& attach(Args&&... args);

        template<typename T>
        void detach();

        template<typename T>
        bool has() const;

        // Asserts the component exists. Use when its absence is a bug.
        template<typename T>
        T& fetch() const;

        // Null when absent. Use when its absence is expected.
        template<typename T>
        T* find() const;

        const std::string& name() const;
        void setName(std::string value);

        void despawn();

        friend bool operator==(const Entity& a, const Entity& b) {
            return a.owner == b.owner && a.handle == b.handle;
        }

        friend bool operator!=(const Entity& a, const Entity& b) { return !(a == b); }

    private:
        World* owner = nullptr;
        EntityId handle{};
    };

    // Container of entities and their components.
    //
    // Sparse-set storage rather than archetypes: constant-time attach, detach
    // and lookup, iteration that touches only occupied memory, and a structure
    // simple enough to be obviously correct. Archetypes iterate a little faster
    // for wide queries but pay for every structural change by moving the
    // entity's whole row between tables, which is the wrong trade while the
    // engine still has no profile to argue from.
    class World {
    public:
        World() = default;

        World(const World&) = delete;
        World& operator=(const World&) = delete;

        // ---- Lifetime ------------------------------------------------------

        Entity spawn(std::string name = {});

        void despawn(EntityId entity);

        bool alive(EntityId entity) const;

        // Live entities, in no particular order.
        std::vector<Entity> all();

        std::size_t entityCount() const { return liveCount; }

        Entity lookup(EntityId id) { return Entity{*this, id}; }

        // First entity with this name, or a null Entity. Linear; for editor and
        // test convenience, not for per-frame use.
        Entity findByName(std::string_view name);

        const std::string& nameOf(EntityId entity) const;
        void setName(EntityId entity, std::string value);

        // ---- Components ----------------------------------------------------

        template<typename T, typename... Args>
        T& attach(EntityId entity, Args&&... args);

        template<typename T>
        void detach(EntityId entity);

        template<typename T>
        bool has(EntityId entity) const;

        template<typename T>
        T& fetch(EntityId entity);

        template<typename T>
        T* find(EntityId entity);

        // ---- Queries -------------------------------------------------------
        //
        // each() is the primary way systems are written. It iterates the
        // smallest pool among the requested components and tests the rest,
        // so the cost is bounded by the rarest component rather than by the
        // entity count.
        //
        //     world.each<Transform, PointLight>(
        //         [](Entity e, Transform& t, PointLight& l) { ... });
        //
        //     world.each<Transform>(Without<Frozen>{},
        //         [](Entity e, Transform& t) { ... });

        template<typename... Components, typename Fn>
        void each(Fn&& fn);

        template<typename... Components, typename... Excluded, typename Fn>
        void each(Without<Excluded...>, Fn&& fn);

        template<typename... Components, typename... Required, typename Fn>
        void each(With<Required...>, Fn&& fn);

        // Number of entities matching a component set, without visiting them
        // through a callback.
        template<typename... Components>
        std::size_t count();

        // ---- Reflection-facing ---------------------------------------------

        // Pools that exist, for the serializer and the inspector.
        const std::unordered_map<std::type_index, std::unique_ptr<ComponentPoolBase>>& pools()
            const {
            return componentPools;
        }

    private:
        template<typename T>
        ComponentPool<T>& poolFor();

        template<typename T>
        const ComponentPool<T>* poolIfExists() const;

        // Chooses the pool with the fewest entries among the requested
        // components, so iteration walks the shortest list.
        template<typename... Components>
        const std::vector<EntityId>* smallestPoolEntities();

        struct EntitySlot {
            EntityId::Storage generation = 0;
            bool alive = false;
            std::string name;
        };

        std::vector<EntitySlot> slots;
        // Indices of despawned slots, reused before the array grows, so index
        // space stays compact.
        std::vector<EntityId::Storage> freeList;
        std::size_t liveCount = 0;

        std::unordered_map<std::type_index, std::unique_ptr<ComponentPoolBase>> componentPools;
    };

}  // namespace ege

#include "scene/World.inl"
