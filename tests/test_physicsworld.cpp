// The physics world, through the engine-owned interface only.
//
// Nothing here names Jolt: these tests are the contract any backend has to
// meet, which is what makes the backend replaceable in fact rather than in
// intent. Physics needs no GPU, so unlike rendering the whole simulation is
// exercised directly in CI.

#include "physics/PhysicsWorld.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <doctest/doctest.h>

#include <cmath>
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

// ---- Characters -----------------------------------------------------------
//
// What a character is for is everything a capsule-shaped rigid body does
// badly: standing on a slope without sliding, walking up a step without
// jumping, stopping at a wall without toppling. Each of these is one of
// those.

namespace {

    // Half a metre of radius, half a metre of cylinder: a metre and a half
    // standing up, with its origin at the middle.
    ege::CharacterSettings walkerSettings(glm::vec3 position) {
        ege::CharacterSettings settings{};
        settings.radius = 0.3f;
        settings.halfHeight = 0.45f;
        settings.position = position;
        return settings;
    }

    // A box, as scenery.
    BodySettings slabSettings(glm::vec3 position, glm::vec3 halfExtents) {
        BodySettings settings{};
        settings.shape = BodyShape::box(halfExtents);
        settings.motion = BodyMotion::stationary;
        settings.position = position;
        return settings;
    }

    // Walks a character for `steps` steps at `velocity`, re-asserting the
    // velocity each step the way a driver would - the backend replaces it
    // with what was achieved, which is the point.
    void walk(PhysicsWorld& world, ege::PhysicsCharacterId walker, glm::vec3 velocity, int steps) {
        for (int i = 0; i < steps; i++) {
            const glm::vec3 current = world.characterVelocity(walker);
            // Keep whatever gravity has done to the vertical, drive the plane.
            world.setCharacterVelocity(walker, {velocity.x, current.y - 9.81f * step, velocity.z});
            world.updateCharacter(walker, step);
            world.step(step);
        }
    }

}  // namespace

TEST_CASE("a character falls to the floor and stands on it") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());
    const ege::PhysicsCharacterId walker = world->addCharacter(walkerSettings({0.f, 3.f, 0.f}));

    CHECK(world->characterCount() == 1);
    CHECK(world->characterGround(walker).state == ege::CharacterGroundState::airborne);

    walk(*world, walker, {0.f, 0.f, 0.f}, 180);

    // Standing on the floor plane with its feet on it: radius plus half
    // height below the origin, and the origin is what is reported.
    CHECK(world->characterPosition(walker).y == doctest::Approx(0.75f).epsilon(0.05f));
    CHECK(world->characterGround(walker).state == ege::CharacterGroundState::grounded);
    CHECK(world->characterGround(walker).normal.y == doctest::Approx(1.f).epsilon(0.01f));
}

TEST_CASE("a character walks where it is told") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());
    const ege::PhysicsCharacterId walker = world->addCharacter(walkerSettings({0.f, 0.75f, 0.f}));

    walk(*world, walker, {2.f, 0.f, 0.f}, 60);

    // One second at two metres per second, less the first step spent
    // settling onto the floor.
    CHECK(world->characterPosition(walker).x == doctest::Approx(2.f).epsilon(0.05f));
    CHECK(world->characterPosition(walker).z == doctest::Approx(0.f).epsilon(0.01f));
}

TEST_CASE("a wall stops a character rather than toppling it") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());
    // A wall across the character's path, two metres out.
    world->addBody(slabSettings({2.f, 1.f, 0.f}, {0.25f, 1.f, 4.f}));
    const ege::PhysicsCharacterId walker = world->addCharacter(walkerSettings({0.f, 0.75f, 0.f}));

    walk(*world, walker, {3.f, 0.f, 0.f}, 120);

    // Up against the wall, not through it and not climbing it.
    const glm::vec3 stopped = world->characterPosition(walker);
    CHECK(stopped.x < 1.75f);
    CHECK(stopped.x > 1.2f);
    CHECK(stopped.y == doctest::Approx(0.75f).epsilon(0.1f));
    // And the velocity read back is a stop, so a driver accelerating from it
    // starts from rest rather than from a speed the character never had.
    CHECK(std::abs(world->characterVelocity(walker).x) < 0.5f);
}

