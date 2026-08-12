#pragma once

#include <cstdint>
#include <functional>

namespace ege {

    // Handle to an entity.
    //
    // A packed index and generation rather than a pointer or a bare index. The
    // index addresses the sparse array; the generation is bumped when the slot
    // is recycled, so a handle to a despawned entity is detectably stale rather
    // than silently addressing whatever took its place. That failure - holding
    // a reference across a despawn and quietly operating on the wrong object -
    // is the one that is hardest to find later.
    //
    // 24 bits of index gives 16 million live entities, 8 bits of generation
    // means a slot must be recycled 256 times before a stale handle can alias.
    class EntityId {
    public:
        using Storage = std::uint32_t;

        static constexpr Storage indexBits = 24;
        static constexpr Storage generationBits = 8;
        static constexpr Storage indexMask = (1u << indexBits) - 1u;
        static constexpr Storage maxGeneration = (1u << generationBits) - 1u;
        static constexpr Storage maxEntities = indexMask;

        constexpr EntityId() = default;

        constexpr EntityId(Storage index, Storage generation)
            : value{(index & indexMask) | (generation << indexBits)} {}

        constexpr Storage index() const { return value & indexMask; }

        constexpr Storage generation() const { return value >> indexBits; }

        constexpr Storage raw() const { return value; }

        // The null handle. Index 0 is reserved so that a default-constructed
        // EntityId is never a valid entity.
        constexpr bool isNull() const { return value == nullValue; }

        friend constexpr bool operator==(EntityId a, EntityId b) { return a.value == b.value; }

        friend constexpr bool operator!=(EntityId a, EntityId b) { return a.value != b.value; }

        static constexpr Storage nullValue = 0xFFFFFFFFu;

    private:
        Storage value = nullValue;
    };

}  // namespace ege

template<>
struct std::hash<ege::EntityId> {
    std::size_t operator()(ege::EntityId id) const noexcept {
        return std::hash<ege::EntityId::Storage>{}(id.raw());
    }
};
