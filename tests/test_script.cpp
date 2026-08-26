// Behaviours.
//
// Three properties carry the feature and each has a way of quietly failing:
// onSpawn runs once and only once play has started, a behaviour's fields
// survive a save and load, and a callback that spawns or despawns entities
// does not corrupt the iteration it was called from.

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Prefab.hpp"
#include "scene/SceneSerializer.hpp"
#include "script/Behavior.hpp"
#include "script/BehaviorRegistry.hpp"
#include "script/Behaviors.hpp"
#include "script/Script.hpp"
#include "script/ScriptSystem.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using ege::BehaviorRegistry;
using ege::Entity;
using ege::SceneSerializer;
using ege::Script;
using ege::ScriptSystem;
using ege::Transform;
using ege::World;

namespace {

    // Counters on the type rather than the instance: the registry builds
    // instances itself, so a test has no handle to look inside until after the
    // fact.
    struct Counters {
        int spawned = 0;
        int ticked = 0;
        int fixedTicked = 0;
        int despawned = 0;
    };

    Counters& counters() {
        static Counters instance;
        return instance;
    }

}  // namespace

namespace probe {

    class Recorder : public ege::Behavior {
    public:
        float value = 1.f;
        std::string label = "unset";

        void onSpawn() override { counters().spawned++; }

        void onTick(float) override { counters().ticked++; }

        void onFixedTick(float) override { counters().fixedTicked++; }

        void onDespawn() override { counters().despawned++; }
    };

    // Spawns another entity from inside a callback, which is the shape that
    // invalidates a pool iteration if the system calls behaviours while it is
    // still walking one.
    class Spawner : public ege::Behavior {
    public:
        void onFixedTick(float) override { world().spawn("spawned").attach<Transform>(); }
    };

    // Despawns itself, the other shape of the same hazard.
    class SelfDespawner : public ege::Behavior {
    public:
        void onFixedTick(float) override { self().despawn(); }
    };

}  // namespace probe

EGE_REFLECT(probe::Recorder)
EGE_FIELD(value);
EGE_FIELD(label);
EGE_REFLECT_END()

EGE_BEHAVIOR(probe::Recorder)
EGE_BEHAVIOR(probe::Spawner)
EGE_BEHAVIOR(probe::SelfDespawner)

namespace {

    void ensureRegistered() {
        ege::registerBuiltinTypes();
        ege::registerBuiltinSerializers();
        ege::registerBuiltinComponents();
    }

    Entity attachBehavior(World& world, const char* name, const char* entityName = "Subject") {
        Entity entity = world.spawn(entityName);
        entity.attach<Transform>();
        Script script{};
        Script::Slot slot{};
        slot.behavior = name;
        script.behaviors.push_back(std::move(slot));
        entity.attach<Script>(std::move(script));
        return entity;
    }

}  // namespace

TEST_CASE("a behaviour registers itself under its type name") {
    ensureRegistered();

    const BehaviorRegistry::Entry* entry = BehaviorRegistry::instance().find("probe::Recorder");
    REQUIRE(entry != nullptr);
    CHECK(entry->name == "probe::Recorder");
    REQUIRE(entry->type != nullptr);
    CHECK(entry->type->fields().size() == 2);

    const std::unique_ptr<ege::Behavior> made = entry->create();
    CHECK(made != nullptr);

    CHECK(BehaviorRegistry::instance().find("probe::NotAThing") == nullptr);
}

TEST_CASE("onSpawn runs exactly once, however many times the system is asked") {
    ensureRegistered();
    counters() = Counters{};

    World world;
    attachBehavior(world, "probe::Recorder");

    ScriptSystem scripts;
    scripts.spawnPending(world);
    scripts.spawnPending(world);
    scripts.spawnPending(world);

    CHECK(counters().spawned == 1);
}

TEST_CASE("a behaviour attached mid-play still gets its onSpawn") {
    ensureRegistered();
    counters() = Counters{};

    World world;
    attachBehavior(world, "probe::Recorder", "First");

    ScriptSystem scripts;
    scripts.spawnPending(world);
    REQUIRE(counters().spawned == 1);

    attachBehavior(world, "probe::Recorder", "Second");
    scripts.spawnPending(world);

    CHECK(counters().spawned == 2);
}

TEST_CASE("ticks reach spawned behaviours and stop after despawnAll") {
    ensureRegistered();
    counters() = Counters{};

    World world;
    attachBehavior(world, "probe::Recorder");

    ScriptSystem scripts;
    scripts.spawnPending(world);
    scripts.tick(world, 0.016f);
    scripts.fixedTick(world, 0.016f);

    CHECK(counters().ticked == 1);
    CHECK(counters().fixedTicked == 1);

    scripts.despawnAll(world);
    CHECK(counters().despawned == 1);

    scripts.tick(world, 0.016f);
    scripts.fixedTick(world, 0.016f);
    CHECK(counters().ticked == 1);
    CHECK(counters().fixedTicked == 1);
}