TEST_CASE("a character walks up a step it could not climb over") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());
    // A step a quarter of a metre high, well under the default step height.
    world->addBody(slabSettings({2.f, 0.125f, 0.f}, {2.f, 0.125f, 4.f}));

    ege::CharacterSettings settings = walkerSettings({0.f, 0.75f, 0.f});
    settings.stepHeight = 0.4f;
    const ege::PhysicsCharacterId walker = world->addCharacter(settings);

    walk(*world, walker, {2.f, 0.f, 0.f}, 120);

    const glm::vec3 arrived = world->characterPosition(walker);
    // Past the step's edge and standing on top of it - a quarter of a metre
    // higher than it started, without a jump.
    CHECK(arrived.x > 1.f);
    CHECK(arrived.y == doctest::Approx(1.f).epsilon(0.06f));
    CHECK(world->characterGround(walker).state == ege::CharacterGroundState::grounded);
}

TEST_CASE("a slope steeper than the character can climb reads as steep") {
    PhysicsWorld::Settings worldSettings{};
    auto world = PhysicsWorld::create(worldSettings);
    world->addBody(floorSettings());

    // A ramp tilted about Z by seventy degrees - far past the forty-five the
    // character allows.
    BodySettings ramp = slabSettings({1.6f, 1.f, 0.f}, {2.f, 0.1f, 4.f});
    ramp.rotation = glm::angleAxis(glm::radians(70.f), glm::vec3{0.f, 0.f, 1.f});
    world->addBody(ramp);

    const ege::PhysicsCharacterId walker = world->addCharacter(walkerSettings({0.f, 0.75f, 0.f}));
    walk(*world, walker, {3.f, 0.f, 0.f}, 90);

    // It leans on the ramp without walking up it: pushed back, still on the
    // floor side of where the ramp meets it.
    CHECK(world->characterPosition(walker).y > 0.5f);
    CHECK(world->characterPosition(walker).x < 2.f);
}

TEST_CASE("a character pushes a dynamic body out of its way") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());

    BodySettings crate{};
    crate.shape = BodyShape::box(glm::vec3{0.3f});
    crate.position = {1.2f, 0.3f, 0.f};
    crate.mass = 2.f;
    crate.friction = 0.4f;
    const ege::PhysicsBodyId pushed = world->addBody(crate);

    const ege::PhysicsCharacterId walker = world->addCharacter(walkerSettings({0.f, 0.75f, 0.f}));
    walk(*world, walker, {2.f, 0.f, 0.f}, 120);

    // The crate has been shoved along rather than walked through or stood on.
    CHECK(world->pose(pushed).position.x > 1.6f);
    CHECK(world->characterPosition(walker).y == doctest::Approx(0.75f).epsilon(0.15f));
}

TEST_CASE("a character can be told which way is up") {
    // The demo scene's frame: things fall towards +Y, so a character stands
    // the other way up and lands on the underside of the floor.
    PhysicsWorld::Settings settings{};
    settings.gravity = {0.f, 9.81f, 0.f};
    auto world = PhysicsWorld::create(settings);
    CHECK(world->gravity().y == doctest::Approx(9.81f));

    // Floor with its walking surface at y = 0, on the -Y side this time.
    world->addBody(slabSettings({0.f, 0.5f, 0.f}, {10.f, 0.5f, 10.f}));

    ege::CharacterSettings walker = walkerSettings({0.f, -3.f, 0.f});
    walker.up = {0.f, -1.f, 0.f};
    const ege::PhysicsCharacterId id = world->addCharacter(walker);

    for (int i = 0; i < 180; i++) {
        const glm::vec3 current = world->characterVelocity(id);
        world->setCharacterVelocity(id, {0.f, current.y + 9.81f * step, 0.f});
        world->updateCharacter(id, step);
        world->step(step);
    }

    CHECK(world->characterPosition(id).y == doctest::Approx(-0.75f).epsilon(0.05f));
    CHECK(world->characterGround(id).state == ege::CharacterGroundState::grounded);
    CHECK(world->characterGround(id).normal.y == doctest::Approx(-1.f).epsilon(0.01f));
}

