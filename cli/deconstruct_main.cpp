#include "engine/ModelManager.h"
#include "engine/TranscriptionService.h"

#include "style_scaler/AudioMetadata.h"
#include "style_scaler/MidiStyleAnalyzer.h"
#include "style_scaler/SmfIO.h"
#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <JuceHeader.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
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

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace style_scaler;

namespace {

std::atomic<bool> g_stopRequested{false};

void handleSigInt(int) {
    g_stopRequested.store(true, std::memory_order_release);
}

enum class BatchMode {
    Corpus,
    PerFolder,
    PerSong,
    AutoGenre
};

void printUsage() {
    std::cout <<
        "style_deconstruct - Offline Audio/MIDI Song Deconstructor for StyleScaler\n\n"
        "Usage:\n"
        "  style_deconstruct --input <file_or_dir> [--out <profile.json> | --out-dir <dir>] [options]\n\n"
        "Options:\n"
        "  --input <path>          Input audio/MIDI file or directory (repeatable)\n"
        "  --mode <mode>           corpus | per-folder | per-song | auto-genre\n"
        "                          (default: corpus for --out, per-folder for --out-dir)\n"
        "  --out <file.json>       Single StyleProfile JSON output (corpus mode)\n"
        "  --out-dir <dir>         Directory to write one or more StyleProfile JSONs\n"
        "  --id <id>               Profile ID (corpus mode)\n"
        "  --name <name>           Display name (corpus mode)\n"
        "  --genre <genre>         Explicit genre override\n"
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
        "  --catalog <file.json>   Generate/update SHA-256 preset catalog.json from output dir\n"
        "  --merge-existing        Merge into existing genre JSONs incrementally (default in auto-genre/per-folder)\n"
        "  --no-merge              Overwrite existing genre JSONs instead of merging\n"
        "  --cache-dir <dir>       Per-song transcription checkpoint directory [.deconstruct_cache]\n"
        "  --no-cache              Disable per-song transcription checkpoint cache\n";
}

bool isSupportedMediaFile(const fs::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" ||
           ext == ".flac" || ext == ".ogg" || ext == ".opus" ||
           ext == ".mp3" || ext == ".m4a" || ext == ".aac" ||
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

std::string formatMmSs(double totalSeconds) {
    const int secs = std::max(0, static_cast<int>(std::llround(totalSeconds)));
    const int mins = secs / 60;
    const int rem = secs % 60;
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << mins << ":"
        << std::setw(2) << std::setfill('0') << rem;
    return oss.str();
}

std::string computeFileFingerprint(const fs::path& p, double maxSeconds, bool forceSpectral) {
    std::error_code ec;
    const auto fsize = fs::file_size(p, ec);
    const auto ftime = fs::last_write_time(p, ec).time_since_epoch().count();
    std::ostringstream keySeed;
    keySeed << p.filename().string() << "|" << fsize << "|" << ftime
            << "|ms=" << static_cast<int>(std::llround(maxSeconds * 10.0))
            << "|sp=" << (forceSpectral ? 1 : 0);
    if (!ec && fsize > 0 && fsize <= 64ULL * 1024ULL * 1024ULL) {
        const juce::String fileSha = juce::SHA256(juce::File(p.string())).toHexString().toLowerCase();
        keySeed << "|sha=" << fileSha.toStdString();
    }
    const std::string seedStr = keySeed.str();
    return juce::SHA256(seedStr.data(), seedStr.size()).toHexString().toLowerCase().toStdString();
}

bool loadCachedSong(const fs::path& cacheFile, TranscriptionService::DeconstructedSong& out) {
    if (!fs::exists(cacheFile)) return false;
    try {
        std::ifstream ifs(cacheFile, std::ios::binary);
        if (!ifs) return false;
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (!j.is_object() || !j.contains("notes") || !j["notes"].is_array()) return false;

        out = TranscriptionService::DeconstructedSong{};
        out.backend_name = j.value("backend_name", "MuScriptor (Cached)");
        out.song_input.bpm = j.value("bpm", 120.0);
        out.song_input.time_sig_num = j.value("time_sig_num", 4);
        out.song_input.time_sig_den = j.value("time_sig_den", 4);

        if (j.contains("metadata") && j["metadata"].is_object()) {
            const auto& m = j["metadata"];
            out.metadata.title = m.value("title", "");
            out.metadata.artist = m.value("artist", "");
            out.metadata.album = m.value("album", "");
            out.metadata.raw_genre = m.value("raw_genre", "");
            out.metadata.canonical_genre = m.value("canonical_genre", "");
            out.metadata.genre_source = m.value("genre_source", "");
            out.metadata.bpm = m.value("bpm", 0.0);
            if (m.contains("tags") && m["tags"].is_array()) {
                for (const auto& t : m["tags"]) {
                    if (t.is_string()) out.metadata.tags.push_back(t.get<std::string>());
                }
            }
        }

        for (const auto& item : j["notes"]) {
            NoteEvent ev;
            ev.onset_beats = item.value("o", 0.0);
            ev.duration_beats = item.value("d", 0.25);
            ev.pitch = item.value("p", 60);
            ev.velocity = item.value("v", 96);
            ev.channel = item.value("c", 1);
            ev.is_drum = item.value("dr", false);
            ev.program = item.value("pr", 0);
            out.song_input.notes.push_back(ev);
        }
        out.success = !out.song_input.notes.empty();
        return out.success;
    } catch (...) {
        return false;
    }
}

void saveCachedSong(const fs::path& cacheFile,
                    const std::string& fingerprint,
                    const TranscriptionService::DeconstructedSong& ds) {
    if (!ds.success || ds.song_input.notes.empty()) return;
    try {
        std::error_code ec;
        if (cacheFile.has_parent_path()) fs::create_directories(cacheFile.parent_path(), ec);
        nlohmann::json notesArr = nlohmann::json::array();
        for (const auto& ev : ds.song_input.notes) {
            notesArr.push_back({
                {"o", ev.onset_beats},
                {"d", ev.duration_beats},
                {"p", ev.pitch},
                {"v", ev.velocity},
                {"c", ev.channel},
                {"dr", ev.is_drum},
                {"pr", ev.program}
            });
        }
        nlohmann::json j = {
            {"fingerprint", fingerprint},
            {"backend_name", ds.backend_name},
            {"bpm", ds.song_input.bpm},
            {"time_sig_num", ds.song_input.time_sig_num},
            {"time_sig_den", ds.song_input.time_sig_den},
            {"metadata", {
                {"title", ds.metadata.title},
                {"artist", ds.metadata.artist},
                {"album", ds.metadata.album},
                {"raw_genre", ds.metadata.raw_genre},
                {"canonical_genre", ds.metadata.canonical_genre},
                {"genre_source", ds.metadata.genre_source},
                {"bpm", ds.metadata.bpm},
                {"tags", ds.metadata.tags}
            }},
            {"notes", std::move(notesArr)}
        };
        std::ofstream ofs(cacheFile, std::ios::binary);
        if (ofs) ofs << j.dump() << '\n';
    } catch (...) {}
}

bool isStdoutTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

std::vector<std::string> buildTrackFingerprints(const fs::path& p, double maxSeconds, bool forceSpectral) {
    std::vector<std::string> fps;
    fps.push_back(computeFileFingerprint(p, maxSeconds, forceSpectral));
    if (maxSeconds > 0.0) {
        fps.push_back(computeFileFingerprint(p, 0.0, forceSpectral));
    }
    return fps;
}

bool profileContainsAnyTrack(const fs::path& jsonPath, const std::vector<std::string>& fingerprints) {
    if (!fs::exists(jsonPath)) return false;
    auto prof = loadFromFile(jsonPath);
    if (!prof) return false;
    for (const auto& fp : fingerprints) {
        if (!fp.empty() &&
            std::find(prof->processed_tracks.begin(), prof->processed_tracks.end(), fp) !=
                prof->processed_tracks.end()) {
            return true;
        }
    }
    return false;
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
    std::map<std::string, std::vector<std::string>> genresMap;
    const fs::path catParent = catalogPath.has_parent_path() ? catalogPath.parent_path() : fs::current_path();
    const std::string stamp = currentIsoUtc();

    for (const auto& jf : jsonFiles) {
        if (jf.filename() == "catalog.json") continue;
        auto prof = loadFromFile(jf);
        if (!prof) continue;

        const juce::File jFile(jf.string());
        const std::string sha = juce::SHA256(jFile).toHexString().toLowerCase().toStdString();

        std::error_code ec;
        fs::path rel = fs::relative(jf, catParent, ec);
        std::string relStr = (ec ? jf.filename().string() : rel.generic_string());

        nlohmann::json item = {
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
        };
        if (!prof->tags.empty()) item["tags"] = prof->tags;
        if (!prof->genre_source.empty()) item["genre_source"] = prof->genre_source;
        if (!prof->artist.empty()) item["artist"] = prof->artist;
        presetsArr.push_back(std::move(item));

        const std::string gKey = prof->genre.empty() ? "Uncategorized" : prof->genre;
        genresMap[gKey].push_back(prof->id);
    }

    for (auto& [_, ids] : genresMap) {
        std::sort(ids.begin(), ids.end());
    }

    nlohmann::json catalog = {
        {"schema_version", 1},
        {"catalog_name", "StyleScaler Preset Catalog"},
        {"updated_at", stamp},
        {"genres", genresMap},
        {"presets", presetsArr}
    };

    std::error_code ec;
    if (catalogPath.has_parent_path()) fs::create_directories(catalogPath.parent_path(), ec);
    std::ofstream ofs(catalogPath, std::ios::binary);
    if (!ofs) return false;
    ofs << catalog.dump(2) << '\n';
    return static_cast<bool>(ofs);
}

void renderProgressLine(size_t idx,
                        size_t total,
                        const std::string& filename,
                        float trackProg,
                        const std::string& stage,
                        double elapsedSec) {
    const float clampedTrack = std::clamp(trackProg, 0.0f, 1.0f);
    const double totalFrac = std::clamp((static_cast<double>(idx) + clampedTrack) / static_cast<double>(std::max<size_t>(1, total)), 0.0, 1.0);
    const int totalPct = static_cast<int>(std::llround(totalFrac * 100.0));
    const int trackPct = static_cast<int>(std::llround(clampedTrack * 100.0));
    const double etaSec = (totalFrac > 0.01) ? (elapsedSec * (1.0 - totalFrac) / totalFrac) : 0.0;

    constexpr int kBarW = 20;
    const int filled = std::clamp(static_cast<int>(std::llround(clampedTrack * kBarW)), 0, kBarW);
    std::string bar(static_cast<size_t>(filled), '#');
    bar.append(static_cast<size_t>(kBarW - filled), '-');

    std::string shortName = filename;
    if (shortName.size() > 24) shortName = shortName.substr(0, 21) + "...";
    std::string shortStage = stage;
    if (shortStage.size() > 28) shortStage = shortStage.substr(0, 25) + "...";

    std::ostringstream oss;
    oss << "[" << (idx + 1) << "/" << total << " | Total " << std::setw(3) << totalPct << "%] "
        << shortName << " [" << bar << "] " << std::setw(3) << trackPct << "% | "
        << shortStage << " | Elapsed " << formatMmSs(elapsedSec)
        << " | ETA " << (totalFrac > 0.01 ? formatMmSs(etaSec) : "--:--");
    std::string line = oss.str();
    if (isStdoutTty()) {
        if (line.size() < 118) line.append(118 - line.size(), ' ');
        std::cout << "\r" << line << std::flush;
    } else {
        std::cout << line << "\n" << std::flush;
    }
}

void clearProgressLine() {
    if (isStdoutTty()) {
        std::cout << "\r" << std::string(120, ' ') << "\r" << std::flush;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::signal(SIGINT, handleSigInt);

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
    fs::path cacheDir = ".deconstruct_cache";
    bool useCache = true;
    std::optional<bool> mergeExistingOpt;

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
            else if (m == "auto-genre") explicitMode = BatchMode::AutoGenre;
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
        } else if (flag == "--merge-existing") {
            mergeExistingOpt = true;
        } else if (flag == "--no-merge") {
            mergeExistingOpt = false;
        } else if (flag == "--cache-dir") {
            cacheDir = needVal("--cache-dir");
        } else if (flag == "--no-cache") {
            useCache = false;
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
                return !g_stopRequested.load(std::memory_order_acquire);
            },
            dlErr);
        if (!ok) {
            std::cerr << "Model download failed: " << dlErr << "\n";
            return 1;
        }
        std::cout << "Model ready: " << modelManager.getActiveModelFile().getFullPathName().toStdString() << "\n";
    }

