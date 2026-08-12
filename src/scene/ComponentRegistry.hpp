#pragma once

#include "reflect/Reflect.hpp"
#include "scene/World.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ege {

    // Maps a component's name to the operations needed to manipulate it without
    // knowing its type at compile time.
    //
    // Reflection describes a component's *fields*, but deserialization also has
    // to create one from a name in a file, and the editor has to offer an "add
    // component" list. Neither is expressible through TypeInfo alone, because
    // both need to call World::attach<T> with a T chosen at runtime. This holds
    // the type-erased thunks that close that gap.
    class ComponentRegistry {
    public:
        struct Entry {
            const TypeInfo* type = nullptr;
            std::string name;

            // Attaches a default-constructed component and returns its address,
            // or the existing one if already attached.
            std::function<void*(World&, EntityId)> attach;
            std::function<void*(World&, EntityId)> find;
            std::function<bool(World&, EntityId)> has;
            std::function<void(World&, EntityId)> detach;
        };

        static ComponentRegistry& instance();

        // T must be reflected and default-constructible.
        template<typename T>
        void add();

        const Entry* find(std::string_view name) const;

        const std::vector<Entry>& all() const { return entries; }

    private:
        std::vector<Entry> entries;
        std::unordered_map<std::string, std::size_t> byName;
    };

    // Registers the engine's own components. Called during start-up, and by
    // tests that need scene loading to work.
    void registerBuiltinComponents();

}  // namespace ege

#include "scene/ComponentRegistry.inl"
