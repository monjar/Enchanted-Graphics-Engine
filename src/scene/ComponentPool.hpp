#pragma once

#include "core/Assert.hpp"
#include "scene/Entity.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace ege {

    // Type-erased handle so World can hold pools of differing component types
    // and still destroy and remove through a common interface.
    class ComponentPoolBase {
    public:
        virtual ~ComponentPoolBase() = default;

        virtual bool contains(EntityId entity) const = 0;
        virtual void remove(EntityId entity) = 0;
        virtual std::size_t size() const = 0;

        // Entities holding this component, in dense storage order.
        virtual const std::vector<EntityId>& entities() const = 0;

        // Untyped address of the component, for reflection-driven code such as
        // the inspector and the serializer.
        virtual void* rawGet(EntityId entity) = 0;
    };

    // Sparse set storage for one component type.
    //
    // Two arrays: a dense one holding components packed with no gaps, so
    // iteration is a linear walk with no branching, and a sparse one indexed by
    // entity index that says where in the dense array each entity's component
    // lives. Lookup, insert and erase are all constant time, and iteration
    // touches only memory that is actually occupied.
    //
    // The cost is that removal swaps the last element into the hole, so the
    // dense order is not stable. Nothing may hold a pointer into the dense
    // array across a mutation - which is why the query API hands out references
    // scoped to a single callback rather than storing them.
    template<typename T>
    class ComponentPool final : public ComponentPoolBase {
    public:
        bool contains(EntityId entity) const override {
            const auto index = entity.index();
            return index < sparse.size() && sparse[index] != invalidIndex;
        }

        std::size_t size() const override { return dense.size(); }

        const std::vector<EntityId>& entities() const override { return denseEntities; }

        template<typename... Args>
        T& emplace(EntityId entity, Args&&... args) {
            EGE_ASSERT(!contains(entity), "entity already has this component");

            const auto index = entity.index();
            if (index >= sparse.size()) {
                sparse.resize(index + 1, invalidIndex);
            }

            sparse[index] = static_cast<std::uint32_t>(dense.size());
            dense.emplace_back(std::forward<Args>(args)...);
            denseEntities.push_back(entity);
            return dense.back();
        }

        T* find(EntityId entity) {
            return contains(entity) ? &dense[sparse[entity.index()]] : nullptr;
        }

        const T* find(EntityId entity) const {
            return contains(entity) ? &dense[sparse[entity.index()]] : nullptr;
        }

        T& get(EntityId entity) {
            EGE_VERIFY(contains(entity), "entity does not have this component");
            return dense[sparse[entity.index()]];
        }

        void* rawGet(EntityId entity) override {
            return contains(entity) ? static_cast<void*>(&dense[sparse[entity.index()]]) : nullptr;
        }

        void remove(EntityId entity) override {
            if (!contains(entity)) {
                return;
            }

            const auto slot = sparse[entity.index()];
            const auto last = static_cast<std::uint32_t>(dense.size() - 1);

            // Swap the last element into the hole so the dense array stays
            // packed, then fix up the sparse entry of whatever was moved.
            if (slot != last) {
                dense[slot] = std::move(dense[last]);
                denseEntities[slot] = denseEntities[last];
                sparse[denseEntities[slot].index()] = slot;
            }

            dense.pop_back();
            denseEntities.pop_back();
            sparse[entity.index()] = invalidIndex;
        }

        // Direct access to packed storage, for systems that want to walk every
        // component without caring which entity owns it.
        std::vector<T>& components() { return dense; }

        const std::vector<T>& components() const { return dense; }

    private:
        static constexpr std::uint32_t invalidIndex = 0xFFFFFFFFu;

        std::vector<T> dense;
        std::vector<EntityId> denseEntities;
        std::vector<std::uint32_t> sparse;
    };

}  // namespace ege