    const BatchMode mode = explicitMode.value_or(outDir.has_value() ? BatchMode::PerFolder : BatchMode::Corpus);
    const bool mergeExisting = mergeExistingOpt.value_or(mode == BatchMode::AutoGenre || mode == BatchMode::PerFolder);

    struct JobGroup {
        std::string id;
        std::string name;
        std::string genre;
        bool explicit_genre = false;
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

    auto finalizeSingleSongProfile = [&](const fs::path& destJson,
                                         const SongInput& song,
                                         const std::vector<std::string>& fingerprints,
                                         AnalysisOptions aopts,
                                         bool allowMerge) {
        const std::string primaryFp = fingerprints.empty() ? std::string() : fingerprints.front();
        StyleProfile incoming = MidiStyleAnalyzer::analyze(song.notes, song.bpm, aopts);
        incoming.track_count = 1;
        if (!primaryFp.empty()) incoming.processed_tracks.push_back(primaryFp);

        if (incoming.genre.empty() || incoming.genre == "Analyzed" || incoming.genre == "Deconstructed" ||
            incoming.genre == "Deconstructed Song") {
            incoming.genre = AudioMetadataReader::inferGenreFromProfileHeuristic(incoming);
            if (incoming.genre_source.empty() || incoming.genre_source == "none") {
                incoming.genre_source = "heuristic";
            }
            std::string low = incoming.genre;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (std::find(incoming.tags.begin(), incoming.tags.end(), low) == incoming.tags.end()) {
                incoming.tags.push_back(low);
            }
        }
        if (clearanceOverride.has_value()) {
            incoming.voicing.sub_bass_clearance_midi = std::clamp(
                *clearanceOverride, 36, std::min(60, incoming.voicing.register_high - 6));
            incoming.voicing.register_low = std::max(incoming.voicing.register_low, incoming.voicing.sub_bass_clearance_midi);
        }

        StyleProfile finalProf = incoming;
        if (allowMerge && fs::exists(destJson)) {
            if (auto ex = loadFromFile(destJson)) {
                for (const auto& fp : fingerprints) {
                    if (!fp.empty() &&
                        std::find(ex->processed_tracks.begin(), ex->processed_tracks.end(), fp) !=
                            ex->processed_tracks.end()) {
                        return true;
                    }
                }
                finalProf = MidiStyleAnalyzer::mergeProfiles(*ex, incoming);
            }
        }

        if (!descArg.empty()) {
            finalProf.description = descArg;
        } else {
            finalProf.description = "Offline deconstructed " + finalProf.genre + " from " +
                                    std::to_string(std::max(1, finalProf.track_count)) + " track(s) via " + aopts.source;
            if (!finalProf.genre_source.empty()) {
                finalProf.description += " [genre: " + finalProf.genre_source + "]";
            }
        }
        normalizeProfileDistributions(finalProf);

        if (auto saveRes = saveToFile(finalProf, destJson); !saveRes) {
            std::cerr << "  Error writing " << destJson.string() << ": " << saveRes.error() << "\n";
            return false;
        }
        return true;
    };

