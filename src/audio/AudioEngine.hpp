#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ege {

    // A loaded sound, and one playing instance of one.
    //
    // Two handle spaces, because they are two different things: a clip is
    // loaded once and played many times, and stopping a footstep must not
    // unload footsteps.
    using SoundId = std::uint32_t;
    using VoiceId = std::uint32_t;

    inline constexpr SoundId invalidSound = 0xFFFFFFFFu;
    inline constexpr VoiceId invalidVoice = 0xFFFFFFFFu;

    // What a voice is mixed through. Three is the smallest set that is
    // actually useful: a player turns the music down without turning the
    // footsteps down, and pausing ducks the music without silencing the menu.
    enum class AudioBus { master, music, effects, count };

    inline constexpr std::size_t audioBusCount = static_cast<std::size_t>(AudioBus::count);

    const char* audioBusName(AudioBus bus);

    // Decoded audio, as plain interleaved floats.
    //
    // The engine's own format rather than the backend's: a clip is data, and
    // data that only one backend understands is data the next backend cannot
    // read. It is also what lets a test build a sound without a decoder.
    struct SoundData {
        std::vector<float> samples;
        std::uint32_t channels = 1;
        std::uint32_t sampleRate = 48000;

        std::size_t frames() const { return channels == 0 ? 0 : samples.size() / channels; }

        float duration() const {
            return sampleRate == 0 ? 0.f
                                   : static_cast<float>(frames()) / static_cast<float>(sampleRate);
        }

        bool empty() const { return samples.empty(); }
    };

    // How one voice should sound.
    struct VoiceSettings {
        float volume = 1.f;
        float pitch = 1.f;
        bool loop = false;
        AudioBus bus = AudioBus::effects;
        // A spatial voice is placed in the world and heard from wherever the
        // listener is; a flat one is heard the same everywhere, which is what
        // music and interface sounds want.
        bool spatial = false;
        glm::vec3 position{0.f};
        // Full volume within `minDistance`, silent past `maxDistance`.
        float minDistance = 1.f;
        float maxDistance = 40.f;
    };

    // Where the ears are.
    struct AudioListener {
        glm::vec3 position{0.f};
        glm::vec3 forward{0.f, 0.f, 1.f};
        glm::vec3 up{0.f, 1.f, 0.f};
    };

    // Sound playback behind an engine-owned interface.
    //
    // The backend today is miniaudio, and nothing outside MiniaudioEngine.cpp
    // knows it: no miniaudio type appears here, which is the same constraint
    // PhysicsWorld puts on Jolt and for the same reasons - the backend stays
    // replaceable, and a library that compiles into one translation unit
    // stays out of every file that touches sound.
    //
    // There are two implementations and both are real. The silent one is not
    // a stub: it accepts every clip, hands out voices, expires them at the
    // right time and answers every query the same way the audible one does.
    // That is what makes "the machine has no sound device" a fact about the
    // machine rather than a different code path through the game - and it is
    // what lets the whole suite and the headless render run on CI, which has
    // no speakers and never will.
    class AudioEngine {
    public:
        struct Settings {
            std::uint32_t sampleRate = 48000;
            std::uint32_t channels = 2;
            // Refuses to open a device even if there is one. What a test
            // wants, and what --silent gives a developer whose colleagues are
            // trying to work.
            bool forceSilent = false;
        };

        // Never null: a machine with no sound device gets the silent backend
        // and a line in the log saying so. Audio failing to start is not a
        // reason for a game not to run.
        static std::unique_ptr<AudioEngine> create(const Settings& settings);

        static std::unique_ptr<AudioEngine> create() { return create(Settings{}); }

        AudioEngine() = default;

        virtual ~AudioEngine() = default;

        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        // Whether anything can actually be heard. Gameplay must not branch on
        // this - every call works either way - but a log line and a stats
        // panel should say which one is running.
        virtual bool audible() const = 0;

        // What the backend is called, for that log line.
        virtual const char* backendName() const = 0;

        // Hands a decoded clip to the mixer. The data is copied: a clip
        // outlives whatever loaded it, and a mixer reading a vector somebody
        // else owns is a mixer reading freed memory on a worker thread.
        virtual SoundId load(const SoundData& data) = 0;

        // Stops every voice playing it, then forgets it.
        virtual void unload(SoundId sound) = 0;

        virtual std::size_t soundCount() const = 0;

        // Starts a voice. invalidVoice when the clip is unknown - which the
        // caller may ignore, because a sound that did not play is not an
        // error worth handling at every call site.
        virtual VoiceId play(SoundId sound, const VoiceSettings& settings) = 0;

        virtual void stop(VoiceId voice) = 0;

        virtual void stopAll() = 0;

        virtual bool playing(VoiceId voice) const = 0;

        virtual std::size_t voiceCount() const = 0;

        // Moves a voice that is already playing - a footstep belongs to a
        // character that is still walking.
        virtual void setVoicePosition(VoiceId voice, glm::vec3 position) = 0;

        virtual void setVoiceVolume(VoiceId voice, float volume) = 0;

        virtual void setListener(const AudioListener& listener) = 0;

        virtual AudioListener listener() const = 0;

        // Bus volumes, from 0 to 1. Master multiplies everything.
        virtual void setBusVolume(AudioBus bus, float volume) = 0;

        virtual float busVolume(AudioBus bus) const = 0;

        // Advances the mixer's idea of time and reaps voices that have
        // finished. Called once a frame with the real elapsed time, not the
        // fixed step: sound happens in the world the player is in, and a
        // paused game whose music kept playing is a paused game that sounds
        // right.
        virtual void update(float deltaSeconds) = 0;
    };

}  // namespace ege
