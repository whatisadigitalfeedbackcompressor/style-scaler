#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "engine/AudioCaptureRingBuffer.h"
#include "engine/LockFreePatternSlot.h"
#include "engine/MidiSequencer.h"
#include "engine/ModelManager.h"
#include "engine/PresetCatalogSync.h"
#include "engine/PreviewSynth.h"
#include "engine/TranscriptionService.h"

#include <JuceHeader.h>
#include <cmath>
#include <filesystem>
#include <map>

#if STYLE_SCALER_HAS_MUSCRIPTOR
#include <muscriptor/muscriptor.hpp>
#endif

using namespace style_scaler;
using Catch::Approx;

namespace {

int countNoteOnMessages(const juce::MidiBuffer& buf, int channelFilter = 0) {
    int count = 0;
    for (const auto meta : buf) {
        const auto m = meta.getMessage();
        if (m.isNoteOn() && (channelFilter == 0 || m.getChannel() == channelFilter)) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("MidiSequencer sample accuracy, channel routing, rate scaling, and zero stuck notes", "[sequencer]") {
    LockFreePatternSlot slot;
    auto pat = std::make_unique<PatternBuffer>();
    pat->loop_beats = 4.0;
    pat->events = {
        {0.0, 1.0, 60, 100, 1},
        {0.0, 1.0, 36, 110, 3},
        {1.0, 0.5, 72, 95, 2},
        {3.5, 0.75, 67, 90, 1}
    };
    slot.publish(std::move(pat));

    MidiSequencer seq;
    seq.prepare(48000.0);
    seq.setDawSync(false);
    seq.setInternalBpm(120.0); // 2 beats per second -> 24000 samples per beat
    seq.setInternalPlaying(true);

    std::map<std::pair<int, int>, int> activeRefCount;
    auto trackMidi = [&](const juce::MidiBuffer& buf) {
        for (const auto meta : buf) {
            const auto msg = meta.getMessage();
            if (msg.isNoteOn()) {
                activeRefCount[{msg.getChannel(), msg.getNoteNumber()}] += 1;
            } else if (msg.isNoteOff()) {
                activeRefCount[{msg.getChannel(), msg.getNoteNumber()}] -= 1;
                CHECK(activeRefCount[{msg.getChannel(), msg.getNoteNumber()}] >= 0);
            }
        }
    };

    // Process 2 full loops (8 beats = 4 seconds = 192000 samples in 512-sample blocks)
    int ch1Ons = 0, ch2Ons = 0, ch3Ons = 0;
    for (int b = 0; b < 375; ++b) {
        juce::MidiBuffer midi;
        seq.processBlock(512, nullptr, slot, midi, true, true, true);
        ch1Ons += countNoteOnMessages(midi, 1);
        ch2Ons += countNoteOnMessages(midi, 2);
        ch3Ons += countNoteOnMessages(midi, 3);
        trackMidi(midi);
    }
    CHECK(ch1Ons >= 4);
    CHECK(ch2Ons >= 2);
    CHECK(ch3Ons >= 2);

    // Swap PatternBuffer mid-playback and stop transport -> all active notes must receive clean Note-Offs
    auto pat2 = std::make_unique<PatternBuffer>();
    pat2->loop_beats = 4.0;
    pat2->events = {{0.0, 1.0, 62, 100, 1}};
    slot.publish(std::move(pat2));

    {
        juce::MidiBuffer midi;
        seq.processBlock(512, nullptr, slot, midi, true, true, true);
        trackMidi(midi);
    }

    seq.setInternalPlaying(false);
    {
        juce::MidiBuffer midi;
        seq.processBlock(512, nullptr, slot, midi, true, true, true);
        trackMidi(midi);
    }
    for (const auto& [key, cnt] : activeRefCount) {
        INFO("Channel " << key.first << " Pitch " << key.second);
        CHECK(cnt == 0);
    }

    // Verify 2x vs 0.5x rate scaling
    auto runRateCount = [&](double scale) -> int {
        seq.reset();
        seq.setDawSync(false);
        seq.setInternalBpm(120.0);
        seq.setPlaybackRateScale(scale);
        seq.setInternalPlaying(true);
        int totalOns = 0;
        for (int b = 0; b < 100; ++b) {
            juce::MidiBuffer midi;
            seq.processBlock(512, nullptr, slot, midi, true, true, true);
            totalOns += countNoteOnMessages(midi);
        }
        return totalOns;
    };
    CHECK(runRateCount(2.0) > runRateCount(0.5));
}

TEST_CASE("PreviewSynth produces finite non-silent audio and decays cleanly", "[synth]") {
    PreviewSynth synth;
    synth.prepare(44100.0);

    juce::AudioBuffer<float> buf(2, 512);
    buf.clear();

    juce::MidiBuffer onMidi;
    onMidi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    onMidi.addEvent(juce::MidiMessage::noteOn(2, 72, static_cast<juce::uint8>(90)), 10);
    onMidi.addEvent(juce::MidiMessage::noteOn(3, 36, static_cast<juce::uint8>(110)), 20);

    synth.renderBlock(buf, onMidi, 0.8f, true, true, true);

    float peak = 0.0f;
    for (int ch = 0; ch < 2; ++ch) {
        const float* ptr = buf.getReadPointer(ch);
        for (int i = 0; i < buf.getNumSamples(); ++i) {
            REQUIRE(std::isfinite(ptr[i]));
            CHECK(std::abs(ptr[i]) <= 1.0f);
            peak = std::max(peak, std::abs(ptr[i]));
        }
    }
    CHECK(peak > 0.02f);

    // Send Note-Offs and advance ~2 seconds to verify decay back to near silence
    juce::MidiBuffer offMidi;
    offMidi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    offMidi.addEvent(juce::MidiMessage::noteOff(2, 72), 0);
    offMidi.addEvent(juce::MidiMessage::noteOff(3, 36), 0);
    buf.clear();
    synth.renderBlock(buf, offMidi, 0.8f, true, true, true);

    for (int b = 0; b < 220; ++b) {
        buf.clear();
        juce::MidiBuffer empty;
        synth.renderBlock(buf, empty, 0.8f, true, true, true);
    }
    float tailPeak = 0.0f;
    for (int i = 0; i < buf.getNumSamples(); ++i) {
        tailPeak = std::max(tailPeak, std::abs(buf.getSample(0, i)));
    }
    CHECK(tailPeak < 0.002f);
}

TEST_CASE("AudioCaptureRingBuffer + SpectralFallbackTranscriber + MidiStyleAnalyzer end-to-end", "[capture][transcription]") {
    constexpr double kSr = 44100.0;
    AudioCaptureRingBuffer ring(10.0);
    ring.prepare(kSr);
    ring.startCapture();
    REQUIRE(ring.isCapturing());

    // Synthesize 2 seconds of C minor triad (C4=60, Eb4=63, G4=67) + C2 bass (36)
    const int totalSamples = static_cast<int>(kSr * 2.0);
    std::vector<float> left(totalSamples, 0.0f);
    std::vector<float> right(totalSamples, 0.0f);
    const std::array<int, 4> pitches = {36, 60, 63, 67};
    constexpr double kTwoPi = 6.28318530717958647692;
    for (int i = 0; i < totalSamples; ++i) {
        const double t = static_cast<double>(i) / kSr;
        double sum = 0.0;
        for (int p : pitches) {
            const double hz = 440.0 * std::pow(2.0, (static_cast<double>(p) - 69.0) / 12.0);
            sum += 0.20 * std::sin(kTwoPi * hz * t);
        }
        left[i] = static_cast<float>(sum);
        right[i] = static_cast<float>(sum);
    }

    for (int pos = 0; pos < totalSamples; pos += 512) {
        const int n = std::min(512, totalSamples - pos);
        const float* ptrs[2] = {left.data() + pos, right.data() + pos};
        ring.pushBlock(ptrs, 2, n);
    }
    CHECK(ring.capturedSeconds() == Approx(2.0).margin(0.05));

    const auto captured = ring.extractSnapshot();
    CHECK_FALSE(ring.isCapturing());
    REQUIRE(captured.mono_samples.size() == static_cast<size_t>(totalSamples));

    const auto mono16k = TranscriptionService::resampleTo16kMono(captured.mono_samples, captured.sample_rate);
    REQUIRE(mono16k.size() > 16000);

    const auto notes = SpectralFallbackTranscriber::transcribe(mono16k);
    REQUIRE_FALSE(notes.empty());

    AnalysisOptions opts;
    opts.id = "synth_test";
    opts.name = "Synth Test";
    opts.bpm_hint = 120.0;
    const StyleProfile prof = MidiStyleAnalyzer::analyzeTimed(notes, opts);
    REQUIRE(validate(prof).valid);
    CHECK(prof.harmony.tonic_pc == 0); // C
}

#if STYLE_SCALER_HAS_MUSCRIPTOR
TEST_CASE("NeuralNote muscriptor_ggml linkage and graceful missing-file handling", "[muscriptor]") {
    const auto devs = msl::availableDevices();
    REQUIRE_FALSE(devs.empty());
    bool hasCpu = false;
    for (const auto& d : devs) {
        if (d.backend == "CPU") hasCpu = true;
    }
    CHECK(hasCpu);

    const auto res = msl::Transcriber::load(std::filesystem::path("C:/nonexistent_path/missing_model.gguf"));
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == msl::Error::FileNotFound);
}
#endif

TEST_CASE("StyleScalerAudioProcessor processBlock, state persistence, and MIDI export", "[processor]") {
    StyleScalerAudioProcessor proc;
    proc.prepareToPlay(44100.0, 512);
    proc.setDawSync(false);
    proc.setInternalPlaying(true);

    juce::AudioBuffer<float> audio(2, 512);
    int totalNotes = 0;
    for (int b = 0; b < 4; ++b) {
        juce::MidiBuffer midi;
        audio.clear();
        proc.processBlock(audio, midi);
        totalNotes += countNoteOnMessages(midi);
    }
    CHECK(totalNotes > 0);

    // State save & restore round-trip
    proc.apvts().getParameter(ParamIDs::rhythmBlend)->setValueNotifyingHost(0.85f);
    proc.setSlotLocked(0, true);
    juce::MemoryBlock stateBlob;
    proc.getStateInformation(stateBlob);
    REQUIRE(stateBlob.getSize() > 0);

    StyleScalerAudioProcessor proc2;
    proc2.setStateInformation(stateBlob.getData(), static_cast<int>(stateBlob.getSize()));
    CHECK(proc2.apvts().getRawParameterValue(ParamIDs::rhythmBlend)->load() == Approx(0.85f).margin(0.01f));
    const auto snap2 = proc2.getSnapshot();
    REQUIRE_FALSE(snap2.progression.empty());
    CHECK(snap2.progression.front().locked == true);

    // Export temp MIDI for drag-and-drop
    const juce::File tmpMid = proc.exportTempMidiForDrag(0);
    CHECK(tmpMid.existsAsFile());
    CHECK(tmpMid.getSize() > 32);
    tmpMid.deleteFile();
}

TEST_CASE("StyleScalerAudioProcessorEditor headless creation, half-size default, resize, and paint", "[editor]") {
    StyleScalerAudioProcessor proc;
    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    REQUIRE(editor != nullptr);
    CHECK(editor->isResizable());
    CHECK(editor->getWidth() == 580);
    CHECK(editor->getHeight() == 380);

    for (const auto& sz : std::array<std::pair<int, int>, 5>{{
             {460, 300}, {580, 380}, {900, 420}, {1160, 760}, {1440, 900}}}) {
        editor->setSize(sz.first, sz.second);
        juce::Image img(juce::Image::ARGB, sz.first, sz.second, true);
        juce::Graphics g(img);
        editor->paintEntireComponent(g, false);
        CHECK(editor->getWidth() == sz.first);
        CHECK(editor->getHeight() == sz.second);
    }

    // Verify resized editor dimensions persist in processor state
    editor->setSize(740, 500);
    juce::MemoryBlock stateBlob;
    proc.getStateInformation(stateBlob);
    StyleScalerAudioProcessor proc2;
    proc2.setStateInformation(stateBlob.getData(), static_cast<int>(stateBlob.getSize()));
    std::unique_ptr<juce::AudioProcessorEditor> editor2(proc2.createEditor());
    REQUIRE(editor2 != nullptr);
    CHECK(editor2->getWidth() == 740);
    CHECK(editor2->getHeight() == 500);
}

TEST_CASE("PresetCatalogSync SHA-256 verification, incremental cache, and hot-reload", "[catalog][sync]") {
    const juce::File tempRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getChildFile("StyleScalerCatalogSyncTest");
    tempRoot.deleteRecursively();
    const juce::File remoteDir = tempRoot.getChildFile("remote");
    const juce::File stylesDir = remoteDir.getChildFile("styles");
    const juce::File cacheDir = tempRoot.getChildFile("cache");
    stylesDir.createDirectory();

    StyleProfile ukg = makeDefaultStyleProfile();
    ukg.id = "uk_garage";
    ukg.name = "2-Step UK Garage";
    ukg.genre = "UK Garage";
    ukg.default_bpm = 132.0;

    const juce::File ukgFile = stylesDir.getChildFile("uk_garage.json");
    REQUIRE(saveToFile(ukg, ukgFile.getFullPathName().toStdString()).has_value());
    const juce::String ukgSha = juce::SHA256(ukgFile).toHexString().toLowerCase();

    StyleProfile bad = ukg;
    bad.id = "tampered_style";
    bad.name = "Tampered";
    const juce::File badFile = stylesDir.getChildFile("tampered_style.json");
    REQUIRE(saveToFile(bad, badFile.getFullPathName().toStdString()).has_value());

    const juce::String catalogJson =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"catalog_name\": \"Test Catalog\",\n"
        "  \"presets\": [\n"
        "    {\"id\": \"uk_garage\", \"file\": \"styles/uk_garage.json\", \"sha256\": \"" + ukgSha + "\"},\n"
        "    {\"id\": \"tampered_style\", \"file\": \"styles/tampered_style.json\", \"sha256\": \"0000000000000000000000000000000000000000000000000000000000000000\"}\n"
        "  ]\n"
        "}\n";
    const juce::File catFile = remoteDir.getChildFile("catalog.json");
    REQUIRE(catFile.replaceWithText(catalogJson));

    // First sync: downloads uk_garage, rejects tampered_style
    const auto res1 = PresetCatalogSync::syncBlocking(catFile.getFullPathName().toStdString(), cacheDir);
    CHECK(res1.success);
    CHECK(res1.downloaded == 1);
    CHECK(res1.failed == 1);
    CHECK(res1.skipped == 0);
    CHECK(cacheDir.getChildFile("uk_garage.json").existsAsFile());
    CHECK_FALSE(cacheDir.getChildFile("tampered_style.json").existsAsFile());

    // Second sync: skips already-cached matching SHA-256 preset
    const auto res2 = PresetCatalogSync::syncBlocking(catFile.getFullPathName().toStdString(), cacheDir);
    CHECK(res2.downloaded == 0);
    CHECK(res2.skipped == 1);
    CHECK(res2.failed == 1);

    tempRoot.deleteRecursively();
}

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    return Catch::Session().run(argc, argv);
}


TEST_CASE("TranscriptionService detects WAV RIFF LIST/INFO metadata genre and StyleScalerAudioProcessor indexes genres", "[metadata][plugin]") {
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile("style_scaler_plugin_meta_test");
    tmpDir.createDirectory();
    const juce::File wavFile = tmpDir.getChildFile("ukg_chord_test.wav");

    // Build 1.5s 16-bit mono 16kHz C-minor triad WAV + trailing RIFF LIST/INFO chunk (after 'data')
    constexpr int sampleRate = 16000;
    constexpr int numSamples = 24000;
    std::vector<uint8_t> pcmBytes(static_cast<size_t>(numSamples) * 2u, 0);
    for (int n = 0; n < numSamples; ++n) {
        const double t = static_cast<double>(n) / sampleRate;
        const double s = 0.25 * (std::sin(2.0 * 3.14159265 * 261.63 * t) +
                                 std::sin(2.0 * 3.14159265 * 311.13 * t) +
                                 std::sin(2.0 * 3.14159265 * 392.00 * t));
        const int16_t v = static_cast<int16_t>(std::clamp(s, -0.95, 0.95) * 28000.0);
        pcmBytes[static_cast<size_t>(n) * 2u] = static_cast<uint8_t>(v & 0xFF);
        pcmBytes[static_cast<size_t>(n) * 2u + 1u] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }

    auto appendU32 = [](std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(static_cast<uint8_t>(v & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto appendU16 = [](std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(static_cast<uint8_t>(v & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto appendTag = [](std::vector<uint8_t>& b, const char* s) {
        b.insert(b.end(), s, s + 4);
    };
    auto appendInfoItem = [&](std::vector<uint8_t>& dst, const char* id4, const std::string& val) {
        appendTag(dst, id4);
        const uint32_t sz = static_cast<uint32_t>(val.size() + 1u);
        appendU32(dst, sz);
        dst.insert(dst.end(), val.begin(), val.end());
        dst.push_back(0);
        if ((sz & 1u) != 0) dst.push_back(0);
    };

    std::vector<uint8_t> infoChunk;
    appendTag(infoChunk, "INFO");
    appendInfoItem(infoChunk, "IGNR", "UK Garage");
    appendInfoItem(infoChunk, "INAM", "2Step Test");
    appendInfoItem(infoChunk, "IART", "TestSynth");
    appendInfoItem(infoChunk, "IBPM", "132");

    std::vector<uint8_t> wav;
    appendTag(wav, "RIFF");
    appendU32(wav, 0); // patched below
    appendTag(wav, "WAVE");

    appendTag(wav, "fmt ");
    appendU32(wav, 16);
    appendU16(wav, 1);          // PCM
    appendU16(wav, 1);          // mono
    appendU32(wav, sampleRate);
    appendU32(wav, sampleRate * 2);
    appendU16(wav, 2);
    appendU16(wav, 16);

    appendTag(wav, "data");
    appendU32(wav, static_cast<uint32_t>(pcmBytes.size()));
    wav.insert(wav.end(), pcmBytes.begin(), pcmBytes.end());

    appendTag(wav, "LIST");
    appendU32(wav, static_cast<uint32_t>(infoChunk.size()));
    wav.insert(wav.end(), infoChunk.begin(), infoChunk.end());

    const uint32_t riffLen = static_cast<uint32_t>(wav.size() - 8u);
    wav[4] = static_cast<uint8_t>(riffLen & 0xFF);
    wav[5] = static_cast<uint8_t>((riffLen >> 8) & 0xFF);
    wav[6] = static_cast<uint8_t>((riffLen >> 16) & 0xFF);
    wav[7] = static_cast<uint8_t>((riffLen >> 24) & 0xFF);

    REQUIRE(wavFile.replaceWithData(wav.data(), wav.size()));

    ModelManager mm;
    TranscriptionService tx(mm);
    TranscriptionService::DeconstructOptions opts;
    opts.force_spectral = true;
    const auto ds = tx.deconstructFileBlocking(wavFile, opts);

    REQUIRE(ds.success);
    REQUIRE(ds.metadata.canonical_genre == "UK Garage");
    REQUIRE(ds.metadata.genre_source == "riff_info");
    REQUIRE(ds.metadata.artist == "TestSynth");
    REQUIRE(ds.metadata.bpm == Approx(132.0));
    REQUIRE(ds.song_input.bpm == Approx(132.0));

    StyleScalerAudioProcessor proc;
    const auto genres = proc.getAvailableGenres();
    REQUIRE(std::find(genres.begin(), genres.end(), "Drum & Bass") != genres.end());
    REQUIRE(std::find(genres.begin(), genres.end(), "Hip-Hop") != genres.end());
    REQUIRE(std::find(genres.begin(), genres.end(), "Trap") != genres.end());
    REQUIRE(proc.getPresetsForGenre("Drum & Bass").size() >= 2);

    tmpDir.deleteRecursively();
}
