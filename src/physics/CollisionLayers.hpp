#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ege {

    // Which collision layer something is in. A small integer at runtime and a
    // name everywhere a person has to read it.
    using CollisionLayer = std::uint8_t;

    // Sixteen, because the point of named layers is a matrix somebody can
    // look at, and a sixteen-by-sixteen grid is the largest one that is still
    // a grid rather than a wall.
    inline constexpr CollisionLayer maxCollisionLayers = 16;

    inline constexpr CollisionLayer invalidCollisionLayer = 0xFF;

    // Named collision layers, and the matrix of which of them meet.
    //
    // Until now the engine had exactly two layers - moving and non-moving -
    // and they were a broad-phase optimisation rather than anything gameplay
    // could name: their whole job was to let the broad phase skip pairs that
    // can never collide because neither of them moves. That job has not
    // changed and is still done underneath this; what this adds is the other
    // kind of layer, the one a designer means when they say the player's
    // shots should pass through the player.
    //
    // Everything collides with everything until told otherwise, because the
    // alternative - a fresh layer that collides with nothing - is a layer
    // whose first symptom is objects falling through the floor.
    //
    // Device-free and backend-free: a matrix of bits and a list of names. The
    // physics backend is handed one and asks it questions.
    class CollisionLayers {
    public:
        // Layer zero always exists, is called "Default", and is what anything
        // that never says otherwise is in.
        static constexpr CollisionLayer defaultLayer = 0;

        CollisionLayers();

        // Adds a layer, or returns the one already called that - so a caller
        // may declare its layers without checking first. Returns
        // invalidCollisionLayer, and says so once, when there is no room
        // left.
        CollisionLayer add(std::string name);

        // The layer called `name`, or invalidCollisionLayer.
        CollisionLayer find(std::string_view name) const;

        std::string_view name(CollisionLayer layer) const;

        std::size_t count() const { return names.size(); }

        // Symmetric by construction: "the player collides with pickups" and
        // "pickups collide with the player" are one fact, and a matrix that
        // could disagree with itself is a matrix that eventually does.
        void setCollides(CollisionLayer a, CollisionLayer b, bool collides);

        // By name, for callers that are declaring a matrix rather than
        // holding handles. A name that does not exist is ignored, and says
        // so: a typo that silently turned collisions off would be found much
        // later and somewhere else.
        void setCollides(std::string_view a, std::string_view b, bool collides);

        bool collides(CollisionLayer a, CollisionLayer b) const;

    private:
        static bool inRange(CollisionLayer layer) { return layer < maxCollisionLayers; }

        std::vector<std::string> names;
        // One bit per layer, per layer. Sixteen layers fit in a sixteen-bit
        // word, so a whole row is one comparison.
        std::array<std::uint16_t, maxCollisionLayers> matrix{};
    };

}  // namespace ege
