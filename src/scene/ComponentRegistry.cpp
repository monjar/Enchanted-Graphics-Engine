#include "scene/ComponentRegistry.hpp"

#include "anim/SkeletalAnimator.hpp"
#include "assets/AssetSerialization.hpp"
#include "core/Log.hpp"
#include "physics/PhysicsComponents.hpp"
#include "render/DynamicMesh.hpp"
#include "scene/Components.hpp"
#include "script/Script.hpp"
#include "script/ScriptSerialization.hpp"

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

        // MeshRenderer carries asset references, which need a conversion the
        // reflect layer cannot supply because reading one resolves it through
        // the asset database. Registered here so that anything with the
        // built-in components also has the converters they require.
        registerAssetSerializers();
        // A behaviour list is polymorphic, so reflection cannot describe it
        // and it converts specially too.
        registerScriptSerializer();

        registry.add<Transform>();
        registry.add<MeshRenderer>();
        registry.add<PointLight>();
        registry.add<SpotLight>();
        registry.add<DirectionalLight>();
        registry.add<Hidden>();
        registry.add<Script>();
        registry.add<DynamicMesh>();
        registry.add<RigidBody>();
        registry.add<BoxCollider>();
        registry.add<SphereCollider>();
        registry.add<CapsuleCollider>();
        registry.add<SkeletalAnimator>();

        EGE_DEBUG("Component registry: {} component types", registry.all().size());
    }

}  // namespace ege
