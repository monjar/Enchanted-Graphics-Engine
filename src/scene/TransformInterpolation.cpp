#include "scene/TransformInterpolation.hpp"

#include "scene/World.hpp"

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace ege {

    float interpolateAngle(float previous, float current, float alpha) {
        // The difference, brought into (-pi, pi]. Anything outside that is the
        // long way round the same rotation.
        float difference = current - previous;
        const float turn = glm::two_pi<float>();
        difference = std::fmod(difference + glm::pi<float>(), turn);
        if (difference < 0.f) {
            difference += turn;
        }
        difference -= glm::pi<float>();

        return previous + difference * alpha;
    }

    Transform interpolateTransform(
        const PreviousTransform& previous, const Transform& current, float alpha) {
        Transform drawn{};
        drawn.translation =
            previous.translation + (current.translation - previous.translation) * alpha;
        drawn.scale = previous.scale + (current.scale - previous.scale) * alpha;
        drawn.rotation = {
            interpolateAngle(previous.rotation.x, current.rotation.x, alpha),
            interpolateAngle(previous.rotation.y, current.rotation.y, alpha),
            interpolateAngle(previous.rotation.z, current.rotation.z, alpha)};
        return drawn;
    }

    void recordPreviousTransforms(World& world) {
        world.each<Transform, PreviousTransform>(
            [](Entity, Transform& transform, PreviousTransform& previous) {
                previous.translation = transform.translation;
                previous.scale = transform.scale;
                previous.rotation = transform.rotation;
            });
    }

    void beginInterpolating(World& world, EntityId entity) {
        Transform* transform = world.find<Transform>(entity);
        if (transform == nullptr) {
            return;
        }
        // Starting where it is, so the first frame after this interpolates
        // between one pose and itself rather than from the origin.
        PreviousTransform previous{};
        previous.translation = transform->translation;
        previous.scale = transform->scale;
        previous.rotation = transform->rotation;
        world.attach<PreviousTransform>(entity, previous);
    }

    Transform renderTransform(World& world, EntityId entity, float alpha) {
        Transform* transform = world.find<Transform>(entity);
        if (transform == nullptr) {
            return Transform{};
        }
        const PreviousTransform* previous = world.find<PreviousTransform>(entity);
        if (previous == nullptr) {
            return *transform;
        }
        return interpolateTransform(*previous, *transform, alpha);
    }

}  // namespace ege
