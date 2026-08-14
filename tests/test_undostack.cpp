// Undo and redo.
//
// The stack stores whole-scene mementos, so the cases worth pinning are the
// ones about the stack rather than about what a change was: that recording
// captures the state *before* a change, that a new change discards the redo
// branch, and that the depth limit drops the oldest entry rather than
// refusing the newest.

#include "editor/UndoStack.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"

#include <doctest/doctest.h>

#include <string>

using ege::Entity;
using ege::Transform;
using ege::UndoStack;
using ege::World;

namespace {

    void ensureRegistered() {
        ege::registerBuiltinTypes();
        ege::registerBuiltinSerializers();
        ege::registerBuiltinComponents();
    }

    float xOf(World& world, const char* name) {
        return world.findByName(name).fetch<Transform>().translation.x;
    }

}  // namespace

TEST_CASE("undo returns the value a change replaced") {
    ensureRegistered();

    World world;
    Entity entity = world.spawn("Thing");
    entity.attach<Transform>();

    UndoStack undo;
    CHECK_FALSE(undo.canUndo());
    CHECK_FALSE(undo.canRedo());

    undo.record(world, "move");
    world.findByName("Thing").fetch<Transform>().translation.x = 5.f;

    REQUIRE(undo.canUndo());
    CHECK(undo.undoLabel() == "move");

    undo.undo(world);
    CHECK(xOf(world, "Thing") == doctest::Approx(0.f));
    CHECK(undo.canRedo());

    undo.redo(world);
    CHECK(xOf(world, "Thing") == doctest::Approx(5.f));
}

TEST_CASE("undo covers structural changes, not just properties") {
    // The reason mementos were chosen over inverse commands: deleting a
    // subtree and reversing it needs no code of its own.
    ensureRegistered();

    World world;
    world.spawn("Keep").attach<Transform>();
    world.spawn("Doomed").attach<Transform>();

    UndoStack undo;
    undo.record(world, "delete");
    world.findByName("Doomed").despawn();
    REQUIRE(world.entityCount() == 1);

    undo.undo(world);
    CHECK(world.entityCount() == 2);
    CHECK(world.findByName("Doomed").alive());

    undo.redo(world);
    CHECK(world.entityCount() == 1);
    CHECK_FALSE(world.findByName("Doomed").alive());
}

TEST_CASE("a new change discards the redo branch") {
    ensureRegistered();

    World world;
    world.spawn("Thing").attach<Transform>();

    UndoStack undo;
    undo.record(world, "first");
    world.findByName("Thing").fetch<Transform>().translation.x = 1.f;
    undo.undo(world);
    REQUIRE(undo.canRedo());

    undo.record(world, "second");
    world.findByName("Thing").fetch<Transform>().translation.x = 2.f;

    CHECK_FALSE(undo.canRedo());
    CHECK(undo.undoLabel() == "second");
    undo.undo(world);
    CHECK(xOf(world, "Thing") == doctest::Approx(0.f));
}

TEST_CASE("undoing several changes walks back through them in order") {
    ensureRegistered();

    World world;
    world.spawn("Thing").attach<Transform>();

    UndoStack undo;
    for (int step = 1; step <= 5; step++) {
        undo.record(world, "step " + std::to_string(step));
        world.findByName("Thing").fetch<Transform>().translation.x = static_cast<float>(step);
    }

    for (int step = 5; step >= 1; step--) {
        CHECK(xOf(world, "Thing") == doctest::Approx(static_cast<float>(step)));
        undo.undo(world);
    }
    CHECK(xOf(world, "Thing") == doctest::Approx(0.f));
    CHECK_FALSE(undo.canUndo());
}

TEST_CASE("the stack drops the oldest step rather than refusing the newest") {
    ensureRegistered();

    World world;
    world.spawn("Thing").attach<Transform>();

    UndoStack undo;
    for (std::size_t step = 0; step < UndoStack::maxDepth + 10; step++) {
        undo.record(world, "step");
        world.findByName("Thing").fetch<Transform>().translation.x = static_cast<float>(step + 1);
    }

    CHECK(undo.depth() == UndoStack::maxDepth);

    // Unwinding everything the stack still holds gets back to the oldest
    // state it kept - not to the very first one, which is the honest cost of
    // a bounded history.
    while (undo.canUndo()) {
        undo.undo(world);
    }
    CHECK(xOf(world, "Thing") == doctest::Approx(10.f));
}

TEST_CASE("undo and redo on an empty stack do nothing") {
    ensureRegistered();

    World world;
    world.spawn("Thing").attach<Transform>();

    UndoStack undo;
    undo.undo(world);
    undo.redo(world);

    CHECK(world.entityCount() == 1);
    CHECK(world.findByName("Thing").alive());
}

TEST_CASE("clearing forgets both directions") {
    ensureRegistered();

    World world;
    world.spawn("Thing").attach<Transform>();

    UndoStack undo;
    undo.record(world, "change");
    world.findByName("Thing").fetch<Transform>().translation.x = 3.f;
    undo.undo(world);

    undo.clear();
    CHECK_FALSE(undo.canUndo());
    CHECK_FALSE(undo.canRedo());
    // And the world is left where it was, not restored again.
    CHECK(xOf(world, "Thing") == doctest::Approx(0.f));
}
