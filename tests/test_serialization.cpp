// Scene serialization.
//
// The bar the roadmap set for this phase is that a scene saves, loads and
// saves back byte-identically. That is a stronger check than comparing field
// values one at a time, because it catches anything the round trip drops
// silently - a component that failed to re-attach, a field the writer skipped,
// an ordering that is not stable.

#include "reflect/BuiltinTypes.hpp"
#include "reflect/Serialization.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>
#include <string>

using ege::ComponentRegistry;
using ege::Entity;
using ege::MeshRenderer;
using ege::PointLight;
using ege::SceneSerializer;
using ege::Serializer;
using ege::Transform;
using ege::TypeRegistry;
using ege::World;

namespace {

    // Registration is global and idempotent, so every case can just ask.
    void ensureRegistered() {
        ege::registerBuiltinTypes();
        ege::registerBuiltinSerializers();
        ege::registerBuiltinComponents();
    }

    void buildScene(World& world) {
        Entity floor = world.spawn("Floor");
        Transform floorTransform{};
        floorTransform.translation = {0.f, 0.5f, 0.f};
        floorTransform.scale = {8.f, 1.f, 8.f};
        floorTransform.rotation = {3.14159f, 0.f, 0.f};
        floor.attach<Transform>(floorTransform);
        floor.attach<MeshRenderer>(MeshRenderer{nullptr, nullptr, true});

        Entity light = world.spawn("KeyLight");
        Transform lightTransform{};
        lightTransform.translation = {-1.5f, -1.6f, -1.2f};
        light.attach<Transform>(lightTransform);
        light.attach<PointLight>(PointLight{{1.f, 0.95f, 0.85f}, 6.f, 25.f});

        Entity hidden = world.spawn("HiddenThing");
        hidden.attach<Transform>();
        hidden.attach<ege::Hidden>();
    }

}  // namespace

TEST_CASE("a reflected struct round-trips through JSON") {
    ensureRegistered();

    Transform original{};
    original.translation = {1.5f, -2.25f, 3.125f};
    original.scale = {2.f, 4.f, 8.f};
    original.rotation = {0.25f, 0.5f, 0.75f};

    const nlohmann::json json =
        Serializer::instance().write(TypeRegistry::of<Transform>(), &original);

    Transform restored{};
    Serializer::instance().read(TypeRegistry::of<Transform>(), &restored, json);

    // Exact rather than approximate: these values are all representable, and a
    // serializer that loses precision on them is broken.
    CHECK(restored.translation == original.translation);
    CHECK(restored.scale == original.scale);
    CHECK(restored.rotation == original.rotation);
}

TEST_CASE("vectors serialise as flat arrays") {
    ensureRegistered();

    Transform transform{};
    transform.translation = {1.f, 2.f, 3.f};

    const nlohmann::json json =
        Serializer::instance().write(TypeRegistry::of<Transform>(), &transform);

    REQUIRE(json.contains("translation"));
    REQUIRE(json["translation"].is_array());
    CHECK(json["translation"].size() == 3);
    CHECK(json["translation"][1].get<float>() == doctest::Approx(2.f));
}

TEST_CASE("a field absent from the JSON keeps the instance's existing value") {
    // What makes a scene written by an older build still load: new fields keep
    // their defaults instead of being zeroed or failing the parse.
    ensureRegistered();

    Transform transform{};
    transform.scale = {5.f, 6.f, 7.f};

    const nlohmann::json partial = nlohmann::json::parse(R"({"translation": [9, 9, 9]})");
    Serializer::instance().read(TypeRegistry::of<Transform>(), &transform, partial);

    CHECK(transform.translation.x == doctest::Approx(9.f));
    // Untouched, not reset.
    CHECK(transform.scale.y == doctest::Approx(6.f));
}

TEST_CASE("a value of the wrong type is skipped rather than throwing") {
    ensureRegistered();

    MeshRenderer renderer{};
    renderer.visible = true;

    const nlohmann::json wrong = nlohmann::json::parse(R"({"visible": "not a bool"})");
    CHECK_NOTHROW(Serializer::instance().read(TypeRegistry::of<MeshRenderer>(), &renderer, wrong));
    CHECK(renderer.visible);
}

