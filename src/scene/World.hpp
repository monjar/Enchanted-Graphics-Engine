#pragma once

#include "core/Assert.hpp"
#include "scene/ComponentPool.hpp"
#include "scene/Entity.hpp"
#include "scene/Events.hpp"

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

        // ---- Physics -------------------------------------------------------

        // The physics world the scene's bodies live in while it simulates;
        // null in edit mode. The scene neither owns nor understands it - a
        // forward-declared pointer, carried so gameplay code can reach
        // queries as world().physics()->raycast(...) without the ECS growing
        // a physics dependency. The PhysicsSystem sets it when play begins
        // and clears it when play stops.
        class PhysicsWorld* physics() const { return physicsBackend; }

        void setPhysics(class PhysicsWorld* backend) { physicsBackend = backend; }

        // ---- Audio ---------------------------------------------------------

        // The engine's sound, carried the same way physics and input are and
        // for the same reason: gameplay asks `world().audio()` and the ECS
        // learns nothing about miniaudio.
        //
        // Unlike physics this is never null while the application is
        // running - a machine with no playback device still gets a backend,
        // because a subsystem that can be absent is a subsystem every call
        // site has to check for and nobody ever tests the other branch of.
        class AudioEngine* audio() const { return audioEngine; }

        void setAudio(class AudioEngine* engine) { audioEngine = engine; }

        // ---- Events --------------------------------------------------------

        // The scene's event bus, owned rather than pointed at: unlike physics
        // and input there is nothing underneath it to borrow, and a message
        // between two behaviours needs no device, no window and no backend.
        //
        // It outlives play the way input does, but its listeners do not:
        // stopping clears them, because a subscription is a live behaviour
        // saying "tell me", and Stop is when those behaviours go.
        EventBus& events() { return eventBus; }

        const EventBus& events() const { return eventBus; }

        // ---- Input ---------------------------------------------------------

        // This frame's input, or null where there is no window to read it
        // from - a test, a headless tool, a scene being cooked. Carried the
        // same way and for the same reason as the physics backend: gameplay
        // asks `world().input()`, the ECS learns nothing about GLFW, and a
        // behaviour that wants the keyboard does not have to be handed one
        // through a constructor the editor would also have to know about.
        //
        // Unlike physics this outlives play: the editor reads input too, and
        // a behaviour written to null-check it is a behaviour that works in
        // both.
        class Input* input() const { return inputSource; }

        void setInput(class Input* source) { inputSource = source; }

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

        // Declared before the component pools, so that it is destroyed
        // *after* them: behaviours end their subscriptions as they go, and a
        // bus that had already gone would be a use-after-free on the way
        // down.
        EventBus eventBus;

        std::vector<EntitySlot> slots;
        // Indices of despawned slots, reused before the array grows, so index
        // space stays compact.
        std::vector<EntityId::Storage> freeList;
        std::size_t liveCount = 0;

        std::unordered_map<std::type_index, std::unique_ptr<ComponentPoolBase>> componentPools;

        class PhysicsWorld* physicsBackend = nullptr;
        class AudioEngine* audioEngine = nullptr;
        class Input* inputSource = nullptr;
    };

}  // namespace ege

#include "scene/World.inl"
