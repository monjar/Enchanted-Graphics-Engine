// Play mode.
//
// The contract is one sentence - Play snapshots the world, Stop restores it -
// and every case here is a way that sentence can quietly stop being true:
// a second Play overwriting the state Stop should return to, a step that runs
// more than one tick, a Stop that leaves the world half-restored.

#include "editor/PlayMode.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "script/Behaviors.hpp"
#include "script/Script.hpp"
#include "script/ScriptSystem.hpp"

#include <doctest/doctest.h>

using ege::Entity;
using ege::PlayMode;
using ege::Script;
using ege::ScriptSystem;
using ege::Transform;
using ege::World;

namespace {

    void ensureRegistered() {
        ege::registerBuiltinTypes();
        ege::registerBuiltinSerializers();
        ege::registerBuiltinComponents();
    }

    Entity buildSpinner(World& world) {
        Entity entity = world.spawn("Spinner");
        entity.attach<Transform>();
        Script script{};
        Script::Slot slot{};
        slot.behavior = "ege::Spinner";
        auto spinner = std::make_shared<ege::Spinner>();
        spinner->anglesPerSecond = {0.f, 2.f, 0.f};
        slot.instance = std::move(spinner);
        script.behaviors.push_back(std::move(slot));
        entity.attach<Script>(std::move(script));
        return entity;
    }

    // Play mode drives behaviours through the script system, so the tests
    // drive them the same way rather than calling into a behaviour directly.
    void advance(World& world, ScriptSystem& scripts, int steps) {
        scripts.spawnPending(world);
        for (int step = 0; step < steps; step++) {
            scripts.fixedTick(world, 1.f / 60.f);
        }
    }

}  // namespace

TEST_CASE("play snapshots and stop restores") {
    ensureRegistered();

    World world;
    buildSpinner(world);

    PlayMode play;
    CHECK(play.isEditing());

    play.play(world);
    REQUIRE(play.isPlaying());
    CHECK(play.snapshotSize() > 0);

    ScriptSystem scripts;
    for (int step = 0; step < 30; step++) {
        REQUIRE(play.consumeTick());
    }
    advance(world, scripts, 30);
    CHECK(world.findByName("Spinner").fetch<Transform>().rotation.y > 0.5f);

    play.stop(world);
    CHECK(play.isEditing());
    CHECK(play.snapshotSize() == 0);

    // The entity is respawned rather than rewound, so it has to be found by
    // name again - which is exactly why the editor drops its selection.
    Entity restored = world.findByName("Spinner");
    REQUIRE(restored.alive());
    CHECK(restored.fetch<Transform>().rotation.y == doctest::Approx(0.f));
    CHECK(world.entityCount() == 1);
}

TEST_CASE("nothing advances while editing") {
    ensureRegistered();

    World world;
    PlayMode play;

    CHECK_FALSE(play.consumeTick());
    CHECK_FALSE(play.consumeTick());
}

TEST_CASE("pause holds and resume continues") {
    ensureRegistered();

    World world;
    buildSpinner(world);

    PlayMode play;
    play.play(world);
    play.pause();

    CHECK(play.isPaused());
    CHECK_FALSE(play.consumeTick());

    play.resume();
    CHECK(play.isPlaying());
    CHECK(play.consumeTick());
}

TEST_CASE("step advances exactly one tick and pauses") {
    ensureRegistered();

    World world;
    buildSpinner(world);

    PlayMode play;
    play.play(world);
    play.pause();
    play.step(world);

    CHECK(play.isPaused());
    CHECK(play.consumeTick());
    // One, not one per frame until something clears it.
    CHECK_FALSE(play.consumeTick());
}

TEST_CASE("step from edit mode starts playing first") {
    // Otherwise the button does nothing at all until Play has been pressed,
    // which is not what a step control is taken to mean anywhere.
    ensureRegistered();

    World world;
    buildSpinner(world);

    PlayMode play;
    play.step(world);

    CHECK(play.isPaused());
    CHECK(play.snapshotSize() > 0);
    CHECK(play.consumeTick());
}

TEST_CASE("a second play does not overwrite the snapshot") {
    // The bug this guards: press Play, let the scene move, press Play again,
    // and Stop returns to the moved state rather than the authored one.
    ensureRegistered();

    World world;
    buildSpinner(world);

    PlayMode play;
    ScriptSystem scripts;
    play.play(world);
    advance(world, scripts, 30);

    play.play(world);
    play.stop(world);

    CHECK(world.findByName("Spinner").fetch<Transform>().rotation.y == doctest::Approx(0.f));
}

TEST_CASE("stop while editing does nothing") {
    ensureRegistered();

    World world;
    buildSpinner(world);
    world.findByName("Spinner").fetch<Transform>().translation.x = 5.f;

    PlayMode play;
    play.stop(world);

    // No snapshot was taken, so there is nothing to restore and the world is
    // left exactly as the user has it.
    CHECK(world.findByName("Spinner").fetch<Transform>().translation.x == doctest::Approx(5.f));
    CHECK(world.entityCount() == 1);
}

TEST_CASE("play mode restores hierarchy as well as components") {
    ensureRegistered();

    World world;
    Entity pivot = world.spawn("Pivot");
    pivot.attach<Transform>();
    Script script{};
    Script::Slot slot{};
    slot.behavior = "ege::Spinner";
    auto spinner = std::make_shared<ege::Spinner>();
    spinner->anglesPerSecond = {0.f, 2.f, 0.f};
    slot.instance = std::move(spinner);
    script.behaviors.push_back(std::move(slot));
    pivot.attach<Script>(std::move(script));
    Entity child = world.spawn("Child");
    child.attach<Transform>();
    ege::hierarchy::setParent(world, child.id(), pivot.id());

    PlayMode play;
    ScriptSystem scripts;
    play.play(world);
    advance(world, scripts, 30);
    play.stop(world);

    Entity restoredPivot = world.findByName("Pivot");
    Entity restoredChild = world.findByName("Child");
    REQUIRE(restoredPivot.alive());
    REQUIRE(restoredChild.alive());
    CHECK(ege::hierarchy::parentOf(world, restoredChild.id()) == restoredPivot.id());
    CHECK(restoredPivot.fetch<Transform>().rotation.y == doctest::Approx(0.f));
}