TEST_CASE("nothing runs before the system is asked to start it") {
    // The editor holds behaviours that are not running. Attaching one is a
    // description of what will happen, and Play is what makes it happen.
    ensureRegistered();
    counters() = Counters{};

    World world;
    attachBehavior(world, "probe::Recorder");

    ScriptSystem scripts;
    scripts.tick(world, 0.016f);
    scripts.fixedTick(world, 0.016f);

    CHECK(counters().spawned == 0);
    CHECK(counters().ticked == 0);
}

TEST_CASE("a behaviour may spawn entities from a callback") {
    ensureRegistered();

    World world;
    attachBehavior(world, "probe::Spawner");

    ScriptSystem scripts;
    scripts.spawnPending(world);
    scripts.fixedTick(world, 0.016f);
    scripts.fixedTick(world, 0.016f);

    CHECK(world.entityCount() == 3);
}

TEST_CASE("a behaviour may despawn its own entity from a callback") {
    ensureRegistered();

    World world;
    attachBehavior(world, "probe::SelfDespawner");
    world.spawn("Bystander").attach<Transform>();

    ScriptSystem scripts;
    scripts.spawnPending(world);
    scripts.fixedTick(world, 0.016f);
    // And again, with the entity now gone: the system must not call into a
    // behaviour whose entity died earlier in the same pass.
    scripts.fixedTick(world, 0.016f);

    CHECK(world.entityCount() == 1);
    CHECK(world.findByName("Bystander").alive());
}

TEST_CASE("an unknown behaviour is skipped rather than crashing the scene") {
    ensureRegistered();

    World world;
    attachBehavior(world, "probe::LongSinceDeleted");

    ScriptSystem scripts;
    scripts.spawnPending(world);
    scripts.fixedTick(world, 0.016f);

    CHECK(world.entityCount() == 1);
}

TEST_CASE("behaviour fields survive a save and load") {
    ensureRegistered();

    World world;
    Entity entity = attachBehavior(world, "probe::Recorder");
    {
        Script& script = entity.fetch<Script>();
        script.behaviors[0].instance =
            BehaviorRegistry::instance().find("probe::Recorder")->create();
        auto* recorder = static_cast<probe::Recorder*>(script.behaviors[0].instance.get());
        recorder->value = 42.5f;
        recorder->label = "kept";
    }

    World reloaded;
    SceneSerializer::fromString(reloaded, SceneSerializer::toString(world));

    Entity restored = reloaded.findByName("Subject");
    REQUIRE(restored.alive());
    const Script* script = restored.find<Script>();
    REQUIRE(script != nullptr);
    REQUIRE(script->behaviors.size() == 1);
    CHECK(script->behaviors[0].behavior == "probe::Recorder");
    // Rebuilt on load, so the inspector has something to show before play
    // starts - and so the fields have somewhere to land.
    REQUIRE(script->behaviors[0].instance != nullptr);
    CHECK_FALSE(script->behaviors[0].spawned);

    const auto* recorder = static_cast<const probe::Recorder*>(script->behaviors[0].instance.get());
    CHECK(recorder->value == doctest::Approx(42.5f));
    CHECK(recorder->label == "kept");
}

TEST_CASE("a behaviour attached by name alone saves the same twice") {
    // A slot carrying a name and nothing else is what code attaching a
    // behaviour writes, and what a module-provided behaviour looks like
    // before the module has been asked to make one. Loading it builds the
    // instance, so a save that left the defaults out would not match the save
    // after it - and the engine's own round-trip check would call that
    // instability without saying where it was.
    ensureRegistered();

    World world;
    Entity entity = attachBehavior(world, "probe::Recorder");
    REQUIRE(entity.fetch<Script>().behaviors[0].instance == nullptr);

    const std::string written = SceneSerializer::toString(world);

    World reloaded;
    SceneSerializer::fromString(reloaded, written);
    CHECK(SceneSerializer::toString(reloaded) == written);
}

TEST_CASE("a scene keeps the fields of a behaviour this build does not have") {
    // Opening a scene in a build that is missing one of its behaviours must
    // not be how that behaviour's settings get deleted.
    ensureRegistered();

    const std::string json = R"({
        "version": 1,
        "entities": [
            {
                "name": "Subject",
                "components": {
                    "ege::Script": {
                        "behaviors": [
                            {"type": "probe::LongSinceDeleted", "fields": {"speed": 3.5}}
                        ]
                    }
                }
            }
        ]
    })";

    World world;
    SceneSerializer::fromString(world, json);
    const std::string rewritten = SceneSerializer::toString(world);

    CHECK(rewritten.find("probe::LongSinceDeleted") != std::string::npos);
    CHECK(rewritten.find("3.5") != std::string::npos);
}

