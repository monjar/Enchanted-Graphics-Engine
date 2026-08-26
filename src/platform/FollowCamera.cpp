#include "platform/FollowCamera.hpp"

#include "physics/PhysicsWorld.hpp"

#include <algorithm>
#include <cmath>

namespace ege {

    namespace {

        glm::vec3 safeUp(glm::vec3 up) {
            const float length = glm::length(up);
            return length > 1e-6f ? up / length : glm::vec3{0.f, 1.f, 0.f};
        }

    }  // namespace

    Transform followCameraTarget(
        glm::vec3 subject, float yaw, glm::vec3 up, const FollowCameraSettings& settings) {
        const glm::vec3 axis = safeUp(up);
        // The same forward the rest of the engine builds from a yaw, so the
        // camera ends up behind whatever the player is aiming.
        const glm::vec3 forward{std::sin(yaw), 0.f, std::cos(yaw)};

        Transform pose{};
        pose.translation = subject + axis * settings.height - forward * settings.distance;
        pose.rotation = lookAngles(pose.translation, subject + axis * settings.aimHeight);
        return pose;
    }

    glm::vec3 lookAngles(glm::vec3 from, glm::vec3 to) {
        const glm::vec3 gap = to - from;
        const float length = glm::length(gap);
        if (length <= 1e-6f) {
            return glm::vec3{0.f};
        }
        const glm::vec3 direction = gap / length;
        // forward = (cos(pitch) sin(yaw), -sin(pitch), cos(pitch) cos(yaw)),
        // which is what a Y-then-X rotation does to +Z. Inverting it is the
        // whole of this function.
        const float yaw = std::atan2(direction.x, direction.z);
        const float pitch = -std::asin(std::clamp(direction.y, -1.f, 1.f));
        return glm::vec3{pitch, yaw, 0.f};
    }

    glm::vec3 dampTowards(glm::vec3 current, glm::vec3 target, float rate, float deltaSeconds) {
        if (rate <= 0.f || deltaSeconds <= 0.f) {
            return target;
        }
        // 1 - e^(-rate * dt): the fraction of the gap closed, integrated over
        // the step rather than sampled at its start.
        const float blend = 1.f - std::exp(-rate * deltaSeconds);
        return current + (target - current) * blend;
    }

    void FollowCamera::update(
        glm::vec3 subject,
        float yaw,
        glm::vec3 up,
        float deltaSeconds,
        const PhysicsWorld* physics,
        Transform& viewer) {
        const glm::vec3 axis = safeUp(up);
        const Transform wanted = followCameraTarget(subject, yaw, axis, settings);

        glm::vec3 target = wanted.translation;

        // Cast from where the camera is aiming out to where it wants to be:
        // anything in between would be drawn over the subject, or worse, the
        // camera would be inside it looking at its back faces.
        if (physics != nullptr) {
            const glm::vec3 aim = subject + axis * settings.aimHeight;
            const glm::vec3 gap = target - aim;
            const float reach = glm::length(gap);
            if (reach > 1e-4f) {
                const glm::vec3 direction = gap / reach;
                if (const std::optional<RaycastHit> hit = physics->raycast(aim, direction, reach)) {
                    const float allowed =
                        std::max(hit->distance - settings.wallMargin, settings.minDistance);
                    target = aim + direction * std::min(allowed, reach);
                }
            }
        }

        if (!placed) {
            // A cut, not a move: the first frame of a shot is the shot.
            position = target;
            placed = true;
        } else {
            position = dampTowards(position, target, settings.lag, deltaSeconds);
        }

        viewer.translation = position;
        // Aimed from where the camera actually is rather than from where it
        // was heading, so the subject stays centred while the camera is still
        // catching up.
        viewer.rotation = lookAngles(position, subject + axis * settings.aimHeight);
    }

}  // namespace ege
