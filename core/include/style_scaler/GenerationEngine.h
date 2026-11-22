#pragma once

#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace style_scaler {

struct ProgressionSlot {
    int bar_index = 0;
    double start_beat = 0.0;
    double duration_beats = 4.0;
    std::string roman_symbol = "i";
    RomanChord chord;
    bool locked = false;
    int octave_shift = 0;           // -2..+2
    int inversion_override = -1;    // -1=auto, 0=root, 1=1st, 2=2nd, 3=drop2
    double richness_override = -1.0;// <0=auto, 0..1 overrides extension richness

    bool operator==(const ProgressionSlot&) const = default;
};

struct PerformParams {
    bool groove_sync = true;
    TempoScale tempo_scale = TempoScale::Normal;
    double voicing_spread = 0.5;   // 0..1 UI spread multiplier
    double melody_density = 0.5;   // 0..1 UI melody density control
    double humanize = 0.15;        // 0..1 timing & velocity humanization
    bool enable_chords = true;
    bool enable_melody = true;
    bool enable_bass = true;
};

struct GeneratedPattern {
    double loop_length_beats = 16.0;
    std::vector<ProgressionSlot> progression;
    std::vector<NoteEvent> chords;
    std::vector<NoteEvent> melody;
    std::vector<NoteEvent> bass;
    std::vector<NoteEvent> all_events;
};

class DeterministicRng {
public:
    explicit DeterministicRng(uint64_t seed = 0x5EED1234ULL) noexcept;
    uint64_t nextU64() noexcept;
    double nextDouble() noexcept;                // [0.0, 1.0)
    double nextRange(double lo, double hi) noexcept;
    int nextInt(int loInclusive, int hiInclusive) noexcept;

private:
    uint64_t s_[4]{};
};

class ProgressionGenerator {
public:
    static std::vector<ProgressionSlot> generateProgression(
        const StyleProfile& profile,
        int numBars,
        uint64_t seed,
        const std::vector<ProgressionSlot>& existingSlots = {},
        TempoScale tempoScale = TempoScale::Normal);

    static std::vector<std::pair<std::string, double>> nextChordProbabilities(
        const StyleProfile& profile,
        const std::string& currentSymbol);

    static std::vector<ChordVocabEntry> suggestSubstitutes(
        const StyleProfile& profile,
        const ProgressionSlot& slot,
        size_t maxResults = 8);
};

class VoicingEngine {
public:
    static std::vector<int> voiceChord(
        const RomanChord& chord,
        int tonicPc,
        Mode mode,
        const VoicingAxis& voicing,
        const std::vector<int>& prevVoicing = {},
        double spreadOverride = -1.0,
        int octaveShift = 0,
        int inversionOverride = -1,
        double richnessOverride = -1.0);
};

class PatternGenerator {
public:
    static GeneratedPattern generatePattern(
        const StyleProfile& profile,
        const std::vector<ProgressionSlot>& progression,
        const PerformParams& perform,
        uint64_t seed);
};

} // namespace style_scaler