TEST_CASE("several behaviours can share one entity") {
    ensureRegistered();
    counters() = Counters{};

    World world;
    Entity entity = world.spawn("Subject");
    entity.attach<Transform>();
    Script script{};
    for (const char* name : {"probe::Recorder", "ege::Spinner", "probe::Recorder"}) {
        Script::Slot slot{};
        slot.behavior = name;
        script.behaviors.push_back(std::move(slot));
    }
    entity.attach<Script>(std::move(script));

    ScriptSystem scripts;
    scripts.spawnPending(world);
    scripts.fixedTick(world, 1.f / 60.f);

    CHECK(counters().spawned == 2);
    CHECK(counters().fixedTicked == 2);
    // And the engine behaviour among them ran too.
    CHECK(entity.fetch<Transform>().rotation.y > 0.f);
}

TEST_CASE("contacts reach the behaviours on both sides, each from its own side") {
    // Driven end to end: a real falling body, a real floor, the physics
    // system stepping and the script system delivering - because the
    // property being pinned is the wiring, not any one piece of it.
    World world;

    // A behaviour that remembers what it was told it touched.
    class TouchLog : public ege::Behavior {
    public:
        std::vector<ege::Contact> touches;

        void onContact(const ege::Contact& contact) override { touches.push_back(contact); }
    };

    auto attachLog = [&](Entity entity) {
        Script script{};
        Script::Slot slot{};
        slot.behavior = "TouchLog";
        auto log = std::make_shared<TouchLog>();
        slot.instance = log;
        script.behaviors.push_back(std::move(slot));
        entity.attach<Script>(std::move(script));
        return log;
    };

    Entity floor = world.spawn("Floor");
    Transform floorTransform{};
    floorTransform.translation = {0.f, -0.5f, 0.f};
    floor.attach<Transform>(floorTransform);
    floor.attach<ege::BoxCollider>(ege::BoxCollider{{10.f, 0.5f, 10.f}, {0.f, 0.f, 0.f}});
    auto floorLog = attachLog(floor);

    Entity ball = world.spawn("Ball");
    Transform ballTransform{};
    ballTransform.translation = {0.f, 2.f, 0.f};
    ball.attach<Transform>(ballTransform);
    ball.attach<ege::SphereCollider>();
    ball.attach<ege::RigidBody>();
    auto ballLog = attachLog(ball);

    ScriptSystem scripts;
    ege::PhysicsSystem physics{};
    scripts.spawnPending(world);
    physics.start(world);

    for (int i = 0; i < 180; i++) {
        scripts.fixedTick(world, 1.f / 60.f);
        scripts.deliverContacts(world, physics.fixedTick(world, 1.f / 60.f).contacts);
    }
    physics.stop(world);

    // Both sides heard about the same touch.
    REQUIRE(!ballLog->touches.empty());
    REQUIRE(!floorLog->touches.empty());
    CHECK(ballLog->touches.front().other == floor);
    CHECK(floorLog->touches.front().other == ball);

    // Each side's normal points at the other: the ball fell onto the floor,
    // so the floor lies towards -Y of the ball and the ball towards +Y of
    // the floor. Opposite views of one touch, not two copies of one datum.
    CHECK(ballLog->touches.front().normal.y < 0.f);
    CHECK(floorLog->touches.front().normal.y > 0.f);
}

// ---- reloading a script module ---------------------------------------------
//
// A module is a shared library, so none of what it does can be tested without
// one - but almost nothing that can go wrong is about loading. What can go
// wrong is what happens either side of it: a re-registered behaviour must
// replace the old one rather than sit beside it, and the instances built from
// the old one must be rebuilt from the new with what the author wrote intact.
// Both are the registry and the script system, and neither needs a module.

namespace {

    // Two versions of one behaviour, standing in for the same type before and
    // after a module was rebuilt. Same registered name, different code.
    class BreathingV1 : public ege::Behavior {
    public:
        float rate = 1.f;

        void onSpawn() override { spawns++; }

        void onFixedTick(float deltaSeconds) override { travelled += rate * deltaSeconds; }

        // Not reflected, so not carried across a reload - which is the rule
        // this exists to demonstrate.
        float travelled = 0.f;
        int spawns = 0;
    };

    class BreathingV2 : public ege::Behavior {
    public:
        float rate = 1.f;

        void onSpawn() override { spawns++; }

