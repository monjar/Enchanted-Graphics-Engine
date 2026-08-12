// Template definitions for scene/ComponentRegistry.hpp.

namespace ege {

    template <typename T>
    void ComponentRegistry::add() {
        static_assert(
            std::is_default_constructible_v<T>,
            "a registered component must be default-constructible, because "
            "deserialization creates one before filling in its fields");

        const TypeInfo& type = TypeRegistry::of<T>();
        const std::string name{type.name()};

        if (byName.find(name) != byName.end()) {
            return;
        }

        Entry entry{};
        entry.type = &type;
        entry.name = name;
        entry.attach = [](World& world, EntityId entity) -> void* {
            if (T* existing = world.find<T>(entity)) {
                return existing;
            }
            return &world.attach<T>(entity);
        };
        entry.find = [](World& world, EntityId entity) -> void* {
            return world.find<T>(entity);
        };
        entry.has = [](World& world, EntityId entity) { return world.has<T>(entity); };
        entry.detach = [](World& world, EntityId entity) { world.detach<T>(entity); };

        byName.emplace(name, entries.size());
        entries.push_back(std::move(entry));
    }

}  // namespace ege
