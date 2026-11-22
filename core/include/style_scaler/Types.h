#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace style_scaler {

enum class Mode {
    Minor = 0,
    Major,
    Dorian,
    Phrygian,
    Mixolydian,
    HarmonicMinor,
    Lydian,
    Locrian
};

enum class ChordQuality {
    Maj = 0,
    Min,
    Dim,
    Aug,
    Sus2,
    Sus4,
    Maj7,
    Min7,
    Dom7,
    HDim7,
    Dim7,
    MinMaj7,
    Maj9,
    Min9,
    Dom9,
    Min11,
    Add9,
    MinAdd9,
    Min6,
    Maj6,
    Power5
};

enum class TempoScale {
    Half = 0,
    Normal,
    Double
};

constexpr double tempoScaleFactor(TempoScale scale) noexcept {
    switch (scale) {
        case TempoScale::Half: return 0.5;
        case TempoScale::Normal: return 1.0;
        case TempoScale::Double: return 2.0;
    }
    return 1.0;
}

inline std::string_view tempoScaleName(TempoScale scale) noexcept {
    switch (scale) {
        case TempoScale::Half: return "0.5x";
        case TempoScale::Normal: return "1x";
        case TempoScale::Double: return "2x";
    }
    return "1x";
}

std::optional<TempoScale> parseTempoScale(std::string_view text);

enum class SongSection {
    Intro = 0,
    Chorus = 1,
    Outro = 2
};

inline std::string_view songSectionName(SongSection s) noexcept {
    switch (s) {
        case SongSection::Intro:  return "intro";
        case SongSection::Chorus: return "chorus";
        case SongSection::Outro:  return "outro";
    }
    return "chorus";
}

std::optional<SongSection> parseSongSection(std::string_view text);

struct RomanChord {
    int degree = 1;               // 1..7 relative to tonic major reference
    int accidental = 0;           // -1 = b, 0 = natural, +1 = #
    ChordQuality quality = ChordQuality::Min;
    int bass_degree_offset = 0;   // semitone offset from chord root (0 = root position)

    bool operator==(const RomanChord&) const = default;
};

struct NoteEvent {
    double onset_beats = 0.0;
    double duration_beats = 0.25;
    int pitch = 60;      // 0..127
    int velocity = 100;  // 1..127
    int channel = 1;     // 1=Chords, 2=Melody, 3=Bass
    bool is_drum = false;
    int program = 0;
};

struct TimedNoteSec {
    double onset_sec = 0.0;
    double offset_sec = 0.25;
    int pitch = 60;
    int velocity = 100;
    int program = 0;
    bool is_drum = false;
};

constexpr int normalizePitchClass(int pc) noexcept {
    const int mod = pc % 12;
    return mod < 0 ? mod + 12 : mod;
}

std::array<int, 7> scaleIntervals(Mode mode) noexcept;
std::string_view modeName(Mode mode) noexcept;
std::optional<Mode> parseMode(std::string_view text);

std::vector<int> chordIntervals(ChordQuality quality);
std::string_view qualityName(ChordQuality quality) noexcept;
std::optional<ChordQuality> parseQuality(std::string_view text);
bool isMinorLikeQuality(ChordQuality quality) noexcept;

std::string_view pitchClassName(int pc, bool preferFlats = true) noexcept;
std::optional<int> parsePitchClass(std::string_view text);

std::string formatRomanChord(const RomanChord& chord);
std::optional<RomanChord> parseRomanChord(std::string_view symbol);

int romanChordRootPitchClass(const RomanChord& chord, int tonicPc) noexcept;
std::vector<int> chordPitchClasses(const RomanChord& chord, int tonicPc, Mode mode = Mode::Minor);
std::string formatChordName(int rootPc, ChordQuality quality, bool preferFlats = true);
std::string formatRomanAsConcreteChord(const RomanChord& chord, int tonicPc, Mode mode = Mode::Minor);

// Builds a diatonic RomanChord on scale degree 1..7 for the given Mode.
RomanChord diatonicChordForMode(Mode mode, int degree, bool seventh = false);

} // namespace style_scaler
