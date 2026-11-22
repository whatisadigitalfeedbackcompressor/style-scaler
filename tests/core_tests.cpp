#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "style_scaler/GenerationEngine.h"
#include "style_scaler/MidiStyleAnalyzer.h"
#include "style_scaler/SmfIO.h"
#include "style_scaler/StyleBlender.h"
#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <cmath>
#include <filesystem>
#include <numeric>
#include <set>

namespace fs = std::filesystem;
using namespace style_scaler;
using Catch::Approx;

namespace {

fs::path presetsRoot() {
    return fs::path(STYLE_SCALER_SOURCE_DIR) / "presets" / "styles";
}

} // namespace

TEST_CASE("RomanChord formatting and parsing round-trip", "[types]") {
    const std::vector<std::string> symbols = {
        "i", "i7", "i9", "iv7", "iv11", "V7", "bII", "bIIImaj7", "bVImaj7", "bVImaj9", "bVIIadd9", "iidim7", "iim7b5"
    };
    for (const auto& s : symbols) {
        const auto parsed = parseRomanChord(s);
        REQUIRE(parsed.has_value());
        if (s != "iim7b5") {
            CHECK(formatRomanChord(*parsed) == s);
        }
    }
    const auto bvi = parseRomanChord("bVImaj7");
    REQUIRE(bvi.has_value());
    CHECK(romanChordRootPitchClass(*bvi, 0) == 8); // Ab in key of C
    CHECK(formatRomanAsConcreteChord(*bvi, 0, Mode::Minor) == "Abmaj7");
}

TEST_CASE("StyleProfile JSON round-trip and validation", "[schema]") {
    const StyleProfile def = makeDefaultStyleProfile();
    const auto vr = validate(def);
    REQUIRE(vr.valid);

    const std::string dump = toJson(def).dump(2);
    const auto reloaded = loadFromJsonString(dump);
    REQUIRE(reloaded.has_value());
    CHECK(*reloaded == def);

    // Invalid schema version
    auto badJson = toJson(def);
    badJson["schema_version"] = 99;
    CHECK_FALSE(fromJson(badJson).has_value());

    // Invalid Markov row sum
    auto badRow = toJson(def);
    badRow["harmony"]["transitions"]["i7"]["bVImaj7"] = 0.99;
    CHECK_FALSE(fromJson(badRow).has_value());

    // Invalid register inversion
    auto badReg = toJson(def);
    badReg["voicing"]["register_low"] = 90;
    badReg["voicing"]["register_high"] = 70;
    CHECK_FALSE(fromJson(badReg).has_value());
}

TEST_CASE("All 4 starter genre presets load and pass strict validation", "[presets]") {
    const std::vector<std::string> ids = {"90s_hiphop", "trap", "liquid_dnb", "neuro_dnb"};
    for (const auto& id : ids) {
        const fs::path p = presetsRoot() / (id + ".json");
        INFO("Checking preset: " << p.string());
        REQUIRE(fs::exists(p));
        const auto loaded = loadFromFile(p);
        REQUIRE(loaded.has_value());
        const auto vr = validate(*loaded);
        INFO((vr.errors.empty() ? "" : vr.errors.front()));
        REQUIRE(vr.valid);
        CHECK(loaded->id == id);
        CHECK_FALSE(loaded->harmony.vocabulary.empty());
    }

    const auto hiphop = loadFromFile(presetsRoot() / "90s_hiphop.json");
    const auto liquid = loadFromFile(presetsRoot() / "liquid_dnb.json");
    const auto neuro = loadFromFile(presetsRoot() / "neuro_dnb.json");
    REQUIRE(hiphop.has_value());
    REQUIRE(liquid.has_value());
    REQUIRE(neuro.has_value());
    CHECK(hiphop->rhythm.swing > liquid->rhythm.swing);
    CHECK(liquid->voicing.sub_bass_clearance_midi >= 48);
    CHECK(neuro->voicing.sub_bass_clearance_midi >= 52);
}

