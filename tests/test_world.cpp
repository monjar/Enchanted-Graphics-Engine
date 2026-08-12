// The entity-component system.
//
// The cases that matter here are the ones that fail silently rather than
// loudly: a stale handle addressing a recycled slot, a swap-remove leaving the
// sparse index pointing at the wrong dense element, and a query mutating the
// pool it is walking. Each of those corrupts state without crashing, so each
// gets an explicit test.

#include "scene/World.hpp"

#include <doctest/doctest.h>

#include <string>
#include <unordered_set>
#include <vector>

using ege::Entity;
using ege::EntityId;
using ege::With;
using ege::Without;
using ege::World;

namespace {

    struct Position {
        float x = 0.f;
        float y = 0.f;
    };

    struct Velocity {
        float dx = 0.f;
        float dy = 0.f;
    };

    struct Frozen {};

    struct Tag {
        std::string label;
    };

}  // namespace

TEST_CASE("a fresh world is empty") {
    World world;
    CHECK(world.entityCount() == 0);
    CHECK(world.all().empty());
}

TEST_CASE("spawned entities are alive and named") {
    World world;

    Entity first = world.spawn("First");
    Entity second = world.spawn("Second");

    CHECK(first.alive());
    CHECK(second.alive());
    CHECK(first != second);
    CHECK(first.name() == "First");
    CHECK(second.name() == "Second");
    CHECK(world.entityCount() == 2);
}

TEST_CASE("a default-constructed entity is not alive") {
    Entity none;
    CHECK_FALSE(none.alive());
    CHECK_FALSE(static_cast<bool>(none));
}

TEST_CASE("components attach, read back and detach") {
    World world;
    Entity entity = world.spawn();

    CHECK_FALSE(entity.has<Position>());

    entity.attach<Position>(Position{1.f, 2.f});
    REQUIRE(entity.has<Position>());
    CHECK(entity.fetch<Position>().x == doctest::Approx(1.f));

    entity.fetch<Position>().y = 7.f;
    CHECK(entity.find<Position>()->y == doctest::Approx(7.f));

    entity.detach<Position>();
    CHECK_FALSE(entity.has<Position>());
    CHECK(entity.find<Position>() == nullptr);
}

TEST_CASE("find returns null for an absent component, fetch requires one") {
    World world;
    Entity entity = world.spawn();
    CHECK(entity.find<Velocity>() == nullptr);
}

TEST_CASE("despawning removes the entity and its components") {
    World world;
    Entity entity = world.spawn("Doomed");
    entity.attach<Position>();
    entity.attach<Velocity>();

    CHECK(world.count<Position>() == 1);

    entity.despawn();

    CHECK_FALSE(entity.alive());
    CHECK(world.entityCount() == 0);
    CHECK(world.count<Position>() == 0);
    CHECK(world.count<Velocity>() == 0);
}

TEST_CASE("a handle to a despawned entity does not alias its replacement") {
    // The failure this prevents: hold a handle across a despawn, have the slot
    // recycled, and silently operate on whatever took its place.
    World world;

    Entity original = world.spawn("Original");
    original.attach<Position>(Position{5.f, 5.f});
    const EntityId staleId = original.id();

    original.despawn();

    Entity replacement = world.spawn("Replacement");

    // The slot is reused, so the index matches...
    CHECK(replacement.id().index() == staleId.index());
    // ...but the generation differs, so the stale handle is detectably dead.
    CHECK(replacement.id().generation() != staleId.generation());
    CHECK_FALSE(original.alive());
    CHECK(replacement.alive());

    // And the stale handle cannot read the replacement's components.
    replacement.attach<Position>(Position{9.f, 9.f});
    CHECK(original.find<Position>() == nullptr);
    CHECK(replacement.fetch<Position>().x == doctest::Approx(9.f));
}

TEST_CASE("swap-remove keeps every remaining component addressable") {
    // Removing from the middle of a sparse set moves the last element into the
    // hole. If the moved element's sparse entry is not fixed up, it becomes
    // unreachable or aliases another entity's component - silently.
    World world;

    constexpr int count = 64;
    std::vector<Entity> entities;
    for (int i = 0; i < count; i++) {
        Entity entity = world.spawn("e" + std::to_string(i));
        entity.attach<Position>(Position{static_cast<float>(i), 0.f});
        entities.push_back(entity);
    }

    // Remove every third, from the middle outward, so the swap target is
    // frequently an element that is itself removed later.
    for (int i = count - 1; i >= 0; i -= 3) {
        entities[static_cast<std::size_t>(i)].detach<Position>();
    }

    for (int i = 0; i < count; i++) {
        CAPTURE(i);
        Entity entity = entities[static_cast<std::size_t>(i)];
        if (i % 3 == 0) {
            // 63, 60, ... are removed; those are the multiples of 3 counting
            // down from 63, which is every multiple of 3.
            REQUIRE_FALSE(entity.has<Position>());
        } else {
            REQUIRE(entity.has<Position>());
            // The surviving component must still hold its own value, not a
            // neighbour's.
            REQUIRE(entity.fetch<Position>().x == doctest::Approx(static_cast<float>(i)));
        }
    }
}

