// Audio, in the two halves that can be checked without a sound card.
//
// The arithmetic - how loud a source is, which ear it is in, what sample the
// cursor is on - is device-free by construction, which matters more here than
// elsewhere: CI has no speakers, so a wrong answer is inaudible rather than
// visible, and the only thing standing between "we got the pan backwards" and
// shipping it is a test that says which side is which.
//
// The silent backend is checked as a backend rather than as a stub, because
// that is what it is: a game running on it must behave exactly like a game
// running on a sound card.

#include "audio/AudioEngine.hpp"
#include "audio/AudioMath.hpp"
#include "audio/SilentAudioEngine.hpp"
#include "physics/CharacterMotion.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using ege::AudioBus;
using ege::AudioEngine;
using ege::AudioListener;
using ege::SilentAudioEngine;
using ege::SoundData;
using ege::VoiceSettings;

namespace {

    // A second of silence at 48 kHz, which is all most of these need: what
    // they are about is how long it lasts and how loud it is, not what it
    // sounds like.
    SoundData oneSecond(float seconds = 1.f, std::uint32_t channels = 1) {
        SoundData data{};
        data.channels = channels;
        data.sampleRate = 48000;
        data.samples.assign(static_cast<std::size_t>(seconds * 48000.f) * channels, 0.f);
        return data;
    }

}  // namespace

// ---- Distance -------------------------------------------------------------

TEST_CASE("a source is at full volume until it is far enough to fade") {
    // Inside the near radius a source is "here": walking closer must not make
    // it louder without bound, which is what the physically correct law does
    // as distance approaches zero.
    CHECK(ege::distanceAttenuation(0.f, 1.f, 40.f) == doctest::Approx(1.f));
    CHECK(ege::distanceAttenuation(0.5f, 1.f, 40.f) == doctest::Approx(1.f));
    CHECK(ege::distanceAttenuation(1.f, 1.f, 40.f) == doctest::Approx(1.f));

    // And past the far radius it is not mixed at all, which is what lets a
    // mixer stop mixing it.
    CHECK(ege::distanceAttenuation(40.f, 1.f, 40.f) == doctest::Approx(0.f));
    CHECK(ege::distanceAttenuation(400.f, 1.f, 40.f) == doctest::Approx(0.f));
}

TEST_CASE("attenuation falls off, and never rises") {
    float previous = ege::distanceAttenuation(1.f, 1.f, 40.f);
    for (float distance = 1.5f; distance <= 40.f; distance += 0.5f) {
        const float here = ege::distanceAttenuation(distance, 1.f, 40.f);
        CHECK(here <= previous);
        CHECK(here >= 0.f);
        CHECK(here <= 1.f);
        previous = here;
    }
    // It reaches zero at the far edge rather than stepping off a cliff there:
    // a source walking out of range should not stop mid-note.
    CHECK(ege::distanceAttenuation(39.5f, 1.f, 40.f) < 0.02f);
}

TEST_CASE("a source with no range is on top of you or inaudible") {
    // Degenerate settings answer rather than divide.
    CHECK(ege::distanceAttenuation(0.5f, 1.f, 1.f) == doctest::Approx(1.f));
    CHECK(ege::distanceAttenuation(2.f, 1.f, 1.f) == doctest::Approx(0.f));
    // Ranges the wrong way round collapse to the same rule rather than to a
    // division: full volume inside minDistance, nothing outside it.
    CHECK(ege::distanceAttenuation(2.f, 5.f, 1.f) == doctest::Approx(1.f));
    CHECK(ege::distanceAttenuation(7.f, 5.f, 1.f) == doctest::Approx(0.f));
}

// ---- Panning --------------------------------------------------------------

