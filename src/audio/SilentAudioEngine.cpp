#include "audio/SilentAudioEngine.hpp"

#include <algorithm>

namespace ege {

    const char* audioBusName(AudioBus bus) {
        switch (bus) {
            case AudioBus::master:
                return "master";
            case AudioBus::music:
                return "music";
            case AudioBus::effects:
                return "effects";
            case AudioBus::count:
                break;
        }
        return "unknown";
    }

    SilentAudioEngine::SilentAudioEngine() {
        busVolumes.fill(1.f);
    }

    SoundId SilentAudioEngine::load(const SoundData& data) {
        const SoundId id = nextSound++;
        sounds.emplace(id, data.duration());
        return id;
    }

    void SilentAudioEngine::unload(SoundId sound) {
        // Voices first: a clip that went away while something was playing it
        // would leave a voice nobody can ever stop.
        for (auto voice = voices.begin(); voice != voices.end();) {
            voice = voice->second.sound == sound ? voices.erase(voice) : std::next(voice);
        }
        sounds.erase(sound);
    }

    VoiceId SilentAudioEngine::play(SoundId sound, const VoiceSettings& settings) {
        const auto found = sounds.find(sound);
        if (found == sounds.end()) {
            return invalidVoice;
        }

        Voice voice{};
        voice.sound = sound;
        voice.settings = settings;
        // Pitch changes how long a clip takes as well as what it sounds like,
        // which matters here because how long is the only thing this backend
        // models.
        const float pitch = settings.pitch > 0.f ? settings.pitch : 1.f;
        voice.remaining = found->second / pitch;

        const VoiceId id = nextVoice++;
        voices.emplace(id, voice);
        return id;
    }

    void SilentAudioEngine::stop(VoiceId voice) {
        voices.erase(voice);
    }

    void SilentAudioEngine::stopAll() {
        voices.clear();
    }

    bool SilentAudioEngine::playing(VoiceId voice) const {
        return voices.find(voice) != voices.end();
    }

    void SilentAudioEngine::setVoicePosition(VoiceId voice, glm::vec3 position) {
        if (const auto found = voices.find(voice); found != voices.end()) {
            found->second.settings.position = position;
        }
    }

    void SilentAudioEngine::setVoiceVolume(VoiceId voice, float volume) {
        if (const auto found = voices.find(voice); found != voices.end()) {
            found->second.settings.volume = volume;
        }
    }

    void SilentAudioEngine::setBusVolume(AudioBus bus, float volume) {
        if (bus != AudioBus::count) {
            busVolumes[static_cast<std::size_t>(bus)] = std::clamp(volume, 0.f, 1.f);
        }
    }

    float SilentAudioEngine::busVolume(AudioBus bus) const {
        return bus == AudioBus::count ? 0.f : busVolumes[static_cast<std::size_t>(bus)];
    }

    void SilentAudioEngine::update(float deltaSeconds) {
        for (auto voice = voices.begin(); voice != voices.end();) {
            Voice& state = voice->second;
            if (state.settings.loop) {
                // A looping voice ends when it is stopped and not before,
                // which is the whole of what looping means to a mixer.
                ++voice;
                continue;
            }
            state.remaining -= deltaSeconds;
            voice = state.remaining <= 0.f ? voices.erase(voice) : std::next(voice);
        }
    }

}  // namespace ege