        // The rebuilt version moves twice as fast: what a developer edited.
        void onFixedTick(float deltaSeconds) override { travelled += 2.f * rate * deltaSeconds; }

        float travelled = 0.f;
        int spawns = 0;
    };

}  // namespace

EGE_REFLECT(BreathingV1)
EGE_FIELD(rate);
EGE_REFLECT_END()

EGE_REFLECT(BreathingV2)
EGE_FIELD(rate);
EGE_REFLECT_END()

TEST_CASE("re-registering a behaviour replaces it rather than adding a second") {
    ensureRegistered();
    BehaviorRegistry& registry = BehaviorRegistry::instance();
    const std::size_t before = registry.all().size();
    const std::size_t registrationsBefore = registry.registrations();

    registry.add<BreathingV1>("test::Breathing");
    CHECK(registry.all().size() == before + 1);

    // The reload: the same name, a different type behind it.
    registry.add<BreathingV2>("test::Breathing");
    CHECK(registry.all().size() == before + 1);

    // And the factory hands out the new one.
    const BehaviorRegistry::Entry* entry = registry.find("test::Breathing");
    REQUIRE(entry != nullptr);
    const std::unique_ptr<ege::Behavior> made = entry->create();
    CHECK(dynamic_cast<BreathingV2*>(made.get()) != nullptr);

    // Both registrations count, which is how a module reports what it
    // contributed - counting entries would say the second one did nothing.
    CHECK(registry.registrations() == registrationsBefore + 2);
}

TEST_CASE("describing a type twice does not give it twin fields") {
    ensureRegistered();
    // Every module instantiates the reflection template for the types it
    // touches, and each instantiation registers. Appending rather than
    // replacing would draw every field twice in the inspector and write it
    // twice into a scene file.
    const ege::TypeInfo& first = ege::TypeRegistry::of<BreathingV1>();
    const std::size_t fields = first.fields().size();
    REQUIRE(fields == 1);

    const ege::TypeInfo& again = ege::registerType<BreathingV1>();
    CHECK(&again == &first);
    CHECK(again.fields().size() == fields);
}

TEST_CASE("a reload rebuilds instances and carries the reflected fields across") {
    ensureRegistered();
    ege::World world;
    ScriptSystem scripts;
    BehaviorRegistry& registry = BehaviorRegistry::instance();

    registry.add<BreathingV1>("test::Breathing");

    Entity entity = world.spawn("Breather");
    Script script{};
    Script::Slot slot{};
    slot.behavior = "test::Breathing";
    script.behaviors.push_back(std::move(slot));
    entity.attach<Script>(std::move(script));

    scripts.spawnPending(world);

    {
        Script* live = world.find<Script>(entity.id());
        REQUIRE(live != nullptr);
        auto* behavior = dynamic_cast<BreathingV1*>(live->behaviors[0].instance.get());
        REQUIRE(behavior != nullptr);
        CHECK(behavior->spawns == 1);
        // Whatever the author set, which is what has to survive.
        behavior->rate = 4.f;
        // And something they did not reflect, which will not.
        behavior->travelled = 99.f;
    }

    // The module is rebuilt: the name now means a different type.
    registry.add<BreathingV2>("test::Breathing");
    CHECK(scripts.rebuildInstances(world) == 1);

    Script* live = world.find<Script>(entity.id());
    REQUIRE(live != nullptr);
    auto* rebuilt = dynamic_cast<BreathingV2*>(live->behaviors[0].instance.get());
    REQUIRE(rebuilt != nullptr);

    // The edited field came across.
    CHECK(rebuilt->rate == doctest::Approx(4.f));
    // The unreflected one did not, and onSpawn ran again so the behaviour
    // could work out whatever it keeps privately from where things are now.
    CHECK(rebuilt->travelled == doctest::Approx(0.f));
    CHECK(rebuilt->spawns == 1);
    // Still counted as spawned, so nothing spawns it a second time.
    CHECK(live->behaviors[0].spawned);

    // And it is running the new code: twice the distance for the same step.
    scripts.fixedTick(world, 1.f);
    CHECK(rebuilt->travelled == doctest::Approx(8.f));
}

TEST_CASE("a reload leaves alone a behaviour the new module does not have") {
    ensureRegistered();
    // Removing a behaviour from a module must not be how the scenes using it
    // lose their settings for it.
    ege::World world;
    ScriptSystem scripts;

    Entity entity = world.spawn("Orphan");
    Script script{};
    Script::Slot slot{};
    slot.behavior = "test::NotInThisBuild";
    slot.savedFields = R"({"rate":7.0})";
    script.behaviors.push_back(std::move(slot));
    entity.attach<Script>(std::move(script));

    CHECK(scripts.rebuildInstances(world) == 0);

    Script* live = world.find<Script>(entity.id());
    REQUIRE(live != nullptr);
    CHECK(live->behaviors[0].behavior == "test::NotInThisBuild");
    CHECK(live->behaviors[0].savedFields == R"({"rate":7.0})");
    CHECK(live->behaviors[0].instance == nullptr);
}