TEST_CASE("a sound is in the ear it is beside") {
    // The scene's convention, not the textbook's: this engine looks down +Z
    // and its worlds treat -Y as up, and right is cross(forward, up) - the
    // same formula the character controller turns a stick into a step with.
    const glm::vec3 forward{0.f, 0.f, 1.f};
    const glm::vec3 up{0.f, -1.f, 0.f};

    const ege::StereoGains right = ege::stereoPan({1.f, 0.f, 0.f}, forward, up);
    CHECK(right.right > right.left);
    CHECK(right.right == doctest::Approx(1.f));
    CHECK(right.left == doctest::Approx(0.f));

    const ege::StereoGains left = ege::stereoPan({-1.f, 0.f, 0.f}, forward, up);
    CHECK(left.left > left.right);
    CHECK(left.left == doctest::Approx(1.f));
}

TEST_CASE("which ear a sound is in is the side stepping that way would take you") {
    // The cross-check that keeps two conventions from drifting apart. A
    // player who strafes right and a sound panned right must mean the same
    // side of the world - one of them being mirrored is the classic bug here,
    // and it is inaudible on a machine with one speaker and obvious on
    // headphones.
    const glm::vec3 forward{0.f, 0.f, 1.f};
    const glm::vec3 up{0.f, -1.f, 0.f};

    const glm::vec3 stepped = ege::moveDirection({1.f, 0.f}, forward, up);
    const ege::StereoGains panned = ege::stereoPan(stepped, forward, up);
    CHECK(panned.right > panned.left);
    CHECK(panned.right == doctest::Approx(1.f));
}

TEST_CASE("straight ahead and straight behind are both centred") {
    const glm::vec3 forward{0.f, 0.f, 1.f};
    const glm::vec3 up{0.f, -1.f, 0.f};

    const ege::StereoGains ahead = ege::stereoPan({0.f, 0.f, 5.f}, forward, up);
    CHECK(ahead.left == doctest::Approx(ahead.right));

    // Behind sounds the same as ahead, which is what two speakers can do and
    // is the honest limit of stereo panning: front and back are the same
    // angle from both ears.
    const ege::StereoGains behind = ege::stereoPan({0.f, 0.f, -5.f}, forward, up);
    CHECK(behind.left == doctest::Approx(behind.right));
    CHECK(behind.left == doctest::Approx(ahead.left));
}

TEST_CASE("panning keeps its power across the whole sweep") {
    const glm::vec3 forward{0.f, 0.f, 1.f};
    const glm::vec3 up{0.f, -1.f, 0.f};

    // The reason for the cosine and sine rather than a straight line: a sound
    // crossing the centre must not dip, and with linear gains it audibly
    // does.
    for (float angle = 0.f; angle < 6.28f; angle += 0.2f) {
        const glm::vec3 around{std::sin(angle), 0.f, std::cos(angle)};
        const ege::StereoGains gains = ege::stereoPan(around, forward, up);
        CHECK(gains.left * gains.left + gains.right * gains.right == doctest::Approx(1.f));
    }
}

TEST_CASE("a sound with nowhere to be is centred rather than undefined") {
    // On top of the listener: no direction to pan along.
    const ege::StereoGains here =
        ege::stereoPan({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, -1.f, 0.f});
    CHECK(here.left == doctest::Approx(here.right));

    // Looking straight up: forward and up are the same line, so there is no
    // "right" to be on.
    const ege::StereoGains overhead =
        ege::stereoPan({1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f});
    CHECK(overhead.left == doctest::Approx(overhead.right));
}

TEST_CASE("the listener's facing decides which ear, not the world's axes") {
    const glm::vec3 up{0.f, -1.f, 0.f};
    const glm::vec3 source{1.f, 0.f, 0.f};

    const ege::StereoGains facingAway = ege::stereoPan(source, {0.f, 0.f, 1.f}, up);
    // Turned to face the other way, the same source is in the other ear.
    const ege::StereoGains turned = ege::stereoPan(source, {0.f, 0.f, -1.f}, up);

    CHECK(facingAway.right == doctest::Approx(turned.left));
    CHECK(facingAway.left == doctest::Approx(turned.right));
}