TEST_CASE("StyleBlender maintains strict per-axis independence", "[blender]") {
    const auto slotA = loadFromFile(presetsRoot() / "90s_hiphop.json");
    const auto slotB = loadFromFile(presetsRoot() / "liquid_dnb.json");
    REQUIRE(slotA.has_value());
    REQUIRE(slotB.has_value());

    BlendParams p;
    p.harmony = 0.0;
    p.rhythm = 1.0;
    p.voicing = 0.0;
    p.melody = 0.0;

    const StyleProfile blended = StyleBlender::blend(*slotA, *slotB, p);
    REQUIRE(validate(blended).valid);
    CHECK(blended.harmony == slotA->harmony);
    CHECK(blended.rhythm == slotB->rhythm);
    CHECK(blended.voicing == slotA->voicing);
    CHECK(blended.melody == slotA->melody);

    // Midpoint harmony blend preserves normalized transition rows
    BlendParams mid;
    mid.harmony = 0.5;
    mid.rhythm = 0.5;
    mid.voicing = 0.5;
    mid.melody = 0.5;
    const StyleProfile midBlend = StyleBlender::blend(*slotA, *slotB, mid);
    REQUIRE(validate(midBlend).valid);
    CHECK(midBlend.harmony.vocabulary.size() >= slotA->harmony.vocabulary.size());
    for (const auto& [fromSym, row] : midBlend.harmony.transitions) {
        double sum = 0.0;
        for (const auto& [toSym, prob] : row) sum += prob;
        CHECK(sum == Approx(1.0).margin(1e-4));
    }
}

TEST_CASE("VoicingEngine strictly enforces sub-bass clearance and smooth voice leading", "[voicing]") {
    const auto neuro = loadFromFile(presetsRoot() / "neuro_dnb.json");
    REQUIRE(neuro.has_value());

    std::vector<int> prev;
    for (const auto& entry : neuro->harmony.vocabulary) {
        // Even with aggressive negative octave shift, chord notes must respect sub_bass_clearance_midi
        const auto notes = VoicingEngine::voiceChord(
            entry.chord, neuro->harmony.tonic_pc, neuro->harmony.mode,
            neuro->voicing, prev, 0.7, -2, -1, -1.0);
        REQUIRE_FALSE(notes.empty());
        for (int midi : notes) {
            CHECK(midi >= neuro->voicing.sub_bass_clearance_midi);
        }
        prev = notes;
    }
}

TEST_CASE("ProgressionGenerator and PatternGenerator determinism, locks, scaling, and channels", "[generation]") {
    const auto hiphop = loadFromFile(presetsRoot() / "90s_hiphop.json");
    const auto liquid = loadFromFile(presetsRoot() / "liquid_dnb.json");
    REQUIRE(hiphop.has_value());
    REQUIRE(liquid.has_value());

    BlendParams bp;
    bp.harmony = 0.15;
    bp.rhythm = 0.85;
    bp.voicing = 0.65;
    bp.melody = 0.70;
    const StyleProfile hybrid = StyleBlender::blend(*hiphop, *liquid, bp);

    // Next chord probabilities sum to 1.0
    const auto nextProbs = ProgressionGenerator::nextChordProbabilities(
        hybrid, hybrid.harmony.vocabulary.front().symbol);
    REQUIRE_FALSE(nextProbs.empty());
    double probSum = 0.0;
    for (const auto& [sym, pr] : nextProbs) probSum += pr;
    CHECK(probSum == Approx(1.0).margin(1e-4));

    // Determinism & lock preservation
    auto prog1 = ProgressionGenerator::generateProgression(hybrid, 8, 12345ULL);
    auto prog2 = ProgressionGenerator::generateProgression(hybrid, 8, 12345ULL);
    REQUIRE(prog1 == prog2);
    REQUIRE(prog1.size() >= 4);

    // Lock slot 1 with a specific substitute chord
    const auto subs = ProgressionGenerator::suggestSubstitutes(hybrid, prog1[1]);
    REQUIRE_FALSE(subs.empty());
    prog1[1].locked = true;
    prog1[1].roman_symbol = subs.front().symbol;
    prog1[1].chord = subs.front().chord;
    prog1[1].octave_shift = 1;

    const auto prog3 = ProgressionGenerator::generateProgression(hybrid, 8, 99999ULL, prog1);
    REQUIRE(prog3.size() == prog1.size());
    CHECK(prog3[1].locked == true);
    CHECK(prog3[1].roman_symbol == subs.front().symbol);
    CHECK(prog3[1].octave_shift == 1);

    // Pattern determinism + channel separation + sub-bass clearance
    PerformParams perf;
    perf.tempo_scale = TempoScale::Normal;
    const auto patA = PatternGenerator::generatePattern(hybrid, prog3, perf, 777ULL);
    const auto patB = PatternGenerator::generatePattern(hybrid, prog3, perf, 777ULL);
    REQUIRE(patA.all_events.size() == patB.all_events.size());
    CHECK_FALSE(patA.chords.empty());
    CHECK_FALSE(patA.melody.empty());
    CHECK_FALSE(patA.bass.empty());

    for (const auto& ev : patA.chords) {
        CHECK(ev.channel == 1);
        CHECK(ev.pitch >= hybrid.voicing.sub_bass_clearance_midi);
    }
    for (const auto& ev : patA.melody) {
        CHECK(ev.channel == 2);
    }
    for (const auto& ev : patA.bass) {
        CHECK(ev.channel == 3);
        CHECK(ev.pitch < hybrid.voicing.sub_bass_clearance_midi);
    }

    // 2x vs 0.5x tempo scaling alters rhythmic density noticeably
    PerformParams perfHalf = perf;
    perfHalf.tempo_scale = TempoScale::Half;
    PerformParams perfDouble = perf;
    perfDouble.tempo_scale = TempoScale::Double;
    const auto patHalf = PatternGenerator::generatePattern(hybrid, prog3, perfHalf, 777ULL);
    const auto patDouble = PatternGenerator::generatePattern(hybrid, prog3, perfDouble, 777ULL);
    CHECK(patDouble.all_events.size() > patHalf.all_events.size());
}

