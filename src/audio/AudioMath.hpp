#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace ege {

    // How a sound in the world reaches a listener.
    //
    // Device-free and backend-free, like every other piece of arithmetic in
    // this engine that could be: what a sound *should* sound like is a
    // question with an answer, and answering it here means it can be checked
    // without a sound card - which matters more here than elsewhere, because
    // CI has no speakers and a wrong answer is inaudible rather than visible.
    //
    // The backend is handed gains, not geometry. That keeps the one file that
    // knows miniaudio's name free of any opinion about how loud things are,
    // and it means swapping the backend cannot change how the world sounds.

    // How loud a source is at `distance`, as a factor between 0 and 1.
    //
    // Full volume within `minDistance` - inside that radius the source is
    // "here" and moving nearer should not make it louder without bound - then
    // inverse-distance falloff out to `maxDistance`, past which it is silent.
    //
    // Inverse distance rather than inverse square: the physically correct law
    // drops so fast that anything more than a few metres away is inaudible,
    // which is why almost nobody ships it. The cut to zero at maxDistance is
    // what lets a mixer stop mixing a voice at all.
    float distanceAttenuation(float distance, float minDistance, float maxDistance);

    // Left and right gains for a source at `toSource` (a direction *from* the
    // listener, in world space) heard by a listener facing `forward` with
    // `up` overhead.
    //
    // Constant-power panning: the two gains are the cosine and sine of one
    // angle, so their squares sum to one and a sound crossing the centre does
    // not dip in the middle - which the obvious linear pan does, audibly.
    //
    // A source at the listener's own position has no direction, and gets the
    // centre.
    struct StereoGains {
        float left = 0.70710678f;
        float right = 0.70710678f;
    };

    StereoGains stereoPan(glm::vec3 toSource, glm::vec3 forward, glm::vec3 up, float width = 1.f);

    // The two together, plus the source's own volume and its bus's: what the
    // mixer is actually given for one voice.
    struct VoiceGains {
        float left = 0.f;
        float right = 0.f;
    };

    // One frame of a clip, mixed down to mono, interpolated between the two
    // frames `cursor` falls between.
    //
    // Mono because what reaches the speakers is this sample times two gains:
    // a stereo clip played spatially has already been placed by the geometry,
    // and playing its own left channel out of the left speaker as well would
    // place it twice.
    //
    // Interpolated because `cursor` is fractional whenever the clip's rate
    // and the device's differ, which is almost always - and taking the
    // nearest frame instead is the difference between a clip and a clip with
    // a hiss on it.
    float sampleMono(const struct SoundData& sound, double cursor);

    // How far the cursor moves per output frame: the clip's rate against the
    // device's, times the pitch asked for. This one multiply is the whole of
    // the resampler.
    double playbackStep(std::uint32_t clipRate, std::uint32_t deviceRate, float pitch);

    VoiceGains spatialGains(
        glm::vec3 listener,
        glm::vec3 forward,
        glm::vec3 up,
        glm::vec3 source,
        float minDistance,
        float maxDistance,
        float volume);

}  // namespace ege
