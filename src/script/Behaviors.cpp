#include "script/Behaviors.hpp"

#include "render/DynamicMesh.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "script/BehaviorRegistry.hpp"

#include <cmath>

namespace ege {

    namespace {

        // Every behaviour that moves something has to say so: world matrices
        // are cached behind a dirty flag, and a script writing a Transform is
        // in exactly the position the inspector is.
        void moved(Behavior& behavior) {
            hierarchy::markDirty(behavior.world(), behavior.self().id());
        }

    }  // namespace

    void Spinner::onFixedTick(float deltaSeconds) {
        Transform* transform = self().find<Transform>();
        if (transform == nullptr) {
            return;
        }
        transform->rotation += anglesPerSecond * deltaSeconds;
        moved(*this);
    }

    void Orbit::onSpawn() {
        angle = phase;
    }

    void Orbit::onFixedTick(float deltaSeconds) {
        Transform* transform = self().find<Transform>();
        if (transform == nullptr) {
            return;
        }
        angle += radiansPerSecond * deltaSeconds;
        transform->translation.x = center.x + std::cos(angle) * radius;
        transform->translation.z = center.z + std::sin(angle) * radius;
        moved(*this);
    }

    void Bobbing::onSpawn() {
        // Read rather than authored: the behaviour works wherever it is
        // dropped, and Stop putting the scene back also puts the ride back.
        if (const Transform* transform = self().find<Transform>()) {
            origin = transform->translation;
        }
        time = 0.f;
    }

    void Bobbing::onFixedTick(float deltaSeconds) {
        Transform* transform = self().find<Transform>();
        if (transform == nullptr) {
            return;
        }
        time += deltaSeconds;
        transform->translation = origin;
        transform->translation.y = origin.y + std::sin(time * radiansPerSecond) * amplitude;
        moved(*this);
    }

    void Ripple::onSpawn() {
        time = 0.f;
    }

    void Ripple::onFixedTick(float deltaSeconds) {
        DynamicMesh* mesh = self().find<DynamicMesh>();
        if (mesh == nullptr || mesh->vertices.empty()) {
            return;
        }

        time += deltaSeconds;

        // Height is a pure function of position and elapsed time, so the
        // surface never drifts and Stop restoring the clock restores the
        // shape. A behaviour that integrated its vertices instead would come
        // back from a snapshot looking like wherever it happened to be.
        const float frequency = 6.283185f / wavelength;
        for (Model::Vertex& vertex : mesh->vertices) {
            const float distance = std::sqrt(
                vertex.position.x * vertex.position.x + vertex.position.z * vertex.position.z);
            vertex.position.y = std::sin(distance * frequency - time * speed) * amplitude;
        }

        mesh->recalculateNormals();
        mesh->markDirty();
    }

}  // namespace ege

EGE_BEHAVIOR(ege::Spinner)
EGE_BEHAVIOR(ege::Orbit)
EGE_BEHAVIOR(ege::Bobbing)
EGE_BEHAVIOR(ege::Ripple)