TEST_CASE("a tag component serialises as an empty object") {
    ensureRegistered();

    ege::Hidden tag{};
    const nlohmann::json json = Serializer::instance().write(TypeRegistry::of<ege::Hidden>(), &tag);

    CHECK(json.is_object());
    CHECK(json.empty());
}

TEST_CASE("a scene saves, loads and saves back byte-identically") {
    ensureRegistered();

    World original;
    buildScene(original);

    const std::string first = SceneSerializer::toString(original);

    World reloaded;
    SceneSerializer::fromString(reloaded, first);

    const std::string second = SceneSerializer::toString(reloaded);

    CHECK(first == second);
    CHECK(reloaded.entityCount() == original.entityCount());
}

TEST_CASE("loading restores entities, names and component values") {
    ensureRegistered();

    World original;
    buildScene(original);
    const std::string json = SceneSerializer::toString(original);

    World reloaded;
    SceneSerializer::fromString(reloaded, json);

    Entity floor = reloaded.findByName("Floor");
    REQUIRE(floor.alive());
    REQUIRE(floor.has<Transform>());
    CHECK(floor.fetch<Transform>().scale.x == doctest::Approx(8.f));
    CHECK(floor.has<MeshRenderer>());

    Entity light = reloaded.findByName("KeyLight");
    REQUIRE(light.alive());
    REQUIRE(light.has<PointLight>());
    CHECK(light.fetch<PointLight>().intensity == doctest::Approx(6.f));
    CHECK(light.fetch<PointLight>().color.g == doctest::Approx(0.95f));
    CHECK(light.fetch<Transform>().translation.x == doctest::Approx(-1.5f));

    Entity hidden = reloaded.findByName("HiddenThing");
    REQUIRE(hidden.alive());
    CHECK(hidden.has<ege::Hidden>());
    // The tag must not drag unrelated components along with it.
    CHECK_FALSE(hidden.has<PointLight>());
}

TEST_CASE("loading replaces the world's contents rather than merging") {
    ensureRegistered();

    World source;
    buildScene(source);
    const std::string json = SceneSerializer::toString(source);

    World destination;
    destination.spawn("Leftover").attach<Transform>();
    REQUIRE(destination.entityCount() == 1);

    SceneSerializer::fromString(destination, json);

    CHECK(destination.entityCount() == 3);
    CHECK_FALSE(destination.findByName("Leftover").alive());
}

TEST_CASE("an unknown component is skipped without failing the load") {
    // A scene written by a build that has components this one does not must
    // still open, minus those components.
    ensureRegistered();

    const std::string json = R"({
        "version": 1,
        "entities": [
            {
                "name": "Partial",
                "components": {
                    "ege::Transform": {"translation": [1, 2, 3], "scale": [1, 1, 1], "rotation": [0, 0, 0]},
                    "some::FutureComponent": {"whatever": 5}
                }
            }
        ]
    })";

    World world;
    CHECK_NOTHROW(SceneSerializer::fromString(world, json));

    Entity entity = world.findByName("Partial");
    REQUIRE(entity.alive());
    CHECK(entity.fetch<Transform>().translation.z == doctest::Approx(3.f));
}

TEST_CASE("malformed JSON throws rather than leaving a half-loaded world") {
    ensureRegistered();

    World world;
    CHECK_THROWS(SceneSerializer::fromString(world, "{ this is not json"));
}

TEST_CASE("an empty world round-trips") {
    ensureRegistered();

    World empty;
    const std::string json = SceneSerializer::toString(empty);

    World reloaded;
    reloaded.spawn("ToBeCleared");
    SceneSerializer::fromString(reloaded, json);

    CHECK(reloaded.entityCount() == 0);
    CHECK(SceneSerializer::toString(reloaded) == json);
}

TEST_CASE("every registered component is serializable") {
    // Guards the failure where a component is added to the registry but its
    // fields use a type with no converter, so it silently writes nulls.
    ensureRegistered();

    for (const ComponentRegistry::Entry& entry : ComponentRegistry::instance().all()) {
        CAPTURE(entry.name);
        REQUIRE(entry.type != nullptr);

        for (const ege::FieldInfo& field : entry.type->fields()) {
            CAPTURE(field.name());
            // Composite fields recurse; leaves need a registered converter.
            const bool serializable = !field.type().isLeaf() || field.type().isReflected() ||
                                      Serializer::instance().hasLeafConverter(field.type());
            REQUIRE(serializable);
        }
    }
}
