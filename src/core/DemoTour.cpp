#include "core/DemoTour.hpp"

#include <algorithm>
#include <cmath>

namespace ege {

    namespace {

        // Ease in and out. A tour that starts and stops abruptly reads as a
        // bug in the camera rather than as a deliberate move.
        float smoothstep(float t) {
            const float clamped = std::clamp(t, 0.f, 1.f);
            return clamped * clamped * (3.f - 2.f * clamped);
        }

        glm::vec3 mix(const glm::vec3& from, const glm::vec3& to, float t) {
            return from + (to - from) * t;
        }

    }  // namespace

    DemoTour::DemoTour(std::vector<Shot> shots) : path{std::move(shots)} {
        for (std::size_t index = 0; index < path.size(); index++) {
            total += (index == 0 ? 0.f : path[index].travelSeconds) + path[index].holdSeconds;
        }
    }

    DemoTour DemoTour::demoScene() {
        // Remember the scene treats -Y as up, so the camera's height is
        // negative and looking down is a negative pitch.
        return DemoTour{{
            // Open wide, on the whole set, so there is something to orient by
            // before anything moves.
            Shot{{0.f, -1.f, -3.6f}, {-0.30f, 0.f, 0.f}, 0.f, 1.2f},
            // Down the row of metal spheres. The roughness sweep only reads as
            // a sweep when the highlight travels along it.
            Shot{{-2.3f, -0.55f, 0.2f}, {-0.12f, 0.55f, 0.f}, 2.0f, 0.6f},
            Shot{{2.3f, -0.55f, 0.2f}, {-0.12f, -0.55f, 0.f}, 2.8f, 0.6f},
            // Close on the near-mirror sphere, which is the payoff of
            // image-based lighting: it is reflecting a sky that only exists
            // because the engine generated one at start-up.
            Shot{{-1.15f, -0.42f, 0.35f}, {-0.10f, 0.15f, 0.f}, 1.8f, 0.9f},
            // Low and along the floor, where the sun's shadows are longest.
            Shot{{-1.9f, -0.35f, -1.5f}, {-0.05f, 0.75f, 0.f}, 2.0f, 0.8f},
            // Up and over, ending on the imported glTF torus against the sky.
            Shot{{0.9f, -1.6f, -1.4f}, {-0.42f, -0.25f, 0.f}, 2.4f, 1.4f},
        }};
    }

    void DemoTour::restart() {
        time = 0.f;
    }

    bool DemoTour::advance(float deltaSeconds, Transform& viewer) {
        if (path.empty()) {
            return false;
        }

        time += deltaSeconds;

        // Walk the schedule from the start each frame rather than tracking a
        // current segment. It is a handful of shots, it costs nothing, and it
        // means the pose is a pure function of elapsed time - so a recording
        // made at one frame rate frames identically to one made at another.
        float cursor = 0.f;
        viewer.translation = path.front().position;
        viewer.rotation = path.front().rotation;

        for (std::size_t index = 0; index < path.size(); index++) {
            const Shot& shot = path[index];
            const float travel = index == 0 ? 0.f : shot.travelSeconds;

            if (time < cursor + travel) {
                const Shot& previous = path[index - 1];
                const float t = smoothstep((time - cursor) / travel);
                viewer.translation = mix(previous.position, shot.position, t);
                viewer.rotation = mix(previous.rotation, shot.rotation, t);
                return true;
            }
            cursor += travel;

            if (time < cursor + shot.holdSeconds) {
                viewer.translation = shot.position;
                viewer.rotation = shot.rotation;
                return true;
            }
            cursor += shot.holdSeconds;
        }

        // Past the end: hold the last pose so the final frame of a recording
        // is the shot it was aimed at, not wherever the interpolation stopped.
        viewer.translation = path.back().position;
        viewer.rotation = path.back().rotation;
        return false;
    }

}  // namespace ege
