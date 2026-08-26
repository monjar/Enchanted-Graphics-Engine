#include "anim/AnimationSystem.hpp"

#include "anim/SkeletalAnimator.hpp"
#include "core/Log.hpp"

#include <algorithm>

namespace ege {

    void AnimationSystem::update(
        World& world, float deltaSeconds, std::vector<glm::mat4>& palette) {
        palette.clear();

        world.each<SkeletalAnimator>([&](Entity, SkeletalAnimator& animator) {
            if (animator.rig == nullptr || animator.rig->clips.empty() ||
                animator.rig->skeleton.joints.empty()) {
                return;
            }
            const AnimationRig& rig = *animator.rig;
            const std::size_t jointCount = rig.skeleton.joints.size();

            if (palette.size() + jointCount > paletteCapacity) {
                // Holding last frame's base keeps the entity posed rather
                // than exploding it; saying so once keeps the budget a
                // number someone raises rather than a mystery.
                if (!warnedFull) {
                    EGE_WARN(
                        "animation palette full at {} matrices; some animators hold their pose",
                        paletteCapacity);
                    warnedFull = true;
                }
                return;
            }

            // The inspector can point past the rig's clips - a scene from an
            // import with more of them, or a slider dragged high. Clamping
            // here rather than asserting keeps that an editing moment.
            const int clipCount = static_cast<int>(rig.clips.size());
            const int clipIndex = std::clamp(animator.clip, 0, clipCount - 1);
            const AnimationClip& clip = rig.clips[static_cast<std::size_t>(clipIndex)];

            samplePose(rig.skeleton, clip, animator.time, animator.loop, pose);

            // A crossfade samples both clips at their own times and blends
            // the poses. The fade runs from the old clip to the new one, so
            // the moment it starts the pose is entirely what was playing -
            // which is what makes a transition invisible rather than a step
            // followed by a slide.
            const bool fading = animator.fadeRemaining > 0.f && animator.fadeDuration > 0.f &&
                                animator.previousClip >= 0 && animator.previousClip < clipCount;
            const AnimationClip* previous = nullptr;
            if (fading) {
                previous = &rig.clips[static_cast<std::size_t>(animator.previousClip)];
                samplePose(
                    rig.skeleton, *previous, animator.previousTime, animator.loop, fadingPose);
                const float progress = 1.f - animator.fadeRemaining / animator.fadeDuration;
                blendPoses(fadingPose, pose, std::clamp(progress, 0.f, 1.f), blended);
                pose.swap(blended);
            }

            globalTransforms(rig.skeleton, pose, globals);
            skinningMatrices(rig.skeleton, globals, skins);

            animator.paletteBase = static_cast<uint32_t>(palette.size());
            palette.insert(palette.end(), skins.begin(), skins.end());

            // Advance after sampling, so time zero's pose is actually the
            // first frame anyone sees.
            if (animator.playing) {
                animator.time = clipTime(
                    animator.time + deltaSeconds * animator.speed, clip.duration, animator.loop);
                if (fading) {
                    // The clip being faded out keeps playing while it fades.
                    // A frozen outgoing pose is a character that stops dead
                    // for a tenth of a second every time it changes its mind.
                    animator.previousTime = clipTime(
                        animator.previousTime + deltaSeconds * animator.speed,
                        previous->duration,
                        animator.loop);
                }
            }
            if (fading) {
                animator.fadeRemaining = std::max(animator.fadeRemaining - deltaSeconds, 0.f);
                if (animator.fadeRemaining <= 0.f) {
                    animator.previousClip = -1;
                    animator.fadeDuration = 0.f;
                }
            }
        });
    }

}  // namespace ege