TEST_CASE("each visits exactly the entities holding every component") {
    World world;

    Entity both = world.spawn("Both");
    both.attach<Position>(Position{1.f, 0.f});
    both.attach<Velocity>(Velocity{1.f, 0.f});

    Entity positionOnly = world.spawn("PositionOnly");
    positionOnly.attach<Position>(Position{2.f, 0.f});

    Entity velocityOnly = world.spawn("VelocityOnly");
    velocityOnly.attach<Velocity>(Velocity{3.f, 0.f});

    world.spawn("Neither");

    std::unordered_set<EntityId::Storage> visited;
    world.each<Position, Velocity>(
        [&](Entity entity, Position&, Velocity&) { visited.insert(entity.id().raw()); });

    CHECK(visited.size() == 1);
    CHECK(visited.count(both.id().raw()) == 1);

    CHECK(world.count<Position>() == 2);
    CHECK(world.count<Velocity>() == 2);
    CHECK(world.count<Position, Velocity>() == 1);
}

TEST_CASE("each hands out references that write through to storage") {
    World world;

    for (int i = 0; i < 10; i++) {
        Entity entity = world.spawn();
        entity.attach<Position>(Position{static_cast<float>(i), 0.f});
        entity.attach<Velocity>(Velocity{1.f, 2.f});
    }

    world.each<Position, Velocity>([](Entity, Position& position, Velocity& velocity) {
        position.x += velocity.dx;
        position.y += velocity.dy;
    });

    int index = 0;
    world.each<Position>([&](Entity, Position& position) {
        CHECK(position.y == doctest::Approx(2.f));
        index++;
    });
    CHECK(index == 10);
}

TEST_CASE("queries over a component nothing has visit nothing") {
    World world;
    world.spawn().attach<Position>();

    int visits = 0;
    world.each<Position, Velocity>([&](Entity, Position&, Velocity&) { visits++; });
    CHECK(visits == 0);
}

TEST_CASE("Without excludes and With requires") {
    World world;

    Entity moving = world.spawn("Moving");
    moving.attach<Position>();
    moving.attach<Velocity>();

    Entity frozen = world.spawn("Frozen");
    frozen.attach<Position>();
    frozen.attach<Velocity>();
    frozen.attach<Frozen>();

    int unfrozen = 0;
    world.each<Position>(Without<Frozen>{}, [&](Entity, Position&) { unfrozen++; });
    CHECK(unfrozen == 1);

    int withVelocity = 0;
    world.each<Position>(With<Velocity>{}, [&](Entity, Position&) { withVelocity++; });
    CHECK(withVelocity == 2);

    int frozenWithVelocity = 0;
    world.each<Position>(With<Frozen>{}, [&](Entity, Position&) { frozenWithVelocity++; });
    CHECK(frozenWithVelocity == 1);
}

TEST_CASE("despawning from inside a query does not corrupt the iteration") {
    // The callback mutating the pool being walked is the classic way to
    // invalidate an iterator mid-loop.
    World world;

    std::vector<Entity> entities;
    for (int i = 0; i < 32; i++) {
        Entity entity = world.spawn();
        entity.attach<Position>(Position{static_cast<float>(i), 0.f});
        entities.push_back(entity);
    }

    int visited = 0;
    world.each<Position>([&](Entity entity, Position& position) {
        visited++;
        if (static_cast<int>(position.x) % 2 == 0) {
            entity.despawn();
        }
    });

    CHECK(visited == 32);
    CHECK(world.entityCount() == 16);
    CHECK(world.count<Position>() == 16);

    // Every survivor is an odd index, and holds its own value.
    world.each<Position>(
        [](Entity, Position& position) { CHECK(static_cast<int>(position.x) % 2 == 1); });
}

TEST_CASE("attaching from inside a query does not corrupt the iteration") {
    World world;

    for (int i = 0; i < 20; i++) {
        world.spawn().attach<Position>(Position{static_cast<float>(i), 0.f});
    }

    world.each<Position>(
        [](Entity entity, Position&) { entity.attach<Velocity>(Velocity{1.f, 1.f}); });

    CHECK(world.count<Position, Velocity>() == 20);
}

TEST_CASE("slots are reused so the index space stays compact") {
    World world;

    std::vector<Entity> entities;
    for (int i = 0; i < 100; i++) {
        entities.push_back(world.spawn());
    }
    for (Entity& entity : entities) {
        entity.despawn();
    }
    for (int i = 0; i < 100; i++) {
        Entity entity = world.spawn();
        // Reused, so no index exceeds the high-water mark.
        CHECK(entity.id().index() < 100);
    }
    CHECK(world.entityCount() == 100);
}

TEST_CASE("entities can be looked up by name") {
    World world;
    world.spawn("Alpha");
    Entity beta = world.spawn("Beta");

    CHECK(world.findByName("Beta") == beta);
    CHECK_FALSE(world.findByName("Gamma").alive());
}

TEST_CASE("components holding non-trivial types survive being moved") {
    // Swap-remove moves components. A component owning heap memory must come
    // through that intact rather than being shallow-copied or double-freed.
    World world;

    std::vector<Entity> entities;
    for (int i = 0; i < 16; i++) {
        Entity entity = world.spawn();
        entity.attach<Tag>(Tag{"label-" + std::to_string(i)});
        entities.push_back(entity);
    }

    entities[3].detach<Tag>();
    entities[7].detach<Tag>();
    entities[15].detach<Tag>();

    for (int i = 0; i < 16; i++) {
        CAPTURE(i);
        if (i == 3 || i == 7 || i == 15) {
            REQUIRE_FALSE(entities[static_cast<std::size_t>(i)].has<Tag>());
        } else {
            REQUIRE(
                entities[static_cast<std::size_t>(i)].fetch<Tag>().label ==
                "label-" + std::to_string(i));
        }
    }
}
