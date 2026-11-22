#include "style_scaler/GenerationEngine.h"
#include "style_scaler/MidiStyleAnalyzer.h"
#include "style_scaler/SmfIO.h"
#include "style_scaler/StyleBlender.h"
#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace style_scaler;

namespace {

void printUsage() {
    std::cout <<
        "style_cli - StyleScaler Cross-Genre MIDI Exporter & Style Analyzer\n\n"
        "Usage (Generate & Blend MIDI):\n"
        "  style_cli --slot-a <path_or_id> --slot-b <path_or_id> --out <output.mid> [options]\n\n"
        "Options:\n"
        "  --presets-dir <dir>   Directory of <id>.json presets (default: presets/styles)\n"
        "  --harmony <0..1>      Harmony axis blend (0=Slot A, 1=Slot B) [0.0]\n"
        "  --rhythm <0..1>       Rhythm/Groove axis blend [0.0]\n"
        "  --voicing <0..1>      Voicing/Space axis blend [0.0]\n"
        "  --melody <0..1>       Melody phrasing axis blend [0.0]\n"
        "  --scale <0.5x|1x|2x>  Tempo scaling [1x]\n"
        "  --bars <4|8|16>       Progression length in bars [8]\n"
        "  --bpm <float>         Output MIDI BPM (overrides blended default_bpm)\n"
        "  --seed <uint64>       Deterministic PRNG seed [42]\n"
        "  --spread <0..1>       Voicing spread control [0.5]\n"
        "  --density <0..1>      Melody density control [0.5]\n"
        "  --humanize <0..1>     Timing & velocity humanize [0.15]\n"
        "  --groove <0|1>        Groove sync toggle [1]\n"
        "  --key <C..B>          Key root override (e.g. C, F#, Bb)\n"
        "  --mode <name>         Mode override (minor, major, dorian, phrygian, mixolydian, ...)\n"
        "  --section <name>      Song section DNA (intro, chorus, outro) [chorus]\n"
        "  --out-profile <json>  Also write blended StyleProfile JSON\n\n"
        "Usage (Analyze .mid into StyleProfile JSON):\n"
        "  style_cli --analyze <input.mid> --out-profile <out.json> [--id <id>] [--name <name>] [--genre <genre>]\n";
}

std::optional<fs::path> resolveProfilePath(const std::string& arg, const fs::path& presetsDir) {
    fs::path p(arg);
    if (fs::exists(p)) return p;
    fs::path inDir = presetsDir / arg;
    if (fs::exists(inDir)) return inDir;
    fs::path inDirJson = presetsDir / (arg + ".json");
    if (fs::exists(inDirJson)) return inDirJson;
    // Try walking up from current directory in case run from build/Release
    fs::path cur = fs::current_path();
    for (int i = 0; i < 4; ++i) {
        fs::path cand = cur / "presets" / "styles" / (arg + ".json");
        if (fs::exists(cand)) return cand;
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc <= 1) {
        printUsage();
        return 0;
    }

    std::string slotAArg = "90s_hiphop";
    std::string slotBArg = "liquid_dnb";
    fs::path presetsDir = "presets/styles";
    fs::path outMid = "out.mid";
    std::optional<fs::path> analyzeInput;
    std::optional<fs::path> outProfilePath;
    std::string analyzeId = "analyzed_midi";
    std::string analyzeName = "Analyzed MIDI";
    std::string analyzeGenre = "Custom";

    BlendParams blendParams;
    PerformParams perform;
    int bars = 8;
    std::optional<double> bpmOverride;
    uint64_t seed = 42;

    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto needVal = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (flag == "--help" || flag == "-h") {
            printUsage();
            return 0;
        } else if (flag == "--slot-a") {
            slotAArg = needVal("--slot-a");
        } else if (flag == "--slot-b") {
            slotBArg = needVal("--slot-b");
        } else if (flag == "--presets-dir") {
            presetsDir = needVal("--presets-dir");
        } else if (flag == "--harmony") {
            blendParams.harmony = std::stod(needVal("--harmony"));
        } else if (flag == "--rhythm") {
            blendParams.rhythm = std::stod(needVal("--rhythm"));
        } else if (flag == "--voicing") {
            blendParams.voicing = std::stod(needVal("--voicing"));
        } else if (flag == "--melody") {
            blendParams.melody = std::stod(needVal("--melody"));
        } else if (flag == "--scale") {
            const auto scStr = needVal("--scale");
            if (auto sc = parseTempoScale(scStr)) {
                blendParams.tempo_scale = *sc;
                perform.tempo_scale = *sc;
            } else {
                std::cerr << "Invalid --scale: " << scStr << "\n";
                return 2;
            }
        } else if (flag == "--bars") {
            bars = std::stoi(needVal("--bars"));
        } else if (flag == "--bpm") {
            bpmOverride = std::stod(needVal("--bpm"));
        } else if (flag == "--seed") {
            seed = std::stoull(needVal("--seed"));
        } else if (flag == "--spread") {
            perform.voicing_spread = std::stod(needVal("--spread"));
        } else if (flag == "--density") {
            perform.melody_density = std::stod(needVal("--density"));
        } else if (flag == "--humanize") {
            perform.humanize = std::stod(needVal("--humanize"));
        } else if (flag == "--groove") {
            perform.groove_sync = (std::stoi(needVal("--groove")) != 0);
        } else if (flag == "--key") {
            const auto kStr = needVal("--key");
            if (auto pc = parsePitchClass(kStr)) {
                blendParams.key_override_pc = *pc;
            } else {
                std::cerr << "Invalid --key: " << kStr << "\n";
                return 2;
            }
        } else if (flag == "--mode") {
            const auto mStr = needVal("--mode");
            if (auto m = parseMode(mStr)) {
                blendParams.mode_override = *m;
            } else {
                std::cerr << "Invalid --mode: " << mStr << "\n";
                return 2;
            }
        } else if (flag == "--section") {
            const auto sStr = needVal("--section");
            if (auto sec = parseSongSection(sStr)) {
                blendParams.section = *sec;
                perform.section = *sec;
            } else {
                std::cerr << "Invalid --section: " << sStr << "\n";
                return 2;
            }
        } else if (flag == "--out") {
            outMid = needVal("--out");
        } else if (flag == "--analyze") {
            analyzeInput = needVal("--analyze");
        } else if (flag == "--out-profile") {
            outProfilePath = needVal("--out-profile");
        } else if (flag == "--id") {
            analyzeId = needVal("--id");
        } else if (flag == "--name") {
            analyzeName = needVal("--name");
        } else if (flag == "--genre") {
            analyzeGenre = needVal("--genre");
        } else {
            std::cerr << "Unknown option: " << flag << "\n";
            return 2;
        }
    }

