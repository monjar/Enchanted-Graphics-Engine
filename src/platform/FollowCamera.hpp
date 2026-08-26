#pragma once

#include "scene/Components.hpp"

#include <glm/glm.hpp>

namespace ege {

    class PhysicsWorld;

    // How a third-person camera sits behind whoever it is watching.
    struct FollowCameraSettings {
        // How far back along the look direction, and how far above the
        // subject, the camera would like to be.
        float distance = 2.2f;
        float height = 0.75f;
        // The camera aims at a point this far above the subject's origin
        // rather than at the origin itself, so a character does not spend the
        // whole game in the bottom half of the screen.
        float aimHeight = 0.3f;
        // The fraction of the remaining gap the camera closes each second.
        // Higher is tighter; too high and the camera stops being a camera and
        // becomes a rigid arm.
        float lag = 9.f;
        // How close the camera may be pulled when something is in the way,
        // and how far off a wall it stops. Larger than the subject: the
        // subject is solid too, and everything nearer than the closest the
        // camera may come is by definition not what stopped it.
        float minDistance = 0.45f;
        float wallMargin = 0.08f;
        // The subject these numbers were written for. Every length above is
        // really "this much, for a subject this tall": a camera framing a
        // character twice the size wants to be twice as far back, and one
        // set of defaults that only frames one size of character is a set of
        // defaults every project has to retune before it can see anything.
        float writtenForHeight = 0.6f;
    };

    // The same framing, for a subject of a different height.
    //
    // Lengths scale; `lag` does not, because it is a rate - how quickly a
    // camera catches up is a matter of feel rather than of size, and a big
    // character followed by a sluggish camera is not what anybody meant.
    FollowCameraSettings framedFor(const FollowCameraSettings& settings, float subjectHeight);

    // Where the camera wants to be and which way it wants to look: `distance`
    // back along `yaw`, `height` up, aimed at the subject.
    //
    // Device-free, so what the camera *decides* is testable even though what
    // it *sees* is not.
    Transform followCameraTarget(
        glm::vec3 subject, float yaw, glm::vec3 up, const FollowCameraSettings& settings);

    // The engine's Tait-Bryan angles for a camera at `from` looking at `to`:
    // the inverse of the forward vector the rest of the engine builds from a
    // yaw and a pitch. Roll is always zero - a third-person camera that rolls
    // is a bug, not a feature.
    glm::vec3 lookAngles(glm::vec3 from, glm::vec3 to);

    // Exponential smoothing that does not depend on the frame rate: the
    // fraction of the gap closed in one second is the same whether that
    // second took thirty frames or three hundred. The naive
    // `current + (target - current) * rate * dt` is not - it closes more of
    // the gap at a high frame rate, so a camera tuned on one machine lags on
    // another.
    glm::vec3 dampTowards(glm::vec3 current, glm::vec3 target, float rate, float deltaSeconds);

    // A third-person camera: follows a subject, keeps it in frame, and stops
    // at whatever is between it and the subject rather than standing inside
    // the scenery.
    class FollowCamera {
    public:
        FollowCameraSettings settings{};

        // Moves `viewer` towards where it should be to watch `subject` from
        // behind `yaw`. `physics` may be null - in the editor, before play
        // begins, there is no world to ask what is in the way - in which case
        // the camera keeps its full distance.
        void update(
            glm::vec3 subject,
            float yaw,
            glm::vec3 up,
            float deltaSeconds,
            const PhysicsWorld* physics,
            Transform& viewer);

        // Puts the camera exactly where it belongs on the next update rather
        // than sliding to it, which is what a cut wants: a camera that eases
        // in from wherever the editor left it is a camera that has to travel
        // across the scene while the shot has already started.
        void cut() { placed = false; }

    private:
        bool placed = false;
        glm::vec3 position{0.f};
    };

}  // namespace ege
