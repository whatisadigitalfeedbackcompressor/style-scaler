#include "engine/ModelManager.h"
#include "engine/TranscriptionService.h"

#include "style_scaler/MidiStyleAnalyzer.h"
#include "style_scaler/SmfIO.h"
#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <JuceHeader.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace style_scaler;

namespace {

enum class BatchMode {
    Corpus,
    PerFolder,
    PerSong
};

void printUsage() {
    std::cout <<
        "style_deconstruct - Offline Audio/MIDI Song Deconstructor for StyleScaler\n\n"
        "Usage:\n"
        "  style_deconstruct --input <file_or_dir> [--out <profile.json> | --out-dir <dir>] [options]\n\n"
        "Options:\n"
        "  --input <path>          Input audio/MIDI file or directory (repeatable)\n"
        "  --mode <mode>           corpus | per-folder | per-song (default: corpus for --out, per-folder for --out-dir)\n"
        "  --out <file.json>       Single StyleProfile JSON output (corpus mode)\n"
        "  --out-dir <dir>         Directory to write one or more StyleProfile JSONs\n"
        "  --id <id>               Profile ID (corpus mode)\n"
        "  --name <name>           Display name (corpus mode)\n"
        "  --genre <genre>         Genre tag\n"
        "  --description <text>    Profile description\n"
        "  --model <path.gguf>     Path to MuScriptor GGUF checkpoint\n"
        "  --download-model        Download & verify muscriptor-small-f16.gguf (~209 MB) first\n"
        "  --spectral              Force zero-model SpectralFallbackTranscriber\n"
        "  --threads <N>           CPU threads for MuScriptor (default: hardware_concurrency - 1)\n"
        "  --max-seconds <sec>     Max seconds per track (0 = full song up to 600s) [0]\n"
        "  --bpm <float>           Override BPM detection\n"
        "  --key <C..B>            Override key root\n"
        "  --scale-mode <name>     Override mode (minor, dorian, phrygian, ...)\n"
        "  --clearance <midi>      Override sub_bass_clearance_midi (36..60)\n"
        "  --save-midi-dir <dir>   Also export transcribed .mid files into <dir>\n"
        "  --catalog <file.json>   Generate/update SHA-256 preset catalog.json from output dir\n";
}

bool isSupportedMediaFile(const fs::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" ||
           ext == ".flac" || ext == ".ogg" || ext == ".mp3" ||
           ext == ".mid" || ext == ".midi";
}

std::string sanitizeSlug(std::string s) {
    std::string out;
    bool prevUnderscore = false;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            prevUnderscore = false;
        } else if (!prevUnderscore && !out.empty()) {
            out.push_back('_');
            prevUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "style_profile" : out;
}

std::string prettifyTitle(std::string s) {
    std::string out;
    bool capNext = true;
    for (char c : s) {
        if (c == '_' || c == '-') {
            out.push_back(' ');
            capNext = true;
        } else {
            out.push_back(capNext ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
            capNext = std::isspace(static_cast<unsigned char>(c)) != 0;
        }
    }
    return out.empty() ? "Deconstructed Style" : out;
}

std::string currentIsoUtc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmUtc{};
#ifdef _WIN32
    gmtime_s(&tmUtc, &tt);
#else
    gmtime_r(&tt, &tmUtc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

bool writeCatalogFromDir(const fs::path& stylesDir, const fs::path& catalogPath) {
    std::vector<fs::path> jsonFiles;
    if (fs::exists(stylesDir) && fs::is_directory(stylesDir)) {
        for (const auto& entry : fs::directory_iterator(stylesDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                if (fs::weakly_canonical(entry.path()) == fs::weakly_canonical(catalogPath)) continue;
                jsonFiles.push_back(entry.path());
            }
        }
    }
    std::sort(jsonFiles.begin(), jsonFiles.end());

    nlohmann::json presetsArr = nlohmann::json::array();
    const fs::path catParent = catalogPath.has_parent_path() ? catalogPath.parent_path() : fs::current_path();
    const std::string stamp = currentIsoUtc();

    for (const auto& jf : jsonFiles) {
        auto prof = loadFromFile(jf);
        if (!prof) continue;

        const juce::File jFile(jf.string());
        const std::string sha = juce::SHA256(jFile).toHexString().toLowerCase().toStdString();

        std::error_code ec;
        fs::path rel = fs::relative(jf, catParent, ec);
        std::string relStr = (ec ? jf.filename().string() : rel.generic_string());

        presetsArr.push_back({
            {"id", prof->id},
            {"name", prof->name},
            {"genre", prof->genre},
            {"description", prof->description},
            {"file", relStr},
            {"sha256", sha},
            {"bpm_min", prof->bpm_min},
            {"bpm_max", prof->bpm_max},
            {"default_bpm", prof->default_bpm},
            {"mode", std::string(modeName(prof->harmony.mode))},
            {"tonic_pc", prof->harmony.tonic_pc},
            {"updated_at", stamp}
        });
    }

    nlohmann::json catalog = {
        {"schema_version", 1},
        {"catalog_name", "StyleScaler Preset Catalog"},
        {"updated_at", stamp},
        {"presets", presetsArr}
    };

    std::error_code ec;
    if (catalogPath.has_parent_path()) fs::create_directories(catalogPath.parent_path(), ec);
    std::ofstream ofs(catalogPath, std::ios::binary);
    if (!ofs) return false;
    ofs << catalog.dump(2) << '\n';
    return static_cast<bool>(ofs);
}

} // namespace

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc <= 1) {
        printUsage();
        return 0;
    }