    TranscriptionService txService(modelManager);
    TranscriptionService::DeconstructOptions dopts;
    dopts.max_seconds = maxSeconds;
    dopts.n_threads = threads;
    dopts.bpm_hint = bpmHint;
    dopts.force_spectral = forceSpectral;

    auto processOneTrack = [&](size_t idx,
                               size_t total,
                               const fs::path& f,
                               const std::chrono::steady_clock::time_point& batchStart,
                               const std::vector<std::string>& fingerprints,
                               bool& outFromCache,
                               double& outTrackSec) -> TranscriptionService::DeconstructedSong {
        outFromCache = false;
        const auto t0 = std::chrono::steady_clock::now();
        TranscriptionService::DeconstructedSong ds;
        const std::string primaryFp = fingerprints.empty() ? std::string() : fingerprints.front();

        if (useCache) {
            for (const auto& fp : fingerprints) {
                const fs::path cacheFile = cacheDir / (fp + ".json");
                if (loadCachedSong(cacheFile, ds)) {
                    outFromCache = true;
                    outTrackSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    return ds;
                }
            }
        }

        int lastPct = -4;
        const bool tty = isStdoutTty();
        const int minPctStep = tty ? 1 : 3;
        auto lastPrint = std::chrono::steady_clock::now() - std::chrono::milliseconds(200);
        ds = txService.deconstructFileBlocking(
            juce::File(f.string()), dopts,
            [&](float trackProg, std::string stage) -> bool {
                if (g_stopRequested.load(std::memory_order_acquire)) {
                    txService.cancel();
                    return false;
                }
                const int pct = static_cast<int>(std::llround(trackProg * 100.0f));
                const auto now = std::chrono::steady_clock::now();
                const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count();
                if (std::abs(pct - lastPct) >= minPctStep || (tty && dtMs >= 65)) {
                    lastPct = pct;
                    lastPrint = now;
                    const double elapsed = std::chrono::duration<double>(now - batchStart).count();
                    renderProgressLine(idx, total, f.filename().string(), trackProg, stage, elapsed);
                }
                return true;
            });

        outTrackSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (ds.success && useCache && !primaryFp.empty()) {
            saveCachedSong(cacheDir / (primaryFp + ".json"), primaryFp, ds);
        }
        return ds;
    };

