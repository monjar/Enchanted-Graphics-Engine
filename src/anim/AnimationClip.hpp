#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ege {

    // Which part of a joint's pose a channel drives.
    enum class AnimationPath {
        translation,
        rotation,
        scale,
    };

    // One joint property over time: keyframe times and the values at them,
    // interpolated between. Times ascend; values hold a vec3 in xyz for
    // translation and scale, and a quaternion in xyzw for rotation - one
    // storage type rather than a variant, because a channel is sampled a
    // thousand times for every once it is inspected.
    struct AnimationChannel {
        uint32_t joint = 0;
        AnimationPath path = AnimationPath::translation;
        // A stepped channel holds each key's value until the next key rather
        // than interpolating - what glTF calls STEP, and what blocky
        // deliberately-choppy animations are made of.
        bool stepped = false;
        std::vector<float> times;
        std::vector<glm::vec4> values;
    };

    // A named animation: walk, run, jump. Duration is the last key time of
    // any channel, and looping wraps against it.
    struct AnimationClip {
        std::string name;
        float duration = 0.f;
        std::vector<AnimationChannel> channels;
    };

}  // namespace ege
