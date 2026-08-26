// Named collision layers and the matrix between them.
//
// A table of names and a grid of bits, so all of it answers without a
// backend. What the backend then does with it is checked in
// test_physicsworld.cpp, where two bodies that should ignore each other are
// dropped through each other.

#include "physics/CollisionLayers.hpp"

#include <doctest/doctest.h>

using ege::CollisionLayer;
using ege::CollisionLayers;
using ege::invalidCollisionLayer;
using ege::maxCollisionLayers;

TEST_CASE("a fresh set has one layer and everything collides") {
    CollisionLayers layers;
    CHECK(layers.count() == 1);
    CHECK(layers.name(CollisionLayers::defaultLayer) == "Default");

    const CollisionLayer player = layers.add("Player");
    const CollisionLayer pickups = layers.add("Pickups");
    CHECK(layers.count() == 3);

    // The default, and it matters: a new layer that collided with nothing
    // would first be noticed as objects falling through the floor.
    CHECK(layers.collides(player, pickups));
    CHECK(layers.collides(player, CollisionLayers::defaultLayer));
    CHECK(layers.collides(player, player));
}

TEST_CASE("adding a name twice returns the same layer") {
    CollisionLayers layers;
    const CollisionLayer first = layers.add("Player");
    const CollisionLayer again = layers.add("Player");
    CHECK(first == again);
    CHECK(layers.count() == 2);
}

TEST_CASE("layers are found by name and named by number") {
    CollisionLayers layers;
    const CollisionLayer enemies = layers.add("Enemies");

    CHECK(layers.find("Enemies") == enemies);
    CHECK(layers.name(enemies) == "Enemies");
    CHECK(layers.find("Default") == CollisionLayers::defaultLayer);

    // A name nobody declared is not a layer, and does not become one by being
    // asked for.
    CHECK(layers.find("Ghosts") == invalidCollisionLayer);
    CHECK(layers.count() == 2);
}

TEST_CASE("the matrix is symmetric however it is written") {
    CollisionLayers layers;
    const CollisionLayer player = layers.add("Player");
    const CollisionLayer shots = layers.add("Shots");

    layers.setCollides(player, shots, false);
    CHECK_FALSE(layers.collides(player, shots));
    // The same fact from the other side: a matrix that could disagree with
    // itself is a matrix that eventually does.
    CHECK_FALSE(layers.collides(shots, player));

    // And turning one pair off leaves the others alone.
    CHECK(layers.collides(player, CollisionLayers::defaultLayer));
    CHECK(layers.collides(shots, shots));

    layers.setCollides(shots, player, true);
    CHECK(layers.collides(player, shots));
}

TEST_CASE("a layer can be told not to collide with itself") {
    CollisionLayers layers;
    const CollisionLayer debris = layers.add("Debris");
    layers.setCollides(debris, debris, false);
    CHECK_FALSE(layers.collides(debris, debris));
    CHECK(layers.collides(debris, CollisionLayers::defaultLayer));
}

TEST_CASE("the matrix can be written by name") {
    CollisionLayers layers;
    layers.add("Player");
    layers.add("Pickups");

    layers.setCollides("Player", "Pickups", false);
    CHECK_FALSE(layers.collides(layers.find("Player"), layers.find("Pickups")));

    // A name that does not exist is ignored rather than silently creating a
    // layer or turning off the wrong pair - a typo that quietly disabled a
    // collision would be found much later and somewhere else.
    layers.setCollides("Player", "Plyaers", false);
    CHECK(layers.collides(layers.find("Player"), CollisionLayers::defaultLayer));
}

TEST_CASE("running out of layers is refused rather than wrapped") {
    CollisionLayers layers;
    for (int i = 1; i < maxCollisionLayers; i++) {
        CHECK(layers.add("Layer" + std::to_string(i)) != invalidCollisionLayer);
    }
    CHECK(layers.count() == maxCollisionLayers);

    // One past the end is refused. Wrapping to zero would put a body in the
    // default layer while its name said otherwise, which is the kind of thing
    // that is debugged for an afternoon.
    CHECK(layers.add("OneTooMany") == invalidCollisionLayer);
    CHECK(layers.count() == maxCollisionLayers);
    CHECK(layers.find("OneTooMany") == invalidCollisionLayer);
}

TEST_CASE("a layer outside the table collides with nothing and breaks nothing") {
    CollisionLayers layers;
    CHECK_FALSE(layers.collides(invalidCollisionLayer, CollisionLayers::defaultLayer));
    CHECK(layers.name(invalidCollisionLayer).empty());
    // And writing through one is a no-op rather than a stray bit somewhere.
    layers.setCollides(invalidCollisionLayer, CollisionLayers::defaultLayer, false);
    CHECK(layers.collides(CollisionLayers::defaultLayer, CollisionLayers::defaultLayer));
}
