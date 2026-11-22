#include "style_scaler/Types.h"

#include <algorithm>
#include <cctype>

namespace style_scaler {

namespace {

std::string toLowerCopy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trimCopy(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

constexpr std::array<int, 7> kMajorScaleRef = {0, 2, 4, 5, 7, 9, 11};

} // namespace

std::optional<TempoScale> parseTempoScale(std::string_view text) {
    const auto t = toLowerCopy(trimCopy(text));
    if (t == "0.5x" || t == "0.5" || t == "half") return TempoScale::Half;
    if (t == "1x" || t == "1" || t == "1.0" || t == "normal") return TempoScale::Normal;
    if (t == "2x" || t == "2" || t == "2.0" || t == "double") return TempoScale::Double;
    return std::nullopt;
}

std::optional<SongSection> parseSongSection(std::string_view text) {
    const auto t = toLowerCopy(trimCopy(text));
    if (t == "intro" || t == "0") return SongSection::Intro;
    if (t == "chorus" || t == "drop" || t == "main" || t == "1") return SongSection::Chorus;
    if (t == "outro" || t == "ending" || t == "2") return SongSection::Outro;
    return std::nullopt;
}

std::array<int, 7> scaleIntervals(Mode mode) noexcept {
    switch (mode) {
        case Mode::Minor:         return {0, 2, 3, 5, 7, 8, 10};
        case Mode::Major:         return {0, 2, 4, 5, 7, 9, 11};
        case Mode::Dorian:        return {0, 2, 3, 5, 7, 9, 10};
        case Mode::Phrygian:      return {0, 1, 3, 5, 7, 8, 10};
        case Mode::Mixolydian:    return {0, 2, 4, 5, 7, 9, 10};
        case Mode::HarmonicMinor: return {0, 2, 3, 5, 7, 8, 11};
        case Mode::Lydian:        return {0, 2, 4, 6, 7, 9, 11};
        case Mode::Locrian:       return {0, 1, 3, 5, 6, 8, 10};
    }
    return {0, 2, 3, 5, 7, 8, 10};
}

std::string_view modeName(Mode mode) noexcept {
    switch (mode) {
        case Mode::Minor:         return "minor";
        case Mode::Major:         return "major";
        case Mode::Dorian:        return "dorian";
        case Mode::Phrygian:      return "phrygian";
        case Mode::Mixolydian:    return "mixolydian";
        case Mode::HarmonicMinor: return "harmonic_minor";
        case Mode::Lydian:        return "lydian";
        case Mode::Locrian:       return "locrian";
    }
    return "minor";
}

std::optional<Mode> parseMode(std::string_view text) {
    const auto t = toLowerCopy(trimCopy(text));
    if (t == "minor" || t == "aeolian" || t == "nat_minor" || t == "natural_minor" || t == "min")
        return Mode::Minor;
    if (t == "major" || t == "ionian" || t == "maj")
        return Mode::Major;
    if (t == "dorian")
        return Mode::Dorian;
    if (t == "phrygian")
        return Mode::Phrygian;
    if (t == "mixolydian")
        return Mode::Mixolydian;
    if (t == "harmonic_minor" || t == "harmonicminor" || t == "harm_minor")
        return Mode::HarmonicMinor;
    if (t == "lydian")
        return Mode::Lydian;
    if (t == "locrian")
        return Mode::Locrian;
    return std::nullopt;
}

std::vector<int> chordIntervals(ChordQuality quality) {
    switch (quality) {
        case ChordQuality::Maj:     return {0, 4, 7};
        case ChordQuality::Min:     return {0, 3, 7};
        case ChordQuality::Dim:     return {0, 3, 6};
        case ChordQuality::Aug:     return {0, 4, 8};
        case ChordQuality::Sus2:    return {0, 2, 7};
        case ChordQuality::Sus4:    return {0, 5, 7};
        case ChordQuality::Maj7:    return {0, 4, 7, 11};
        case ChordQuality::Min7:    return {0, 3, 7, 10};
        case ChordQuality::Dom7:    return {0, 4, 7, 10};
        case ChordQuality::HDim7:   return {0, 3, 6, 10};
        case ChordQuality::Dim7:    return {0, 3, 6, 9};
        case ChordQuality::MinMaj7: return {0, 3, 7, 11};
        case ChordQuality::Maj9:    return {0, 4, 7, 11, 14};
        case ChordQuality::Min9:    return {0, 3, 7, 10, 14};
        case ChordQuality::Dom9:    return {0, 4, 7, 10, 14};
        case ChordQuality::Min11:   return {0, 3, 7, 10, 14, 17};
        case ChordQuality::Add9:    return {0, 4, 7, 14};
        case ChordQuality::MinAdd9: return {0, 3, 7, 14};
        case ChordQuality::Min6:    return {0, 3, 7, 9};
        case ChordQuality::Maj6:    return {0, 4, 7, 9};
        case ChordQuality::Power5:  return {0, 7};
    }
    return {0, 3, 7};
}

std::string_view qualityName(ChordQuality quality) noexcept {
    switch (quality) {
        case ChordQuality::Maj:     return "maj";
        case ChordQuality::Min:     return "min";
        case ChordQuality::Dim:     return "dim";
        case ChordQuality::Aug:     return "aug";
        case ChordQuality::Sus2:    return "sus2";
        case ChordQuality::Sus4:    return "sus4";
        case ChordQuality::Maj7:    return "maj7";
        case ChordQuality::Min7:    return "min7";
        case ChordQuality::Dom7:    return "dom7";
        case ChordQuality::HDim7:   return "hdim7";
        case ChordQuality::Dim7:    return "dim7";
        case ChordQuality::MinMaj7: return "minmaj7";
        case ChordQuality::Maj9:    return "maj9";
        case ChordQuality::Min9:    return "min9";
        case ChordQuality::Dom9:    return "dom9";
        case ChordQuality::Min11:   return "min11";
        case ChordQuality::Add9:    return "add9";
        case ChordQuality::MinAdd9: return "minadd9";
        case ChordQuality::Min6:    return "min6";
        case ChordQuality::Maj6:    return "maj6";
        case ChordQuality::Power5:  return "power5";
    }
    return "min";
}

std::optional<ChordQuality> parseQuality(std::string_view text) {
    const auto t = toLowerCopy(trimCopy(text));
    if (t == "maj" || t == "major" || t == "") return ChordQuality::Maj;
    if (t == "min" || t == "minor" || t == "m") return ChordQuality::Min;
    if (t == "dim" || t == "o") return ChordQuality::Dim;
    if (t == "aug" || t == "+") return ChordQuality::Aug;
    if (t == "sus2") return ChordQuality::Sus2;
    if (t == "sus4" || t == "sus") return ChordQuality::Sus4;
    if (t == "maj7" || t == "j7") return ChordQuality::Maj7;
    if (t == "min7" || t == "m7") return ChordQuality::Min7;
    if (t == "dom7" || t == "7") return ChordQuality::Dom7;
    if (t == "hdim7" || t == "m7b5") return ChordQuality::HDim7;
    if (t == "dim7" || t == "o7") return ChordQuality::Dim7;
    if (t == "minmaj7" || t == "mmaj7") return ChordQuality::MinMaj7;
    if (t == "maj9") return ChordQuality::Maj9;
    if (t == "min9" || t == "m9") return ChordQuality::Min9;
    if (t == "dom9" || t == "9") return ChordQuality::Dom9;
    if (t == "min11" || t == "m11" || t == "11") return ChordQuality::Min11;
    if (t == "add9") return ChordQuality::Add9;
    if (t == "minadd9" || t == "madd9") return ChordQuality::MinAdd9;
    if (t == "min6" || t == "m6") return ChordQuality::Min6;
    if (t == "maj6" || t == "6") return ChordQuality::Maj6;
    if (t == "power5" || t == "5") return ChordQuality::Power5;
    return std::nullopt;
}

bool isMinorLikeQuality(ChordQuality quality) noexcept {
    switch (quality) {
        case ChordQuality::Min:
        case ChordQuality::Dim:
        case ChordQuality::Min7:
        case ChordQuality::HDim7:
        case ChordQuality::Dim7:
        case ChordQuality::MinMaj7:
        case ChordQuality::Min9:
        case ChordQuality::Min11:
        case ChordQuality::MinAdd9:
        case ChordQuality::Min6:
            return true;
        default:
            return false;
    }
}

std::string_view pitchClassName(int pc, bool preferFlats) noexcept {
    static constexpr std::array<std::string_view, 12> kFlats = {
        "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
    };
    static constexpr std::array<std::string_view, 12> kSharps = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const int n = normalizePitchClass(pc);
    return preferFlats ? kFlats[static_cast<size_t>(n)] : kSharps[static_cast<size_t>(n)];
}

std::optional<int> parsePitchClass(std::string_view text) {
    const auto t = trimCopy(text);
    if (t.empty()) return std::nullopt;

    int base = -1;
    switch (std::toupper(static_cast<unsigned char>(t[0]))) {
        case 'C': base = 0; break;
        case 'D': base = 2; break;
        case 'E': base = 4; break;
        case 'F': base = 5; break;
        case 'G': base = 7; break;
        case 'A': base = 9; break;
        case 'B': base = 11; break;
        default: return std::nullopt;
    }

    int acc = 0;
    for (size_t i = 1; i < t.size(); ++i) {
        if (t[i] == '#' || t[i] == '+') {
            ++acc;
        } else if (t[i] == 'b' || t[i] == '-') {
            --acc;
        } else {
            return std::nullopt;
        }
    }
    return normalizePitchClass(base + acc);
}

std::string formatRomanChord(const RomanChord& chord) {
    static constexpr std::array<std::string_view, 7> kUpper = {
        "I", "II", "III", "IV", "V", "VI", "VII"
    };
    static constexpr std::array<std::string_view, 7> kLower = {
        "i", "ii", "iii", "iv", "v", "vi", "vii"
    };

    const int degIdx = std::clamp(chord.degree, 1, 7) - 1;
    std::string out;
    if (chord.accidental < 0) {
        out.append(static_cast<size_t>(-chord.accidental), 'b');
    } else if (chord.accidental > 0) {
        out.append(static_cast<size_t>(chord.accidental), '#');
    }

    const bool lower = isMinorLikeQuality(chord.quality);
    out += lower ? kLower[static_cast<size_t>(degIdx)] : kUpper[static_cast<size_t>(degIdx)];

    switch (chord.quality) {
        case ChordQuality::Maj:     break;
        case ChordQuality::Min:     break;
        case ChordQuality::Dim:     out += "dim"; break;
        case ChordQuality::Aug:     out += "aug"; break;
        case ChordQuality::Sus2:    out += "sus2"; break;
        case ChordQuality::Sus4:    out += "sus4"; break;
        case ChordQuality::Maj7:    out += "maj7"; break;
        case ChordQuality::Min7:    out += "7"; break;
        case ChordQuality::Dom7:    out += "7"; break;
        case ChordQuality::HDim7:   out += "m7b5"; break;
        case ChordQuality::Dim7:    out += "dim7"; break;
        case ChordQuality::MinMaj7: out += "mMaj7"; break;
        case ChordQuality::Maj9:    out += "maj9"; break;
        case ChordQuality::Min9:    out += "9"; break;
        case ChordQuality::Dom9:    out += "9"; break;
        case ChordQuality::Min11:   out += "11"; break;
        case ChordQuality::Add9:    out += "add9"; break;
        case ChordQuality::MinAdd9: out += "add9"; break;
        case ChordQuality::Min6:    out += "6"; break;
        case ChordQuality::Maj6:    out += "6"; break;
        case ChordQuality::Power5:  out += "5"; break;
    }

    if (chord.bass_degree_offset != 0) {
        out += "/";
        out += std::to_string(normalizePitchClass(chord.bass_degree_offset));
    }
    return out;
}

std::optional<RomanChord> parseRomanChord(std::string_view symbol) {
    auto s = trimCopy(symbol);
    if (s.empty()) return std::nullopt;

    RomanChord result;

    // Optional slash bass offset (e.g. "/4" or "/7")
    if (const auto slashPos = s.find('/'); slashPos != std::string::npos) {
        const auto bassStr = s.substr(slashPos + 1);
        try {
            result.bass_degree_offset = normalizePitchClass(std::stoi(bassStr));
        } catch (...) {
            return std::nullopt;
        }
        s = s.substr(0, slashPos);
    }

    size_t pos = 0;
    while (pos < s.size() && (s[pos] == 'b' || s[pos] == '#')) {
        result.accidental += (s[pos] == 'b') ? -1 : 1;
        ++pos;
    }
    if (pos >= s.size()) return std::nullopt;

    // Parse Roman numeral (longest match first)
    struct NumMatch {
        const char* upper;
        const char* lower;
        int degree;
    };
    static constexpr std::array<NumMatch, 7> kMatches = {{
        {"VII", "vii", 7},
        {"III", "iii", 3},
        {"VI",  "vi",  6},
        {"IV",  "iv",  4},
        {"II",  "ii",  2},
        {"V",   "v",   5},
        {"I",   "i",   1}
    }};

    bool matched = false;
    bool isLower = false;
    const auto rest = std::string_view(s).substr(pos);
    for (const auto& m : kMatches) {
        const auto uLen = std::char_traits<char>::length(m.upper);
        if (rest.size() >= uLen && rest.substr(0, uLen) == m.upper) {
            result.degree = m.degree;
            isLower = false;
            pos += uLen;
            matched = true;
            break;
        }
        if (rest.size() >= uLen && rest.substr(0, uLen) == m.lower) {
            result.degree = m.degree;
            isLower = true;
            pos += uLen;
            matched = true;
            break;
        }
    }
    if (!matched) return std::nullopt;

    const auto suffixRaw = s.substr(pos);
    const auto suffix = toLowerCopy(suffixRaw);

    if (suffix.empty()) {
        result.quality = isLower ? ChordQuality::Min : ChordQuality::Maj;
    } else if (suffix == "dim" || suffix == "o") {
        result.quality = ChordQuality::Dim;
    } else if (suffix == "aug" || suffix == "+") {
        result.quality = ChordQuality::Aug;
    } else if (suffix == "sus2") {
        result.quality = ChordQuality::Sus2;
    } else if (suffix == "sus4" || suffix == "sus") {
        result.quality = ChordQuality::Sus4;
    } else if (suffix == "maj7" || suffix == "j7") {
        result.quality = ChordQuality::Maj7;
    } else if (suffix == "7") {
        result.quality = isLower ? ChordQuality::Min7 : ChordQuality::Dom7;
    } else if (suffix == "m7" || suffix == "min7") {
        result.quality = ChordQuality::Min7;
    } else if (suffix == "m7b5" || suffix == "hdim7") {
        result.quality = ChordQuality::HDim7;
    } else if (suffix == "dim7" || suffix == "o7") {
        result.quality = ChordQuality::Dim7;
    } else if (suffix == "mmaj7" || suffix == "minmaj7") {
        result.quality = ChordQuality::MinMaj7;
    } else if (suffix == "maj9") {
        result.quality = ChordQuality::Maj9;
    } else if (suffix == "9") {
        result.quality = isLower ? ChordQuality::Min9 : ChordQuality::Dom9;
    } else if (suffix == "m9" || suffix == "min9") {
        result.quality = ChordQuality::Min9;
    } else if (suffix == "11" || suffix == "m11" || suffix == "min11") {
        result.quality = ChordQuality::Min11;
    } else if (suffix == "add9") {
        result.quality = isLower ? ChordQuality::MinAdd9 : ChordQuality::Add9;
    } else if (suffix == "madd9" || suffix == "minadd9") {
        result.quality = ChordQuality::MinAdd9;
    } else if (suffix == "6") {
        result.quality = isLower ? ChordQuality::Min6 : ChordQuality::Maj6;
    } else if (suffix == "m6" || suffix == "min6") {
        result.quality = ChordQuality::Min6;
    } else if (suffix == "5") {
        result.quality = ChordQuality::Power5;
    } else {
        return std::nullopt;
    }

    return result;
}

int romanChordRootPitchClass(const RomanChord& chord, int tonicPc) noexcept {
    const int degIdx = std::clamp(chord.degree, 1, 7) - 1;
    return normalizePitchClass(tonicPc + kMajorScaleRef[static_cast<size_t>(degIdx)] + chord.accidental);
}

std::vector<int> chordPitchClasses(const RomanChord& chord, int tonicPc, Mode /*mode*/) {
    const int rootPc = romanChordRootPitchClass(chord, tonicPc);
    const auto intervals = chordIntervals(chord.quality);
    std::vector<int> pcs;
    pcs.reserve(intervals.size());
    for (int iv : intervals) {
        pcs.push_back(normalizePitchClass(rootPc + iv));
    }
    return pcs;
}

std::string formatChordName(int rootPc, ChordQuality quality, bool preferFlats) {
    std::string out(pitchClassName(rootPc, preferFlats));
    switch (quality) {
        case ChordQuality::Maj:     break;
        case ChordQuality::Min:     out += "m"; break;
        case ChordQuality::Dim:     out += "dim"; break;
        case ChordQuality::Aug:     out += "aug"; break;
        case ChordQuality::Sus2:    out += "sus2"; break;
        case ChordQuality::Sus4:    out += "sus4"; break;
        case ChordQuality::Maj7:    out += "maj7"; break;
        case ChordQuality::Min7:    out += "m7"; break;
        case ChordQuality::Dom7:    out += "7"; break;
        case ChordQuality::HDim7:   out += "m7b5"; break;
        case ChordQuality::Dim7:    out += "dim7"; break;
        case ChordQuality::MinMaj7: out += "mMaj7"; break;
        case ChordQuality::Maj9:    out += "maj9"; break;
        case ChordQuality::Min9:    out += "m9"; break;
        case ChordQuality::Dom9:    out += "9"; break;
        case ChordQuality::Min11:   out += "m11"; break;
        case ChordQuality::Add9:    out += "add9"; break;
        case ChordQuality::MinAdd9: out += "m(add9)"; break;
        case ChordQuality::Min6:    out += "m6"; break;
        case ChordQuality::Maj6:    out += "6"; break;
        case ChordQuality::Power5:  out += "5"; break;
    }
    return out;
}

std::string formatRomanAsConcreteChord(const RomanChord& chord, int tonicPc, Mode mode) {
    const bool preferFlats = (mode == Mode::Minor || mode == Mode::Dorian ||
                              mode == Mode::Phrygian || mode == Mode::HarmonicMinor ||
                              mode == Mode::Locrian || tonicPc == 5 || tonicPc == 10 ||
                              tonicPc == 3 || tonicPc == 8 || tonicPc == 1);
    const int rootPc = romanChordRootPitchClass(chord, tonicPc);
    std::string name = formatChordName(rootPc, chord.quality, preferFlats);
    if (chord.bass_degree_offset != 0) {
        const int bassPc = normalizePitchClass(rootPc + chord.bass_degree_offset);
        name += "/";
        name += pitchClassName(bassPc, preferFlats);
    }
    return name;
}

RomanChord diatonicChordForMode(Mode mode, int degree, bool seventh) {
    const int d = std::clamp(degree, 1, 7);
    const auto scale = scaleIntervals(mode);
    const int root = scale[static_cast<size_t>(d - 1)];
    const int third = (scale[static_cast<size_t>((d - 1 + 2) % 7)] + (d - 1 + 2 >= 7 ? 12 : 0)) - root;
    const int fifth = (scale[static_cast<size_t>((d - 1 + 4) % 7)] + (d - 1 + 4 >= 7 ? 12 : 0)) - root;
    const int sev = (scale[static_cast<size_t>((d - 1 + 6) % 7)] + (d - 1 + 6 >= 7 ? 12 : 0)) - root;

    const int accidental = root - kMajorScaleRef[static_cast<size_t>(d - 1)];

    ChordQuality q = ChordQuality::Maj;
    if (!seventh) {
        if (third == 3 && fifth == 7) q = ChordQuality::Min;
        else if (third == 3 && fifth == 6) q = ChordQuality::Dim;
        else if (third == 4 && fifth == 8) q = ChordQuality::Aug;
        else if (third == 4 && fifth == 7) q = ChordQuality::Maj;
        else if (third == 2 && fifth == 7) q = ChordQuality::Sus2;
        else if (third == 5 && fifth == 7) q = ChordQuality::Sus4;
    } else {
        if (third == 4 && fifth == 7 && sev == 11) q = ChordQuality::Maj7;
        else if (third == 4 && fifth == 7 && sev == 10) q = ChordQuality::Dom7;
        else if (third == 3 && fifth == 7 && sev == 10) q = ChordQuality::Min7;
        else if (third == 3 && fifth == 7 && sev == 11) q = ChordQuality::MinMaj7;
        else if (third == 3 && fifth == 6 && sev == 10) q = ChordQuality::HDim7;
        else if (third == 3 && fifth == 6 && sev == 9) q = ChordQuality::Dim7;
        else if (third == 4 && fifth == 8) q = ChordQuality::Aug;
    }

    return RomanChord{d, accidental, q, 0};
}

} // namespace style_scaler