namespace {

    // Counts arrivals and departures, from whichever side it is attached to.
    class TriggerLog : public ege::Behavior {
    public:
        std::vector<ege::Entity> arrived;
        std::vector<ege::Entity> departed;

        void onTriggerEnter(ege::Entity other) override { arrived.push_back(other); }

        void onTriggerExit(ege::Entity other) override { departed.push_back(other); }
    };

}  // namespace

EGE_REFLECT(TriggerLog)
EGE_REFLECT_END()

TEST_CASE("both sides of a trigger hear about it") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinComponents();
    ege::BehaviorRegistry::instance().add<TriggerLog>("TriggerLog");

    World world;

    const auto attachLog = [](Entity entity) {
        Script script{};
        Script::Slot slot{};
        slot.behavior = "TriggerLog";
        auto log = std::make_shared<TriggerLog>();
        slot.instance = log;
        script.behaviors.push_back(std::move(slot));
        entity.attach<Script>(std::move(script));
        return log;
    };

    Entity floor = world.spawn("Floor");
    Transform floorTransform{};
    floorTransform.translation = {0.f, -0.5f, 0.f};
    floor.attach<Transform>(floorTransform);
    floor.attach<ege::BoxCollider>(ege::BoxCollider{{10.f, 0.5f, 10.f}, {0.f, 0.f, 0.f}});

    Entity plate = world.spawn("Plate");
    Transform plateTransform{};
    plateTransform.translation = {0.f, 0.5f, 0.f};
    plate.attach<Transform>(plateTransform);
    plate.attach<ege::BoxCollider>(ege::BoxCollider{{1.f, 0.1f, 1.f}, {0.f, 0.f, 0.f}});
    plate.attach<ege::Trigger>();
    auto plateLog = attachLog(plate);

    Entity ball = world.spawn("Ball");
    Transform ballTransform{};
    ballTransform.translation = {0.f, 3.f, 0.f};
    ball.attach<Transform>(ballTransform);
    ball.attach<ege::SphereCollider>();
    ege::RigidBody bouncy{};
    bouncy.restitution = 0.8f;
    ball.attach<ege::RigidBody>(bouncy);
    auto ballLog = attachLog(ball);

    ScriptSystem scripts;
    ege::PhysicsSystem physics{};
    scripts.spawnPending(world);
    physics.start(world);

    for (int i = 0; i < 300; i++) {
        scripts.fixedTick(world, 1.f / 60.f);
        const ege::PhysicsEvents events = physics.fixedTick(world, 1.f / 60.f);
        scripts.deliverTriggers(world, events.entered, true);
        scripts.deliverTriggers(world, events.left, false);
    }
    physics.stop(world);

    // The plate was told who stepped on it, and whoever stepped was told
    // which plate: either end can be the one that knows what to do.
    REQUIRE(!plateLog->arrived.empty());
    REQUIRE(!ballLog->arrived.empty());
    CHECK(plateLog->arrived.front() == ball);
    CHECK(ballLog->arrived.front() == plate);

    // And the bounce back out was heard on both sides too.
    REQUIRE(!plateLog->departed.empty());
    REQUIRE(!ballLog->departed.empty());
    CHECK(plateLog->departed.front() == ball);
    CHECK(ballLog->departed.front() == plate);
}

