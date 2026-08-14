#pragma once

#include <type_traits>
#include <utility>

namespace ege {

    template<typename T>
    void BehaviorRegistry::add(std::string name) {
        static_assert(std::is_base_of_v<Behavior, T>, "a behaviour must derive from Behavior");
        static_assert(
            std::is_default_constructible_v<T>,
            "a behaviour must be default-constructible: the registry creates one from a name in a "
            "scene file, with nothing else to go on");

        const auto existing = byName.find(name);
        Entry entry{};
        entry.name = name;
        // Only reflected behaviours have fields worth storing. of<T> would
        // register an unreflected type as a nameless leaf, which is harmless
        // but means the inspector would draw "<T>" instead of nothing.
        entry.type = detail::Describe<T>::reflected ? &TypeRegistry::of<T>() : nullptr;
        entry.create = [] { return std::unique_ptr<Behavior>{new T{}}; };

        if (existing != byName.end()) {
            // A reloaded script module re-registers what it already had.
            // Replacing rather than appending is what makes that idempotent.
            entries[existing->second] = std::move(entry);
            return;
        }

        byName.emplace(std::move(name), entries.size());
        entries.push_back(std::move(entry));
    }

}  // namespace ege
