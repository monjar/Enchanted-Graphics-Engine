#pragma once

#include "audio/AudioEngine.hpp"

#include <array>
#include <unordered_map>

namespace ege {

    // Audio for a machine with nothing to play it on.
    //
    // Not a stub, and the distinction is the point. It accepts every clip,
    // hands out voices, expires them after exactly as long as the clip lasts,
    // and answers every query the audible backend answers. A game running on
    // it behaves identically to one running on a sound card - the footsteps
    // still start and stop, the music still ends, the code that waits for a
    // voice to finish still gets its answer - and the only difference is that
    // nobody hears it.
    //
    // That is what makes CI's silence a fact about CI rather than a second
    // code path through the game, and it is why this is the fallback rather
    // than a null pointer: `if (audio)` at every call site is how a subsystem
    // becomes optional and then becomes untested.
    class SilentAudioEngine final : public AudioEngine {
    public:
        SilentAudioEngine();

        bool audible() const override { return false; }

        const char* backendName() const override { return "silent"; }

        SoundId load(const SoundData& data) override;
        void unload(SoundId sound) override;

        std::size_t soundCount() const override { return sounds.size(); }

        VoiceId play(SoundId sound, const VoiceSettings& settings) override;
        void stop(VoiceId voice) override;
        void stopAll() override;
        bool playing(VoiceId voice) const override;

        std::size_t voiceCount() const override { return voices.size(); }

        void setVoicePosition(VoiceId voice, glm::vec3 position) override;
        void setVoiceVolume(VoiceId voice, float volume) override;

        void setListener(const AudioListener& where) override { ears = where; }

        AudioListener listener() const override { return ears; }

        void setBusVolume(AudioBus bus, float volume) override;
        float busVolume(AudioBus bus) const override;

        void update(float deltaSeconds) override;

    private:
        struct Voice {
            SoundId sound = invalidSound;
            VoiceSettings settings{};
            // Seconds of clip left. The only thing this backend models, and
            // the only thing gameplay can actually observe.
            float remaining = 0.f;
        };

        // Clip durations, which is all a silent mixer needs of a clip.
        std::unordered_map<SoundId, float> sounds;
        std::unordered_map<VoiceId, Voice> voices;
        std::array<float, audioBusCount> busVolumes{};
        AudioListener ears{};
        SoundId nextSound = 1;
        VoiceId nextVoice = 1;
    };

}  // namespace ege