    if (analyzeInput.has_value()) {
        auto smf = SmfReader::readFile(*analyzeInput);
        if (!smf) {
            std::cerr << "MIDI read error: " << smf.error() << "\n";
            return 1;
        }
        AnalysisOptions opts;
        opts.id = analyzeId;
        opts.name = analyzeName;
        opts.genre = analyzeGenre;
        opts.source = "cli_midi_analysis";
        opts.time_sig_num = smf->time_sig_num;
        opts.time_sig_den = smf->time_sig_den;
        opts.key_override_pc = blendParams.key_override_pc;
        opts.mode_override = blendParams.mode_override;

        const double bpm = bpmOverride.value_or(smf->bpm);
        const auto profile = MidiStyleAnalyzer::analyze(smf->notes, bpm, opts);
        const auto outPath = outProfilePath.value_or(fs::path(analyzeId + ".json"));
        auto res = saveToFile(profile, outPath);
        if (!res) {
            std::cerr << "Failed saving StyleProfile: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Analyzed " << analyzeInput->string() << " (" << smf->notes.size()
                  << " notes @ " << bpm << " BPM) -> " << outPath.string() << "\n"
                  << "Detected Key: " << pitchClassName(profile.harmony.tonic_pc) << " "
                  << modeName(profile.harmony.mode) << "\n";
        return 0;
    }

    const auto pathA = resolveProfilePath(slotAArg, presetsDir);
    if (!pathA) {
        std::cerr << "Could not resolve Slot A profile: " << slotAArg << "\n";
        return 1;
    }
    const auto pathB = resolveProfilePath(slotBArg, presetsDir);
    if (!pathB) {
        std::cerr << "Could not resolve Slot B profile: " << slotBArg << "\n";
        return 1;
    }

    auto profA = loadFromFile(*pathA);
    if (!profA) {
        std::cerr << "Slot A load error: " << profA.error() << "\n";
        return 1;
    }
    auto profB = loadFromFile(*pathB);
    if (!profB) {
        std::cerr << "Slot B load error: " << profB.error() << "\n";
        return 1;
    }

    const StyleProfile blended = StyleBlender::blend(*profA, *profB, blendParams);
    const double outBpm = bpmOverride.value_or(blended.default_bpm);
    const auto progression = ProgressionGenerator::generateProgression(
        blended, bars, seed, {}, blendParams.tempo_scale);
    const auto pattern = PatternGenerator::generatePattern(blended, progression, perform, seed);

    auto writeRes = SmfWriter::writeType1(outMid, outBpm, blended.time_sig_num, blended.time_sig_den, pattern.all_events);
    if (!writeRes) {
        std::cerr << "MIDI write error: " << writeRes.error() << "\n";
        return 1;
    }

    if (outProfilePath.has_value()) {
        if (auto pRes = saveToFile(blended, *outProfilePath); !pRes) {
            std::cerr << "Profile write error: " << pRes.error() << "\n";
            return 1;
        }
    }

    std::cout << "StyleScaler Blend: " << profA->name << " <-> " << profB->name << "\n"
              << "  Axes: H=" << blendParams.harmony << " R=" << blendParams.rhythm
              << " V=" << blendParams.voicing << " M=" << blendParams.melody
              << " | Scale=" << tempoScaleName(blendParams.tempo_scale) << "\n"
              << "  Key: " << pitchClassName(blended.harmony.tonic_pc) << " "
              << modeName(blended.harmony.mode)
              << " | BPM: " << outBpm
              << " | Sub-bass clearance: MIDI " << blended.voicing.sub_bass_clearance_midi << "\n"
              << "  Progression (" << progression.size() << " slots / " << bars << " bars):\n";
    for (const auto& s : progression) {
        std::cout << "    Bar " << (s.bar_index + 1) << " [" << s.start_beat << ".."
                  << (s.start_beat + s.duration_beats) << "]: "
                  << formatRomanAsConcreteChord(s.chord, blended.harmony.tonic_pc, blended.harmony.mode)
                  << " (" << s.roman_symbol << ")\n";
    }
    std::cout << "  Events: Ch1(Chords)=" << pattern.chords.size()
              << " Ch2(Melody)=" << pattern.melody.size()
              << " Ch3(Bass)=" << pattern.bass.size()
              << " Total=" << pattern.all_events.size() << "\n"
              << "  Wrote: " << outMid.string() << "\n";

    return 0;
}
