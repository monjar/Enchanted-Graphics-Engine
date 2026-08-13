// Entity hierarchy.
//
// The two failures that matter are a cycle - which makes every later traversal
// hang rather than return a wrong answer - and a sibling chain left inconsistent
// by an unlink, which loses children silently.

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"

#include <glm/gtc/constants.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using ege::Entity;
using ege::EntityId;
using ege::Hierarchy;
using ege::Transform;
using ege::World;
namespace hierarchy = ege::hierarchy;

namespace {

    Entity spawnAt(World& world, const char* name, glm::vec3 position) {
        Entity entity = world.spawn(name);
        Transform transform{};
        transform.translation = position;
        entity.attach<Transform>(transform);
        return entity;
    }

    bool contains(const std::vector<EntityId>& list, EntityId id) {
        return std::find(list.begin(), list.end(), id) != list.end();
    }

}  // namespace

TEST_CASE("an entity with no hierarchy component is a root with no children") {
    World world;
    Entity lone = spawnAt(world, "Lone", {1.f, 2.f, 3.f});

    CHECK(hierarchy::parentOf(world, lone.id()).isNull());
    CHECK(hierarchy::childrenOf(world, lone.id()).empty());
    // Its world matrix is just its local one.
    const glm::mat4 world0 = hierarchy::worldMatrix(world, lone.id());
    CHECK(world0[3][0] == doctest::Approx(1.f));
}

TEST_CASE("parenting links both directions") {
    World world;
    Entity parent = spawnAt(world, "Parent", {0.f, 0.f, 0.f});
    Entity child = spawnAt(world, "Child", {0.f, 0.f, 0.f});

    REQUIRE(hierarchy::setParent(world, child.id(), parent.id()));

    CHECK(hierarchy::parentOf(world, child.id()) == parent.id());
    const auto children = hierarchy::childrenOf(world, parent.id());
    REQUIRE(children.size() == 1);
    CHECK(children[0] == child.id());
}

TEST_CASE("children are reported in insertion order") {
    World world;
    Entity parent = spawnAt(world, "Parent", {});

    std::vector<EntityId> expected;
    for (int i = 0; i < 5; i++) {
        Entity child = spawnAt(world, "Child", {});
        REQUIRE(hierarchy::setParent(world, child.id(), parent.id()));
        expected.push_back(child.id());
    }

    CHECK(hierarchy::childrenOf(world, parent.id()) == expected);
}

TEST_CASE("reparenting removes the child from its previous parent") {
    World world;
    Entity first = spawnAt(world, "First", {});
    Entity second = spawnAt(world, "Second", {});
    Entity child = spawnAt(world, "Child", {});

    REQUIRE(hierarchy::setParent(world, child.id(), first.id()));
    REQUIRE(hierarchy::setParent(world, child.id(), second.id()));

    CHECK(hierarchy::childrenOf(world, first.id()).empty());
    CHECK(hierarchy::childrenOf(world, second.id()).size() == 1);
    CHECK(hierarchy::parentOf(world, child.id()) == second.id());
}

TEST_CASE("unlinking a middle child keeps the sibling chain intact") {
    // The failure this catches: a broken chain silently drops every sibling
    // after the removed one.
    World world;
    Entity parent = spawnAt(world, "Parent", {});

    std::vector<Entity> children;
    for (int i = 0; i < 5; i++) {
        Entity child = spawnAt(world, "Child", {});
        REQUIRE(hierarchy::setParent(world, child.id(), parent.id()));
        children.push_back(child);
    }

    // Remove from the middle.
    REQUIRE(hierarchy::setParent(world, children[2].id(), EntityId{}));

    const auto remaining = hierarchy::childrenOf(world, parent.id());
    CHECK(remaining.size() == 4);
    CHECK_FALSE(contains(remaining, children[2].id()));
    for (int i : {0, 1, 3, 4}) {
        CAPTURE(i);
        CHECK(contains(remaining, children[static_cast<std::size_t>(i)].id()));
    }

    // And the first and last are still reachable.
    REQUIRE(hierarchy::setParent(world, children[0].id(), EntityId{}));
    REQUIRE(hierarchy::setParent(world, children[4].id(), EntityId{}));
    CHECK(hierarchy::childrenOf(world, parent.id()).size() == 2);
}

TEST_CASE("a cycle is refused rather than created") {
    // Left unguarded this hangs every later traversal, so it has to fail at the
    // point of the attempt.
    World world;
    Entity a = spawnAt(world, "A", {});
    Entity b = spawnAt(world, "B", {});
    Entity c = spawnAt(world, "C", {});

    REQUIRE(hierarchy::setParent(world, b.id(), a.id()));
    REQUIRE(hierarchy::setParent(world, c.id(), b.id()));

    // Direct self-parenting.
    CHECK_FALSE(hierarchy::setParent(world, a.id(), a.id()));
    // Parenting an ancestor under its own descendant.
    CHECK_FALSE(hierarchy::setParent(world, a.id(), c.id()));
    CHECK_FALSE(hierarchy::setParent(world, a.id(), b.id()));

    // The chain is untouched by the refusals.
    CHECK(hierarchy::parentOf(world, a.id()).isNull());
    CHECK(hierarchy::parentOf(world, b.id()) == a.id());
    CHECK(hierarchy::parentOf(world, c.id()) == b.id());
}

