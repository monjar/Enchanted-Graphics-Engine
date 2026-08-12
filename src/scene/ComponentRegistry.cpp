#include "scene/ComponentRegistry.hpp"

#include "core/Log.hpp"
#include "scene/Components.hpp"

namespace ege {

    ComponentRegistry& ComponentRegistry::instance() {
        static ComponentRegistry registry;
        return registry;
    }

    const ComponentRegistry::Entry* ComponentRegistry::find(std::string_view name) const {
        const auto found = byName.find(std::string{name});
        return found == byName.end() ? nullptr : &entries[found->second];
    }

    void registerBuiltinComponents() {
        ComponentRegistry& registry = ComponentRegistry::instance();

        registry.add<Transform>();
        registry.add<MeshRenderer>();
        registry.add<PointLight>();
        registry.add<DirectionalLight>();
        registry.add<Hidden>();

        EGE_DEBUG("Component registry: {} component types", registry.all().size());
    }

}  // namespace ege