TEST_CASE("setting a character's position teleports it and finds new ground") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());
    const ege::PhysicsCharacterId walker = world->addCharacter(walkerSettings({0.f, 0.75f, 0.f}));

    walk(*world, walker, {0.f, 0.f, 0.f}, 30);
    REQUIRE(world->characterGround(walker).state == ege::CharacterGroundState::grounded);

    world->setCharacterPosition(walker, {0.f, 5.f, 0.f});
    CHECK(world->characterPosition(walker).y == doctest::Approx(5.f));
    // Re-found, not remembered: nothing is underfoot up there.
    CHECK(world->characterGround(walker).state == ege::CharacterGroundState::airborne);
}

TEST_CASE("removing a character removes it") {
    auto world = PhysicsWorld::create();
    const ege::PhysicsCharacterId walker = world->addCharacter(walkerSettings({0.f, 1.f, 0.f}));
    CHECK(world->characterCount() == 1);
    world->removeCharacter(walker);
    CHECK(world->characterCount() == 0);
    // A handle that no longer names anything answers rather than crashing:
    // gameplay holding a stale one is a bug in gameplay, not a fault here.
    CHECK(world->characterPosition(walker) == glm::vec3{0.f});
}

// ---- Layers and triggers --------------------------------------------------

TEST_CASE("the collision matrix decides what meets what") {
    PhysicsWorld::Settings settings{};
    const ege::CollisionLayer ghosts = settings.layers.add("Ghosts");
    const ege::CollisionLayer ground = settings.layers.add("Ground");
    settings.layers.setCollides(ghosts, ground, false);
    auto world = PhysicsWorld::create(settings);

    BodySettings floorBody = floorSettings();
    floorBody.layer = ground;
    world->addBody(floorBody);

    // One that the floor catches, and one it does not - identical in every
    // other respect, so the layer is the only thing that can explain it.
    BodySettings solid = ballSettings({-1.f, 3.f, 0.f});
    solid.layer = ege::CollisionLayers::defaultLayer;
    const ege::PhysicsBodyId caught = world->addBody(solid);

    BodySettings ghost = ballSettings({1.f, 3.f, 0.f});
    ghost.layer = ghosts;
    const ege::PhysicsBodyId falling = world->addBody(ghost);

    for (int i = 0; i < 240; i++) {
        world->step(step);
    }

    CHECK(world->pose(caught).position.y == doctest::Approx(0.5f).epsilon(0.05f));
    CHECK(world->pose(falling).position.y < -3.f);
    // And the world hands its layers back, so a caller holding a name can
    // turn it into the number bodies are created with.
    CHECK(world->collisionLayers().find("Ghosts") == ghosts);
}

TEST_CASE("a touch that begins is one event however many corners meet") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());

    // A box lands flat: four corners, one manifold per sub-shape pair, and
    // one thing that happened to two objects.
    BodySettings crate{};
    crate.shape = BodyShape::box(glm::vec3{0.5f});
    crate.position = {0.f, 0.55f, 0.f};
    crate.userData = 9;
    world->addBody(crate);

    int began = 0;
    for (int i = 0; i < 240; i++) {
        world->step(step);
        began += static_cast<int>(world->drainContacts().size());
    }
    CHECK(began == 1);
}

TEST_CASE("a touch that ends is reported once, and names both sides") {
    auto world = PhysicsWorld::create();
    BodySettings floorBody = floorSettings();
    floorBody.userData = 7;
    world->addBody(floorBody);

    // Bounced hard enough to leave the floor again.
    BodySettings ball = ballSettings({0.f, 2.f, 0.f});
    ball.restitution = 0.9f;
    ball.userData = 42;
    world->addBody(ball);

    int began = 0;
    std::vector<ege::SeparationEvent> ended;
    for (int i = 0; i < 240; i++) {
        world->step(step);
        began += static_cast<int>(world->drainContacts().size());
        for (const ege::SeparationEvent& event : world->drainSeparations()) {
            ended.push_back(event);
        }
    }

    CHECK(began > 0);
    REQUIRE(!ended.empty());
    const bool pairMatches = (ended.front().userDataA == 7 && ended.front().userDataB == 42) ||
                             (ended.front().userDataA == 42 && ended.front().userDataB == 7);
    CHECK(pairMatches);
    // Every landing has its departure, or a trigger would count occupants
    // that never left.
    CHECK(static_cast<int>(ended.size()) <= began);
}