TEST_CASE("descendants are reported for the whole subtree") {
    World world;
    Entity root = spawnAt(world, "Root", {});
    Entity a = spawnAt(world, "A", {});
    Entity b = spawnAt(world, "B", {});
    Entity a1 = spawnAt(world, "A1", {});

    REQUIRE(hierarchy::setParent(world, a.id(), root.id()));
    REQUIRE(hierarchy::setParent(world, b.id(), root.id()));
    REQUIRE(hierarchy::setParent(world, a1.id(), a.id()));

    const auto descendants = hierarchy::descendantsOf(world, root.id());
    CHECK(descendants.size() == 3);
    CHECK(contains(descendants, a.id()));
    CHECK(contains(descendants, b.id()));
    CHECK(contains(descendants, a1.id()));

    CHECK(hierarchy::isDescendantOf(world, a1.id(), root.id()));
    CHECK_FALSE(hierarchy::isDescendantOf(world, root.id(), a1.id()));
}

TEST_CASE("world transforms compose parents outermost first") {
    World world;

    Entity parent = spawnAt(world, "Parent", {10.f, 0.f, 0.f});
    Entity child = spawnAt(world, "Child", {5.f, 0.f, 0.f});
    REQUIRE(hierarchy::setParent(world, child.id(), parent.id()));

    const glm::mat4 childWorld = hierarchy::worldMatrix(world, child.id());
    // Child at +5 under a parent at +10 lands at +15.
    CHECK(childWorld[3][0] == doctest::Approx(15.f));
}

TEST_CASE("a parent's rotation carries its children around with it") {
    World world;

    Entity parent = world.spawn("Parent");
    Transform parentTransform{};
    parentTransform.rotation = {0.f, glm::half_pi<float>(), 0.f};
    parent.attach<Transform>(parentTransform);

    Entity child = spawnAt(world, "Child", {1.f, 0.f, 0.f});
    REQUIRE(hierarchy::setParent(world, child.id(), parent.id()));

    const glm::mat4 childWorld = hierarchy::worldMatrix(world, child.id());
    // A quarter turn about Y maps the child's +X offset onto -Z.
    CHECK(childWorld[3][0] == doctest::Approx(0.f).epsilon(1e-5).scale(1.f));
    CHECK(childWorld[3][2] == doctest::Approx(-1.f).epsilon(1e-5));
}

TEST_CASE("three levels compose correctly") {
    World world;
    Entity a = spawnAt(world, "A", {1.f, 0.f, 0.f});
    Entity b = spawnAt(world, "B", {2.f, 0.f, 0.f});
    Entity c = spawnAt(world, "C", {4.f, 0.f, 0.f});

    REQUIRE(hierarchy::setParent(world, b.id(), a.id()));
    REQUIRE(hierarchy::setParent(world, c.id(), b.id()));

    CHECK(hierarchy::worldMatrix(world, c.id())[3][0] == doctest::Approx(7.f));
}

TEST_CASE("moving a parent updates its descendants after markDirty") {
    World world;
    Entity parent = spawnAt(world, "Parent", {0.f, 0.f, 0.f});
    Entity child = spawnAt(world, "Child", {1.f, 0.f, 0.f});
    REQUIRE(hierarchy::setParent(world, child.id(), parent.id()));

    CHECK(hierarchy::worldMatrix(world, child.id())[3][0] == doctest::Approx(1.f));

    parent.fetch<Transform>().translation.x = 100.f;
    hierarchy::markDirty(world, parent.id());

    // Without the dirty propagation the child would still report its cached
    // matrix, which is the whole reason the flag exists.
    CHECK(hierarchy::worldMatrix(world, child.id())[3][0] == doctest::Approx(101.f));
}

TEST_CASE("despawning a subtree removes every entity under it") {
    World world;
    Entity keep = spawnAt(world, "Keep", {});
    Entity root = spawnAt(world, "Root", {});
    Entity a = spawnAt(world, "A", {});
    Entity a1 = spawnAt(world, "A1", {});

    REQUIRE(hierarchy::setParent(world, a.id(), root.id()));
    REQUIRE(hierarchy::setParent(world, a1.id(), a.id()));
    REQUIRE(world.entityCount() == 4);

    hierarchy::despawnSubtree(world, root.id());

    CHECK(world.entityCount() == 1);
    CHECK(keep.alive());
    CHECK_FALSE(root.alive());
    CHECK_FALSE(a.alive());
    CHECK_FALSE(a1.alive());
}

TEST_CASE("unlinking promotes children to the root rather than orphaning them") {
    World world;
    Entity parent = spawnAt(world, "Parent", {});
    Entity child = spawnAt(world, "Child", {});
    REQUIRE(hierarchy::setParent(world, child.id(), parent.id()));

    hierarchy::unlink(world, parent.id());

    CHECK(child.alive());
    // Its parent link must not point at an entity that is about to vanish.
    CHECK(hierarchy::parentOf(world, child.id()).isNull());
}

TEST_CASE("resolveTransforms warms every cached matrix") {
    World world;
    Entity a = spawnAt(world, "A", {1.f, 0.f, 0.f});
    Entity b = spawnAt(world, "B", {2.f, 0.f, 0.f});
    REQUIRE(hierarchy::setParent(world, b.id(), a.id()));

    hierarchy::resolveTransforms(world);

    REQUIRE(b.has<Hierarchy>());
    CHECK_FALSE(b.fetch<Hierarchy>().dirty);
    CHECK(b.fetch<Hierarchy>().worldMatrix[3][0] == doctest::Approx(3.f));
}

TEST_CASE("parenting to a dead entity is refused") {
    World world;
    Entity child = spawnAt(world, "Child", {});
    Entity doomed = spawnAt(world, "Doomed", {});
    const EntityId staleId = doomed.id();
    doomed.despawn();

    CHECK_FALSE(hierarchy::setParent(world, child.id(), staleId));
    CHECK(hierarchy::parentOf(world, child.id()).isNull());
}