TEST_CASE("a pressure plate opens its door while something stands on it") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinComponents();

    World world;

    Entity door = world.spawn("Door");
    Transform doorTransform{};
    doorTransform.translation = {5.f, 0.f, 0.f};
    door.attach<Transform>(doorTransform);

    Entity plate = world.spawn("Plate");
    plate.attach<Transform>();
    Script script{};
    Script::Slot slot{};
    slot.behavior = "ege::PressurePlate";
    auto pressure = std::make_shared<ege::PressurePlate>();
    pressure->door = "Door";
    pressure->opening = {0.f, -1.f, 0.f};
    pressure->speed = 2.f;
    slot.instance = pressure;
    script.behaviors.push_back(std::move(slot));
    plate.attach<Script>(std::move(script));

    Entity first = world.spawn("First");
    Entity second = world.spawn("Second");

    ScriptSystem scripts;
    scripts.spawnPending(world);

    const auto tick = [&](int steps) {
        for (int i = 0; i < steps; i++) {
            scripts.fixedTick(world, 1.f / 60.f);
        }
    };

    // Closed is where the door was, read rather than authored.
    tick(30);
    CHECK(door.fetch<Transform>().translation.y == doctest::Approx(0.f));

    // Two things step on: two arrivals, and the door opens once.
    scripts.deliverTriggers(world, {{plate, first}, {plate, second}}, true);
    tick(60);
    CHECK(door.fetch<Transform>().translation.y == doctest::Approx(-1.f).epsilon(0.02f));

    // One steps off, and the door stays open - the whole reason the plate
    // counts rather than remembering a yes or a no. A door that shut here
    // would shut on whoever was still standing there.
    scripts.deliverTriggers(world, {{plate, first}}, false);
    tick(60);
    CHECK(door.fetch<Transform>().translation.y == doctest::Approx(-1.f).epsilon(0.02f));

    // The last one steps off and it closes again.
    scripts.deliverTriggers(world, {{plate, second}}, false);
    tick(60);
    CHECK(door.fetch<Transform>().translation.y == doctest::Approx(0.f).epsilon(0.02f));

    // And it never goes further than closed, however many departures arrive:
    // a count that could go negative is a plate that sticks open.
    scripts.deliverTriggers(world, {{plate, first}, {plate, second}}, false);
    tick(60);
    CHECK(door.fetch<Transform>().translation.y == doctest::Approx(0.f).epsilon(0.02f));
    scripts.deliverTriggers(world, {{plate, first}}, true);
    tick(60);
    CHECK(door.fetch<Transform>().translation.y == doctest::Approx(-1.f).epsilon(0.02f));
}

TEST_CASE("a spawner stamps out its prefab when something enters") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinSerializers();
    ege::registerBuiltinComponents();

    // The fragment, built in a scratch world and written out - which is what
    // an editor's "save as prefab" would have done.
    World source;
    Entity crate = source.spawn("Crate");
    crate.attach<Transform>();
    const std::string document = ege::prefab::write(source, crate.id());

    World world;
    Entity pad = world.spawn("Pad");
    Transform padTransform{};
    padTransform.translation = {4.f, 0.f, 0.f};
    pad.attach<Transform>(padTransform);

    Script script{};
    Script::Slot slot{};
    slot.behavior = "ege::Spawner";
    auto spawner = std::make_shared<ege::Spawner>();
    // Resolved by hand rather than through the database: what a reference is
    // for is naming an asset, and what a spawner needs is the document.
    spawner->prefab = ege::PrefabRef{
        ege::Guid::fromName("prefab:test"), std::make_shared<ege::Prefab>(ege::Prefab{document})};
    spawner->offset = {0.f, 1.f, 0.f};
    spawner->limit = 2;
    spawner->cooldown = 0.5f;
    slot.instance = spawner;
    script.behaviors.push_back(std::move(slot));
    pad.attach<Script>(std::move(script));

    ScriptSystem scripts;
    scripts.spawnPending(world);

    Entity visitor = world.spawn("Visitor");
    const auto enter = [&] { scripts.deliverTriggers(world, {{pad, visitor}}, true); };
    const auto tick = [&](int steps) {
        for (int i = 0; i < steps; i++) {
            scripts.fixedTick(world, 1.f / 60.f);
        }
    };

    enter();
    CHECK(world.findByName("Crate").alive());
    // Placed where the spawner said, relative to the spawner - whatever the
    // prefab's own root transform held is where it sits inside the fragment.
    CHECK(world.findByName("Crate").fetch<Transform>().translation.x == doctest::Approx(4.f));
    CHECK(world.findByName("Crate").fetch<Transform>().translation.y == doctest::Approx(1.f));

    // Straight back in, inside the cooldown: a character standing in the
    // volume is not a fountain.
    const std::size_t afterFirst = world.entityCount();
    enter();
    CHECK(world.entityCount() == afterFirst);

    // Past the cooldown, a second one - and then never again, because the
    // limit is what stops a spawner filling the world it is standing in.
    tick(60);
    enter();
    CHECK(world.entityCount() == afterFirst + 1);
    tick(60);
    enter();
    CHECK(world.entityCount() == afterFirst + 1);
}

// ---- Timers and events ----------------------------------------------------

namespace {

    struct Ping {
        int value = 0;
    };

    // Sets a timer on spawn and records when it fired, in ticks.
    class Ticker : public ege::Behavior {
    public:
        float delay = 1.f;
        int firedAtTick = -1;
        int ticks = 0;
        int fires = 0;
        // A timer that another timer sets, to prove a callback may add to the
        // list it was called from.
        bool chain = false;
        int chained = 0;
        bool cancelImmediately = false;