TEST_CASE("the two together are what a voice is mixed at") {
    const ege::VoiceGains near = ege::spatialGains(
        {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, 1.f, 40.f, 1.f);
    const ege::VoiceGains far = ege::spatialGains(
        {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, -1.f, 0.f}, {20.f, 0.f, 0.f}, 1.f, 40.f, 1.f);

    // Same side, quieter.
    CHECK(near.right > near.left);
    CHECK(far.right > far.left);
    CHECK(far.right < near.right);

    // And the source's own volume multiplies through.
    const ege::VoiceGains half = ege::spatialGains(
        {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, 1.f, 40.f, 0.5f);
    CHECK(half.right == doctest::Approx(near.right * 0.5f));
}

// ---- Reading a clip -------------------------------------------------------

TEST_CASE("the cursor reads between frames rather than snapping to one") {
    SoundData ramp{};
    ramp.channels = 1;
    ramp.sampleRate = 48000;
    ramp.samples = {0.f, 1.f, 2.f, 3.f};

    CHECK(ege::sampleMono(ramp, 0.0) == doctest::Approx(0.f));
    CHECK(ege::sampleMono(ramp, 1.0) == doctest::Approx(1.f));
    // Halfway between the second and third frames. Snapping to the nearest
    // instead is the difference between a clip and a clip with a hiss on it.
    CHECK(ege::sampleMono(ramp, 1.5) == doctest::Approx(1.5f));
    CHECK(ege::sampleMono(ramp, 2.25) == doctest::Approx(2.25f));

    // The last frame interpolates with itself rather than with whatever is
    // past the end of the buffer.
    CHECK(ege::sampleMono(ramp, 3.5) == doctest::Approx(3.f));
    // And past the end there is nothing to read.
    CHECK(ege::sampleMono(ramp, 4.0) == doctest::Approx(0.f));
    CHECK(ege::sampleMono(ramp, -1.0) == doctest::Approx(0.f));
}

TEST_CASE("a stereo clip is read as the average of its channels") {
    SoundData stereo{};
    stereo.channels = 2;
    stereo.sampleRate = 48000;
    stereo.samples = {0.f, 1.f, 2.f, 3.f};  // two frames: (0,1) and (2,3)

    CHECK(stereo.frames() == 2);
    CHECK(ege::sampleMono(stereo, 0.0) == doctest::Approx(0.5f));
    CHECK(ege::sampleMono(stereo, 1.0) == doctest::Approx(2.5f));
}

TEST_CASE("the resampling step is the ratio of the rates, times the pitch") {
    CHECK(ege::playbackStep(48000, 48000, 1.f) == doctest::Approx(1.0));
    // A clip recorded at half the device's rate advances half a frame per
    // output frame, which is what makes it come out at the right speed.
    CHECK(ege::playbackStep(24000, 48000, 1.f) == doctest::Approx(0.5));
    CHECK(ege::playbackStep(48000, 44100, 1.f) == doctest::Approx(48000.0 / 44100.0));
    CHECK(ege::playbackStep(48000, 48000, 2.f) == doctest::Approx(2.0));

    // A pitch of zero would stop the cursor, which is a voice that never ends
    // rather than a voice played slowly.
    CHECK(ege::playbackStep(48000, 48000, 0.f) == doctest::Approx(1.0));
    CHECK(ege::playbackStep(48000, 48000, -1.f) == doctest::Approx(1.0));
    CHECK(ege::playbackStep(48000, 0, 1.f) == doctest::Approx(0.0));
}

// ---- The silent backend ---------------------------------------------------

TEST_CASE("a silent engine is a backend, not a stub") {
    SilentAudioEngine audio;
    CHECK_FALSE(audio.audible());

    const ege::SoundId clip = audio.load(oneSecond(0.5f));
    CHECK(audio.soundCount() == 1);

    const ege::VoiceId voice = audio.play(clip, VoiceSettings{});
    CHECK(voice != ege::invalidVoice);
    CHECK(audio.playing(voice));
    CHECK(audio.voiceCount() == 1);

    // Half a second of clip is over after half a second, which is the whole
    // point: code that waits for a voice to finish gets the same answer on a
    // machine with no speakers.
    audio.update(0.3f);
    CHECK(audio.playing(voice));
    audio.update(0.3f);
    CHECK_FALSE(audio.playing(voice));
    CHECK(audio.voiceCount() == 0);
}

TEST_CASE("a looping voice plays until it is stopped") {
    SilentAudioEngine audio;
    const ege::SoundId clip = audio.load(oneSecond(0.2f));

    VoiceSettings settings{};
    settings.loop = true;
    const ege::VoiceId voice = audio.play(clip, settings);

    for (int i = 0; i < 100; i++) {
        audio.update(0.1f);
    }
    CHECK(audio.playing(voice));

    audio.stop(voice);
    CHECK_FALSE(audio.playing(voice));
}

TEST_CASE("pitch changes how long a voice lasts") {
    SilentAudioEngine audio;
    const ege::SoundId clip = audio.load(oneSecond(1.f));

    VoiceSettings settings{};
    settings.pitch = 2.f;
    const ege::VoiceId quick = audio.play(clip, settings);

    // Twice the pitch is half the time - the same arithmetic the audible
    // mixer's cursor does, and the reason this backend models duration at
    // all.
    audio.update(0.4f);
    CHECK(audio.playing(quick));
    audio.update(0.2f);
    CHECK_FALSE(audio.playing(quick));
}

TEST_CASE("unloading a clip stops what was playing it") {
    SilentAudioEngine audio;
    const ege::SoundId clip = audio.load(oneSecond(10.f));
    const ege::VoiceId voice = audio.play(clip, VoiceSettings{});
    REQUIRE(audio.playing(voice));

    // Otherwise there is a voice nobody can ever stop, playing a clip that no
    // longer exists.
    audio.unload(clip);
    CHECK_FALSE(audio.playing(voice));
    CHECK(audio.soundCount() == 0);
    CHECK(audio.voiceCount() == 0);
}

TEST_CASE("playing a clip that is not loaded is refused rather than crashed") {
    SilentAudioEngine audio;
    CHECK(audio.play(ege::invalidSound, VoiceSettings{}) == ege::invalidVoice);
    CHECK(audio.play(12345, VoiceSettings{}) == ege::invalidVoice);
    CHECK(audio.voiceCount() == 0);

    // And stopping something that is not playing is what the caller wanted
    // anyway.
    audio.stop(ege::invalidVoice);
    audio.stop(999);
}

TEST_CASE("buses and the listener answer back") {
    SilentAudioEngine audio;
    CHECK(audio.busVolume(AudioBus::master) == doctest::Approx(1.f));

    audio.setBusVolume(AudioBus::music, 0.25f);
    CHECK(audio.busVolume(AudioBus::music) == doctest::Approx(0.25f));
    CHECK(audio.busVolume(AudioBus::effects) == doctest::Approx(1.f));

    // Clamped, because a bus at 400% is a distorted mix and a bus at -1 is a
    // sign flip nobody meant.
    audio.setBusVolume(AudioBus::master, 4.f);
    CHECK(audio.busVolume(AudioBus::master) == doctest::Approx(1.f));
    audio.setBusVolume(AudioBus::master, -1.f);
    CHECK(audio.busVolume(AudioBus::master) == doctest::Approx(0.f));

    AudioListener ears{};
    ears.position = {1.f, 2.f, 3.f};
    audio.setListener(ears);
    CHECK(audio.listener().position.z == doctest::Approx(3.f));
}

TEST_CASE("the factory always hands back an engine") {
    // Whatever the machine has or has not got, there is something to call.
    // `if (audio)` at every call site is how a subsystem becomes optional and
    // then becomes untested.
    AudioEngine::Settings settings{};
    settings.forceSilent = true;
    const std::unique_ptr<AudioEngine> audio = AudioEngine::create(settings);
    REQUIRE(audio != nullptr);
    CHECK_FALSE(audio->audible());
    CHECK(std::string{audio->backendName()} == "silent");
}

// ---- Sounds described rather than recorded ---------------------------------

#include "assets/AssetDatabase.hpp"
#include "audio/SoundSynth.hpp"

#include <filesystem>
#include <fstream>

using ege::SoundRecipe;

namespace {

    SoundRecipe blip(float duration = 0.1f) {
        SoundRecipe recipe{};
        recipe.sampleRate = 48000;
        recipe.duration = duration;
        ege::SoundLayer layer{};
        layer.wave = ege::Waveform::sine;
        layer.frequency = 440.f;
        layer.gain = 0.5f;
        recipe.layers.push_back(layer);
        return recipe;
    }

}  // namespace

TEST_CASE("a rendered sound is as long as it says it is") {
    const SoundData rendered = ege::renderSound(blip(0.25f));
    CHECK(rendered.channels == 1);
    CHECK(rendered.sampleRate == 48000);
    CHECK(rendered.frames() == 12000);
    CHECK(rendered.duration() == doctest::Approx(0.25f));
}

TEST_CASE("a rendered sound starts and ends at silence") {
    const SoundData rendered = ege::renderSound(blip());
    REQUIRE(!rendered.empty());

    // The envelope's whole job: a sound that begins at full amplitude clicks,
    // and so does one that stops there.
    CHECK(std::abs(rendered.samples.front()) < 0.01f);
    CHECK(std::abs(rendered.samples.back()) < 0.01f);

    // And it is not silent in between, which is the other half of the claim.
    float peak = 0.f;
    for (const float sample : rendered.samples) {
        peak = std::max(peak, std::abs(sample));
    }
    CHECK(peak > 0.3f);
}

TEST_CASE("a swept sound stays continuous through the sweep") {
    SoundRecipe recipe = blip(0.3f);
    recipe.layers[0].frequency = 200.f;
    recipe.layers[0].frequencyEnd = 3000.f;

    const SoundData rendered = ege::renderSound(recipe);
    REQUIRE(rendered.frames() > 100);

    // The reason phase is accumulated rather than computed from the time: a
    // sweep evaluated as sin(2*pi*f(t)*t) jumps every time the frequency
    // changes, and the jumps are audible. Adjacent samples of a wave at three
    // kilohertz can differ by a good deal, but never by more than the whole
    // range.
    for (std::size_t frame = 1; frame < rendered.frames(); frame++) {
        const float step = std::abs(rendered.samples[frame] - rendered.samples[frame - 1]);
        CHECK(step < 0.75f);
    }
}

TEST_CASE("layers are summed and the result is kept in range") {
    SoundRecipe loud{};
    loud.sampleRate = 48000;
    loud.duration = 0.2f;
    for (int i = 0; i < 6; i++) {
        ege::SoundLayer layer{};
        layer.frequency = 300.f + static_cast<float>(i) * 30.f;
        layer.gain = 0.9f;
        layer.attack = 0.001f;
        layer.decay = 0.02f;
        loud.layers.push_back(layer);
    }

    const SoundData rendered = ege::renderSound(loud);
    for (const float sample : rendered.samples) {
        // Scaled rather than clamped: clipping changes the shape of a sound
        // and scaling only changes how loud it is.
        CHECK(sample <= 1.0001f);
        CHECK(sample >= -1.0001f);
    }
}

TEST_CASE("the same recipe renders to the same samples every time") {
    SoundRecipe noisy{};
    noisy.sampleRate = 48000;
    noisy.duration = 0.05f;
    ege::SoundLayer layer{};
    layer.wave = ege::Waveform::noise;
    layer.gain = 0.5f;
    noisy.layers.push_back(layer);

    const SoundData first = ege::renderSound(noisy);
    const SoundData second = ege::renderSound(noisy);
    REQUIRE(first.samples.size() == second.samples.size());

    // Bit for bit, even for noise: a recorded run has to be reproducible, and
    // a generator seeded from the clock would make one frame of a recording
    // differ from the next run's.
    for (std::size_t i = 0; i < first.samples.size(); i++) {
        CHECK(first.samples[i] == second.samples[i]);
    }
}

TEST_CASE("a recipe with nothing in it renders silence rather than failing") {
    SoundRecipe empty{};
    empty.duration = 0.1f;
    const SoundData rendered = ege::renderSound(empty);
    CHECK(rendered.frames() == 4800);
    for (const float sample : rendered.samples) {
        CHECK(sample == doctest::Approx(0.f));
    }

    SoundRecipe instant = blip(0.f);
    CHECK(ege::renderSound(instant).empty());
}

TEST_CASE("a sound recipe reads from its document, with defaults") {
    SoundRecipe recipe{};
    std::string error;
    const bool read = ege::parseSoundRecipe(
        R"({"version": 1, "duration": 0.4, "layers": [
              {"wave": "square", "frequency": 220, "frequencyEnd": 440, "gain": 0.3},
              {"wave": "noise"}
            ]})",
        recipe,
        error);

    REQUIRE(read);
    CHECK(recipe.duration == doctest::Approx(0.4f));
    CHECK(recipe.sampleRate == 48000);
    REQUIRE(recipe.layers.size() == 2);
    CHECK(recipe.layers[0].wave == ege::Waveform::square);
    CHECK(recipe.layers[0].frequencyEnd == doctest::Approx(440.f));
    // A layer that does not say where it is going stays where it is.
    CHECK(recipe.layers[1].wave == ege::Waveform::noise);
    CHECK(recipe.layers[1].frequencyEnd == doctest::Approx(recipe.layers[1].frequency));
}

