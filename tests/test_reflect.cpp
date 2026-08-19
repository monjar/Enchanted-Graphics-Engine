// Reflection.
//
// The offset checks are the load-bearing ones. Everything downstream - the
// inspector, scene serialization, script state preservation across a hot
// reload - reads and writes fields through a void* plus an offset, so an
// offset that is wrong by even one byte corrupts silently rather than failing
// loudly.

#include "reflect/BuiltinTypes.hpp"
#include "reflect/Reflect.hpp"
#include "scene/Components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

namespace {

    struct Nested {
        float scalar = 1.f;
        glm::vec3 vector{};
    };

    struct Sample {
        bool flag = false;
        std::int32_t count = 0;
        float weight = 0.f;
        glm::vec3 position{};
        Nested nested{};
    };

}  // namespace

EGE_REFLECT(Nested)
EGE_FIELD(scalar);
EGE_FIELD(vector);
EGE_REFLECT_END()

EGE_REFLECT(Sample)
EGE_FIELD(flag);
EGE_FIELD(count).readOnly();
EGE_FIELD(weight).range(0.f, 1.f).tooltip("Normalised weight");
EGE_FIELD(position).asColor();
EGE_FIELD(nested);
EGE_REFLECT_END()

using ege::TypeRegistry;

TEST_CASE("a reflected type reports its name, size and fields") {
    const ege::TypeInfo& type = TypeRegistry::of<Sample>();

    CHECK(type.name() == "Sample");
    CHECK(type.size() == sizeof(Sample));
    CHECK(type.alignment() == alignof(Sample));
    CHECK(type.fields().size() == 5);
    CHECK_FALSE(type.isLeaf());
}

TEST_CASE("field offsets match the real layout") {
    const ege::TypeInfo& type = TypeRegistry::of<Sample>();

    // Computed against a real object rather than trusting the reflected value,
    // so the test fails if memberOffset is wrong rather than agreeing with it.
    Sample sample{};
    auto* base = reinterpret_cast<const std::byte*>(&sample);

    auto actualOffset = [&](const void* member) {
        return static_cast<std::size_t>(reinterpret_cast<const std::byte*>(member) - base);
    };

    CHECK(type.field("flag")->offset() == actualOffset(&sample.flag));
    CHECK(type.field("count")->offset() == actualOffset(&sample.count));
    CHECK(type.field("weight")->offset() == actualOffset(&sample.weight));
    CHECK(type.field("position")->offset() == actualOffset(&sample.position));
    CHECK(type.field("nested")->offset() == actualOffset(&sample.nested));
}

TEST_CASE("fields carry their declared type") {
    const ege::TypeInfo& type = TypeRegistry::of<Sample>();

    CHECK(type.field("flag")->type().name() == "bool");
    CHECK(type.field("count")->type().name() == "int32");
    CHECK(type.field("weight")->type().name() == "float");
    CHECK(type.field("position")->type().name() == "vec3");
    CHECK(type.field("nested")->type().name() == "Nested");

    // Leaf types have no fields of their own; reflected ones do.
    CHECK(type.field("weight")->type().isLeaf());
    CHECK_FALSE(type.field("nested")->type().isLeaf());
}

TEST_CASE("unknown field lookup returns null") {
    CHECK(TypeRegistry::of<Sample>().field("nonexistent") == nullptr);
}

TEST_CASE("attributes are recorded on the field they chain from") {
    const ege::TypeInfo& type = TypeRegistry::of<Sample>();

    const ege::FieldInfo* weight = type.field("weight");
    REQUIRE(weight != nullptr);
    CHECK(weight->hasRange());
    CHECK(weight->range().min == doctest::Approx(0.f));
    CHECK(weight->range().max == doctest::Approx(1.f));
    CHECK(std::string_view{weight->tooltip()} == "Normalised weight");

    CHECK(type.field("count")->isReadOnly());
    CHECK(type.field("position")->isColor());

    // Attributes must not leak onto neighbouring fields.
    CHECK_FALSE(type.field("flag")->hasRange());
    CHECK_FALSE(type.field("flag")->isReadOnly());
    CHECK_FALSE(type.field("weight")->isColor());
}