    std::vector<fs::path> inputs;
    std::optional<BatchMode> explicitMode;
    std::optional<fs::path> outFile;
    std::optional<fs::path> outDir;
    std::optional<fs::path> catalogPath;
    std::optional<fs::path> saveMidiDir;
    std::optional<fs::path> modelPath;

    std::string idArg;
    std::string nameArg;
    std::string genreArg;
    std::string descArg;
    bool downloadModel = false;
    bool forceSpectral = false;
    double maxSeconds = 0.0;
    int threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
    double bpmHint = 0.0;
    std::optional<int> keyOverride;
    std::optional<Mode> modeOverride;
    std::optional<int> clearanceOverride;

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
        } else if (flag == "--input") {
            inputs.emplace_back(needVal("--input"));
        } else if (flag == "--mode") {
            const auto m = needVal("--mode");
            if (m == "corpus") explicitMode = BatchMode::Corpus;
            else if (m == "per-folder") explicitMode = BatchMode::PerFolder;
            else if (m == "per-song") explicitMode = BatchMode::PerSong;
            else {
                std::cerr << "Invalid --mode: " << m << "\n";
                return 2;
            }
        } else if (flag == "--out") {
            outFile = needVal("--out");
        } else if (flag == "--out-dir") {
            outDir = needVal("--out-dir");
        } else if (flag == "--id") {
            idArg = needVal("--id");
        } else if (flag == "--name") {
            nameArg = needVal("--name");
        } else if (flag == "--genre") {
            genreArg = needVal("--genre");
        } else if (flag == "--description") {
            descArg = needVal("--description");
        } else if (flag == "--model") {
            modelPath = needVal("--model");
        } else if (flag == "--download-model") {
            downloadModel = true;
        } else if (flag == "--spectral") {
            forceSpectral = true;
        } else if (flag == "--threads") {
            threads = std::max(1, std::stoi(needVal("--threads")));
        } else if (flag == "--max-seconds") {
            maxSeconds = std::stod(needVal("--max-seconds"));
        } else if (flag == "--bpm") {
            bpmHint = std::stod(needVal("--bpm"));
        } else if (flag == "--key") {
            const auto k = needVal("--key");
            keyOverride = parsePitchClass(k);
            if (!keyOverride) {
                std::cerr << "Invalid --key: " << k << "\n";
                return 2;
            }
        } else if (flag == "--scale-mode") {
            const auto m = needVal("--scale-mode");
            modeOverride = parseMode(m);
            if (!modeOverride) {
                std::cerr << "Invalid --scale-mode: " << m << "\n";
                return 2;
            }
        } else if (flag == "--clearance") {
            clearanceOverride = std::stoi(needVal("--clearance"));
        } else if (flag == "--save-midi-dir") {
            saveMidiDir = needVal("--save-midi-dir");
        } else if (flag == "--catalog") {
            catalogPath = needVal("--catalog");
        } else {
            std::cerr << "Unknown option: " << flag << "\n";
            return 2;
        }
    }

    if (inputs.empty()) {
        std::cerr << "At least one --input file or directory is required.\n";
        return 2;
    }

    ModelManager modelManager;
    if (modelPath.has_value()) {
        std::string err;
        if (!modelManager.setCustomModelFile(juce::File(modelPath->string()), err)) {
            std::cerr << "Model error: " << err << "\n";
            return 1;
        }
    }
    if (downloadModel && !modelManager.isModelAvailable()) {
        std::cout << "Downloading MuScriptor small GGUF (~209 MB)...\n";
        std::string dlErr;
        int lastPct = -1;
        const bool ok = modelManager.downloadDefaultModelBlocking(
            [&](ModelManager::DownloadState s, float prog) {
                const int pct = static_cast<int>(prog * 100.0f);
                if (s == ModelManager::DownloadState::Downloading && pct >= lastPct + 10) {
                    lastPct = pct;
                    std::cout << "  " << pct << "%...\n" << std::flush;
                } else if (s == ModelManager::DownloadState::Verifying) {
                    std::cout << "  Verifying SHA-256...\n" << std::flush;
                }
                return true;
            },
            dlErr);
        if (!ok) {
            std::cerr << "Model download failed: " << dlErr << "\n";
            return 1;
        }
        std::cout << "Model ready: " << modelManager.getActiveModelFile().getFullPathName().toStdString() << "\n";
    }

    const BatchMode mode = explicitMode.value_or(outDir.has_value() ? BatchMode::PerFolder : BatchMode::Corpus);

    // Group input files according to BatchMode
    struct JobGroup {
        std::string id;
        std::string name;
        std::string genre;
        fs::path destJson;
        std::vector<fs::path> files;
    };
    std::vector<JobGroup> jobs;

    auto collectRecursive = [](const fs::path& dir) {
        std::vector<fs::path> found;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() && isSupportedMediaFile(entry.path())) {
                found.push_back(entry.path());
            }
        }
        std::sort(found.begin(), found.end());
        return found;
    };

    if (mode == BatchMode::Corpus) {
        JobGroup g;
        std::vector<fs::path> allFiles;
        for (const auto& in : inputs) {
            if (fs::is_directory(in)) {
                auto sub = collectRecursive(in);
                allFiles.insert(allFiles.end(), sub.begin(), sub.end());
            } else if (fs::is_regular_file(in) && isSupportedMediaFile(in)) {
                allFiles.push_back(in);
            }
        }
        const std::string fallbackStem = inputs.front().stem().string();
        g.id = !idArg.empty() ? sanitizeSlug(idArg) : sanitizeSlug(fallbackStem);
        g.name = !nameArg.empty() ? nameArg : prettifyTitle(g.id);
        g.genre = !genreArg.empty() ? genreArg : prettifyTitle(g.id);
        g.destJson = outFile.value_or(outDir.value_or(fs::path(".")) / (g.id + ".json"));
        g.files = std::move(allFiles);
        if (!g.files.empty()) jobs.push_back(std::move(g));
    } else if (mode == BatchMode::PerFolder) {
        const fs::path destRoot = outDir.value_or(fs::path("presets/styles"));
        for (const auto& in : inputs) {
            if (fs::is_directory(in)) {
                bool hasSubdirs = false;
                for (const auto& e : fs::directory_iterator(in)) {
                    if (e.is_directory()) {
                        auto files = collectRecursive(e.path());
                        if (!files.empty()) {
                            hasSubdirs = true;
                            JobGroup g;
                            g.id = sanitizeSlug(e.path().filename().string());
                            g.name = prettifyTitle(g.id);
                            g.genre = !genreArg.empty() ? genreArg : g.name;
                            g.destJson = destRoot / (g.id + ".json");
                            g.files = std::move(files);
                            jobs.push_back(std::move(g));
                        }
                    }
                }
                if (!hasSubdirs) {
                    auto files = collectRecursive(in);
                    if (!files.empty()) {
                        JobGroup g;
                        g.id = !idArg.empty() ? sanitizeSlug(idArg) : sanitizeSlug(in.filename().string());
                        g.name = !nameArg.empty() ? nameArg : prettifyTitle(g.id);
                        g.genre = !genreArg.empty() ? genreArg : g.name;
                        g.destJson = destRoot / (g.id + ".json");
                        g.files = std::move(files);
                        jobs.push_back(std::move(g));
                    }
                }
            } else if (fs::is_regular_file(in) && isSupportedMediaFile(in)) {
                JobGroup g;
                g.id = sanitizeSlug(in.stem().string());
                g.name = prettifyTitle(g.id);
                g.genre = !genreArg.empty() ? genreArg : "Deconstructed";
                g.destJson = destRoot / (g.id + ".json");
                g.files = {in};
                jobs.push_back(std::move(g));
            }
        }
    } else { // PerSong
        const fs::path destRoot = outDir.value_or(fs::path("presets/styles"));
        std::vector<fs::path> allFiles;
        for (const auto& in : inputs) {
            if (fs::is_directory(in)) {
                auto sub = collectRecursive(in);
                allFiles.insert(allFiles.end(), sub.begin(), sub.end());
            } else if (fs::is_regular_file(in) && isSupportedMediaFile(in)) {
                allFiles.push_back(in);
            }
        }
        for (const auto& f : allFiles) {
            JobGroup g;
            g.id = sanitizeSlug(f.stem().string());
            g.name = prettifyTitle(f.stem().string());
            g.genre = !genreArg.empty() ? genreArg : "Deconstructed Song";
            g.destJson = destRoot / (g.id + ".json");
            g.files = {f};
            jobs.push_back(std::move(g));
        }
    }

    if (jobs.empty()) {
        std::cerr << "No supported audio/MIDI files found in inputs.\n";
        return 1;
    }

    TranscriptionService txService(modelManager);
    TranscriptionService::DeconstructOptions dopts;
    dopts.max_seconds = maxSeconds;
    dopts.n_threads = threads;
    dopts.bpm_hint = bpmHint;
    dopts.force_spectral = forceSpectral;

    int succeeded = 0;
    for (const auto& job : jobs) {
        std::cout << "=== Deconstructing [" << job.id << "] (" << job.files.size() << " file(s)) ===\n";
        std::vector<SongInput> songs;
        std::string lastBackend;

        for (const auto& f : job.files) {
            std::cout << "  -> " << f.filename().string() << " ... " << std::flush;
            const auto ds = txService.deconstructFileBlocking(juce::File(f.string()), dopts);
            if (!ds.success) {
                std::cout << "SKIPPED (" << ds.error << ")\n";
                continue;
            }
            lastBackend = ds.backend_name;
            std::cout << ds.backend_name << " (" << ds.song_input.notes.size()
                      << " notes @ " << std::fixed << std::setprecision(1) << ds.song_input.bpm << " BPM)\n";

            if (saveMidiDir.has_value()) {
                const fs::path midOut = *saveMidiDir / (sanitizeSlug(f.stem().string()) + ".mid");
                (void)SmfWriter::writeType1(midOut, ds.song_input.bpm,
                                            ds.song_input.time_sig_num, ds.song_input.time_sig_den,
                                            ds.song_input.notes);
            }
            songs.push_back(ds.song_input);
        }

        if (songs.empty()) {
            std::cerr << "  No valid tracks extracted for " << job.id << "\n";
            continue;
        }

        AnalysisOptions aopts;
        aopts.id = job.id;
        aopts.name = job.name;
        aopts.genre = job.genre;
        aopts.source = lastBackend.empty() ? "offline_deconstruct" : ("offline_" + lastBackend);
        aopts.bpm_hint = bpmHint;
        aopts.key_override_pc = keyOverride;
        aopts.mode_override = modeOverride;

        StyleProfile prof = MidiStyleAnalyzer::analyzeCorpus(songs, aopts);
        if (!descArg.empty()) {
            prof.description = descArg;
        } else {
            prof.description = "Offline deconstructed from " + std::to_string(songs.size()) +
                               " track(s) via " + aopts.source;
        }
        if (clearanceOverride.has_value()) {
            prof.voicing.sub_bass_clearance_midi = std::clamp(
                *clearanceOverride, 36, std::min(60, prof.voicing.register_high - 6));
            prof.voicing.register_low = std::max(prof.voicing.register_low, prof.voicing.sub_bass_clearance_midi);
        }
        normalizeProfileDistributions(prof);

        if (auto saveRes = saveToFile(prof, job.destJson); !saveRes) {
            std::cerr << "  Error writing " << job.destJson.string() << ": " << saveRes.error() << "\n";
            continue;
        }

        std::cout << "  Wrote " << job.destJson.string()
                  << " | Key=" << pitchClassName(prof.harmony.tonic_pc) << " " << modeName(prof.harmony.mode)
                  << " | Vocab=" << prof.harmony.vocabulary.size()
                  << " | Floor=" << prof.voicing.sub_bass_clearance_midi << "\n";
        ++succeeded;
    }

    if (succeeded == 0) {
        std::cerr << "No profiles were successfully created.\n";
        return 1;
    }

    if (catalogPath.has_value()) {
        const fs::path scanDir = outDir.value_or(jobs.front().destJson.has_parent_path()
                                                     ? jobs.front().destJson.parent_path()
                                                     : fs::path("."));
        if (writeCatalogFromDir(scanDir, *catalogPath)) {
            std::cout << "Updated catalog: " << catalogPath->string() << "\n";
        } else {
            std::cerr << "Failed writing catalog: " << catalogPath->string() << "\n";
            return 1;
        }
    }

    return 0;
}
