#pragma once

#include "anim/AnimationClip.hpp"
#include "anim/Skeleton.hpp"
#include "reflect/Reflect.hpp"

#include <memory>
#include <vector>

namespace ege {

    // A rig and its clips, shared by every entity animating the same import.
    // Immutable once built - an animator holds where it is in a clip, never
    // a changed copy of the clip.
    struct AnimationRig {
        Skeleton skeleton;
        std::vector<AnimationClip> clips;
    };

    // Plays clips on a skinned mesh. The system samples the active clip into
    // a pose each frame and writes the skinning matrices into the frame's
    // palette; the entity's MeshRenderer must hold a skinned mesh, whose
    // vertices name the joints these matrices move.
    //
    // The rig arrives from the importer and does not survive a scene load
    // yet - rigs become database assets when the animation import grows an
    // asset type, the same road meshes and materials already travelled. The
    // reflected fields do survive, so a loaded scene remembers what was
    // playing even before it can play it.
    struct SkeletalAnimator {
        std::shared_ptr<const AnimationRig> rig;

        // Which of the rig's clips plays, clamped by the system to what the
        // rig actually has.
        int clip = 0;
        float speed = 1.f;
        bool playing = true;
        bool loop = true;

        // Where in the clip playback is, in seconds. Runtime state rather
        // than a reflected field, on the same reasoning as a behaviour's
        // unreflected members: a loaded scene starts its animations from the
        // top rather than mid-stride.
        float time = 0.f;

        // A crossfade in progress: what was playing before it started, where
        // that clip had got to, and how much of the fade is left.
        //
        // Two clips sampled independently and blended pose by pose, rather
        // than one clip's matrices blended into another's: a pose keeps
        // rotation apart from translation and scale, so the halfway point
        // between two poses is a pose. Halfway between two matrices is
        // shear.
        int previousClip = -1;
        float previousTime = 0.f;
        float fadeRemaining = 0.f;
        float fadeDuration = 0.f;

        // Where this frame's skinning matrices landed in the palette,
        // written by the system for the draw that consumes them.
        uint32_t paletteBase = 0;

        // Starts `next` playing, crossfading out of whatever was.
        //
        // `fromTheTop` restarts the new clip, which is what a jump wants.
        // False carries the phase across instead - a walk becoming a run is
        // one stride turning into a faster stride, and restarting it mid-step
        // is a stumble. Asking for the clip already playing does nothing, so
        // a driver may call this every tick.
        void play(int next, float seconds = 0.15f, bool fromTheTop = true) {
            if (next == clip) {
                return;
            }
            previousClip = clip;
            previousTime = time;
            fadeDuration = seconds > 0.f ? seconds : 0.f;
            fadeRemaining = fadeDuration;

            float phase = 0.f;
            if (!fromTheTop && rig != nullptr) {
                const auto count = static_cast<int>(rig->clips.size());
                if (clip >= 0 && clip < count && next >= 0 && next < count) {
                    const float from = rig->clips[static_cast<std::size_t>(clip)].duration;
                    const float to = rig->clips[static_cast<std::size_t>(next)].duration;
                    phase = from > 0.f ? (time / from) * to : 0.f;
                }
            }
            clip = next;
            time = phase;
        }
    };

}  // namespace ege

EGE_REFLECT(ege::SkeletalAnimator)
EGE_FIELD(clip).range(0.f, 32.f).tooltip("Which of the rig's clips plays");
EGE_FIELD(speed).range(-4.f, 4.f).tooltip("Playback rate; negative plays backwards");
EGE_FIELD(playing);
EGE_FIELD(loop);
EGE_REFLECT_END()