TEST_CASE("values can be read and written through reflection") {
    Sample sample{};
    const ege::TypeInfo& type = TypeRegistry::of<Sample>();

    type.field("weight")->valueIn<float>(&sample) = 0.75f;
    type.field("count")->valueIn<std::int32_t>(&sample) = 42;
    type.field("position")->valueIn<glm::vec3>(&sample) = glm::vec3{1.f, 2.f, 3.f};

    // Written through reflection, read through the real member.
    CHECK(sample.weight == doctest::Approx(0.75f));
    CHECK(sample.count == 42);
    CHECK(sample.position.y == doctest::Approx(2.f));

    // And the reverse direction.
    sample.flag = true;
    CHECK(type.field("flag")->valueIn<bool>(&sample));
}

TEST_CASE("nested types are reachable through their parent's field") {
    Sample sample{};
    const ege::FieldInfo* nestedField = TypeRegistry::of<Sample>().field("nested");
    REQUIRE(nestedField != nullptr);

    // Walk one level down: address of the nested struct, then of its field.
    void* nestedInstance = nestedField->addressIn(&sample);
    const ege::FieldInfo* scalarField = nestedField->type().field("scalar");
    REQUIRE(scalarField != nullptr);

    scalarField->valueIn<float>(nestedInstance) = 9.5f;
    CHECK(sample.nested.scalar == doctest::Approx(9.5f));
}

TEST_CASE("types are registered once and looked up by name") {
    const ege::TypeInfo& first = TypeRegistry::of<Sample>();
    const ege::TypeInfo& second = TypeRegistry::of<Sample>();
    CHECK(&first == &second);

    ege::registerBuiltinTypes();
    const ege::TypeInfo* found = TypeRegistry::instance().find("Sample");
    REQUIRE(found != nullptr);
    CHECK(found == &first);

    CHECK(TypeRegistry::instance().find("vec3") == &TypeRegistry::of<glm::vec3>());
    CHECK(TypeRegistry::instance().find("no such type") == nullptr);
}

TEST_CASE("the engine's own transform is reflected") {
    const ege::TypeInfo& type = TypeRegistry::of<ege::Transform>();

    CHECK(type.name() == "ege::Transform");
    CHECK(type.fields().size() == 3);

    ege::Transform transform{};
    type.field("translation")->valueIn<glm::vec3>(&transform) = glm::vec3{4.f, 5.f, 6.f};
    CHECK(transform.translation.z == doctest::Approx(6.f));

    // Every field must be describable, or the inspector cannot draw it.
    for (const ege::FieldInfo& field : type.fields()) {
        CHECK(field.type().name() == "vec3");
        CHECK(field.tooltip() != nullptr);
    }
}

TEST_CASE("registering a type twice yields the same description") {
    // Not a hypothetical. Type registration happens in a function-local static
    // inside a template, and a template instantiated in two modules - the
    // executable and the engine's shared library - gets one of those each.
    // Both then register, and everything that identifies a type by comparing
    // TypeInfo pointers depends on both registrations landing on one object.
    const ege::TypeInfo& first = ege::TypeRegistry::of<int>();
    const ege::TypeInfo& again = ege::registerType<int>();

    CHECK(&first == &again);

    // And the registry lists it once, not once per caller.
    const std::vector<const ege::TypeInfo*> all = ege::TypeRegistry::instance().all();
    const std::size_t occurrences =
        static_cast<std::size_t>(std::count(all.begin(), all.end(), &first));
    CHECK(occurrences == 1);

    // Lookup by name finds the same one, rather than an earlier copy the
    // second registration shadowed.
    CHECK(ege::TypeRegistry::instance().find(first.name()) == &first);
}