        void onSpawn() override {
            const ege::TimerId timer = after(delay, [this]() {
                fires++;
                firedAtTick = ticks;
                if (chain) {
                    after(0.f, [this]() { chained++; });
                }
            });
            if (cancelImmediately) {
                cancel(timer);
            }
        }

        void onFixedTick(float) override { ticks++; }
    };

    // Listens for a Ping for as long as it exists.
    //
    // The count lives outside the behaviour on purpose: what these tests want
    // to know is what happens *after* the behaviour is gone, and a counter
    // inside it would keep it alive to be read.
    class Listener : public ege::Behavior {
    public:
        std::shared_ptr<int> heard = std::make_shared<int>(0);
        std::shared_ptr<int> despawns = std::make_shared<int>(0);

        void onSpawn() override {
            std::shared_ptr<int> counter = heard;
            on<Ping>([counter](const Ping&) { (*counter)++; });
        }

        void onDespawn() override { (*despawns)++; }
    };

}  // namespace

EGE_REFLECT(Ticker)
EGE_REFLECT_END()

EGE_REFLECT(Listener)
EGE_REFLECT_END()

namespace {

    // Attaches one behaviour to a fresh entity and hands back the instance.
    template<typename T>
    std::shared_ptr<T> attachTo(Entity entity, const char* name) {
        Script script{};
        Script::Slot slot{};
        slot.behavior = name;
        auto instance = std::make_shared<T>();
        slot.instance = instance;
        script.behaviors.push_back(std::move(slot));
        entity.attach<Script>(std::move(script));
        return instance;
    }

}  // namespace

TEST_CASE("a timer fires once, after the time it was given") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinComponents();
    ege::BehaviorRegistry::instance().add<Ticker>("Ticker");

    World world;
    auto ticker = attachTo<Ticker>(world.spawn("Ticker"), "Ticker");
    ticker->delay = 0.5f;

    ScriptSystem scripts;
    scripts.spawnPending(world);

    for (int i = 0; i < 60; i++) {
        scripts.fixedTick(world, 1.f / 60.f);
    }

    // Half a second at sixty ticks is thirty of them. Timers advance before
    // onFixedTick, so the count seen by the callback is the number of ticks
    // that had already completed.
    CHECK(ticker->fires == 1);
    CHECK(ticker->firedAtTick == 29);

    // Once, not once per tick from then on.
    for (int i = 0; i < 60; i++) {
        scripts.fixedTick(world, 1.f / 60.f);
    }
    CHECK(ticker->fires == 1);
}

TEST_CASE("a timer set from inside a timer is fine, and zero means next tick") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinComponents();
    ege::BehaviorRegistry::instance().add<Ticker>("Ticker");

    World world;
    auto ticker = attachTo<Ticker>(world.spawn("Ticker"), "Ticker");
    ticker->delay = 0.f;
    ticker->chain = true;

    ScriptSystem scripts;
    scripts.spawnPending(world);

    // The first tick fires the zero-delay timer, which sets another.
    scripts.fixedTick(world, 1.f / 60.f);
    CHECK(ticker->fires == 1);
    // Not in the same tick: a timer gets its whole duration before it is next
    // looked at, which is what makes `after(0.f, ...)` mean "next tick".
    CHECK(ticker->chained == 0);

    scripts.fixedTick(world, 1.f / 60.f);
    CHECK(ticker->chained == 1);
}

TEST_CASE("a cancelled timer never fires") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinComponents();
    ege::BehaviorRegistry::instance().add<Ticker>("Ticker");

    World world;
    auto ticker = attachTo<Ticker>(world.spawn("Ticker"), "Ticker");
    ticker->delay = 0.2f;
    ticker->cancelImmediately = true;

    ScriptSystem scripts;
    scripts.spawnPending(world);
    for (int i = 0; i < 120; i++) {
        scripts.fixedTick(world, 1.f / 60.f);
    }
    CHECK(ticker->fires == 0);
}

TEST_CASE("a timer dies with the entity that set it") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinComponents();
    ege::BehaviorRegistry::instance().add<Ticker>("Ticker");

    World world;
    Entity entity = world.spawn("Ticker");
    auto ticker = attachTo<Ticker>(entity, "Ticker");
    ticker->delay = 0.5f;

    ScriptSystem scripts;
    scripts.spawnPending(world);
    scripts.fixedTick(world, 1.f / 60.f);

    // Despawned with the timer pending. A door that opened for something no
    // longer there is the bug this prevents; the instance is kept alive here
    // only so the test can read it.
    entity.despawn();
    for (int i = 0; i < 120; i++) {
        scripts.fixedTick(world, 1.f / 60.f);
    }
    CHECK(ticker->fires == 0);
}

