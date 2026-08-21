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
        scripts.deliverContacts(world, physics.fixedTick(world, 1.f / 60.f));
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
