#pragma once

#include "assets/AssetRef.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "render/Light.hpp"
#include "render/Material.hpp"
#include "render/Model.hpp"
#include "scene/Entity.hpp"

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

        // The inverse of mat4(): the fields that would compose this matrix.
        // Exact for a translation, rotation and positive scale; shear has no
        // representation here and is silently dropped, which is the same
        // constraint mat4() imposes going the other way.
        //
        // Needed wherever a matrix arrives from outside - a glTF node, a
        // gizmo drag - and has to become something an inspector can show as
        // nine numbers.
        static Transform fromMatrix(const glm::mat4& matrix);

        // Exact for this strict T*R*S composition - see the derivation at the
        // definition. Must be revisited if shear is ever introduced.
        glm::mat3 normalMatrix() const;
    };

    // Makes an entity drawable.
    //
    // Asset references rather than bare pointers, so that saving a scene
    // records what to draw and loading one can find it again. Before the asset
    // database this component was the reason a reloaded scene came back with
    // its transforms, names and lights intact and nothing visible in it.
    struct MeshRenderer {
        MeshRef mesh;
        MaterialRef material;
        bool visible = true;
    };

    // Tag excluding an entity from rendering without detaching its renderer.
    struct Hidden {};

    // Parent and child links.
    //
    // Stored as an intrusive linked list - first child plus next sibling -
    // rather than a vector of children per entity. A vector per parent means a
    // heap allocation for every entity that has children and a linear scan to
    // unlink one; the list keeps unlinking constant time and adds two handles
    // to entities that have no children at all.
    //
    // The links are entity handles, so a child that is despawned leaves a
    // stale handle the traversal detects rather than a dangling pointer.
    struct Hierarchy {
        EntityId parent{};
        EntityId firstChild{};
        EntityId nextSibling{};
        EntityId previousSibling{};

        // World matrix from the last time transforms were resolved, and whether
        // it needs recomputing. Cached because a deep hierarchy otherwise
        // recomputes the same parent chain once per descendant per frame.
        glm::mat4 worldMatrix{1.f};
        bool dirty = true;
    };

}  // namespace ege

EGE_REFLECT(ege::Transform)
EGE_FIELD(translation).tooltip("Position in world space");
EGE_FIELD(scale).tooltip("Non-uniform scale is supported");
EGE_FIELD(rotation).tooltip("Tait-Bryan angles in radians, applied Y then X then Z");
EGE_REFLECT_END()

EGE_REFLECT(ege::MeshRenderer)
EGE_FIELD(mesh).tooltip("Geometry to draw");
EGE_FIELD(material).tooltip("How to shade it");
EGE_FIELD(visible);
EGE_REFLECT_END()

// A tag has no fields. It is still reflected so that it has a name, which is
// what serialization and the editor's component list key on.
EGE_REFLECT(ege::Hidden)
EGE_REFLECT_END()

// Hierarchy is deliberately not reflected for serialization: the links are
// runtime entity handles, and an EntityId is an index into a running world's
// slot table, which means nothing in the next process. A scene file records
// parenting as a position in its own entity array instead - see
// SceneSerializer, which reads them back in a second pass because a child may
// be written before its parent.