TEST_CASE("a recipe that will not read says so rather than rendering rubbish") {
    SoundRecipe recipe{};
    std::string error;

    CHECK_FALSE(ege::parseSoundRecipe("{ not json", recipe, error));
    CHECK(!error.empty());
    CHECK_FALSE(ege::parseSoundRecipe("[1, 2, 3]", recipe, error));
    CHECK_FALSE(ege::parseSoundRecipe(R"({"duration": 0.2})", recipe, error));
}

TEST_CASE("a sound file is catalogued and loaded like everything else") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "ege-assets-sound";
    std::error_code errorCode;
    fs::remove_all(root, errorCode);
    fs::create_directories(root / "sounds", errorCode);

    {
        std::ofstream file{root / "sounds" / "beep.egesound"};
        file << R"({"version": 1, "duration": 0.15,
                    "layers": [{"wave": "sine", "frequency": 660, "gain": 0.5}]})";
    }

    ege::AudioEngine::Settings settings{};
    settings.forceSilent = true;
    const std::unique_ptr<ege::AudioEngine> audio = ege::AudioEngine::create(settings);

    ege::AssetDatabase& database = ege::AssetDatabase::instance();
    database.clear();
    CHECK(database.scan(root) == 1);

    const ege::AssetRecord* record = database.findByPath(fs::path{"sounds"} / "beep.egesound");
    REQUIRE(record != nullptr);
    CHECK(record->kind == ege::AssetKind::sound);
    // The sidecar was written, so the id survives the file being moved - the
    // same property every other asset has.
    CHECK(fs::exists(root / "sounds" / "beep.egesound.egameta"));

    // No device anywhere in this test, and it still loads: samples are
    // samples, and whether anything can play them is the audio engine's
    // business rather than the database's.
    const std::shared_ptr<SoundData> loaded = database.sound(record->id);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->duration() == doctest::Approx(0.15f));

    // And it is playable, which is the row's whole "done when".
    const ege::SoundId clip = audio->load(*loaded);
    const ege::VoiceId voice = audio->play(clip, VoiceSettings{});
    CHECK(voice != ege::invalidVoice);
    audio->update(0.2f);
    CHECK_FALSE(audio->playing(voice));

    database.clear();
    fs::remove_all(root, errorCode);
}
