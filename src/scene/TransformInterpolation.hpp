#pragma once

#include "scene/Components.hpp"
#include "scene/Entity.hpp"

namespace ege {

    class World;

    // Drawing between two fixed steps.
    //
    // The simulation runs at a fixed rate because it has to be reproducible;
    // the display runs at whatever rate it runs at. When the display is faster
    // - 144 Hz against a 60 Hz step - the same simulated pose is drawn two or
    // three frames running and then jumps, which reads as judder even though
    // nothing about the simulation is wrong. Time::fixedAlpha has existed
    // since Phase 1 to say how far through a step the current frame sits; this
    // is what spends it.
    //
    // Everything here is arithmetic on two poses, so it is tested without a
    // world or a device around it.

    // Where an entity was when the last fixed step began.
    //
    // Not reflected, and deliberately: it is this run's answer about a pose
    // between two steps, it means nothing in a saved scene, and an inspector
    // showing it would be showing a number the user cannot author. The cached
    // physics body handle is left out of serialization for the same reason.
    //
    // Its presence is also the opt-in. Nothing interpolates an entity that
    // does not carry one, which is what keeps anything moved on the variable
    // clock - a camera, a script running in tick rather than fixedTick - from
    // being drawn a frame behind where it actually is.
    struct PreviousTransform {
        glm::vec3 translation{0.f};
        glm::vec3 scale{1.f, 1.f, 1.f};
        glm::vec3 rotation{0.f};
    };

    // The angle to draw, between two a step apart.
    //
    // Interpolating Euler angles naively is wrong exactly once per turn: an
    // object that passed from just under a full turn to just over reads as
    // having gone almost all the way back round, and draws a frame spinning
    // the wrong way. Taking the shorter way round the circle is what fixes it,
    // and is why this is not a lerp.
    float interpolateAngle(float previous, float current, float alpha);

    // The pose to draw. Translation and scale are straight lerps; only the
    // rotation needs the care above.
    Transform interpolateTransform(
        const PreviousTransform& previous, const Transform& current, float alpha);

    // Records where everything that interpolates is now, as the pose to
    // interpolate away from. Called at the top of each fixed step, so a frame
    // that runs two steps interpolates from the second rather than the first.
    void recordPreviousTransforms(World& world);

    // Gives an entity a pose to interpolate from, starting where it is.
    // Called by whatever moves it on the fixed clock - the physics system does
    // this for every body it will move.
    void beginInterpolating(World& world, EntityId entity);

    // The pose to draw this entity at: interpolated when it has a previous
    // pose, and its own transform when it does not.
    Transform renderTransform(World& world, EntityId entity, float alpha);

}  // namespace ege
