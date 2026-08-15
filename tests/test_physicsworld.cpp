// The physics world, through the engine-owned interface only.
//
// Nothing here names Jolt: these tests are the contract any backend has to
// meet, which is what makes the backend replaceable in fact rather than in
// intent. Physics needs no GPU, so unlike rendering the whole simulation is
// exercised directly in CI.

#include "physics/PhysicsWorld.hpp"

#include <glm/glm.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <vector>

using ege::BodyMotion;
using ege::BodySettings;
using ege::BodyShape;
using ege::ContactEvent;
using ege::PhysicsWorld;

namespace {

    constexpr float step = 1.f / 60.f;

    // A 20x1x20 slab whose top face is the plane y = 0.
    BodySettings floorSettings() {
        BodySettings settings{};
        settings.shape = BodyShape::box(glm::vec3{10.f, 0.5f, 10.f});
        settings.motion = BodyMotion::stationary;
        settings.position = {0.f, -0.5f, 0.f};
        return settings;
    }

    BodySettings ballSettings(glm::vec3 position) {
        BodySettings settings{};
        settings.shape = BodyShape::sphere(0.5f);
        settings.position = position;
        return settings;
    }

}  // namespace

TEST_CASE("a dynamic body falls and comes to rest on a stationary one") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());
    const ege::PhysicsBodyId ball = world->addBody(ballSettings({0.f, 3.f, 0.f}));

    CHECK(world->bodyCount() == 2);

    for (int i = 0; i < 300; i++) {
        world->step(step);
    }

    const ege::BodyPose rest = world->pose(ball);
    // Rests with its radius above the floor plane, give or take the collision
    // margin and however much restitution left it settling.
    CHECK(rest.position.y == doctest::Approx(0.5f).epsilon(0.05f));
    CHECK(glm::length(world->linearVelocity(ball)) < 0.01f);
}

TEST_CASE("gravity is the world's to choose") {
    PhysicsWorld::Settings settings{};
    settings.gravity = {0.f, 9.81f, 0.f};  // a -Y-up scene, like the demo's
    auto world = PhysicsWorld::create(settings);
    const ege::PhysicsBodyId ball = world->addBody(ballSettings({0.f, 0.f, 0.f}));

    for (int i = 0; i < 60; i++) {
        world->step(step);
    }

    CHECK(world->pose(ball).position.y > 1.f);
}

TEST_CASE("the same simulation run twice is the same simulation") {
    // Whole-scene determinism is the property Phase 8 promised: same binary,
    // same steps, bit-identical results. Without it a replay drifts and a
    // networked simulation cannot reconcile.
    auto runOnce = [] {
        auto world = PhysicsWorld::create();
        world->addBody(floorSettings());
        std::vector<ege::PhysicsBodyId> balls;
        for (int i = 0; i < 8; i++) {
            BodySettings settings = ballSettings(
                {static_cast<float>(i % 3) * 0.4f - 0.4f,
                 2.f + static_cast<float>(i) * 1.1f,
                 static_cast<float>(i % 2) * 0.3f});
            settings.restitution = 0.4f;
            balls.push_back(world->addBody(settings));
        }
        for (int i = 0; i < 240; i++) {
            world->step(step);
        }
        std::vector<glm::vec3> positions;
        for (const ege::PhysicsBodyId ball : balls) {
            positions.push_back(world->pose(ball).position);
        }
        return positions;
    };

    const std::vector<glm::vec3> first = runOnce();
    const std::vector<glm::vec3> second = runOnce();

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); i++) {
        // Bitwise equality, not approximate: determinism that is almost exact
        // is not determinism.
        CHECK(first[i].x == second[i].x);
        CHECK(first[i].y == second[i].y);
        CHECK(first[i].z == second[i].z);
    }
}

