// Asset identifiers.
//
// The properties pinned here are the ones the asset database's correctness
// rests on: a printed id reads back as itself, a name always derives the same
// id, and a malformed id is rejected rather than quietly becoming null - which
// would turn a corrupt scene file into a scene with missing geometry and no
// error anywhere.

#include "core/Guid.hpp"

#include <doctest/doctest.h>

#include <string>
#include <unordered_set>

using ege::Guid;

TEST_CASE("a default-constructed id is null and prints as zeroes") {
    const Guid id{};

    CHECK(id.isNull());
    CHECK(id.toString() == std::string(32, '0'));
}

TEST_CASE("generated ids are never null and do not repeat") {
    std::unordered_set<Guid> seen;
    for (int iteration = 0; iteration < 10000; iteration++) {
        const Guid id = Guid::generate();
        REQUIRE_FALSE(id.isNull());
        REQUIRE(seen.insert(id).second);
    }
}

TEST_CASE("printing and parsing round-trip") {
    for (int iteration = 0; iteration < 1000; iteration++) {
        const Guid id = Guid::generate();
        const std::optional<Guid> parsed = Guid::parse(id.toString());

        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == id);
    }
}

TEST_CASE("dashes in a printed id are ignored") {
    const Guid id{0x0123456789abcdefull, 0xfedcba9876543210ull};

    CHECK(id.toString() == "0123456789abcdeffedcba9876543210");
    CHECK(Guid::parse("01234567-89ab-cdef-fedc-ba9876543210") == id);
    CHECK(Guid::parse("0123456789ABCDEFFEDCBA9876543210") == id);
}

TEST_CASE("malformed ids are rejected rather than parsed as null") {
    CHECK_FALSE(Guid::parse("").has_value());
    CHECK_FALSE(Guid::parse("not an id").has_value());
    // One digit short, one too many, and a non-hex digit in an otherwise
    // well-formed id.
    CHECK_FALSE(Guid::parse(std::string(31, 'a')).has_value());
    CHECK_FALSE(Guid::parse(std::string(33, 'a')).has_value());
    CHECK_FALSE(Guid::parse(std::string(31, 'a') + "g").has_value());
}

TEST_CASE("a name always derives the same id") {
    CHECK(Guid::fromName("mesh:box") == Guid::fromName("mesh:box"));
    CHECK(Guid::fromName("mesh:box") != Guid::fromName("mesh:sphere"));
    CHECK_FALSE(Guid::fromName("mesh:box").isNull());
    CHECK_FALSE(Guid::fromName("").isNull());

    // The value itself is part of the contract, not an implementation
    // detail: a scene file written by one build has to resolve in the next,
    // so changing the derivation is a breaking change and this is what says
    // so out loud.
    CHECK(Guid::fromName("mesh:box").toString() == "501c1205fb5c65efd01a229cbf2d53ad");
}

TEST_CASE("derived names do not collide across a realistic set") {
    std::unordered_set<Guid> seen;
    for (const char* prefix : {"mesh:", "material:", "texture:"}) {
        for (int index = 0; index < 2000; index++) {
            const Guid id = Guid::fromName(std::string{prefix} + std::to_string(index));
            REQUIRE(seen.insert(id).second);
        }
    }
}
