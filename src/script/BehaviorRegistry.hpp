#pragma once

#include "reflect/Reflect.hpp"
#include "script/Behavior.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ege {

    // Behaviour types, by name.
    //
    // The same gap the component registry fills, for the same reason:
    // reflection describes a behaviour's fields, but a scene file names a
    // behaviour in text and the editor offers a list to pick from, and neither
    // can call `new T` with a T chosen at runtime. This holds the factory that
    // closes it.
    //
    // Names are what a scene file stores, so renaming a behaviour type breaks
    // scenes that use it. That is a real cost and the alternative - an id in a
    // sidecar, as assets have - is worse here: a behaviour is code, it lives in
    // version control beside the scenes that use it, and a rename is a
    // refactor both can see.
    class BehaviorRegistry {
    public:
        struct Entry {
            std::string name;
            // The concrete behaviour's reflected fields, or null if it declares
            // none. This is what the inspector edits and the scene stores.
            const TypeInfo* type = nullptr;
            std::function<std::unique_ptr<Behavior>()> create;
        };

        static BehaviorRegistry& instance();

        // T must derive from Behavior and be default-constructible. Reflecting
        // it is optional; an unreflected behaviour simply has no editable
        // fields and nothing to serialize.
        template<typename T>
        void add(std::string name);

        const Entry* find(std::string_view name) const;

        const std::vector<Entry>& all() const { return entries; }

        // Forgets everything. Only for tests, and for the moment a reloaded
        // script module replaces the behaviours the old one registered.
        void clear();

    private:
        std::vector<Entry> entries;
        std::unordered_map<std::string, std::size_t> byName;
    };

}  // namespace ege

// Registers a behaviour under its own type name, at static-initialisation
// time, from the translation unit that defines it. No central list to add to,
// which is the property that lets a project's behaviours live entirely in the
// project.
//
// The registry is a function-local static, so it is constructed on first use
// and static-initialisation order across translation units cannot bite.
#define EGE_BEHAVIOR_JOIN_INNER(a, b) a##b
#define EGE_BEHAVIOR_JOIN(a, b) EGE_BEHAVIOR_JOIN_INNER(a, b)

#define EGE_BEHAVIOR(Type)                                                   \
    namespace {                                                              \
        const bool EGE_BEHAVIOR_JOIN(egeBehaviorRegistered, __LINE__) = [] { \
            ::ege::BehaviorRegistry::instance().add<Type>(#Type);            \
            return true;                                                     \
        }();                                                                 \
    }

#include "script/BehaviorRegistry.inl"
