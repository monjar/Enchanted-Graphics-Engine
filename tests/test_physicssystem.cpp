// The physics system: the ECS and the physics world kept agreeing.
//
// These tests drive components and transforms only - the same surface
// gameplay and the editor use - and read the results back off components and
// transforms. The physics world underneath is reached through nothing but
// the PhysicsSystem, which is the point: that is the whole coupling.

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"

#include <glm/glm.hpp>

#include <doctest/doctest.h>

using ege::BoxCollider;
using ege::Entity;
using ege::EntityContact;
using ege::PhysicsSystem;
using ege::PhysicsWorld;
using ege::RigidBody;
using ege::SphereCollider;
using ege::Transform;
using ege::World;

namespace {

    constexpr float step = 1.f / 60.f;

    // Conventional Y-up for these tests; the floor's top face is y = 0.
    Entity spawnFloor(World& world) {
        Entity floor = world.spawn("Floor");
        Transform transform{};
        transform.translation = {0.f, -0.5f, 0.f};
        floor.attach<Transform>(transform);
        floor.attach<BoxCollider>(BoxCollider{{10.f, 0.5f, 10.f}, {0.f, 0.f, 0.f}});
        return floor;
    }

    Entity spawnBall(World& world, glm::vec3 position) {
        Entity ball = world.spawn("Ball");
        Transform transform{};
        transform.translation = position;
        ball.attach<Transform>(transform);
        ball.attach<SphereCollider>();
        ball.attach<RigidBody>();
        return ball;
    }

    void simulate(PhysicsSystem& physics, World& world, int steps) {
        for (int i = 0; i < steps; i++) {
            physics.fixedTick(world, step);
        }
    }

}  // namespace

TEST_CASE("a collider without a RigidBody is scenery, and a RigidBody lands on it") {
    World world;
    PhysicsSystem physics{};
    spawnFloor(world);
    Entity ball = spawnBall(world, {0.f, 3.f, 0.f});

    physics.start(world);
    simulate(physics, world, 300);

    // The simulation's result arrives where everything else reads it: the
    // Transform component.
    const Transform& rest = ball.fetch<Transform>();
    CHECK(rest.translation.y == doctest::Approx(0.5f).epsilon(0.05f));
    // The floor was landed on, not moved: no RigidBody means not simulated.
    CHECK(world.findByName("Floor").fetch<Transform>().translation.y == doctest::Approx(-0.5f));

    physics.stop(world);
}

TEST_CASE("scale is applied to the collider when the body is built") {
    World world;
    PhysicsSystem physics{};
    spawnFloor(world);

    Entity ball = spawnBall(world, {0.f, 4.f, 0.f});
    ball.fetch<Transform>().scale = glm::vec3{2.f};

    physics.start(world);
    simulate(physics, world, 300);

    // Radius 0.5 scaled by 2 rests one unit above the floor plane.
    CHECK(ball.fetch<Transform>().translation.y == doctest::Approx(1.f).epsilon(0.05f));
    physics.stop(world);
}

TEST_CASE("a parented body writes its pose back through the parent") {
    World world;
    PhysicsSystem physics{};
    spawnFloor(world);

    Entity anchor = world.spawn("Anchor");
    Transform anchorTransform{};
    anchorTransform.translation = {5.f, 0.f, 0.f};
    anchor.attach<Transform>(anchorTransform);

    Entity ball = spawnBall(world, {0.f, 3.f, 0.f});
    ege::hierarchy::setParent(world, ball.id(), anchor.id());

    physics.start(world);
    simulate(physics, world, 300);

    // The ball fell straight down in world space - which, expressed in its
    // parent's frame, keeps its local x at zero while the world x is the
    // anchor's.
    const Transform& local = ball.fetch<Transform>();
    CHECK(local.translation.x == doctest::Approx(0.f).epsilon(0.01f));
    CHECK(local.translation.y == doctest::Approx(0.5f).epsilon(0.05f));

    const glm::vec3 worldPosition = glm::vec3{ege::hierarchy::worldMatrix(world, ball.id())[3]};
    CHECK(worldPosition.x == doctest::Approx(5.f).epsilon(0.01f));
    physics.stop(world);
}

