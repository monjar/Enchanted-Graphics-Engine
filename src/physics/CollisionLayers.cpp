#include "physics/CollisionLayers.hpp"

#include "core/Log.hpp"

#include <algorithm>

namespace ege {

    CollisionLayers::CollisionLayers() {
        names.emplace_back("Default");
        // Everything collides with everything. A new layer that collided with
        // nothing would look like objects falling through the floor, which is
        // a long way from the mistake that caused it.
        matrix.fill(0xFFFF);
    }

    CollisionLayer CollisionLayers::add(std::string name) {
        if (const CollisionLayer existing = find(name); existing != invalidCollisionLayer) {
            return existing;
        }
        if (names.size() >= maxCollisionLayers) {
            EGE_ERROR(
                "collision layers are full at {}; '{}' was not added and its bodies will be "
                "in the default layer",
                static_cast<int>(maxCollisionLayers),
                name);
            return invalidCollisionLayer;
        }
        names.push_back(std::move(name));
        return static_cast<CollisionLayer>(names.size() - 1);
    }

    CollisionLayer CollisionLayers::find(std::string_view name) const {
        const auto found = std::find(names.begin(), names.end(), name);
        return found == names.end()
                   ? invalidCollisionLayer
                   : static_cast<CollisionLayer>(std::distance(names.begin(), found));
    }

    std::string_view CollisionLayers::name(CollisionLayer layer) const {
        return layer < names.size() ? std::string_view{names[layer]} : std::string_view{};
    }

    void CollisionLayers::setCollides(CollisionLayer a, CollisionLayer b, bool collides) {
        if (!inRange(a) || !inRange(b)) {
            return;
        }
        const auto bitA = static_cast<std::uint16_t>(1u << a);
        const auto bitB = static_cast<std::uint16_t>(1u << b);
        if (collides) {
            matrix[a] = static_cast<std::uint16_t>(matrix[a] | bitB);
            matrix[b] = static_cast<std::uint16_t>(matrix[b] | bitA);
        } else {
            matrix[a] = static_cast<std::uint16_t>(matrix[a] & ~bitB);
            matrix[b] = static_cast<std::uint16_t>(matrix[b] & ~bitA);
        }
    }

    void CollisionLayers::setCollides(std::string_view a, std::string_view b, bool collides) {
        const CollisionLayer first = find(a);
        const CollisionLayer second = find(b);
        if (first == invalidCollisionLayer || second == invalidCollisionLayer) {
            EGE_WARN(
                "collision matrix names a layer that does not exist ('{}' or '{}'); ignored", a, b);
            return;
        }
        setCollides(first, second, collides);
    }

    bool CollisionLayers::collides(CollisionLayer a, CollisionLayer b) const {
        if (!inRange(a) || !inRange(b)) {
            return false;
        }
        return (matrix[a] & static_cast<std::uint16_t>(1u << b)) != 0;
    }

}  // namespace ege
