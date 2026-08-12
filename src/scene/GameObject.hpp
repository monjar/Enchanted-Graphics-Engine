#pragma once

#include "reflect/BuiltinTypes.hpp"
#include "render/Material.hpp"
#include "render/Model.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <memory>
#include <unordered_map>

namespace ege {

    struct TransformComponent {
        glm::vec3 translation{};
        glm::vec3 scale{1.f, 1.f, 1.f};
        glm::vec3 rotation{};

        glm::mat4 mat4() const;
        glm::mat3 normalMatrix() const;
    };

    class GameObject {
    public:
        using id_t = unsigned int;
        using Map = std::unordered_map<id_t, GameObject>;

        static GameObject createGameObject() {
            static id_t currentId = 0;

            return GameObject{currentId++};
        }

        GameObject(const GameObject&) = delete;
        GameObject& operator=(const GameObject&) = delete;
        GameObject(GameObject&&) = default;
        GameObject& operator=(GameObject&&) = default;

        id_t getId() const { return id; }

        std::shared_ptr<Model> model{};
        std::shared_ptr<Material> material{};
        glm::vec3 color{};
        TransformComponent transform{};

    private:
        GameObject(id_t objId) : id{objId} {}

        id_t id;
    };
}  // namespace ege

// Reflected so the editor inspector can draw a transform, and so scene
// serialization can round-trip it, without either knowing the field list.
EGE_REFLECT(ege::TransformComponent)
EGE_FIELD(translation).tooltip("Position in world space");
EGE_FIELD(rotation).tooltip("Tait-Bryan angles in radians, applied Y then X then Z");
EGE_FIELD(scale).tooltip("Non-uniform scale is supported");
EGE_REFLECT_END()