TEST_CASE("a kinematic body arrives where it was sent") {
    auto world = PhysicsWorld::create();
    BodySettings settings{};
    settings.shape = BodyShape::box(glm::vec3{0.5f});
    settings.motion = BodyMotion::kinematic;
    settings.gravityFactor = 0.f;
    const ege::PhysicsBodyId mover = world->addBody(settings);

    ege::BodyPose target{};
    target.position = {2.f, 1.f, -1.f};
    world->moveKinematic(mover, target, step);
    world->step(step);

    const ege::BodyPose pose = world->pose(mover);
    CHECK(pose.position.x == doctest::Approx(2.f));
    CHECK(pose.position.y == doctest::Approx(1.f));
    CHECK(pose.position.z == doctest::Approx(-1.f));
}

TEST_CASE("beginning to touch raises one contact event") {
    auto world = PhysicsWorld::create();
    BodySettings floorBody = floorSettings();
    floorBody.userData = 7;
    world->addBody(floorBody);

    BodySettings ballBody = ballSettings({0.f, 1.f, 0.f});
    ballBody.userData = 42;
    world->addBody(ballBody);

    std::vector<ContactEvent> events;
    for (int i = 0; i < 120; i++) {
        world->step(step);
        for (const ContactEvent& event : world->drainContacts()) {
            events.push_back(event);
        }
    }

    REQUIRE(!events.empty());
    const ContactEvent& touch = events.front();
    const bool pairMatches = (touch.userDataA == 7 && touch.userDataB == 42) ||
                             (touch.userDataA == 42 && touch.userDataB == 7);
    CHECK(pairMatches);
    // The touch happens on the floor plane.
    CHECK(touch.point.y == doctest::Approx(0.f).epsilon(0.1f));
}

TEST_CASE("a sensor reports the overlap and stops nothing") {
    auto world = PhysicsWorld::create();

    BodySettings gate{};
    gate.shape = BodyShape::box(glm::vec3{2.f, 0.25f, 2.f});
    gate.motion = BodyMotion::kinematic;
    gate.sensor = true;
    gate.userData = 1;
    world->addBody(gate);

    BodySettings ball = ballSettings({0.f, 3.f, 0.f});
    ball.userData = 2;
    const ege::PhysicsBodyId falling = world->addBody(ball);

    bool sensed = false;
    for (int i = 0; i < 240; i++) {
        world->step(step);
        for (const ContactEvent& event : world->drainContacts()) {
            if (event.userDataA + event.userDataB == 3) {
                sensed = true;
            }
        }
    }

    CHECK(sensed);
    // Straight through: a trigger that also blocks is just a wall.
    CHECK(world->pose(falling).position.y < -2.f);
}

TEST_CASE("raycast finds the first body along the ray") {
    auto world = PhysicsWorld::create();
    BodySettings floorBody = floorSettings();
    floorBody.userData = 7;
    world->addBody(floorBody);

    const auto hit = world->raycast({0.f, 5.f, 0.f}, {0.f, -1.f, 0.f}, 10.f);
    REQUIRE(hit.has_value());
    CHECK(hit->distance == doctest::Approx(5.f).epsilon(0.01f));
    CHECK(hit->point.y == doctest::Approx(0.f).epsilon(0.01f));
    CHECK(hit->normal.y == doctest::Approx(1.f).epsilon(0.01f));
    CHECK(hit->userData == 7);

    // A ray pointed away from everything hits nothing.
    CHECK(!world->raycast({0.f, 5.f, 0.f}, {0.f, 1.f, 0.f}, 10.f).has_value());
}

TEST_CASE("an impulse sets a resting body moving") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());
    const ege::PhysicsBodyId ball = world->addBody(ballSettings({0.f, 0.5f, 0.f}));

    // Let it settle and fall asleep first, so the test also proves an impulse
    // wakes a sleeping body - the failure mode where a push does nothing
    // because the body had been put to bed.
    for (int i = 0; i < 300; i++) {
        world->step(step);
    }

    world->addImpulse(ball, {4.f, 0.f, 0.f});
    for (int i = 0; i < 30; i++) {
        world->step(step);
    }

    CHECK(world->pose(ball).position.x > 0.5f);
}

TEST_CASE("removing a body removes it") {
    auto world = PhysicsWorld::create();
    const ege::PhysicsBodyId ball = world->addBody(ballSettings({0.f, 1.f, 0.f}));
    CHECK(world->bodyCount() == 1);
    world->removeBody(ball);
    CHECK(world->bodyCount() == 0);
}
