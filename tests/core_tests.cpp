#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "style_scaler/AudioMetadata.h"
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

TEST_CASE("All 6 starter genre presets load, pass validation, and have non-degenerate Markov entropy", "[presets]") {
    const std::vector<std::string> ids = {
        "90s_hiphop", "trap", "liquid_dnb", "neuro_dnb", "neo_soul", "melodic_house"
    };
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

        for (const auto& [fromSym, row] : loaded->harmony.transitions) {
            if (row.size() >= 2) {
                double maxP = 0.0;
                for (const auto& [toSym, pr] : row) maxP = std::max(maxP, pr);
                INFO("Preset " << id << " row " << fromSym << " maxP=" << maxP);
                CHECK(maxP < 0.70);
            }
        }
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


namespace {

void pushU32BE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void pushU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void pushSynchsafe(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 21) & 0x7F));
    buf.push_back(static_cast<uint8_t>((v >> 14) & 0x7F));
    buf.push_back(static_cast<uint8_t>((v >> 7) & 0x7F));
    buf.push_back(static_cast<uint8_t>(v & 0x7F));
}

void pushStr(std::vector<uint8_t>& buf, std::string_view s) {
    buf.insert(buf.end(), s.begin(), s.end());
}

std::vector<uint8_t> buildId3v23Tag(const std::vector<std::pair<std::string, std::string>>& textFrames) {
    std::vector<uint8_t> body;
    for (const auto& [fid, val] : textFrames) {
        pushStr(body, fid);
        pushU32BE(body, static_cast<uint32_t>(1 + val.size()));
        body.push_back(0);
        body.push_back(0);
        body.push_back(3); // UTF-8
        pushStr(body, val);
    }
    std::vector<uint8_t> out = {'I', 'D', '3', 3, 0, 0};
    pushSynchsafe(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<uint8_t> buildRiffChunk(std::string_view fourcc, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    pushStr(out, fourcc);
    pushU32LE(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    if ((payload.size() & 1u) != 0) out.push_back(0);
    return out;
}

} // namespace

TEST_CASE("AudioMetadataReader parses ID3v2.3, ID3v1, FLAC, WAV, MIDI and key/genre helpers", "[metadata]") {
    SECTION("ID3v2.3 MP3 header tags") {
        auto bytes = buildId3v23Tag({
            {"TCON", "(127)Liquid Drum & Bass"},
            {"TIT2", "Midnight"},
            {"TPE1", "Calibre"},
            {"TBPM", "174"},
            {"TKEY", "8A"},
        });
        bytes.resize(bytes.size() + 32, 0xFF); // fake MP3 frames after header

        const auto meta = AudioMetadataReader::readFromMemory(bytes.data(), bytes.size(), ".mp3");
        REQUIRE(meta.canonical_genre == "Liquid DnB");
        REQUIRE(meta.genre_source == "id3v2");
        REQUIRE(meta.title == "Midnight");
        REQUIRE(meta.artist == "Calibre");
        REQUIRE(meta.bpm == Approx(174.0));
        REQUIRE(meta.key_pc.has_value());
        REQUIRE(*meta.key_pc == 9);
        REQUIRE(meta.mode == Mode::Minor);
    }

    SECTION("ID3v1 footer") {
        std::vector<uint8_t> mp3(256, 0);
        const size_t off = mp3.size() - 128;
        mp3[off] = 'T';
        mp3[off + 1] = 'A';
        mp3[off + 2] = 'G';
        std::string title = "Classic";
        std::copy(title.begin(), title.end(), mp3.begin() + static_cast<ptrdiff_t>(off + 3));
        mp3[off + 127] = 127; // Drum & Bass in Winamp extended table

        const auto meta = AudioMetadataReader::readFromMemory(mp3.data(), mp3.size(), ".mp3");
        REQUIRE(meta.canonical_genre == "Drum & Bass");
        REQUIRE(meta.genre_source == "id3v1");
        REQUIRE(meta.title == "Classic");
    }

    SECTION("FLAC Vorbis Comment") {
        std::vector<uint8_t> vc;
        pushU32LE(vc, 0); // empty vendor
        pushU32LE(vc, 2); // 2 comments
        const std::string c1 = "GENRE=Boom Bap";
        const std::string c2 = "ARTIST=J Dilla";
        pushU32LE(vc, static_cast<uint32_t>(c1.size()));
        pushStr(vc, c1);
        pushU32LE(vc, static_cast<uint32_t>(c2.size()));
        pushStr(vc, c2);

        std::vector<uint8_t> flac = {'f', 'L', 'a', 'C', 0x84}; // isLast | type 4 (VORBIS_COMMENT)
        const uint32_t sz = static_cast<uint32_t>(vc.size());
        flac.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
        flac.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
        flac.push_back(static_cast<uint8_t>(sz & 0xFF));
        flac.insert(flac.end(), vc.begin(), vc.end());

        const auto meta = AudioMetadataReader::readFromMemory(flac.data(), flac.size(), ".flac");
        REQUIRE(meta.canonical_genre == "90s Hip-Hop");
        REQUIRE(meta.genre_source == "vorbis");
        REQUIRE(meta.artist == "J Dilla");
    }

    SECTION("WAV LIST/INFO and embedded id3 chunk") {
        std::vector<uint8_t> infoBody;
        pushStr(infoBody, "INFO");
        auto ignr = buildRiffChunk("IGNR", {'U', 'K', ' ', 'G', 'a', 'r', 'a', 'g', 'e', 0});
        auto iart = buildRiffChunk("IART", {'B', 'u', 'r', 'i', 'a', 'l', 0});
        infoBody.insert(infoBody.end(), ignr.begin(), ignr.end());
        infoBody.insert(infoBody.end(), iart.begin(), iart.end());
        auto listChunk = buildRiffChunk("LIST", infoBody);

        std::vector<uint8_t> wav;
        pushStr(wav, "RIFF");
        pushU32LE(wav, static_cast<uint32_t>(4 + listChunk.size()));
        pushStr(wav, "WAVE");
        wav.insert(wav.end(), listChunk.begin(), listChunk.end());

        const auto meta1 = AudioMetadataReader::readFromMemory(wav.data(), wav.size(), ".wav");
        REQUIRE(meta1.canonical_genre == "UK Garage");
        REQUIRE(meta1.genre_source == "riff_info");
        REQUIRE(meta1.artist == "Burial");

        auto id3Body = buildId3v23Tag({{"TCON", "Neurofunk"}});
        auto id3Chunk = buildRiffChunk("id3 ", id3Body);
        std::vector<uint8_t> wav2;
        pushStr(wav2, "RIFF");
        pushU32LE(wav2, static_cast<uint32_t>(4 + id3Chunk.size()));
        pushStr(wav2, "WAVE");
        wav2.insert(wav2.end(), id3Chunk.begin(), id3Chunk.end());

        const auto meta2 = AudioMetadataReader::readFromMemory(wav2.data(), wav2.size(), ".wav");
        REQUIRE(meta2.canonical_genre == "Neuro DnB");
        REQUIRE(meta2.genre_source == "id3v2");
    }

    SECTION("MIDI meta text and Key Signature table") {
        std::vector<uint8_t> trk;
        const std::string gText = "Genre: Melodic Techno";
        trk.push_back(0x00);
        trk.push_back(0xFF);
        trk.push_back(0x01);
        trk.push_back(static_cast<uint8_t>(gText.size()));
        pushStr(trk, gText);
        // Key Sig: sf = -3 (0xFD, 3 flats = Eb Maj / C min), mi = 1 (minor) -> C minor (pc=0, Minor)
        trk.insert(trk.end(), {0x00, 0xFF, 0x59, 0x02, 0xFD, 0x01});
        // End of track
        trk.insert(trk.end(), {0x00, 0xFF, 0x2F, 0x00});

        std::vector<uint8_t> mid;
        pushStr(mid, "MThd");
        pushU32BE(mid, 6);
        mid.insert(mid.end(), {0, 0, 0, 1, 1, 0xE0});
        pushStr(mid, "MTrk");
        pushU32BE(mid, static_cast<uint32_t>(trk.size()));
        mid.insert(mid.end(), trk.begin(), trk.end());

        const auto meta = AudioMetadataReader::readFromMemory(mid.data(), mid.size(), ".mid");
        REQUIRE(meta.canonical_genre == "Melodic Techno");
        REQUIRE(meta.genre_source == "midi_meta");
        REQUIRE(meta.key_pc.has_value());
        REQUIRE(*meta.key_pc == 0);
        REQUIRE(meta.mode == Mode::Minor);
    }

    SECTION("Path fallback, heuristic fallback, and corrupt buffer safety") {
        REQUIRE(AudioMetadataReader::detectGenreFromPath("C:/Samples/Dark_Trap_Pack/beat01.wav") == "Trap");

        StyleProfile dnbProf = makeDefaultStyleProfile();
        dnbProf.default_bpm = 174.0;
        dnbProf.harmony.chromaticism = 0.12;
        dnbProf.rhythm.syncopation = 0.25;
        REQUIRE(AudioMetadataReader::inferGenreFromProfileHeuristic(dnbProf) == "Liquid DnB");

        StyleProfile hhProf = makeDefaultStyleProfile();
        hhProf.default_bpm = 92.0;
        hhProf.rhythm.swing = 0.18;
        hhProf.voicing.extension_richness = 0.30;
        REQUIRE(AudioMetadataReader::inferGenreFromProfileHeuristic(hhProf) == "90s Hip-Hop");

        const std::vector<uint8_t> garbage = {'I', 'D', '3', 3, 0, 0, 0x7F, 0x7F, 0x7F, 0x7F, 'T', 'C'};
        const auto safe = AudioMetadataReader::readFromMemory(garbage.data(), garbage.size(), ".mp3");
        REQUIRE(!safe.hasGenre());
    }

    SECTION("StyleProfile metadata JSON round-trip preserves schema_version 1") {
        auto prof = loadFromFile(presetsRoot() / "90s_hiphop.json");
        REQUIRE(prof.has_value());
        REQUIRE(validate(*prof).valid);

        prof->tags = {"hip-hop", "boom bap"};
        prof->genre_source = "id3v2";
        prof->artist = "Sample Artist";
        prof->album = "Sample Album";
        const auto rt = fromJson(toJson(*prof));
        REQUIRE(rt.has_value());
        REQUIRE(rt->tags == prof->tags);
        REQUIRE(rt->genre_source == "id3v2");
        REQUIRE(rt->artist == "Sample Artist");
        REQUIRE(rt->album == "Sample Album");
    }
}

TEST_CASE("Transcribed audio TimedNoteSec yields non-empty melody, phi0 alignment, and Intro/Chorus/Outro sections", "[analyzer][sections]") {
    // 16-bar song at 120 BPM (0.5 s/beat) shifted by +0.31s silence pre-roll,
    // using only piano (prog=0) + bass (prog=33) so ch2 is initially absent.
    constexpr double preRoll = 0.31;
    std::vector<TimedNoteSec> notes;
    for (int bar = 0; bar < 16; ++bar) {
        const double bSec = preRoll + static_cast<double>(bar) * 2.0;
        // Intro (bars 0..3): sparse pad; Chorus (4..11): dense + melody; Outro (12..15): sustained
        notes.push_back({bSec, bSec + 1.4, 36, 105, 33, false});
        notes.push_back({bSec, bSec + 1.5, 48, 88, 0, false});
        notes.push_back({bSec, bSec + 1.5, 55, 88, 0, false});
        notes.push_back({bSec, bSec + 1.5, 63, 88, 0, false});
        if (bar >= 4 && bar < 12) {
            notes.push_back({bSec + 0.5, bSec + 0.9, 72, 96, 0, false});
            notes.push_back({bSec + 1.0, bSec + 1.4, 75, 96, 0, false});
            notes.push_back({bSec + 1.5, bSec + 1.9, 79, 96, 0, false});
        } else if (bar < 4) {
            notes.push_back({bSec + 1.0, bSec + 1.8, 72, 76, 0, false});
        }
    }

    AnalysisOptions opts;
    opts.id = "timed_sections_test";
    opts.name = "Timed Sections";
    opts.bpm_hint = 120.0;
    const StyleProfile prof = MidiStyleAnalyzer::analyzeTimed(notes, opts);
    REQUIRE(validate(prof).valid);

    // Beat-phase recovery aligns downbeat back near step 0
    CHECK(prof.rhythm.chord_onset_grid[0] > 0.5);

    // Non-empty melody generation
    const auto prog = ProgressionGenerator::generateProgression(prof, 4, 42ULL);
    const auto pat = PatternGenerator::generatePattern(prof, prog, PerformParams{}, 42ULL);
    CHECK_FALSE(pat.melody.empty());

    // Intro / Chorus / Outro sections populated & distinct
    REQUIRE(prof.sections.count("intro") == 1);
    REQUIRE(prof.sections.count("chorus") == 1);
    REQUIRE(prof.sections.count("outro") == 1);
    CHECK(prof.sections.at("intro").populated);
    CHECK(prof.sections.at("chorus").populated);
    CHECK(prof.sections.at("outro").populated);
    CHECK(prof.sections.at("chorus").melody.motif_density >= prof.sections.at("intro").melody.motif_density);

    // StyleBlender resolves requested SongSection cleanly
    BlendParams bpIntro;
    bpIntro.section = SongSection::Intro;
    const auto blendedIntro = StyleBlender::blend(prof, prof, bpIntro);
    CHECK(validate(blendedIntro).valid);
}

TEST_CASE("MidiStyleAnalyzer::mergeProfiles weights transitions, increments track_count, and deduplicates tracks", "[analyzer][merge]") {
    auto a = makeDefaultStyleProfile();
    a.id = "merged_genre";
    a.track_count = 2;
    a.processed_tracks = {"hash_a", "hash_b"};

    auto b = makeDefaultStyleProfile();
    b.id = "merged_genre";
    b.track_count = 1;
    b.default_bpm = 135.0;
    b.processed_tracks = {"hash_b", "hash_c"};

    const StyleProfile merged = MidiStyleAnalyzer::mergeProfiles(a, b);
    REQUIRE(validate(merged).valid);
    CHECK(merged.track_count == 3);
    CHECK(merged.processed_tracks == std::vector<std::string>{"hash_a", "hash_b", "hash_c"});
    CHECK(merged.default_bpm == Approx((2.0 * 120.0 + 135.0) / 3.0).margin(0.1));
}

TEST_CASE("Bijective voice leading avoids low-interval mud and 4-bar melody avoids boundary pinning", "[voicing][melody]") {
    const auto liquid = loadFromFile(presetsRoot() / "liquid_dnb.json");
    REQUIRE(liquid.has_value());

    std::vector<int> prev;
    for (const auto& entry : liquid->harmony.vocabulary) {
        const auto notes = VoicingEngine::voiceChord(
            entry.chord, liquid->harmony.tonic_pc, liquid->harmony.mode, liquid->voicing, prev);
        for (size_t k = 0; k + 1 < notes.size(); ++k) {
            if (notes[k + 1] < 60) {
                CHECK((notes[k + 1] - notes[k]) >= 3);
            }
        }
        prev = notes;
    }

    const auto prog = ProgressionGenerator::generateProgression(*liquid, 16, 777ULL);
    const auto pat = PatternGenerator::generatePattern(*liquid, prog, PerformParams{}, 777ULL);
    REQUIRE_FALSE(pat.melody.empty());
    const int melLo = std::max(liquid->melody.register_low, liquid->voicing.sub_bass_clearance_midi + 7);
    const int melHi = std::max(melLo + 12, liquid->melody.register_high);
    int pinned = 0;
    for (const auto& ev : pat.melody) {
        if (ev.pitch == melLo || ev.pitch == melHi) ++pinned;
    }
    CHECK(pinned < static_cast<int>(pat.melody.size()) / 2);
}

TEST_CASE("Program 52 (MuScriptor Voice) routes to melody, extracts song idioms, and avoids min11 hallucination", "[analyzer][voice][idioms]") {
    std::vector<TimedNoteSec> notes;
    // 8 bars at 120 BPM (0.5 s/beat, 2.0 s/bar): clean C major triads + C/F/G/Am bass + pr=52 vocal melody
    const std::array<std::tuple<int, std::array<int, 3>>, 4> chords = {{
        {48, {60, 64, 67}}, // I (C)
        {41, {53, 57, 60}}, // IV (F)
        {43, {55, 59, 62}}, // V (G)
        {45, {57, 60, 64}}  // vi (Am)
    }};
    const std::array<int, 4> melSeq = {72, 74, 76, 79};
    for (int bar = 0; bar < 8; ++bar) {
        const double bSec = static_cast<double>(bar) * 2.0;
        const auto& [bassP, triad] = chords[static_cast<size_t>(bar % 4)];
        notes.push_back({bSec, bSec + 1.8, bassP, 100, 33, false});
        for (int cp : triad) {
            notes.push_back({bSec, bSec + 1.6, cp, 88, 0, false});
        }
        // Vocal melody on GM program 52
        for (int m = 0; m < 4; ++m) {
            const double mOn = bSec + static_cast<double>(m) * 0.5;
            const int pitch = melSeq[static_cast<size_t>((bar + m) % 4)];
            notes.push_back({mOn, mOn + 0.42, pitch, 96, 52, false});
        }
    }

    AnalysisOptions opts;
    opts.id = "voice_idiom_test";
    opts.name = "Voice Idiom Test";
    opts.bpm_hint = 120.0;
    const StyleProfile prof = MidiStyleAnalyzer::analyzeTimed(notes, opts);
    REQUIRE(validate(prof).valid);

    // Chords should be clean triads, not bloated min11
    bool hasMin11 = false;
    for (const auto& v : prof.harmony.vocabulary) {
        if (v.chord.quality == ChordQuality::Min11) hasMin11 = true;
    }
    CHECK_FALSE(hasMin11);
    CHECK_FALSE(prof.harmony.reference_progression.empty());
    CHECK_FALSE(prof.melody.reference_motif.empty());
    CHECK(prof.rhythm.chord_pattern[0] > 0.5);
    CHECK(prof.rhythm.sustain_ratio >= 0.45);

    double cwSum = 0.0;
    for (const auto& v : prof.harmony.vocabulary) cwSum += v.cadence_weight;
    CHECK(cwSum <= 1.001);
}

TEST_CASE("Repeated mergeProfiles keeps cadence_weight normalized and transitions non-uniform", "[analyzer][merge][cadence]") {
    StyleProfile base = makeDefaultStyleProfile();
    base.harmony.transitions["i7"] = {{"bVImaj7", 0.70}, {"iv7", 0.15}, {"bVII", 0.10}, {"v7", 0.05}, {"i7", 0.0}};
    normalizeProfileDistributions(base);

    StyleProfile acc = base;
    for (int k = 0; k < 10; ++k) {
        StyleProfile next = base;
        next.processed_tracks = {"track_" + std::to_string(k)};
        acc = MidiStyleAnalyzer::mergeProfiles(acc, next);
    }
    REQUIRE(validate(acc).valid);

    double cwSum = 0.0;
    for (const auto& v : acc.harmony.vocabulary) {
        CHECK(v.cadence_weight <= 1.0 + 1e-6);
        cwSum += v.cadence_weight;
    }
    CHECK(cwSum == Approx(1.0).margin(1e-3));

    const auto& row = acc.harmony.transitions.at("i7");
    CHECK(row.at("bVImaj7") > 0.45);
    CHECK(row.at("bVImaj7") > row.at("v7") * 3.0);
}

TEST_CASE("Song Original (seed 0) replays reference_progression and B->A transfers groove cleanly", "[generation][transfer]") {
    StyleProfile songA = makeDefaultStyleProfile();
    songA.id = "song_a";
    songA.name = "Song A";
    songA.artist = "Artist A";
    songA.harmony.tonic_pc = 0;
    songA.harmony.mode = Mode::Major;
    songA.harmony.vocabulary.clear();
    for (const auto& sym : std::vector<std::string>{"I", "V", "vi", "IV"}) {
        auto rc = parseRomanChord(sym);
        REQUIRE(rc.has_value());
        songA.harmony.vocabulary.push_back(ChordVocabEntry{sym, *rc, 0.25, sym == "IV" ? 0.5 : 0.1});
    }
    songA.harmony.transitions = {
        {"I",  {{"V", 0.8}, {"vi", 0.1}, {"IV", 0.1}, {"I", 0.0}}},
        {"V",  {{"vi", 0.8}, {"IV", 0.1}, {"I", 0.1}, {"V", 0.0}}},
        {"vi", {{"IV", 0.8}, {"I", 0.1}, {"V", 0.1}, {"vi", 0.0}}},
        {"IV", {{"I", 0.8}, {"V", 0.1}, {"vi", 0.1}, {"IV", 0.0}}}
    };
    songA.harmony.reference_progression = {"I", "V", "vi", "IV"};
    songA.rhythm.chord_pattern = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    songA.melody.reference_motif = {
        {0, 0, 4.0, true}, {4, 1, 4.0, false}, {8, 2, 4.0, true}, {16, 4, 4.0, true}
    };
    normalizeProfileDistributions(songA);

    StyleProfile songB = songA;
    songB.id = "song_b";
    songB.rhythm.chord_pattern = {1.0, 0.0, 0.0, 0.9, 0.0, 0.0, 0.9, 0.0, 0.0, 0.0, 0.9, 0.0, 0.0, 0.0, 0.0, 0.0};

    // seed == 0 reproduces reference_progression verbatim
    const auto origProg = ProgressionGenerator::generateProgression(songA, 4, 0ULL);
    REQUIRE(origProg.size() == 4);
    CHECK(origProg[0].roman_symbol == "I");
    CHECK(origProg[1].roman_symbol == "V");
    CHECK(origProg[2].roman_symbol == "vi");
    CHECK(origProg[3].roman_symbol == "IV");

    // B Groove -> A keeps A's harmony and applies B's crisp chord pattern
    BlendParams bp;
    bp.harmony = 0.0;
    bp.rhythm = 1.0;
    bp.voicing = 0.0;
    bp.melody = 0.0;
    const StyleProfile grooveTransferred = StyleBlender::blend(songA, songB, bp);
    CHECK(grooveTransferred.harmony.reference_progression == songA.harmony.reference_progression);
    CHECK(grooveTransferred.rhythm.chord_pattern == songB.rhythm.chord_pattern);

    const auto patOrig = PatternGenerator::generatePattern(songA, origProg, PerformParams{}, 0ULL);
    const auto patGroove = PatternGenerator::generatePattern(grooveTransferred, origProg, PerformParams{}, 0ULL);
    CHECK(patGroove.chords.size() > patOrig.chords.size());
    CHECK_FALSE(patOrig.melody.empty());
}

TEST_CASE("Intro and Outro preserve harmony/registers and thin arrangement in first/last bars", "[sections][arrangement]") {
    std::vector<TimedNoteSec> notes;
    const std::array<std::tuple<int, std::array<int, 3>>, 4> chords = {{
        {36, {48, 51, 55}}, // i (Cm)
        {44, {56, 60, 63}}, // bVI (Ab)
        {41, {53, 56, 60}}, // iv (Fm)
        {43, {55, 59, 62}}  // V (G)
    }};
    for (int bar = 0; bar < 16; ++bar) {
        const double bSec = static_cast<double>(bar) * 2.0;
        if (bar < 4) {
            // Sparse weird intro slice with high percussive chime & single pedal note
            notes.push_back({bSec, bSec + 1.8, 36, 72, 33, false});
            notes.push_back({bSec + 0.5, bSec + 0.7, 84, 68, 0, false});
            continue;
        }
        const auto& [bassP, triad] = chords[static_cast<size_t>((bar - 4) % 4)];
        for (int beat = 0; beat < 4; ++beat) {
            const double t = bSec + static_cast<double>(beat) * 0.5;
            notes.push_back({t, t + 0.35, bassP, 102, 33, false});
            for (int cp : triad) {
                notes.push_back({t, t + 0.32, cp, 88, 0, false});
            }
        }
        notes.push_back({bSec + 0.25, bSec + 0.65, 72 + ((bar % 3) * 2), 96, 52, false});
        notes.push_back({bSec + 1.00, bSec + 1.45, 75, 94, 52, false});
    }

    AnalysisOptions opts;
    opts.id = "section_conductor_test";
    opts.name = "Section Conductor";
    opts.bpm_hint = 120.0;
    const StyleProfile prof = MidiStyleAnalyzer::analyzeTimed(notes, opts);
    REQUIRE(validate(prof).valid);
    REQUIRE(prof.sections.count("intro") == 1);
    REQUIRE(prof.sections.count("chorus") == 1);
    REQUIRE(prof.sections.count("outro") == 1);

    const auto& secIntro = prof.sections.at("intro");
    const auto& secChorus = prof.sections.at("chorus");
    CHECK(secIntro.populated);
    CHECK(secIntro.voicing.register_high < secIntro.melody.register_high);
    CHECK(secChorus.melody.motif_density >= secIntro.melody.motif_density);

    auto runSec = [&](SongSection sec) {
        BlendParams bp;
        bp.section = sec;
        const StyleProfile eff = StyleBlender::blend(prof, prof, bp);
        const auto prog = ProgressionGenerator::generateProgression(eff, 4, 0ULL);
        PerformParams perf;
        perf.section = sec;
        return PatternGenerator::generatePattern(eff, prog, perf, 0ULL);
    };

    const auto patIntro = runSec(SongSection::Intro);
    const auto patChorus = runSec(SongSection::Chorus);
    const auto patOutro = runSec(SongSection::Outro);

    auto countFirstHalfOnsets = [](const std::vector<NoteEvent>& evs) {
        std::set<int> ticks;
        for (const auto& e : evs) {
            if (e.onset_beats < 8.0 - 1e-4) {
                ticks.insert(static_cast<int>(std::llround(e.onset_beats * 4.0)));
            }
        }
        return ticks.size();
    };
    CHECK(countFirstHalfOnsets(patIntro.chords) <= countFirstHalfOnsets(patChorus.chords));
    CHECK(countFirstHalfOnsets(patIntro.bass) <= countFirstHalfOnsets(patChorus.bass));
    CHECK((countFirstHalfOnsets(patIntro.chords) + countFirstHalfOnsets(patIntro.bass)) <
          (countFirstHalfOnsets(patChorus.chords) + countFirstHalfOnsets(patChorus.bass)));

    int outroLastBarBass = 0;
    for (const auto& e : patOutro.bass) {
        if (e.onset_beats >= 12.0 - 1e-4) ++outroLastBarBass;
    }
    CHECK(outroLastBarBass == 1);
}

TEST_CASE("Idiom-preserving Randomise keeps original bigrams, varies unlocked slots, and honors locks", "[progression][idiom]") {
    StyleProfile song = makeDefaultStyleProfile();
    song.id = "idiom_song";
    song.harmony.tonic_pc = 5; // F Major
    song.harmony.mode = Mode::Major;
    song.harmony.vocabulary.clear();
    const std::vector<std::string> syms = {"I", "III7", "IV", "iv", "vi7", "ii7", "V7"};
    for (const auto& sym : syms) {
        auto rc = parseRomanChord(sym);
        REQUIRE(rc.has_value());
        double cw = (sym == "iv" || sym == "V7") ? 0.40 : 0.05;
        song.harmony.vocabulary.push_back(ChordVocabEntry{sym, *rc, 1.0 / syms.size(), cw});
    }
    song.harmony.reference_progression = {"I", "III7", "IV", "iv"};
    song.harmony.transitions = {
        {"I",    {{"III7", 0.70}, {"vi7", 0.15}, {"IV", 0.10}, {"ii7", 0.05}}},
        {"III7", {{"IV", 0.75},   {"vi7", 0.15}, {"V7", 0.10}}},
        {"IV",   {{"iv", 0.75},   {"V7", 0.15},  {"ii7", 0.10}}},
        {"iv",   {{"I", 0.75},    {"V7", 0.15},  {"ii7", 0.10}}},
        {"vi7",  {{"IV", 0.50},   {"ii7", 0.30}, {"V7", 0.20}}},
        {"ii7",  {{"V7", 0.65},   {"iv", 0.20},  {"I", 0.15}}},
        {"V7",   {{"I", 0.75},    {"vi7", 0.15}, {"IV", 0.10}}}
    };
    normalizeProfileDistributions(song);

    const auto orig = ProgressionGenerator::generateProgression(song, 4, 0ULL);
    REQUIRE(orig.size() == 4);
    CHECK(orig[0].roman_symbol == "I");
    CHECK(orig[1].roman_symbol == "III7");
    CHECK(orig[2].roman_symbol == "IV");
    CHECK(orig[3].roman_symbol == "iv");

    const auto varA = ProgressionGenerator::generateProgression(song, 4, 101ULL);
    const auto varB = ProgressionGenerator::generateProgression(song, 4, 101ULL);
    REQUIRE(varA == varB);
    REQUIRE(varA.size() == 4);

    int diffs = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (varA[i].roman_symbol != orig[i].roman_symbol) ++diffs;
    }
    CHECK(diffs >= 1);

    std::set<std::pair<std::string, std::string>> origBigrams = {
        {"I", "III7"}, {"III7", "IV"}, {"IV", "iv"}, {"iv", "I"}
    };
    int intactBigrams = 0;
    for (size_t i = 0; i < 3; ++i) {
        if (origBigrams.count({varA[i].roman_symbol, varA[i + 1].roman_symbol})) ++intactBigrams;
    }
    if (origBigrams.count({varA[3].roman_symbol, varA[0].roman_symbol})) ++intactBigrams;
    CHECK(intactBigrams >= 1);

    auto lockedProg = orig;
    lockedProg[1].locked = true;
    const auto varLocked = ProgressionGenerator::generateProgression(song, 4, 202ULL, lockedProg);
    CHECK(varLocked[1].locked == true);
    CHECK(varLocked[1].roman_symbol == "III7");
}

TEST_CASE("Two-bar groove skeletons round-trip, preserve blend endpoints, and rank varied melody", "[analyzer][groove][critic]") {
    std::vector<TimedNoteSec> notes;
    for (int bar = 0; bar < 12; ++bar) {
        const double bSec = static_cast<double>(bar) * 2.0;
        const bool isEven = (bar % 2 == 0);
        // Even bars: 2 hits (beats 1, 3); Odd bars: 4 hits (beats 1, 2, 3, 4)
        const std::vector<double> chordBeats = isEven ? std::vector<double>{0.0, 1.0}
                                                      : std::vector<double>{0.0, 0.5, 1.0, 1.5};
        for (double cb : chordBeats) {
            notes.push_back({bSec + cb, bSec + cb + 0.35, 36, 100, 33, false});
            notes.push_back({bSec + cb, bSec + cb + 0.35, 60, 88, 0, false});
            notes.push_back({bSec + cb, bSec + cb + 0.35, 63, 88, 0, false});
            notes.push_back({bSec + cb, bSec + cb + 0.35, 67, 88, 0, false});
        }
        notes.push_back({bSec + 0.25, bSec + 0.65, 72 + (bar % 4) * 2, 95, 52, false});
        notes.push_back({bSec + 1.00, bSec + 1.40, 75 + (bar % 3) * 2, 95, 52, false});
    }

    AnalysisOptions opts;
    opts.id = "two_bar_groove";
    opts.name = "Two Bar Groove";
    opts.bpm_hint = 120.0;
    const StyleProfile prof = MidiStyleAnalyzer::analyzeTimed(notes, opts);
    REQUIRE(validate(prof).valid);
    CHECK(prof.rhythm.chord_pattern != prof.rhythm.chord_pattern_bar2);
    CHECK(prof.rhythm.chord_pattern_bar2[4] > 0.3);

    const auto dumped = toJson(prof);
    CHECK(dumped.at("schema_version") == 1);
    const auto reloaded = fromJson(dumped);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->rhythm.chord_pattern_bar2 == prof.rhythm.chord_pattern_bar2);
    CHECK(reloaded->rhythm.bass_pattern_bar2 == prof.rhythm.bass_pattern_bar2);

    const auto starter = loadFromFile(presetsRoot() / "90s_hiphop.json");
    REQUIRE(starter.has_value());
    BlendParams bp0;
    bp0.rhythm = 0.0;
    CHECK(StyleBlender::blend(prof, *starter, bp0).rhythm == prof.rhythm);
    BlendParams bp1;
    bp1.rhythm = 1.0;
    CHECK(StyleBlender::blend(prof, *starter, bp1).rhythm == starter->rhythm);

    const auto prog = ProgressionGenerator::generateProgression(prof, 8, 555ULL);
    const auto pat = PatternGenerator::generatePattern(prof, prog, PerformParams{}, 555ULL);
    REQUIRE(pat.melody.size() >= 4);
    std::set<int> uniqMel;
    for (const auto& m : pat.melody) uniqMel.insert(m.pitch);
    CHECK(uniqMel.size() >= 3);
    for (const auto& c : pat.chords) CHECK(c.pitch >= prof.voicing.sub_bass_clearance_midi);
    for (const auto& b : pat.bass) CHECK(b.pitch < prof.voicing.sub_bass_clearance_midi);
}
