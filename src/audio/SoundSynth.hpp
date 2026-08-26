#pragma once

#include "audio/AudioEngine.hpp"

#include <string>
#include <vector>

namespace ege {

    // A sound described rather than recorded.
    //
    // This exists because of a rule the repository has kept since the first
    // commit: **a clean checkout ships no binary assets**. Models are text
    // glTF, materials and scenes are JSON, and a `.wav` would be the first
    // file in the tree nobody can read in a diff or edit without a tool.
    //
    // So the demo's sounds are *descriptions* - a few oscillators, an
    // envelope, a pitch sweep - rendered to samples when they load. Which
    // turns out to be what placeholder audio wants anyway: a coin is a rising
    // blip, a fall is a falling one, and either can be retuned by editing two
    // numbers and saving the file, with hot reload doing the rest.
    //
    // Real projects load real files; `.wav`, `.mp3` and `.flac` decode
    // through the audio backend. This is the format for the ones that come
    // with the engine.

    enum class Waveform { sine, square, saw, triangle, noise };

    // One oscillator with an envelope on it. A sound is a few of these summed.
    struct SoundLayer {
        Waveform wave = Waveform::sine;
        // Hertz at the start, and at the end if `sweep` is on: a blip that
        // rises reads as a reward and one that falls reads as a loss, which
        // is most of what a placeholder sound has to do.
        float frequency = 440.f;
        float frequencyEnd = 440.f;
        float gain = 0.5f;
        // Seconds. The attack keeps a sound from clicking at the start, the
        // decay keeps it from clicking at the end, and between them they are
        // the whole envelope: sustain and release matter for instruments and
        // not for a coin.
        float attack = 0.005f;
        float decay = 0.08f;
        // Where in the sound this layer starts, so two blips can be one file.
        float delay = 0.f;
    };

    struct SoundRecipe {
        std::uint32_t sampleRate = 48000;
        float duration = 0.25f;
        std::vector<SoundLayer> layers;
    };

    // Renders a recipe to samples. Mono, because everything the engine
    // generates is placed in the world by the mixer rather than by itself.
    SoundData renderSound(const SoundRecipe& recipe);

    // Reads a `.egesound` document. Returns false and says why on anything it
    // cannot make sense of; a recipe that renders to nothing is not an error,
    // because a silent sound is a legitimate placeholder.
    bool parseSoundRecipe(const std::string& document, SoundRecipe& out, std::string& error);

    const char* waveformName(Waveform wave);

}  // namespace ege
