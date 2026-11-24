#pragma once

#include "style_scaler/Types.h"

#include <array>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace style_scaler {

struct ChordVocabEntry {
    std::string symbol;
    RomanChord chord;
    double weight = 1.0;
    double cadence_weight = 0.0;

    bool operator==(const ChordVocabEntry&) const = default;
};

struct HarmonyAxis {
    int tonic_pc = 0;
    Mode mode = Mode::Minor;
    std::vector<ChordVocabEntry> vocabulary;
    std::map<std::string, std::map<std::string, double>> transitions;
    double chromaticism = 0.15;
    std::vector<std::string> reference_progression;

    bool operator==(const HarmonyAxis&) const = default;
};

struct RhythmAxis {
    int subdivision = 16;
    std::array<double, 16> chord_onset_grid = {
        1.0, 0.0, 0.0, 0.25, 0.0, 0.0, 0.35, 0.0,
        0.5, 0.0, 0.0, 0.2,  0.0, 0.0, 0.3,  0.0
    };
    std::array<double, 16> melody_onset_grid = {
        0.7, 0.1, 0.4, 0.2, 0.6, 0.1, 0.45, 0.2,
        0.65, 0.15, 0.4, 0.25, 0.5, 0.1, 0.35, 0.15
    };
    std::array<double, 16> bass_onset_grid = {
        1.0, 0.0, 0.0, 0.35, 0.0, 0.0, 0.45, 0.0,
        0.25, 0.0, 0.55, 0.0, 0.0, 0.2, 0.3, 0.0
    };
    std::array<double, 16> chord_pattern = {};
    std::array<double, 16> bass_pattern = {};
    std::array<double, 16> melody_pattern = {};
    double syncopation = 0.35;
    double anticipation = 0.2;
    double swing = 0.0;
    double harmonic_pacing = 1.0; // chord changes per bar
    double sustain_ratio = 0.85;

    bool operator==(const RhythmAxis&) const = default;
};

struct VoicingAxis {
    int register_low = 48;
    int register_high = 76;
    int center_pitch = 62;
    double spread_semitones = 14.0;
    int min_notes = 3;
    int max_notes = 5;
    double extension_richness = 0.6;
    double drop2_prob = 0.25;
    double inversion_prob = 0.4;
    double voice_leading_tightness = 0.8;
    int sub_bass_clearance_midi = 48;
    int bass_register_low = 28;
    int bass_register_high = 45;

    bool operator==(const VoicingAxis&) const = default;
};

struct MelodyMotifStep {
    int step16 = 0;               // 0..31 (2-bar 16th-note position)
    int scale_degree_offset = 0;  // diatonic steps relative to phrase anchor
    double duration_steps = 2.0;  // duration in 16th notes
    bool is_chord_tone = true;

    bool operator==(const MelodyMotifStep&) const = default;
};

struct MelodyAxis {
    // Signed semitone steps -12..+12, index = delta + 12
    std::array<double, 25> interval_histogram = {};
    double chord_tone_ratio = 0.72;
    double tension_tone_ratio = 0.18;
    double motif_density = 0.45;
    double motif_repetition = 0.6;
    double arch_contour = 0.5;
    int register_low = 64;
    int register_high = 86;
    double legato_ratio = 0.7;
    std::vector<MelodyMotifStep> reference_motif;

    bool operator==(const MelodyAxis&) const = default;
};

struct SectionDNA {
    HarmonyAxis harmony;
    RhythmAxis rhythm;
    VoicingAxis voicing;
    MelodyAxis melody;
    bool populated = false;

    bool operator==(const SectionDNA&) const = default;
};

struct StyleProfile {
    static constexpr int kCurrentSchemaVersion = 1;

    int schema_version = kCurrentSchemaVersion;
    std::string id = "default";
    std::string name = "Default Minor";
    std::string genre = "Electronic";
    std::string description;

    double bpm_min = 85.0;
    double bpm_max = 175.0;
    double default_bpm = 120.0;
    int time_sig_num = 4;
    int time_sig_den = 4;
    std::string source = "preset";

    int track_count = 1;
    std::vector<std::string> processed_tracks;

    // Optional song/preset metadata (schema_version remains 1)
    std::vector<std::string> tags;
    std::string genre_source;
    std::string artist;
    std::string album;

    HarmonyAxis harmony;
    RhythmAxis rhythm;
    VoicingAxis voicing;
    MelodyAxis melody;

    std::map<std::string, SectionDNA> sections;

    bool operator==(const StyleProfile&) const = default;
};

struct ValidationResult {
    bool valid = true;
    std::vector<std::string> errors;
};

StyleProfile makeDefaultStyleProfile();

ValidationResult validate(const StyleProfile& profile);

// Normalizes transition rows, vocabulary weights, and melody interval histogram in place.
void normalizeProfileDistributions(StyleProfile& profile);

SectionDNA deriveSectionDNA(const StyleProfile& profile, SongSection section);
SectionDNA resolveSectionDNA(const StyleProfile& profile, SongSection section);

nlohmann::json toJson(const StyleProfile& profile);
std::expected<StyleProfile, std::string> fromJson(const nlohmann::json& j);
std::expected<StyleProfile, std::string> loadFromJsonString(std::string_view jsonText);
std::expected<StyleProfile, std::string> loadFromFile(const std::filesystem::path& path);
std::expected<void, std::string> saveToFile(const StyleProfile& profile, const std::filesystem::path& path);

} // namespace style_scaler