TEST_CASE("a behaviour's subscription ends when the behaviour does") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinComponents();
    ege::BehaviorRegistry::instance().add<Listener>("Listener");

    World world;
    Entity entity = world.spawn("Listener");

    std::shared_ptr<int> heard;
    std::shared_ptr<int> despawns;
    {
        auto listener = attachTo<Listener>(entity, "Listener");
        heard = listener->heard;
        despawns = listener->despawns;
        // The test's own reference goes here, so that from now on the only
        // things holding the behaviour are its component and the script
        // system - which is the arrangement a real scene has.
    }

    ScriptSystem scripts;
    scripts.spawnPending(world);

    world.events().raise(Ping{1});
    CHECK(*heard == 1);
    CHECK(world.events().subscriberCount() == 1);

    // Despawning destroys the Script component and the behaviour with it, and
    // the behaviour ends its subscriptions on the way out. A listener that
    // outlived its listener would be a call into freed memory.
    entity.despawn();
    scripts.spawnPending(world);

    CHECK(*despawns == 1);
    CHECK(world.events().subscriberCount() == 0);
    world.events().raise(Ping{2});
    CHECK(*heard == 1);
}

TEST_CASE("a behaviour hears about its own despawn, mid-play") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinComponents();
    ege::BehaviorRegistry::instance().add<Listener>("Listener");

    World world;
    Entity entity = world.spawn("Listener");
    std::shared_ptr<int> despawns;
    {
        auto listener = attachTo<Listener>(entity, "Listener");
        despawns = listener->despawns;
    }

    ScriptSystem scripts;
    scripts.spawnPending(world);
    CHECK(*despawns == 0);

    // Not only at Stop: an entity removed while the game runs is a despawn
    // too, and a behaviour that had a thing to undo needs to hear about it.
    // That takes a reference the system holds itself, because the component
    // that held the other one has just gone.
    entity.despawn();
    scripts.spawnPending(world);
    CHECK(*despawns == 1);

    // Once, not once per frame from then on.
    scripts.spawnPending(world);
    CHECK(*despawns == 1);
}

TEST_CASE("stopping play clears the listeners") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinComponents();
    ege::BehaviorRegistry::instance().add<Listener>("Listener");

    World world;
    attachTo<Listener>(world.spawn("Listener"), "Listener");

    ScriptSystem scripts;
    scripts.spawnPending(world);
    CHECK(world.events().subscriberCount() == 1);

    scripts.despawnAll(world);
    CHECK(world.events().subscriberCount() == 0);
}

TEST_CASE("a pickup, a goal and a gate that have never met") {
    ege::registerBuiltinTypes();
    ege::registerBuiltinSerializers();
    ege::registerBuiltinComponents();

    World world;

    // The rule: three pickups make a level, then a beat, then the news.
    Entity rules = world.spawn("Rules");
    auto goal = attachTo<ege::Goal>(rules, "ege::Goal");
    goal->needed = 3;
    goal->celebrateAfter = 0.5f;

    // The gate, which has never heard of a pickup.
    Entity gate = world.spawn("Gate");
    Transform gateTransform{};
    gateTransform.translation = {0.f, 0.f, 0.f};
    gate.attach<Transform>(gateTransform);
    auto opener = attachTo<ege::OpenOnLevelComplete>(gate, "ege::OpenOnLevelComplete");
    opener->opening = {0.f, -1.f, 0.f};
    opener->speed = 4.f;

    ScriptSystem scripts;
    scripts.spawnPending(world);

    const auto tick = [&](int steps) {
        for (int i = 0; i < steps; i++) {
            scripts.fixedTick(world, 1.f / 60.f);
        }
    };

    // Two is not three.
    world.events().raise(ege::PickupCollected{});
    world.events().raise(ege::PickupCollected{});
    tick(60);
    CHECK(goal->collected() == 2);
    CHECK(gate.fetch<Transform>().translation.y == doctest::Approx(0.f));

    // The third starts the clock, and the gate has not moved yet - the beat
    // is what makes the two read as cause and effect.
    world.events().raise(ege::PickupCollected{});
    tick(6);
    CHECK(gate.fetch<Transform>().translation.y == doctest::Approx(0.f));

    // Past the beat, it opens, and goes on opening until it is open.
    tick(90);
    CHECK(gate.fetch<Transform>().translation.y == doctest::Approx(-1.f).epsilon(0.02f));

    // And a fourth pickup does not re-announce anything: the level is over
    // once.
    const float settled = gate.fetch<Transform>().translation.y;
    world.events().raise(ege::PickupCollected{});
    tick(120);
    CHECK(gate.fetch<Transform>().translation.y == doctest::Approx(settled));
}
