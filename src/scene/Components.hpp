#pragma once

#include "reflect/BuiltinTypes.hpp"
#include "render/Light.hpp"
#include "render/Material.hpp"
#include "render/Model.hpp"

#include <glm/glm.hpp>

#include <memory>

namespace ege {

    // Position, rotation and scale.
    //
    // Rotation is Tait-Bryan angles applied Y then X then Z. Euler angles are
    // the wrong representation for interpolation and will gimbal lock, but they
    // are what an inspector can present as three numbers, so they stay until
    // there is an editor to justify quaternions plus a display conversion.
    struct Transform {
        glm::vec3 translation{0.f};
        glm::vec3 scale{1.f, 1.f, 1.f};
        glm::vec3 rotation{0.f};

        glm::mat4 mat4() const;

        // Exact for this strict T*R*S composition - see the derivation at the
        // definition. Must be revisited if shear is ever introduced.
        glm::mat3 normalMatrix() const;
    };

    // Makes an entity drawable.
    struct MeshRenderer {
        std::shared_ptr<Model> model;
        std::shared_ptr<Material> material;
        bool visible = true;
    };

    // Tag excluding an entity from rendering without detaching its renderer.
    struct Hidden {};

}  // namespace ege

EGE_REFLECT(ege::Transform)
EGE_FIELD(translation).tooltip("Position in world space");
EGE_FIELD(scale).tooltip("Non-uniform scale is supported");
EGE_FIELD(rotation).tooltip("Tait-Bryan angles in radians, applied Y then X then Z");
EGE_REFLECT_END()

EGE_REFLECT(ege::MeshRenderer)
EGE_FIELD(visible);
EGE_REFLECT_END()

// A tag has no fields. It is still reflected so that it has a name, which is
// what serialization and the editor's component list key on.
EGE_REFLECT(ege::Hidden)
EGE_REFLECT_END()
