#include "audio/AudioMath.hpp"

#include "audio/AudioEngine.hpp"

#include <algorithm>
#include <cmath>

namespace ege {

    float distanceAttenuation(float distance, float minDistance, float maxDistance) {
        // A source with no extent and no reach is either on top of you or
        // inaudible; either way there is nothing to interpolate between.
        if (maxDistance <= minDistance) {
            return distance <= minDistance ? 1.f : 0.f;
        }
        if (distance <= minDistance) {
            return 1.f;
        }
        if (distance >= maxDistance) {
            return 0.f;
        }

        // Inverse distance, then faded to nothing across the whole range so
        // that the cut at maxDistance is not a click. Without the fade a
        // source walking out of range stops mid-note.
        const float inverse = minDistance / std::max(distance, 1e-4f);
        const float fade = 1.f - (distance - minDistance) / (maxDistance - minDistance);
        return std::clamp(inverse * fade, 0.f, 1.f);
    }

    StereoGains stereoPan(glm::vec3 toSource, glm::vec3 forward, glm::vec3 up, float width) {
        StereoGains gains{};

        const float length = glm::length(toSource);
        if (length < 1e-5f) {
            return gains;
        }

        const glm::vec3 direction = toSource / length;
        const glm::vec3 ahead =
            glm::length(forward) > 1e-5f ? glm::normalize(forward) : glm::vec3{0.f, 0.f, 1.f};
        const glm::vec3 overhead =
            glm::length(up) > 1e-5f ? glm::normalize(up) : glm::vec3{0.f, 1.f, 0.f};

        // The listener's right is forward crossed with up - the same formula
        // `moveDirection` turns a stick into a step with, and deliberately
        // so: a player who strafes right and a sound panned right have to
        // mean the same side of the world. One of the two being mirrored is
        // the classic bug here, inaudible on a laptop speaker and obvious on
        // headphones.
        //
        // A degenerate pair - looking straight up - leaves no right to speak
        // of, and the centre is the honest answer.
        const glm::vec3 right = glm::cross(ahead, overhead);
        if (glm::length(right) < 1e-5f) {
            return gains;
        }

        // How far to one side, from -1 (hard left) to +1 (hard right).
        const float side =
            std::clamp(glm::dot(direction, glm::normalize(right)) * width, -1.f, 1.f);

        // Constant power: the pan angle sweeps a quarter turn, and the gains
        // are its cosine and sine, so left^2 + right^2 stays 1 the whole way
        // across. Linear panning would dip audibly through the middle.
        const float angle = (side + 1.f) * 0.25f * 3.14159265358979323846f;
        gains.left = std::cos(angle);
        gains.right = std::sin(angle);
        return gains;
    }

    float sampleMono(const SoundData& sound, double cursor) {
        const std::size_t frames = sound.frames();
        if (frames == 0 || sound.channels == 0 || cursor < 0.0) {
            return 0.f;
        }
        const auto first = static_cast<std::size_t>(cursor);
        if (first >= frames) {
            return 0.f;
        }
        // The last frame interpolates with itself rather than with whatever
        // is past the end of the buffer.
        const std::size_t second = std::min(first + 1, frames - 1);
        const auto blend = static_cast<float>(cursor - static_cast<double>(first));

        float value = 0.f;
        for (std::uint32_t channel = 0; channel < sound.channels; channel++) {
            const float a = sound.samples[first * sound.channels + channel];
            const float b = sound.samples[second * sound.channels + channel];
            value += a + (b - a) * blend;
        }
        return value / static_cast<float>(sound.channels);
    }

    double playbackStep(std::uint32_t clipRate, std::uint32_t deviceRate, float pitch) {
        if (deviceRate == 0) {
            return 0.0;
        }
        // A pitch of zero or less would stop the cursor, which is a voice
        // that never ends rather than a voice played slowly.
        const double rate = static_cast<double>(pitch > 0.f ? pitch : 1.f);
        return static_cast<double>(clipRate) / static_cast<double>(deviceRate) * rate;
    }

    VoiceGains spatialGains(
        glm::vec3 listener,
        glm::vec3 forward,
        glm::vec3 up,
        glm::vec3 source,
        float minDistance,
        float maxDistance,
        float volume) {
        const glm::vec3 toSource = source - listener;
        const float attenuation =
            distanceAttenuation(glm::length(toSource), minDistance, maxDistance);
        const float level = std::max(volume, 0.f) * attenuation;

        const StereoGains pan = stereoPan(toSource, forward, up);
        return VoiceGains{pan.left * level, pan.right * level};
    }

}  // namespace ege
