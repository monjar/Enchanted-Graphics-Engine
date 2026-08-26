#include "audio/SoundSynth.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace ege {

    namespace {

        constexpr float twoPi = 6.28318530717958647692f;

        // One cycle of a waveform, from a phase in [0, 1).
        //
        // The naive square and saw are what these are: an effect a hundred
        // milliseconds long has no time to show the aliasing a sustained tone
        // would, and band-limiting them would be arithmetic nobody can hear
        // the benefit of here.
        float oscillator(Waveform wave, float phase, std::uint32_t& noiseState) {
            switch (wave) {
                case Waveform::sine:
                    return std::sin(phase * twoPi);
                case Waveform::square:
                    return phase < 0.5f ? 1.f : -1.f;
                case Waveform::saw:
                    return phase * 2.f - 1.f;
                case Waveform::triangle:
                    return 4.f * std::abs(phase - 0.5f) - 1.f;
                case Waveform::noise:
                    // A deterministic generator rather than rand(): the same
                    // file must render to the same samples on every machine,
                    // or a recorded run stops being reproducible.
                    noiseState = noiseState * 1664525u + 1013904223u;
                    return static_cast<float>(noiseState >> 8) / 8388608.f - 1.f;
            }
            return 0.f;
        }

        // Attack in, decay out, and flat in between when there is room.
        float envelope(float time, float attack, float decay, float duration) {
            if (time < 0.f || time > duration) {
                return 0.f;
            }
            const float rise = attack > 0.f ? std::min(time / attack, 1.f) : 1.f;
            const float remaining = duration - time;
            const float fall = decay > 0.f ? std::min(remaining / decay, 1.f) : 1.f;
            return std::clamp(rise * fall, 0.f, 1.f);
        }

        Waveform waveformFrom(const std::string& name) {
            if (name == "square") {
                return Waveform::square;
            }
            if (name == "saw") {
                return Waveform::saw;
            }
            if (name == "triangle") {
                return Waveform::triangle;
            }
            if (name == "noise") {
                return Waveform::noise;
            }
            return Waveform::sine;
        }

    }  // namespace

    const char* waveformName(Waveform wave) {
        switch (wave) {
            case Waveform::sine:
                return "sine";
            case Waveform::square:
                return "square";
            case Waveform::saw:
                return "saw";
            case Waveform::triangle:
                return "triangle";
            case Waveform::noise:
                return "noise";
        }
        return "sine";
    }

    SoundData renderSound(const SoundRecipe& recipe) {
        SoundData data{};
        data.channels = 1;
        data.sampleRate = std::max(recipe.sampleRate, 1u);

        const auto frames = static_cast<std::size_t>(
            std::max(recipe.duration, 0.f) * static_cast<float>(data.sampleRate));
        data.samples.assign(frames, 0.f);
        if (frames == 0 || recipe.layers.empty()) {
            return data;
        }

        for (const SoundLayer& layer : recipe.layers) {
            // Phase is accumulated rather than computed from the time, which
            // is what makes a pitch sweep continuous: a sweep evaluated as
            // sin(2*pi*f(t)*t) jumps every time the frequency changes,
            // audibly, and is the classic way to get a sweep wrong.
            float phase = 0.f;
            std::uint32_t noiseState = 0x9E3779B9u;
            const float layerDuration = std::max(recipe.duration - layer.delay, 0.f);
            if (layerDuration <= 0.f) {
                continue;
            }

            for (std::size_t frame = 0; frame < frames; frame++) {
                const float time =
                    static_cast<float>(frame) / static_cast<float>(data.sampleRate) - layer.delay;
                if (time < 0.f) {
                    continue;
                }

                const float sweep =
                    layerDuration > 0.f ? std::clamp(time / layerDuration, 0.f, 1.f) : 0.f;
                const float frequency =
                    layer.frequency + (layer.frequencyEnd - layer.frequency) * sweep;

                const float level = envelope(time, layer.attack, layer.decay, layerDuration);
                data.samples[frame] +=
                    oscillator(layer.wave, phase, noiseState) * layer.gain * level;

                phase += frequency / static_cast<float>(data.sampleRate);
                phase -= std::floor(phase);
            }
        }

        // Summed layers can leave the range, and a sample past 1 clips on the
        // way out of the speaker rather than politely. Scaled rather than
        // clamped, because clamping changes the shape and scaling only
        // changes the volume.
        float peak = 0.f;
        for (const float sample : data.samples) {
            peak = std::max(peak, std::abs(sample));
        }
        if (peak > 1.f) {
            for (float& sample : data.samples) {
                sample /= peak;
            }
        }
        return data;
    }

    bool parseSoundRecipe(const std::string& document, SoundRecipe& out, std::string& error) {
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(document);
        } catch (const std::exception& thrown) {
            error = thrown.what();
            return false;
        }
        if (!json.is_object()) {
            error = "a sound recipe is an object";
            return false;
        }

        out = SoundRecipe{};
        out.sampleRate = json.value("sampleRate", 48000u);
        out.duration = json.value("duration", 0.25f);

        const auto layers = json.find("layers");
        if (layers == json.end() || !layers->is_array()) {
            error = "a sound recipe needs a layers array";
            return false;
        }

        for (const nlohmann::json& record : *layers) {
            if (!record.is_object()) {
                continue;
            }
            SoundLayer layer{};
            layer.wave = waveformFrom(record.value("wave", std::string{"sine"}));
            layer.frequency = record.value("frequency", 440.f);
            // A layer that does not say where it is going stays where it is.
            layer.frequencyEnd = record.value("frequencyEnd", layer.frequency);
            layer.gain = record.value("gain", 0.5f);
            layer.attack = record.value("attack", 0.005f);
            layer.decay = record.value("decay", 0.08f);
            layer.delay = record.value("delay", 0.f);
            out.layers.push_back(layer);
        }
        return true;
    }

}  // namespace ege