    int succeeded = 0;
    int completedTracks = 0;

    if (mode == BatchMode::AutoGenre) {
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
        if (allFiles.empty()) {
            std::cerr << "No supported audio/MIDI files found in inputs.\n";
            return 1;
        }

        std::map<std::string, bool> initializedInRun;
        const auto batchStart = std::chrono::steady_clock::now();
        std::cout << "=== Auto-Genre Deconstruction (" << allFiles.size() << " track(s), merge="
                  << (mergeExisting ? "ON" : "OFF") << ") ===\n";

        for (size_t idx = 0; idx < allFiles.size(); ++idx) {
            if (g_stopRequested.load(std::memory_order_acquire)) break;
            const auto& f = allFiles[idx];
            const auto fps = buildTrackFingerprints(f, maxSeconds, forceSpectral);

            // Fast deduplication check across existing target JSONs in destRoot when merging
            if (mergeExisting && fs::exists(destRoot)) {
                bool alreadyPresent = false;
                for (const auto& e : fs::directory_iterator(destRoot)) {
                    if (e.is_regular_file() && e.path().extension() == ".json" &&
                        e.path().filename() != "catalog.json" &&
                        profileContainsAnyTrack(e.path(), fps)) {
                        alreadyPresent = true;
                        break;
                    }
                }
                if (alreadyPresent) {
                    std::cout << "  ✓ [" << (idx + 1) << "/" << allFiles.size() << "] "
                              << f.filename().string() << " | [SKIP - already in profile]\n";
                    ++completedTracks;
                    continue;
                }
            }

            bool fromCache = false;
            double trackSec = 0.0;
            const auto ds = processOneTrack(idx, allFiles.size(), f, batchStart, fps, fromCache, trackSec);
            clearProgressLine();

            if (g_stopRequested.load(std::memory_order_acquire) && !ds.success) {
                break;
            }
            if (!ds.success) {
                std::cout << "  - [" << (idx + 1) << "/" << allFiles.size() << "] "
                          << f.filename().string() << " | SKIPPED (" << ds.error << ")\n";
                continue;
            }

            if (saveMidiDir.has_value()) {
                const fs::path midOut = *saveMidiDir / (sanitizeSlug(f.stem().string()) + ".mid");
                (void)SmfWriter::writeType1(midOut, ds.song_input.bpm,
                                            ds.song_input.time_sig_num, ds.song_input.time_sig_den,
                                            ds.song_input.notes);
            }

            std::string detectedGenre = ds.metadata.canonical_genre;
            std::string detectedSource = ds.metadata.genre_source;
            if (!genreArg.empty() && detectedGenre.empty()) {
                detectedGenre = AudioMetadataReader::canonicalizeGenre(genreArg);
                detectedSource = "cli_fallback";
            }
            AnalysisOptions tmpOpts;
            tmpOpts.bpm_hint = ds.song_input.bpm;
            tmpOpts.key_override_pc = keyOverride;
            tmpOpts.mode_override = modeOverride;
            const auto singleProf = MidiStyleAnalyzer::analyze(ds.song_input.notes, ds.song_input.bpm, tmpOpts);
            if (detectedGenre.empty()) {
                detectedGenre = AudioMetadataReader::inferGenreFromProfileHeuristic(singleProf);
                detectedSource = "heuristic";
            }

            const std::string slug = AudioMetadataReader::sanitizeGenreSlug(detectedGenre);
            const fs::path destJson = destRoot / (slug + ".json");

            AnalysisOptions aopts;
            aopts.id = slug;
            aopts.name = detectedGenre;
            aopts.genre = detectedGenre;
            aopts.source = ds.backend_name.empty() ? "offline_deconstruct" : ("offline_" + ds.backend_name);
            aopts.tags = ds.metadata.tags;
            aopts.genre_source = detectedSource;
            aopts.bpm_hint = bpmHint;
            aopts.key_override_pc = keyOverride;
            aopts.mode_override = modeOverride;
            std::string canonLow = detectedGenre;
            std::transform(canonLow.begin(), canonLow.end(), canonLow.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (std::find(aopts.tags.begin(), aopts.tags.end(), canonLow) == aopts.tags.end()) {
                aopts.tags.push_back(canonLow);
            }

            const bool allowMerge = mergeExisting || initializedInRun[slug];
            if (finalizeSingleSongProfile(destJson, ds.song_input, fps, std::move(aopts), allowMerge)) {
                initializedInRun[slug] = true;
                ++succeeded;
                ++completedTracks;
                if (catalogPath.has_value()) {
                    (void)writeCatalogFromDir(destRoot, *catalogPath);
                }
            }

            std::cout << "  ✓ [" << (idx + 1) << "/" << allFiles.size() << "] "
                      << f.filename().string() << " | " << ds.backend_name
                      << (fromCache ? " [CACHED]" : "")
                      << " | " << ds.song_input.notes.size() << " notes"
                      << " | " << std::fixed << std::setprecision(1) << ds.song_input.bpm << " BPM"
                      << " | " << pitchClassName(singleProf.harmony.tonic_pc) << " " << modeName(singleProf.harmony.mode)
                      << " | [" << detectedGenre << " via " << detectedSource << "]"
                      << " | Sections: Intro/Chorus/Outro (" << std::setprecision(1) << trackSec << "s)\n";
        }

        if (g_stopRequested.load(std::memory_order_acquire)) {
            std::cout << "Interrupted (Ctrl+C): flushed " << completedTracks
                      << " completed track(s). Re-run the same command to resume remaining tracks from cache.\n";
            return 0;
        }
        if (succeeded == 0) {
            if (completedTracks > 0) {
                if (catalogPath.has_value()) {
                    (void)writeCatalogFromDir(destRoot, *catalogPath);
                }
                std::cout << "All " << completedTracks << " track(s) already processed.\n";
                return 0;
            }
            std::cerr << "No profiles were successfully created.\n";
            return 1;
        }
        if (catalogPath.has_value()) {
            if (writeCatalogFromDir(destRoot, *catalogPath)) {
                std::cout << "Updated catalog: " << catalogPath->string() << "\n";
            } else {
                std::cerr << "Failed writing catalog: " << catalogPath->string() << "\n";
                return 1;
            }
        }
        return 0;
    }

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
        g.genre = !genreArg.empty() ? AudioMetadataReader::canonicalizeGenre(genreArg) : "";
        g.explicit_genre = !genreArg.empty();
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
                            g.genre = !genreArg.empty() ? AudioMetadataReader::canonicalizeGenre(genreArg) : "";
                            g.explicit_genre = !genreArg.empty();
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
                        g.genre = !genreArg.empty() ? AudioMetadataReader::canonicalizeGenre(genreArg) : "";
                        g.explicit_genre = !genreArg.empty();
                        g.destJson = destRoot / (g.id + ".json");
                        g.files = std::move(files);
                        jobs.push_back(std::move(g));
                    }
                }
            } else if (fs::is_regular_file(in) && isSupportedMediaFile(in)) {
                JobGroup g;
                g.id = sanitizeSlug(in.stem().string());
                g.name = prettifyTitle(g.id);
                g.genre = !genreArg.empty() ? AudioMetadataReader::canonicalizeGenre(genreArg) : "";
                g.explicit_genre = !genreArg.empty();
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
            g.genre = !genreArg.empty() ? AudioMetadataReader::canonicalizeGenre(genreArg) : "";
            g.explicit_genre = !genreArg.empty();
            g.destJson = destRoot / (g.id + ".json");
            g.files = {f};
            jobs.push_back(std::move(g));
        }
    }

    if (jobs.empty()) {
        std::cerr << "No supported audio/MIDI files found in inputs.\n";
        return 1;
    }

    const auto batchStart = std::chrono::steady_clock::now();
    for (const auto& job : jobs) {
        if (g_stopRequested.load(std::memory_order_acquire)) break;
        std::cout << "=== Deconstructing [" << job.id << "] (" << job.files.size() << " file(s)) ===\n";
        bool jobInitialized = false;

        for (size_t idx = 0; idx < job.files.size(); ++idx) {
            if (g_stopRequested.load(std::memory_order_acquire)) break;
            const auto& f = job.files[idx];
            const auto fps = buildTrackFingerprints(f, maxSeconds, forceSpectral);

            if (mergeExisting && profileContainsAnyTrack(job.destJson, fps)) {
                std::cout << "  ✓ [" << (idx + 1) << "/" << job.files.size() << "] "
                          << f.filename().string() << " | [SKIP - already in profile]\n";
                ++completedTracks;
                continue;
            }

            bool fromCache = false;
            double trackSec = 0.0;
            const auto ds = processOneTrack(idx, job.files.size(), f, batchStart, fps, fromCache, trackSec);
            clearProgressLine();

            if (g_stopRequested.load(std::memory_order_acquire) && !ds.success) break;
            if (!ds.success) {
                std::cout << "  - [" << (idx + 1) << "/" << job.files.size() << "] "
                          << f.filename().string() << " | SKIPPED (" << ds.error << ")\n";
                continue;
            }

            if (saveMidiDir.has_value()) {
                const fs::path midOut = *saveMidiDir / (sanitizeSlug(f.stem().string()) + ".mid");
                (void)SmfWriter::writeType1(midOut, ds.song_input.bpm,
                                            ds.song_input.time_sig_num, ds.song_input.time_sig_den,
                                            ds.song_input.notes);
            }

            AnalysisOptions aopts;
            aopts.id = job.id;
            aopts.name = (job.files.size() == 1 && !ds.metadata.title.empty() && nameArg.empty())
                             ? ds.metadata.title
                             : job.name;
            aopts.genre = job.explicit_genre ? job.genre
                                             : (ds.metadata.hasGenre() ? ds.metadata.canonical_genre : "");
            aopts.genre_source = job.explicit_genre ? "cli"
                                                    : (ds.metadata.hasGenre() ? ds.metadata.genre_source : "");
            aopts.source = ds.backend_name.empty() ? "offline_deconstruct" : ("offline_" + ds.backend_name);
            aopts.tags = ds.metadata.tags;
            if (job.files.size() == 1) {
                aopts.artist = ds.metadata.artist;
                aopts.album = ds.metadata.album;
            }
            aopts.bpm_hint = bpmHint;
            aopts.key_override_pc = keyOverride;
            aopts.mode_override = modeOverride;

            const bool allowMerge = mergeExisting || jobInitialized;
            if (finalizeSingleSongProfile(job.destJson, ds.song_input, fps, std::move(aopts), allowMerge)) {
                jobInitialized = true;
                ++succeeded;
                ++completedTracks;
                if (catalogPath.has_value()) {
                    const fs::path scanDir = outDir.value_or(job.destJson.has_parent_path()
                                                                 ? job.destJson.parent_path()
                                                                 : fs::path("."));
                    (void)writeCatalogFromDir(scanDir, *catalogPath);
                }
            }

            std::cout << "  ✓ [" << (idx + 1) << "/" << job.files.size() << "] "
                      << f.filename().string() << " | " << ds.backend_name
                      << (fromCache ? " [CACHED]" : "")
                      << " | " << ds.song_input.notes.size() << " notes"
                      << " | " << std::fixed << std::setprecision(1) << ds.song_input.bpm << " BPM"
                      << (ds.metadata.hasGenre()
                              ? (" | [" + ds.metadata.canonical_genre + " via " + ds.metadata.genre_source + "]")
                              : "")
                      << " | Sections: Intro/Chorus/Outro (" << std::setprecision(1) << trackSec << "s)\n";
        }
    }

    if (g_stopRequested.load(std::memory_order_acquire)) {
        std::cout << "Interrupted (Ctrl+C): flushed " << completedTracks
                  << " completed track(s). Re-run the same command to resume remaining tracks from cache.\n";
        return 0;
    }

    if (succeeded == 0) {
        if (completedTracks > 0) {
            if (catalogPath.has_value()) {
                const fs::path scanDir = outDir.value_or(jobs.front().destJson.has_parent_path()
                                                             ? jobs.front().destJson.parent_path()
                                                             : fs::path("."));
                (void)writeCatalogFromDir(scanDir, *catalogPath);
            }
            std::cout << "All " << completedTracks << " track(s) already processed.\n";
            return 0;
        }
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
