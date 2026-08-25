#pragma once

#include "anim/AnimationClip.hpp"
#include "anim/Skeleton.hpp"

#include <vector>

namespace ege {

    // The arithmetic of playing an animation, kept device-free: sample a
    // clip into a pose, blend poses, turn a pose into the matrices the
    // skinning shader consumes. Everything a GPU will eventually read is
    // computed here first, where a test can pin it - the same arrangement
    // every piece of shader-bound maths in this engine lives under.

    // Where a looping or clamping clip actually reads at `time`. Looping
    // wraps against the duration; clamping holds the ends. A zero-duration
    // clip reads its start, whatever was asked.
    float clipTime(float time, float duration, bool loop);

    // The clip at `time`, written over the skeleton's rest pose: a channel
    // drives its one property of its one joint, and everything no channel
    // names keeps resting. Sized and overwritten in place, so a caller
    // sampling every frame allocates nothing after the first.
    void samplePose(
        const Skeleton& skeleton,
        const AnimationClip& clip,
        float time,
        bool loop,
        std::vector<JointPose>& pose);

    // Between two poses of the same skeleton: translations and scales
    // lerped, rotations slerped along the shorter arc. t of zero is `from`,
    // one is `to`. This is the whole of what a crossfade is.
    void blendPoses(
        const std::vector<JointPose>& from,
        const std::vector<JointPose>& to,
        float t,
        std::vector<JointPose>& out);

    // One joint's local matrix: translate, then rotate, then scale, the
    // composition order every transform in this engine uses.
    glm::mat4 localMatrix(const JointPose& pose);

    // Local poses to model-space transforms: one forward sweep, legal
    // because parents precede children in every skeleton the importer
    // builds.
    void globalTransforms(
        const Skeleton& skeleton, const std::vector<JointPose>& pose, std::vector<glm::mat4>& out);

    // What the skinning shader multiplies vertices by: each joint's global
    // transform times its inverse bind, so a vertex modelled in bind pose
    // lands where the joint is now.
    void skinningMatrices(
        const Skeleton& skeleton,
        const std::vector<glm::mat4>& globals,
        std::vector<glm::mat4>& out);

}  // namespace ege
