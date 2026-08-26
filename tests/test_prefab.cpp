// Prefabs: a scene fragment written out and stamped back in.
//
// All of it is the scene serializer's machinery over a subtree, so what these
// check is the part that is *different*: that only the subtree is written,
// that instantiating adds rather than replaces, that the copy is a copy and
// not a link, and that a fragment saved with internal parenting re-links to
// itself rather than to whatever ids it happened to have.

#include "assets/AssetSerialization.hpp"
#include "core/Guid.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/Prefab.hpp"

#include <doctest/doctest.h>

#include <string>

using ege::Entity;
using ege::Transform;
using ege::World;

namespace {

    void registerEverything() {
        ege::registerBuiltinTypes();
        ege::registerBuiltinSerializers();
        ege::registerBuiltinComponents();
    }

    // A cart with a wheel under it, both named and both placed.
    Entity buildCart(World& world) {
        Entity cart = world.spawn("Cart");
        Transform cartTransform{};
        cartTransform.translation = {1.f, 2.f, 3.f};
        cartTransform.scale = glm::vec3{2.f};
        cart.attach<Transform>(cartTransform);

        Entity wheel = world.spawn("Wheel");
        Transform wheelTransform{};
        wheelTransform.translation = {0.f, -0.5f, 0.f};
        wheel.attach<Transform>(wheelTransform);
        ege::hierarchy::setParent(world, wheel.id(), cart.id());
        return cart;
    }

}  // namespace

TEST_CASE("a prefab holds the subtree and nothing else") {
    registerEverything();

    World world;
    Entity cart = buildCart(world);
    // Something else entirely, which must not end up in the fragment.
    world.spawn("Bystander").attach<Transform>();

    const std::string document = ege::prefab::write(world, cart.id());

    CHECK(document.find("Cart") != std::string::npos);
    CHECK(document.find("Wheel") != std::string::npos);
    CHECK(document.find("Bystander") == std::string::npos);
}

TEST_CASE("spawning a prefab adds to the world rather than replacing it") {
    registerEverything();

    World source;
    const std::string document = ege::prefab::write(source, buildCart(source).id());

    World world;
    world.spawn("Already here").attach<Transform>();

    Entity first = ege::prefab::spawn(world, document);
    REQUIRE(first.alive());
    CHECK(first.name() == "Cart");
    // One that was there, plus a cart and its wheel.
    CHECK(world.entityCount() == 3);

    // Stamped twice, because that is what a prefab is for. The second is a
    // second set of entities, not a second reference to the first.
    Entity second = ege::prefab::spawn(world, document);
    REQUIRE(second.alive());
    CHECK(second != first);
    CHECK(world.entityCount() == 5);

    // And they are independent: moving one does not move the other, which is
    // the difference between a copy and a link.
    first.fetch<Transform>().translation = {9.f, 9.f, 9.f};
    CHECK(second.fetch<Transform>().translation.x == doctest::Approx(1.f));
}

TEST_CASE("a stamped fragment re-links to itself") {
    registerEverything();

    World source;
    const std::string document = ege::prefab::write(source, buildCart(source).id());

    World world;
    Entity first = ege::prefab::spawn(world, document);
    Entity second = ege::prefab::spawn(world, document);

    // Each wheel is under its own cart. Parenting is written as a position
    // inside the document, so the second stamping resolves those positions to
    // its own entities rather than to the first stamping's.
    const std::vector<ege::EntityId> firstChildren = ege::hierarchy::childrenOf(world, first.id());
    const std::vector<ege::EntityId> secondChildren =
        ege::hierarchy::childrenOf(world, second.id());
    REQUIRE(firstChildren.size() == 1);
    REQUIRE(secondChildren.size() == 1);
    CHECK(firstChildren.front() != secondChildren.front());
    CHECK(world.lookup(firstChildren.front()).name() == "Wheel");

    // The root came out of the fragment without whatever it used to sit
    // under, which is what makes a fragment liftable at all.
    CHECK(ege::hierarchy::parentOf(world, first.id()).isNull());
}

TEST_CASE("components and their values survive the round trip") {
    registerEverything();

    World source;
    Entity cart = buildCart(source);

    World world;
    Entity copy = ege::prefab::spawn(world, ege::prefab::write(source, cart.id()));
    REQUIRE(copy.alive());

    const Transform& transform = copy.fetch<Transform>();
    CHECK(transform.translation.x == doctest::Approx(1.f));
    CHECK(transform.translation.z == doctest::Approx(3.f));
    CHECK(transform.scale.y == doctest::Approx(2.f));
}

TEST_CASE("a prefab that will not read costs the spawn and nothing else") {
    registerEverything();

    World world;
    world.spawn("Already here");

    CHECK_FALSE(ege::prefab::spawn(world, "{ not json at all").alive());
    CHECK_FALSE(ege::prefab::spawn(world, R"({"version": 1, "entities": []})").alive());
    CHECK_FALSE(ege::prefab::spawn(world, R"({"version": 1})").alive());
    // An unresolved reference is the same answer: a spawn that did not
    // happen, and a world that is untouched.
    CHECK_FALSE(ege::prefab::spawn(world, ege::PrefabRef{}).alive());
    CHECK(world.entityCount() == 1);
}

TEST_CASE("procedural asset ids are derived from names, and the prefab knows them") {
    // The committed pickup prefab names the demo's procedural box mesh and
    // Pickup material by id, and those ids come from these two strings.
    // `scripts/make_pickup_prefab.py` reimplements this derivation to write
    // the file; this is what keeps the two honest about each other.
    CHECK(ege::Guid::fromName("mesh:box").toString() == "501c1205fb5c65efd01a229cbf2d53ad");
    CHECK(ege::Guid::fromName("material:Pickup").toString() == "5a0ef28e2d933ebac9fafc0b1aba3d4c");
}
