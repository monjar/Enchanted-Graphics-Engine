#pragma once

#include "assets/AssetRef.hpp"
#include "scene/World.hpp"

#include <filesystem>
#include <string>

namespace ege {

    // A scene fragment: one entity, everything under it, and every component
    // on any of them - saved as an asset and stamped out as many times as
    // anyone likes.
    //
    // The loaded form is the document rather than a world, and that is the
    // whole idea: a prefab is a *description*, and each instantiation is a
    // fresh set of entities that no longer has anything to do with the ones
    // it was written from. Editing a prefab does not reach into the copies
    // already spawned - that would want an instance link, which is a
    // different feature and one the editor should ask for before it exists.
    struct Prefab {
        std::string document;
    };

    using PrefabRef = AssetRef<Prefab>;

    namespace prefab {

        // Writes `root` and its descendants. Parents are recorded as
        // positions inside the document, so the fragment re-links to itself
        // rather than to whatever ids it happened to have when it was saved.
        // The root is always position zero.
        std::string write(World& world, EntityId root);

        void save(World& world, EntityId root, const std::filesystem::path& path);

        // Spawns the fragment and hands back its root. Additive, unlike
        // loading a scene: a prefab arrives beside what is already there,
        // because arriving instead of it would make spawning a pickup the
        // last thing that ever happened.
        //
        // A null entity comes back when the document is unreadable or empty,
        // which a caller handles the same way as any other spawn that did not
        // happen.
        Entity spawn(World& world, const std::string& document);

        Entity spawn(World& world, const PrefabRef& reference);

    }  // namespace prefab

}  // namespace ege

EGE_TYPE_NAME(ege::PrefabRef, "PrefabRef")