TEST_CASE("SmfWriter/SmfReader round-trip and MidiStyleAnalyzer recovery", "[smf][analyzer]") {
    const auto hiphop = loadFromFile(presetsRoot() / "90s_hiphop.json");
    REQUIRE(hiphop.has_value());

    const auto prog = ProgressionGenerator::generateProgression(*hiphop, 8, 42ULL);
    PerformParams perf;
    perf.humanize = 0.0;
    const auto pat = PatternGenerator::generatePattern(*hiphop, prog, perf, 42ULL);

    const fs::path tmpMid = fs::temp_directory_path() / "style_scaler_roundtrip_test.mid";
    REQUIRE(SmfWriter::writeType1(tmpMid, hiphop->default_bpm, 4, 4, pat.all_events).has_value());

    const auto readBack = SmfReader::readFile(tmpMid);
    REQUIRE(readBack.has_value());
    CHECK(readBack->bpm == Approx(hiphop->default_bpm).margin(0.5));
    CHECK(readBack->notes.size() == pat.all_events.size());

    AnalysisOptions opts;
    opts.id = "recovered_hiphop";
    opts.name = "Recovered Hip-Hop";
    opts.genre = "Hip-Hop";
    const StyleProfile analyzed = MidiStyleAnalyzer::analyze(readBack->notes, readBack->bpm, opts);
    REQUIRE(validate(analyzed).valid);
    CHECK(analyzed.harmony.tonic_pc == hiphop->harmony.tonic_pc);
    CHECK(analyzed.voicing.sub_bass_clearance_midi >= 44);

    std::error_code ec;
    fs::remove(tmpMid, ec);
}

TEST_CASE("MidiStyleAnalyzer::analyzeCorpus pools multi-key songs into one valid StyleProfile", "[analyzer][corpus]") {
    const auto hiphop = loadFromFile(presetsRoot() / "90s_hiphop.json");
    const auto liquid = loadFromFile(presetsRoot() / "liquid_dnb.json");
    REQUIRE(hiphop.has_value());
    REQUIRE(liquid.has_value());

    // Song 1 in C minor, Song 2 transposed +5 semitones to F minor
    const auto prog1 = ProgressionGenerator::generateProgression(*hiphop, 4, 101ULL);
    const auto pat1 = PatternGenerator::generatePattern(*hiphop, prog1, PerformParams{}, 101ULL);

    StyleProfile fMinorLiquid = *liquid;
    fMinorLiquid.harmony.tonic_pc = 5;
    const auto prog2 = ProgressionGenerator::generateProgression(fMinorLiquid, 4, 202ULL);
    const auto pat2 = PatternGenerator::generatePattern(fMinorLiquid, prog2, PerformParams{}, 202ULL);

    SongInput s1{pat1.all_events, 92.0, 4, 4};
    SongInput s2{pat2.all_events, 174.0, 4, 4};

    AnalysisOptions opts;
    opts.id = "pooled_hybrid";
    opts.name = "Pooled Hybrid";
    opts.genre = "Hybrid";
    const StyleProfile pooled = MidiStyleAnalyzer::analyzeCorpus({s1, s2}, opts);
    REQUIRE(validate(pooled).valid);
    CHECK(pooled.id == "pooled_hybrid");
    CHECK(pooled.bpm_min <= 92.0);
    CHECK(pooled.bpm_max >= 174.0);
    CHECK_FALSE(pooled.harmony.vocabulary.empty());
}