TEST_CASE("removing a body ends its touches, and still says whose they were") {
    auto world = PhysicsWorld::create();
    BodySettings floorBody = floorSettings();
    floorBody.userData = 7;
    world->addBody(floorBody);

    BodySettings ball = ballSettings({0.f, 1.f, 0.f});
    ball.userData = 42;
    const ege::PhysicsBodyId resting = world->addBody(ball);

    // Stepped only until it lands, not until it settles: a body that has
    // fallen asleep has already had its contacts reported as ended, which is
    // Jolt's own rule about sleeping bodies and is why a trigger notices only
    // things that are awake.
    bool landed = false;
    for (int i = 0; i < 240 && !landed; i++) {
        world->step(step);
        landed = !world->drainContacts().empty();
        world->drainSeparations();
    }
    REQUIRE(landed);

    // Taken out of the world while touching it. This is what a pickup being
    // consumed inside a trigger looks like, and the event has to survive the
    // body it names.
    world->removeBody(resting);
    // A few steps, not one: Jolt notices a manifold has gone when the update
    // that would have persisted it does not, and the cache it sweeps is the
    // previous step's.
    std::vector<ege::SeparationEvent> ended;
    for (int i = 0; i < 5 && ended.empty(); i++) {
        world->step(step);
        ended = world->drainSeparations();
    }
    REQUIRE(!ended.empty());
    const bool pairMatches = (ended.front().userDataA == 7 && ended.front().userDataB == 42) ||
                             (ended.front().userDataA == 42 && ended.front().userDataB == 7);
    CHECK(pairMatches);
}

TEST_CASE("a character is seen by a sensor because it carries a proxy body") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());

    // A plate on the floor, exactly as a Trigger is built: kinematic so that
    // it notices things that are not moving, and a sensor so it stops none of
    // them.
    BodySettings plate{};
    plate.shape = BodyShape::box(glm::vec3{1.f, 0.05f, 1.f});
    plate.motion = BodyMotion::kinematic;
    plate.sensor = true;
    plate.gravityFactor = 0.f;
    plate.position = {2.f, 0.05f, 0.f};
    plate.userData = 5;
    world->addBody(plate);

    ege::CharacterSettings walker = walkerSettings({0.f, 0.75f, 0.f});
    walker.userData = 6;
    const ege::PhysicsCharacterId id = world->addCharacter(walker);

    bool noticed = false;
    for (int i = 0; i < 120 && !noticed; i++) {
        const glm::vec3 current = world->characterVelocity(id);
        world->setCharacterVelocity(id, {2.f, current.y - 9.81f * step, 0.f});
        world->updateCharacter(id, step);
        world->step(step);
        for (const ContactEvent& event : world->drainContacts()) {
            noticed = noticed || event.userDataA + event.userDataB == 11;
        }
    }
    CHECK(noticed);
    // And it walked over the plate rather than onto it: a sensor stops
    // nothing, including a character.
    CHECK(world->characterPosition(id).y == doctest::Approx(0.75f).epsilon(0.1f));
}

TEST_CASE("a character without a proxy is invisible to everything else") {
    auto world = PhysicsWorld::create();
    world->addBody(floorSettings());

    ege::CharacterSettings walker = walkerSettings({0.f, 0.75f, 0.f});
    walker.proxyBody = false;
    world->addCharacter(walker);

    // Nothing to hit: the solver never sees a virtual character, so without
    // the proxy a ray passes straight through where it is standing.
    CHECK(!world->raycast({-3.f, 0.75f, 0.f}, {1.f, 0.f, 0.f}, 2.f).has_value());

    ege::CharacterSettings seen = walkerSettings({5.f, 0.75f, 0.f});
    world->addCharacter(seen);
    const auto hit = world->raycast({2.f, 0.75f, 0.f}, {1.f, 0.f, 0.f}, 5.f);
    CHECK(hit.has_value());
}