TEST_CASE("a kinematic body follows the transform and pushes what it meets") {
    World world;
    PhysicsSystem physics{};
    spawnFloor(world);

    // A kinematic slab pushing a resting ball sideways.
    Entity pusher = world.spawn("Pusher");
    Transform pusherTransform{};
    pusherTransform.translation = {-3.f, 0.5f, 0.f};
    pusher.attach<Transform>(pusherTransform);
    pusher.attach<BoxCollider>(BoxCollider{{0.5f, 0.5f, 2.f}, {0.f, 0.f, 0.f}});
    RigidBody kinematic{};
    kinematic.kinematic = true;
    pusher.attach<RigidBody>(kinematic);

    Entity ball = spawnBall(world, {0.f, 0.5f, 0.f});

    physics.start(world);
    for (int i = 0; i < 240; i++) {
        // Driven exactly the way gameplay drives one: write the Transform.
        pusher.fetch<Transform>().translation.x += 1.5f * step;
        ege::hierarchy::markDirty(world, pusher.id());
        physics.fixedTick(world, step);
    }

    // The pusher went where it was written, and the ball is no longer where
    // it rested.
    CHECK(pusher.fetch<Transform>().translation.x > 2.5f);
    CHECK(ball.fetch<Transform>().translation.x > 1.f);
    physics.stop(world);
}

TEST_CASE("bodies follow entities through spawn and despawn") {
    World world;
    PhysicsSystem physics{};
    spawnFloor(world);

    physics.start(world);
    REQUIRE(physics.physicsWorld() != nullptr);
    CHECK(physics.physicsWorld()->bodyCount() == 1);

    // Spawned mid-play: the next tick gives it a body where gameplay put it.
    Entity ball = spawnBall(world, {0.f, 2.f, 0.f});
    physics.fixedTick(world, step);
    CHECK(physics.physicsWorld()->bodyCount() == 2);
    CHECK(ball.fetch<RigidBody>().body != ege::invalidPhysicsBody);

    // Despawned mid-play: the body must not outlive the entity.
    ball.despawn();
    physics.fixedTick(world, step);
    CHECK(physics.physicsWorld()->bodyCount() == 1);

    physics.stop(world);
}

TEST_CASE("start publishes the physics world through the scene and stop retracts it") {
    World world;
    PhysicsSystem physics{};
    spawnFloor(world);
    Entity ball = spawnBall(world, {0.f, 1.f, 0.f});

    CHECK(world.physics() == nullptr);

    physics.start(world);
    REQUIRE(world.physics() != nullptr);
    // The published world answers queries - this is the path a behaviour's
    // raycast takes.
    const auto hit = world.physics()->raycast({0.f, 5.f, 0.f}, {0.f, -1.f, 0.f}, 10.f);
    CHECK(hit.has_value());

    physics.stop(world);
    CHECK(world.physics() == nullptr);
    // The cached handle went with it.
    CHECK(ball.fetch<RigidBody>().body == ege::invalidPhysicsBody);
}

TEST_CASE("contacts come back as entities") {
    World world;
    PhysicsSystem physics{};
    Entity floor = spawnFloor(world);
    Entity ball = spawnBall(world, {0.f, 1.f, 0.f});

    physics.start(world);

    bool touched = false;
    for (int i = 0; i < 120 && !touched; i++) {
        for (const EntityContact& contact : physics.fixedTick(world, step)) {
            const bool pairMatches = (contact.a == floor && contact.b == ball) ||
                                     (contact.a == ball && contact.b == floor);
            touched = touched || pairMatches;
        }
    }
    CHECK(touched);
    physics.stop(world);
}

TEST_CASE("physics components round-trip through a scene file") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinSerializers();
    ege::registerBuiltinComponents();

    World world;
    Entity crate = world.spawn("Crate");
    crate.attach<Transform>();
    crate.attach<BoxCollider>(BoxCollider{{0.3f, 0.4f, 0.5f}, {0.f, 0.1f, 0.f}});
    RigidBody rigidBody{};
    rigidBody.mass = 12.5f;
    rigidBody.kinematic = true;
    rigidBody.restitution = 0.75f;
    rigidBody.sensor = true;
    crate.attach<RigidBody>(rigidBody);

    const std::string saved = ege::SceneSerializer::toString(world);

    World reloaded;
    ege::SceneSerializer::fromString(reloaded, saved);
    Entity restored = reloaded.findByName("Crate");
    REQUIRE(restored.alive());

    const RigidBody& body = restored.fetch<RigidBody>();
    CHECK(body.mass == doctest::Approx(12.5f));
    CHECK(body.kinematic);
    CHECK(body.restitution == doctest::Approx(0.75f));
    CHECK(body.sensor);
    // The runtime handle is not data and must come back empty.
    CHECK(body.body == ege::invalidPhysicsBody);

    const BoxCollider& box = restored.fetch<BoxCollider>();
    CHECK(box.halfExtents.y == doctest::Approx(0.4f));
    CHECK(box.offset.y == doctest::Approx(0.1f));
}
