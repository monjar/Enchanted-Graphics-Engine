#include "anim/AnimationSampling.hpp"

#include "core/Assert.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace ege {

    namespace {

        // The two keys bracketing `time` and how far between them it falls.
        // Before the first key the first holds; after the last, the last -
        // clamping here is what makes a clip's ends quiet rather than
        // undefined.
        struct KeySpan {
            std::size_t left = 0;
            std::size_t right = 0;
            float t = 0.f;
        };

        KeySpan findSpan(const std::vector<float>& times, float time) {
            KeySpan span{};
            if (times.empty()) {
                return span;
            }
            if (time <= times.front()) {
                return span;
            }
            if (time >= times.back()) {
                span.left = span.right = times.size() - 1;
                return span;
            }
            // First key strictly past `time`; its predecessor is the left
            // edge. Binary search, because a long clip sampled per frame
            // should not pay a linear walk per channel.
            const auto after = std::upper_bound(times.begin(), times.end(), time);
            span.right = static_cast<std::size_t>(after - times.begin());
            span.left = span.right - 1;
            const float width = times[span.right] - times[span.left];
            span.t = width > 0.f ? (time - times[span.left]) / width : 0.f;
            return span;
        }

        glm::quat quatAt(const AnimationChannel& channel, std::size_t key) {
            const glm::vec4& value = channel.values[key];
            // glTF stores quaternions xyzw; glm constructs wxyz. This is the
            // one place the order is translated, so it is the one place the
            // order can be wrong.
            return glm::quat{value.w, value.x, value.y, value.z};
        }

        void applyChannel(const AnimationChannel& channel, float time, JointPose& pose) {
            if (channel.times.empty()) {
                return;
            }
            const KeySpan span = findSpan(channel.times, time);
            // A stepped channel holds the left key's value until the next
            // key arrives; interpolation is what it exists to not do.
            const float t = channel.stepped ? 0.f : span.t;

            switch (channel.path) {
                case AnimationPath::translation:
                    pose.translation = glm::mix(
                        glm::vec3{channel.values[span.left]},
                        glm::vec3{channel.values[span.right]},
                        t);
                    break;
                case AnimationPath::rotation:
                    // Slerp, along the shorter arc, then normalised: two
                    // unit quaternions interpolated stay close to unit, but
                    // "close to" compounds over a skeleton's depth.
                    pose.rotation = glm::normalize(
                        glm::slerp(quatAt(channel, span.left), quatAt(channel, span.right), t));
                    break;
                case AnimationPath::scale:
                    pose.scale = glm::mix(
                        glm::vec3{channel.values[span.left]},
                        glm::vec3{channel.values[span.right]},
                        t);
                    break;
            }
        }

    }  // namespace

    float clipTime(float time, float duration, bool loop) {
        if (duration <= 0.f) {
            return 0.f;
        }
        if (!loop) {
            return std::clamp(time, 0.f, duration);
        }
        // fmod keeps the sign of its argument, so a rewound clip - negative
        // time - still lands inside [0, duration).
        const float wrapped = std::fmod(time, duration);
        return wrapped < 0.f ? wrapped + duration : wrapped;
    }

    void samplePose(
        const Skeleton& skeleton,
        const AnimationClip& clip,
        float time,
        bool loop,
        std::vector<JointPose>& pose) {
        pose.resize(skeleton.joints.size());
        for (std::size_t i = 0; i < skeleton.joints.size(); i++) {
            pose[i] = skeleton.joints[i].rest;
        }

        const float at = clipTime(time, clip.duration, loop);
        for (const AnimationChannel& channel : clip.channels) {
            EGE_ASSERT(channel.joint < pose.size(), "channel targets a joint the rig lacks");
            applyChannel(channel, at, pose[channel.joint]);
        }
    }

    void blendPoses(
        const std::vector<JointPose>& from,
        const std::vector<JointPose>& to,
        float t,
        std::vector<JointPose>& out) {
        EGE_ASSERT(from.size() == to.size(), "blended poses must share a skeleton");
        out.resize(from.size());
        for (std::size_t i = 0; i < from.size(); i++) {
            out[i].translation = glm::mix(from[i].translation, to[i].translation, t);
            out[i].rotation = glm::normalize(glm::slerp(from[i].rotation, to[i].rotation, t));
            out[i].scale = glm::mix(from[i].scale, to[i].scale, t);
        }
    }

    glm::mat4 localMatrix(const JointPose& pose) {
        glm::mat4 local = glm::translate(glm::mat4{1.f}, pose.translation);
        local = local * glm::mat4_cast(pose.rotation);
        return glm::scale(local, pose.scale);
    }

    void globalTransforms(
        const Skeleton& skeleton, const std::vector<JointPose>& pose, std::vector<glm::mat4>& out) {
        EGE_ASSERT(pose.size() == skeleton.joints.size(), "pose and skeleton disagree");
        out.resize(skeleton.joints.size());
        for (std::size_t i = 0; i < skeleton.joints.size(); i++) {
            const glm::mat4 local = localMatrix(pose[i]);
            const int parent = skeleton.joints[i].parent;
            EGE_ASSERT(parent < static_cast<int>(i), "parents must precede children in a skeleton");
            out[i] = parent < 0 ? local : out[static_cast<std::size_t>(parent)] * local;
        }
    }

    void skinningMatrices(
        const Skeleton& skeleton,
        const std::vector<glm::mat4>& globals,
        std::vector<glm::mat4>& out) {
        EGE_ASSERT(globals.size() == skeleton.joints.size(), "globals and skeleton disagree");
        out.resize(skeleton.joints.size());
        for (std::size_t i = 0; i < skeleton.joints.size(); i++) {
            out[i] = globals[i] * skeleton.joints[i].inverseBind;
        }
    }

}  // namespace ege
