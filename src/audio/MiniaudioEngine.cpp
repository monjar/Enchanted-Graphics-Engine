// The only file in the engine that knows what the audio library is called.
//
// Everything else talks to AudioEngine, exactly as everything outside
// JoltPhysicsWorld.cpp talks to PhysicsWorld. The whole of miniaudio is
// compiled here, once, behind the same wall.
//
// The mixing is ours rather than miniaudio's. Its high-level engine would do
// spatialisation, buses and voice management for us, and taking it would mean
// the answer to "how loud is that" living inside a library nobody can test
// against - so what is used here is the *device*: a callback asking for
// frames. Everything above that callback is AudioMath and a mixer small
// enough to read, which is what keeps the sound of the world a property of
// the engine rather than of its dependency.

#include "audio/AudioEngine.hpp"
#include "audio/AudioMath.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(EGE_HAS_MINIAUDIO)

// What we do not need, and every one of them is a decoder, a backend or a
// feature the engine has its own answer for. A single header that compiles
// everything by default is a single header that takes a minute to build.
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace ege {

    namespace {

        // A voice, as the mixer sees it.
        //
        // Positions and gains are read on the audio thread and written on the
        // main one, so everything here is guarded by the engine's mutex. The
        // callback holds it for the length of one buffer, which is tens of
        // microseconds of work on a lock nothing else holds for long.
        struct MixVoice {
            SoundId sound = invalidSound;
            VoiceSettings settings{};
            // Where playback has got to, in frames of the clip.
            double cursor = 0.0;
            bool finished = false;
        };

    }  // namespace

    // ---- The backend --------------------------------------------------------

    class MiniaudioEngine final : public AudioEngine {
    public:
        explicit MiniaudioEngine(const Settings& settings) : requested{settings} {
            busVolumes.fill(1.f);
        }

        ~MiniaudioEngine() override {
            if (started) {
                // Uninit stops the callback and waits for it, which is what
                // makes it safe for the members below to go away next.
                ma_device_uninit(&device);
            }
        }

        // Opens a device. False means this machine has none, or none that
        // would start - either way the caller falls back to silence.
        bool start() {
            ma_device_config config = ma_device_config_init(ma_device_type_playback);
            config.playback.format = ma_format_f32;
            config.playback.channels = requested.channels;
            config.sampleRate = requested.sampleRate;
            config.dataCallback = &MiniaudioEngine::feed;
            config.pUserData = this;

            if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
                return false;
            }

            // miniaudio's own null backend is a last resort that always
            // succeeds: it accepts audio and discards it. Taking it would
            // mean an engine that reports itself audible while nobody can
            // hear anything, plus a real audio thread mixing into a bin -
            // and the engine already has an honest answer for a machine with
            // no sound device.
            if (device.pContext->backend == ma_backend_null) {
                ma_device_uninit(&device);
                return false;
            }

            if (ma_device_start(&device) != MA_SUCCESS) {
                ma_device_uninit(&device);
                return false;
            }
            started = true;
            deviceRate = device.sampleRate;
            deviceChannels = device.playback.channels;
            return true;
        }

        bool audible() const override { return true; }

        const char* backendName() const override {
            return started ? ma_get_backend_name(device.pContext->backend) : "miniaudio";
        }

        SoundId load(const SoundData& data) override {
            SoundData sound = data;
            sound.channels = std::max(data.channels, 1u);
            sound.sampleRate = std::max(data.sampleRate, 1u);

            const std::lock_guard<std::mutex> lock{mixer};
            const SoundId id = nextSound++;
            sounds.emplace(id, std::move(sound));
            return id;
        }

        void unload(SoundId sound) override {
            const std::lock_guard<std::mutex> lock{mixer};
            for (auto voice = voices.begin(); voice != voices.end();) {
                voice = voice->second.sound == sound ? voices.erase(voice) : std::next(voice);
            }
            sounds.erase(sound);
        }

        std::size_t soundCount() const override {
            const std::lock_guard<std::mutex> lock{mixer};
            return sounds.size();
        }

        VoiceId play(SoundId sound, const VoiceSettings& settings) override {
            const std::lock_guard<std::mutex> lock{mixer};
            if (sounds.find(sound) == sounds.end()) {
                return invalidVoice;
            }
            MixVoice voice{};
            voice.sound = sound;
            voice.settings = settings;

            const VoiceId id = nextVoice++;
            voices.emplace(id, voice);
            return id;
        }

        void stop(VoiceId voice) override {
            const std::lock_guard<std::mutex> lock{mixer};
            voices.erase(voice);
        }

        void stopAll() override {
            const std::lock_guard<std::mutex> lock{mixer};
            voices.clear();
        }

        bool playing(VoiceId voice) const override {
            const std::lock_guard<std::mutex> lock{mixer};
            return voices.find(voice) != voices.end();
        }

        std::size_t voiceCount() const override {
            const std::lock_guard<std::mutex> lock{mixer};
            return voices.size();
        }

        void setVoicePosition(VoiceId voice, glm::vec3 position) override {
            const std::lock_guard<std::mutex> lock{mixer};
            if (const auto found = voices.find(voice); found != voices.end()) {
                found->second.settings.position = position;
            }
        }

        void setVoiceVolume(VoiceId voice, float volume) override {
            const std::lock_guard<std::mutex> lock{mixer};
            if (const auto found = voices.find(voice); found != voices.end()) {
                found->second.settings.volume = volume;
            }
        }

        void setListener(const AudioListener& where) override {
            const std::lock_guard<std::mutex> lock{mixer};
            ears = where;
        }

        AudioListener listener() const override {
            const std::lock_guard<std::mutex> lock{mixer};
            return ears;
        }

        void setBusVolume(AudioBus bus, float volume) override {
            if (bus == AudioBus::count) {
                return;
            }
            const std::lock_guard<std::mutex> lock{mixer};
            busVolumes[static_cast<std::size_t>(bus)] = std::clamp(volume, 0.f, 1.f);
        }

        float busVolume(AudioBus bus) const override {
            if (bus == AudioBus::count) {
                return 0.f;
            }
            const std::lock_guard<std::mutex> lock{mixer};
            return busVolumes[static_cast<std::size_t>(bus)];
        }

        void update(float) override {
            // The callback marks voices finished rather than erasing them:
            // touching the map from the audio thread would mean allocating on
            // it, and an allocation in a real-time callback is a glitch
            // waiting for a busy moment. Reaping happens here instead.
            const std::lock_guard<std::mutex> lock{mixer};
            for (auto voice = voices.begin(); voice != voices.end();) {
                voice = voice->second.finished ? voices.erase(voice) : std::next(voice);
            }
        }

    private:
        // The audio thread. Everything it does is inside the lock and inside
        // this file.
        static void feed(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
            auto* self = static_cast<MiniaudioEngine*>(device->pUserData);
            self->mix(static_cast<float*>(output), frameCount);
        }

        void mix(float* output, ma_uint32 frameCount) {
            const std::size_t samples = static_cast<std::size_t>(frameCount) * deviceChannels;
            std::fill(output, output + samples, 0.f);

            const std::lock_guard<std::mutex> lock{mixer};
            const float master = busVolumes[static_cast<std::size_t>(AudioBus::master)];

            for (auto& [id, voice] : voices) {
                if (voice.finished) {
                    continue;
                }
                const auto sound = sounds.find(voice.sound);
                if (sound == sounds.end()) {
                    voice.finished = true;
                    continue;
                }
                mixVoice(voice, sound->second, output, frameCount, master);
            }
        }

        void mixVoice(
            MixVoice& voice,
            const SoundData& sound,
            float* output,
            ma_uint32 frameCount,
            float master) {
            const std::size_t frames = sound.frames();
            if (frames == 0) {
                voice.finished = true;
                return;
            }

            // Gains are worked out once per buffer rather than per frame. A
            // buffer is a few milliseconds and nothing in a game moves far
            // enough in that time to be heard stepping.
            const float bus = busVolumes[static_cast<std::size_t>(voice.settings.bus)];
            float left = voice.settings.volume * bus * master;
            float right = left;
            if (voice.settings.spatial) {
                const VoiceGains gains = spatialGains(
                    ears.position,
                    ears.forward,
                    ears.up,
                    voice.settings.position,
                    voice.settings.minDistance,
                    voice.settings.maxDistance,
                    voice.settings.volume);
                left = gains.left * bus * master;
                right = gains.right * bus * master;
            }

            // How far the cursor moves per output frame. The arithmetic is
            // in AudioMath with the rest of it, so the thing most likely to
            // be subtly wrong is the thing a test can check.
            const double step = playbackStep(sound.sampleRate, deviceRate, voice.settings.pitch);

            for (ma_uint32 frame = 0; frame < frameCount; frame++) {
                if (voice.cursor >= static_cast<double>(frames)) {
                    if (!voice.settings.loop) {
                        voice.finished = true;
                        return;
                    }
                    // Wrapped rather than reset, so a loop does not lose the
                    // fraction of a frame it was into the next one - which
                    // over a long loop is a drift you can hear.
                    voice.cursor -= static_cast<double>(frames);
                }

                const float value = sampleMono(sound, voice.cursor);
                float* target = output + static_cast<std::size_t>(frame) * deviceChannels;
                if (deviceChannels == 1) {
                    target[0] += value * (left + right) * 0.5f;
                } else {
                    target[0] += value * left;
                    target[1] += value * right;
                }
                voice.cursor += step;
            }
        }

        Settings requested{};
        ma_device device{};
        bool started = false;
        std::uint32_t deviceRate = 48000;
        std::uint32_t deviceChannels = 2;

        // Held by the audio thread for the length of one buffer and by the
        // main thread for the length of one map operation. Mutable because
        // every query is const and every query has to take it.
        mutable std::mutex mixer;
        std::unordered_map<SoundId, SoundData> sounds;
        std::unordered_map<VoiceId, MixVoice> voices;
        std::array<float, audioBusCount> busVolumes{};
        AudioListener ears{};
        SoundId nextSound = 1;
        VoiceId nextVoice = 1;
    };

}  // namespace ege

#endif  // EGE_HAS_MINIAUDIO

#include "audio/SilentAudioEngine.hpp"

namespace ege {

    std::unique_ptr<AudioEngine> AudioEngine::create(const Settings& settings) {
#if defined(EGE_HAS_MINIAUDIO)
        if (!settings.forceSilent) {
            auto engine = std::make_unique<MiniaudioEngine>(settings);
            if (engine->start()) {
                EGE_INFO("Audio: {} at {} Hz", engine->backendName(), settings.sampleRate);
                return engine;
            }
            // Not a warning: a machine with no sound device is a machine, not
            // a mistake. The line is worth having because "why can I not hear
            // anything" is a question somebody will ask.
            EGE_INFO("Audio: no playback device; running silent");
        } else {
            EGE_INFO("Audio: silenced by request");
        }
#else
        (void)settings;
        EGE_INFO("Audio: built without a backend; running silent");
#endif
        return std::make_unique<SilentAudioEngine>();
    }

}  // namespace ege
