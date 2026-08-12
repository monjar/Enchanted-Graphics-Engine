#pragma once

#include "scene/World.hpp"

#include <filesystem>
#include <string>

namespace ege {

    // Saves and loads a World as JSON.
    //
    // Entirely reflection-driven: nothing here knows what a Transform is. Adding
    // a component to the engine means reflecting it and registering it, and it
    // then serializes with no change to this file. That is the payoff for
    // building reflection first.
    //
    // JSON rather than a binary format because a scene file is something a
    // person reads in a diff and occasionally edits by hand. A cooked binary
    // form belongs in the asset pipeline, alongside the other packaging.
    class SceneSerializer {
    public:
        // Overwrites the destination. Throws on write failure.
        static void save(const World& world, const std::filesystem::path& path);

        static std::string toString(const World& world);

        // Replaces the world's contents. Throws if the file cannot be read or
        // is not valid JSON; individual unknown components are skipped with a
        // warning rather than failing the whole load, so a scene saved by a
        // build with extra components still opens.
        static void load(World& world, const std::filesystem::path& path);

        static void fromString(World& world, const std::string& json);
    };

}  // namespace ege
