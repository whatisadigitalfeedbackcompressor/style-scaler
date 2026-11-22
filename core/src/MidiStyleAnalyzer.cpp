#include "style_scaler/MidiStyleAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

namespace style_scaler {

namespace {

constexpr std::array<int, 7> kMajorScaleRef = {0, 2, 4, 5, 7, 9, 11};

// Krumhansl-Schmuckler / Temperley inspired pitch-class salience weights (tonic-aligned)
struct ModeTemplate {
    Mode mode;
    std::array<double, 12> weights;
};

const std::vector<ModeTemplate>& getModeTemplates() {
    static const std::vector<ModeTemplate> kTemplates = {
        {Mode::Minor,         {6.33, 2.20, 3.52, 5.38, 2.20, 3.53, 2.20, 4.75, 4.00, 2.20, 3.50, 2.30}},
        {Mode::Major,         {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88}},
        {Mode::Dorian,        {6.20, 2.10, 3.50, 5.20, 2.10, 3.55, 2.20, 4.70, 2.25, 4.05, 3.45, 2.10}},
        {Mode::Phrygian,      {6.25, 4.10, 2.10, 5.15, 2.15, 3.55, 2.15, 4.75, 3.90, 2.10, 3.45, 2.10}},
        {Mode::Mixolydian,    {6.20, 2.15, 3.45, 2.20, 4.45, 3.85, 2.20, 5.05, 2.20, 3.55, 4.00, 2.15}},
        {Mode::HarmonicMinor, {6.25, 2.15, 3.45, 5.25, 2.15, 3.55, 2.15, 4.85, 3.90, 2.15, 2.20, 4.10}}
    };
    return kTemplates;
}

double pearsonCorrelation(const std::array<double, 12>& a, const std::array<double, 12>& b, int shift) {
    double meanA = 0.0, meanB = 0.0;
    for (int i = 0; i < 12; ++i) {
        meanA += a[static_cast<size_t>(i)];
        meanB += b[static_cast<size_t>(i)];
    }
    meanA /= 12.0;
    meanB /= 12.0;

    double num = 0.0, denA = 0.0, denB = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double da = a[static_cast<size_t>((i + shift) % 12)] - meanA;
        const double db = b[static_cast<size_t>(i)] - meanB;
        num += da * db;
        denA += da * da;
        denB += db * db;
    }
    const double den = std::sqrt(denA * denB);
    return den > 1e-12 ? (num / den) : 0.0;
}

std::pair<int, Mode> detectKeyAndMode(const std::vector<NoteEvent>& notes) {
    std::array<double, 12> chroma{};
    for (const auto& n : notes) {
        if (n.is_drum) continue;
        const double weight = std::max(0.05, n.duration_beats) * (n.channel == 3 ? 1.4 : 1.0);
        chroma[static_cast<size_t>(normalizePitchClass(n.pitch))] += weight;
    }
    const double sum = std::accumulate(chroma.begin(), chroma.end(), 0.0);
    if (sum <= 1e-9) {
        return {0, Mode::Minor};
    }

    int bestPc = 0;
    Mode bestMode = Mode::Minor;
    double bestScore = -1e9;
    for (int pc = 0; pc < 12; ++pc) {
        for (const auto& tmpl : getModeTemplates()) {
            double score = pearsonCorrelation(chroma, tmpl.weights, pc);
            // Tiny tie-break preference for Minor/Major unless modal intervals clearly win
            if (tmpl.mode == Mode::Minor || tmpl.mode == Mode::Major) {
                score += 0.005;
            }
            if (score > bestScore) {
                bestScore = score;
                bestPc = pc;
                bestMode = tmpl.mode;
            }
        }
    }
    return {bestPc, bestMode};
}

RomanChord matchChordToRoman(const std::array<double, 12>& windowChroma,
                               int bassPc,
                               int tonicPc) {
    static constexpr std::array<ChordQuality, 14> kCandidateQualities = {
        ChordQuality::Maj,
        ChordQuality::Min,
        ChordQuality::Maj7,
        ChordQuality::Min7,
        ChordQuality::Dom7,
        ChordQuality::Maj9,
        ChordQuality::Min9,
        ChordQuality::Add9,
        ChordQuality::MinAdd9,
        ChordQuality::Min11,
        ChordQuality::Sus2,
        ChordQuality::Sus4,
        ChordQuality::HDim7,
        ChordQuality::Dim7
    };

    const double totalMass = std::accumulate(windowChroma.begin(), windowChroma.end(), 0.0);
    if (totalMass <= 1e-9) {
        return RomanChord{1, 0, ChordQuality::Min, 0};
    }

    int bestRoot = tonicPc;
    ChordQuality bestQual = ChordQuality::Min;
    double bestScore = -1e9;

    for (int root = 0; root < 12; ++root) {
        for (ChordQuality q : kCandidateQualities) {
            const auto ivs = chordIntervals(q);
            std::array<bool, 12> inChord{};
            for (int iv : ivs) {
                inChord[static_cast<size_t>(normalizePitchClass(root + iv))] = true;
            }

            double hit = 0.0;
            double miss = 0.0;
            int coveredTones = 0;
            for (int pc = 0; pc < 12; ++pc) {
                const double w = windowChroma[static_cast<size_t>(pc)];
                if (inChord[static_cast<size_t>(pc)]) {
                    hit += w;
                    if (w > totalMass * 0.08) ++coveredTones;
                } else {
                    miss += w;
                }
            }

            double score = (hit - 0.75 * miss) / totalMass;
            if (bassPc >= 0 && bassPc == root) {
                score += 0.25;
            }
            score += 0.06 * static_cast<double>(coveredTones);
            // Prefer simpler triads/7ths over bloated chords unless extra tones actually sound
            score -= 0.045 * static_cast<double>(ivs.size());

            if (score > bestScore) {
                bestScore = score;
                bestRoot = root;
                bestQual = q;
            }
        }
    }

    // Map bestRoot relative to tonicPc into (degree 1..7, accidental -1/0/+1)
    const int rel = normalizePitchClass(bestRoot - tonicPc);
    int bestDegree = 1;
    int bestAcc = 0;
    int minAbsAcc = 99;
    for (int d = 0; d < 7; ++d) {
        int diff = rel - kMajorScaleRef[static_cast<size_t>(d)];
        if (diff > 6) diff -= 12;
        if (diff < -6) diff += 12;
        if (std::abs(diff) < minAbsAcc || (std::abs(diff) == minAbsAcc && diff < 0)) {
            minAbsAcc = std::abs(diff);
            bestDegree = d + 1;
            bestAcc = diff;
        }
    }

    return RomanChord{bestDegree, bestAcc, bestQual, 0};
}

template <typename T>
double percentileSorted(const std::vector<T>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const double idx = std::clamp(p, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    const double frac = idx - static_cast<double>(lo);
    return static_cast<double>(sorted[lo]) * (1.0 - frac) + static_cast<double>(sorted[hi]) * frac;
}

void normalizeGridMax(std::array<double, 16>& grid, const std::array<double, 16>& fallback) {
    double mx = 0.0;
    for (double v : grid) mx = std::max(mx, v);
    if (mx <= 1e-9) {
        grid = fallback;
        return;
    }
    for (double& v : grid) {
        v = std::clamp(v / mx, 0.0, 1.0);
    }
}

std::string toLowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool containsAnyToken(const std::string& haystack, const std::vector<const char*>& needles) {
    for (const char* n : needles) {
        if (haystack.find(n) != std::string::npos) return true;
    }
    return false;
}

double disambiguateBpmOctave(double bpm, const AnalysisOptions& options) {
    double out = std::clamp(bpm > 20.0 ? bpm : 120.0, 40.0, 260.0);
    std::string meta = toLowerStr(options.genre);
    for (const auto& t : options.tags) {
        meta += " " + toLowerStr(t);
    }
    const bool isDnb = containsAnyToken(meta, {"dnb", "drum & bass", "drum and bass", "jungle", "neuro", "liquid"});
    const bool isHiphop = containsAnyToken(meta, {"90s hip-hop", "hip-hop", "hip hop", "boom-bap", "boom bap", "lo-fi"});
    if (isDnb && out < 115.0) {
        out = std::clamp(out * 2.0, 40.0, 240.0);
    } else if (isHiphop && out > 142.0) {
        out = std::clamp(out * 0.5, 40.0, 240.0);
    }
    return out;
}

std::vector<int> chordPitchClassesFromSeg(const RomanChord& chord, int tonicPc, Mode mode) {
    return chordPitchClasses(chord, tonicPc, mode);
}

bool chordsShareAtLeastTwoPcs(const RomanChord& a, const RomanChord& b, int tonicPc, Mode mode) {
    const auto pa = chordPitchClasses(a, tonicPc, mode);
    const auto pb = chordPitchClasses(b, tonicPc, mode);
    int shared = 0;
    for (int x : pa) {
        if (std::find(pb.begin(), pb.end(), x) != pb.end()) ++shared;
    }
    return shared >= 2;
}

HarmonyAxis poolHarmonyAxes(const std::vector<std::pair<HarmonyAxis, double>>& items,
                            std::optional<int> keyOverride,
                            std::optional<Mode> modeOverride,
                            int fallbackTonicPc,
                            Mode fallbackMode) {
    HarmonyAxis out;
    if (items.empty()) return out;
    std::map<std::string, ChordVocabEntry> vocabMap;
    std::map<std::string, std::map<std::string, double>> transAccum;
    std::map<Mode, double> modeWeights;
    double chromSum = 0.0;
    double wSum = 0.0;

    for (const auto& [h, wRaw] : items) {
        const double w = std::max(0.01, wRaw);
        wSum += w;
        modeWeights[h.mode] += w;
        chromSum += h.chromaticism * w;
        for (const auto& v : h.vocabulary) {
            auto& dst = vocabMap[v.symbol];
            dst.symbol = v.symbol;
            dst.chord = v.chord;
            dst.weight += v.weight * w;
            dst.cadence_weight += v.cadence_weight * w;
        }
        for (const auto& [fromSym, row] : h.transitions) {
            for (const auto& [toSym, p] : row) {
                transAccum[fromSym][toSym] += p * w;
            }
        }
    }

    if (modeOverride.has_value()) {
        out.mode = *modeOverride;
    } else {
        double bestW = -1.0;
        out.mode = fallbackMode;
        for (const auto& [m, mw] : modeWeights) {
            if (mw > bestW) {
                bestW = mw;
                out.mode = m;
            }
        }
    }
    out.tonic_pc = keyOverride.has_value() ? normalizePitchClass(*keyOverride) : fallbackTonicPc;
    out.chromaticism = wSum > 0.0 ? std::clamp(chromSum / wSum, 0.0, 1.0) : 0.15;

    for (const auto& [sym, entry] : vocabMap) {
        out.vocabulary.push_back(entry);
    }
    std::sort(out.vocabulary.begin(), out.vocabulary.end(),
              [](const ChordVocabEntry& a, const ChordVocabEntry& b) { return a.weight > b.weight; });
    if (out.vocabulary.size() > 12) {
        out.vocabulary.resize(12);
    }

    for (const auto& fromEntry : out.vocabulary) {
        std::map<std::string, double> row;
        const auto it = transAccum.find(fromEntry.symbol);
        for (const auto& toEntry : out.vocabulary) {
            double val = 0.08 * toEntry.weight;
            if (it != transAccum.end()) {
                if (auto jt = it->second.find(toEntry.symbol); jt != it->second.end()) {
                    val += jt->second;
                }
            }
            if (toEntry.symbol == fromEntry.symbol && out.vocabulary.size() > 1) {
                val *= 0.25;
            }
            row[toEntry.symbol] = val;
        }
        out.transitions[fromEntry.symbol] = std::move(row);
    }
    return out;
}

RhythmAxis poolRhythmAxes(const std::vector<std::pair<RhythmAxis, double>>& items) {
    const auto defR = makeDefaultStyleProfile().rhythm;
    if (items.empty()) return defR;
    RhythmAxis out;
    out.subdivision = 16;
    std::array<double, 16> cg{}, mg{}, bg{};
    double syncSum = 0.0, antSum = 0.0, swingSum = 0.0, paceSum = 0.0, sustSum = 0.0, wSum = 0.0;
    for (const auto& [r, wRaw] : items) {
        const double w = std::max(0.01, wRaw);
        wSum += w;
        for (size_t i = 0; i < 16; ++i) {
            cg[i] += r.chord_onset_grid[i] * w;
            mg[i] += r.melody_onset_grid[i] * w;
            bg[i] += r.bass_onset_grid[i] * w;
        }
        syncSum += r.syncopation * w;
        antSum += r.anticipation * w;
        swingSum += r.swing * w;
        paceSum += r.harmonic_pacing * w;
        sustSum += r.sustain_ratio * w;
    }
    normalizeGridMax(cg, defR.chord_onset_grid);
    normalizeGridMax(mg, defR.melody_onset_grid);
    normalizeGridMax(bg, defR.bass_onset_grid);
    out.chord_onset_grid = cg;
    out.melody_onset_grid = mg;
    out.bass_onset_grid = bg;
    out.syncopation = std::clamp(syncSum / wSum, 0.0, 1.0);
    out.anticipation = std::clamp(antSum / wSum, 0.0, 1.0);
    out.swing = std::clamp(swingSum / wSum, 0.0, 1.0);
    out.harmonic_pacing = std::clamp(paceSum / wSum, 0.25, 4.0);
    out.sustain_ratio = std::clamp(sustSum / wSum, 0.2, 1.5);
    return out;
}

VoicingAxis poolVoicingAxes(const std::vector<std::pair<VoicingAxis, double>>& items) {
    VoicingAxis out;
    if (items.empty()) return out;
    double regLo = 0.0, regHi = 0.0, cent = 0.0, spread = 0.0;
    double ext = 0.0, drop2 = 0.0, inv = 0.0, vl = 0.0, clear = 0.0;
    double bLo = 0.0, bHi = 0.0, wSum = 0.0;
    int minN = 6, maxN = 1;
    for (const auto& [v, wRaw] : items) {
        const double w = std::max(0.01, wRaw);
        wSum += w;
        regLo += v.register_low * w;
        regHi += v.register_high * w;
        cent += v.center_pitch * w;
        spread += v.spread_semitones * w;
        ext += v.extension_richness * w;
        drop2 += v.drop2_prob * w;
        inv += v.inversion_prob * w;
        vl += v.voice_leading_tightness * w;
        clear += v.sub_bass_clearance_midi * w;
        bLo += v.bass_register_low * w;
        bHi += v.bass_register_high * w;
        minN = std::min(minN, v.min_notes);
        maxN = std::max(maxN, v.max_notes);
    }
    out.register_low = std::clamp(static_cast<int>(std::llround(regLo / wSum)), 36, 72);
    out.register_high = std::clamp(static_cast<int>(std::llround(regHi / wSum)), out.register_low + 8, 96);
    out.center_pitch = std::clamp(static_cast<int>(std::llround(cent / wSum)), out.register_low, out.register_high);
    out.spread_semitones = std::clamp(spread / wSum, 5.0, 30.0);
    out.min_notes = std::clamp(minN, 2, 5);
    out.max_notes = std::clamp(maxN, out.min_notes, 6);
    out.extension_richness = std::clamp(ext / wSum, 0.0, 1.0);
    out.drop2_prob = std::clamp(drop2 / wSum, 0.0, 1.0);
    out.inversion_prob = std::clamp(inv / wSum, 0.0, 1.0);
    out.voice_leading_tightness = std::clamp(vl / wSum, 0.1, 0.98);
    out.sub_bass_clearance_midi = std::clamp(
        static_cast<int>(std::llround(clear / wSum)), 36, std::min(60, out.register_high - 6));
    out.bass_register_low = std::clamp(static_cast<int>(std::llround(bLo / wSum)), 21, 50);
    out.bass_register_high = std::clamp(static_cast<int>(std::llround(bHi / wSum)), out.bass_register_low + 5, 58);
    return out;
}

MelodyAxis poolMelodyAxes(const std::vector<std::pair<MelodyAxis, double>>& items) {
    MelodyAxis out;
    if (items.empty()) return out;
    std::array<double, 25> hist{};
    double ct = 0.0, tt = 0.0, md = 0.0, mr = 0.0, ac = 0.0, leg = 0.0, mLo = 0.0, mHi = 0.0, wSum = 0.0;
    for (const auto& [m, wRaw] : items) {
        const double w = std::max(0.01, wRaw);
        wSum += w;
        for (size_t i = 0; i < 25; ++i) hist[i] += m.interval_histogram[i] * w;
        ct += m.chord_tone_ratio * w;
        tt += m.tension_tone_ratio * w;
        md += m.motif_density * w;
        mr += m.motif_repetition * w;
        ac += m.arch_contour * w;
        leg += m.legato_ratio * w;
        mLo += m.register_low * w;
        mHi += m.register_high * w;
    }
    out.interval_histogram = hist;
    out.chord_tone_ratio = std::clamp(ct / wSum, 0.15, 0.95);
    out.tension_tone_ratio = std::clamp(tt / wSum, 0.02, 0.65);
    out.motif_density = std::clamp(md / wSum, 0.1, 1.0);
    out.motif_repetition = std::clamp(mr / wSum, 0.1, 0.95);
    out.arch_contour = std::clamp(ac / wSum, 0.1, 0.9);
    out.legato_ratio = std::clamp(leg / wSum, 0.15, 1.0);
    out.register_low = std::clamp(static_cast<int>(std::llround(mLo / wSum)), 48, 84);
    out.register_high = std::clamp(static_cast<int>(std::llround(mHi / wSum)), out.register_low + 7, 108);
    return out;
}

} // namespace

double MidiStyleAnalyzer::estimateBpm(const std::vector<TimedNoteSec>& notes) {
    if (notes.size() < 4) return 120.0;

    std::vector<double> onsets;
    onsets.reserve(notes.size());
    for (const auto& n : notes) {
        onsets.push_back(n.onset_sec);
    }
    std::sort(onsets.begin(), onsets.end());

    // Deduplicate near-simultaneous chord hits within 35 ms
    std::vector<double> uniqueOnsets;
    for (double t : onsets) {
        if (uniqueOnsets.empty() || (t - uniqueOnsets.back()) > 0.035) {
            uniqueOnsets.push_back(t);
        }
    }
    if (uniqueOnsets.size() < 3) return 120.0;

    // Score candidate integer BPMs from 70 to 185 using beat-grid alignment
    double bestBpm = 120.0;
    double bestScore = -1.0;
    for (int bpm = 70; bpm <= 185; ++bpm) {
        const double spb = 60.0 / static_cast<double>(bpm);
        double score = 0.0;
        for (size_t i = 0; i < uniqueOnsets.size(); ++i) {
            const double beatPhase = std::fmod(uniqueOnsets[i] / spb, 1.0);
            const double sixteenthErr = std::abs(beatPhase * 4.0 - std::round(beatPhase * 4.0));
            score += std::exp(-sixteenthErr * sixteenthErr * 18.0);
            if (i > 0) {
                const double ioi = uniqueOnsets[i] - uniqueOnsets[i - 1];
                const double beats = ioi / spb;
                if (beats >= 0.2 && beats <= 4.0) {
                    const double qErr = std::abs(beats * 2.0 - std::round(beats * 2.0));
                    score += 0.6 * std::exp(-qErr * qErr * 16.0);
                }
            }
        }
        // Mild prior around common musical BPM ranges (90 / 140 / 172)
        if (score > bestScore) {
            bestScore = score;
            bestBpm = static_cast<double>(bpm);
        }
    }
    return bestBpm;
}

double MidiStyleAnalyzer::estimateBeatPhaseBeats(const std::vector<NoteEvent>& events, double beatsPerBar) {
    if (events.size() < 3 || beatsPerBar <= 0.5) return 0.0;

    std::vector<const NoteEvent*> sample;
    sample.reserve(events.size());
    for (const auto& ev : events) {
        if (!ev.is_drum && ev.channel != 10) sample.push_back(&ev);
    }
    if (sample.size() < 3) return 0.0;

    constexpr int kSteps = 64;
    double bestPhi = 0.0;
    double bestScore = -1e18;

    for (int k = 0; k < kSteps; ++k) {
        const double phi = (static_cast<double>(k) * beatsPerBar) / static_cast<double>(kSteps);
        double score = (k == 0) ? 0.15 : 0.0; // slight preference for 0 when already bar-aligned
        double lastUnique = -10.0;
        for (const NoteEvent* ev : sample) {
            double b = ev->onset_beats - phi;
            while (b < 0.0) b += beatsPerBar;
            const double barPos = std::fmod(b, beatsPerBar);
            const double qPos = std::fmod(b, 1.0);
            const double qErr = std::min(qPos, 1.0 - qPos);
            const double sPos = std::fmod(b * 4.0, 1.0);
            const double sErr = std::min(sPos, 1.0 - sPos);

            if ((ev->onset_beats - lastUnique) > 0.05) {
                score += std::exp(-qErr * qErr * 28.0);
                score += 0.35 * std::exp(-sErr * sErr * 18.0);
                lastUnique = ev->onset_beats;
            }
            const double barErr = std::min(barPos, beatsPerBar - barPos);
            if (barErr < 0.12) {
                if (ev->channel == 3 || ev->pitch < 50) score += 0.85;
                else score += 0.35;
            }
        }
        if (score > bestScore) {
            bestScore = score;
            bestPhi = phi;
        }
    }
    return bestPhi;
}

double MidiStyleAnalyzer::estimateBeatPhaseSec(const std::vector<TimedNoteSec>& notes, double bpm, int timeSigNum, int timeSigDen) {
    if (notes.empty() || bpm <= 20.0) return 0.0;
    const double beatsPerSec = bpm / 60.0;
    const double beatsPerBar = std::max(1, timeSigNum) * (4.0 / std::max(1, timeSigDen));
    std::vector<NoteEvent> evs;
    evs.reserve(notes.size());
    for (const auto& n : notes) {
        NoteEvent ev;
        ev.onset_beats = std::max(0.0, n.onset_sec * beatsPerSec);
        ev.pitch = std::clamp(n.pitch, 0, 127);
        ev.is_drum = n.is_drum || n.program == 128;
        ev.channel = ev.is_drum ? 10 : ((n.program >= 32 && n.program <= 39) ? 3 : 1);
        evs.push_back(ev);
    }
    const double phiBeats = estimateBeatPhaseBeats(evs, beatsPerBar);
    return phiBeats / beatsPerSec;
}

StyleProfile MidiStyleAnalyzer::analyzeTimed(const std::vector<TimedNoteSec>& notes,
                                             const AnalysisOptions& options) {
    double bpm = (options.bpm_hint > 20.0) ? options.bpm_hint : estimateBpm(notes);
    bpm = disambiguateBpmOctave(bpm, options);
    const double beatsPerSec = bpm / 60.0;

    std::vector<NoteEvent> events;
    events.reserve(notes.size());
    for (const auto& n : notes) {
        NoteEvent ev;
        ev.onset_beats = std::max(0.0, n.onset_sec * beatsPerSec);
        ev.duration_beats = std::max(0.0625, (n.offset_sec - n.onset_sec) * beatsPerSec);
        ev.pitch = std::clamp(n.pitch, 0, 127);
        ev.velocity = std::clamp(n.velocity, 1, 127);
        ev.program = n.program;
        ev.is_drum = n.is_drum || n.program == 128;
        if (ev.is_drum) {
            ev.channel = 10;
        } else if (ev.program >= 32 && ev.program <= 39) {
            ev.channel = 3; // Bass
        } else if ((ev.program >= 56 && ev.program <= 87) || (ev.program >= 104 && ev.program <= 111)) {
            ev.channel = 2; // Solo / Lead / Reed / Pipe / Pluck
        } else {
            ev.channel = 1;
        }
        events.push_back(ev);
    }

    return analyze(events, bpm, options);
}

StyleProfile MidiStyleAnalyzer::analyze(const std::vector<NoteEvent>& rawEvents,
                                        double bpm,
                                        const AnalysisOptions& options) {
    StyleProfile profile = makeDefaultStyleProfile();
    profile.id = options.id.empty() ? "analyzed_style" : options.id;
    profile.name = options.name.empty() ? "Analyzed Style" : options.name;
    profile.genre = options.genre.empty() ? "Analyzed" : options.genre;
    profile.source = options.source.empty() ? "in_plugin_analysis" : options.source;
    profile.tags = options.tags;
    profile.genre_source = options.genre_source;
    profile.artist = options.artist;
    profile.album = options.album;
    profile.time_sig_num = std::max(1, options.time_sig_num);
    profile.time_sig_den = std::max(1, options.time_sig_den);
    const double safeBpm = disambiguateBpmOctave(bpm, options);
    profile.default_bpm = safeBpm;
    profile.bpm_min = std::max(40.0, safeBpm - 8.0);
    profile.bpm_max = std::min(240.0, safeBpm + 8.0);
    profile.track_count = 1;

    std::vector<NoteEvent> pitched;
    for (const auto& ev : rawEvents) {
        if (!ev.is_drum && ev.channel != 10 && ev.pitch >= 0 && ev.pitch <= 127) {
            pitched.push_back(ev);
        }
    }
    if (pitched.empty()) {
        return profile;
    }

    std::sort(pitched.begin(), pitched.end(), [](const NoteEvent& a, const NoteEvent& b) {
        if (a.onset_beats != b.onset_beats) return a.onset_beats < b.onset_beats;
        return a.pitch < b.pitch;
    });

    const double beatsPerBar = static_cast<double>(profile.time_sig_num) * (4.0 / static_cast<double>(profile.time_sig_den));

    // Task 3B: Beat-phase (phi_0) alignment so Bar 1 Beat 1 aligns to step 0
    const double phi0 = estimateBeatPhaseBeats(pitched, beatsPerBar);
    if (phi0 > 1e-4) {
        for (auto& n : pitched) {
            double shifted = n.onset_beats - phi0;
            if (shifted < -0.125) shifted += beatsPerBar;
            n.onset_beats = std::max(0.0, shifted);
        }
        std::sort(pitched.begin(), pitched.end(), [](const NoteEvent& a, const NoteEvent& b) {
            if (a.onset_beats != b.onset_beats) return a.onset_beats < b.onset_beats;
            return a.pitch < b.pitch;
        });
    }

    auto separateRoles = [](const std::vector<NoteEvent>& notes,
                            std::vector<NoteEvent>& chordNotes,
                            std::vector<NoteEvent>& melodyNotes,
                            std::vector<NoteEvent>& bassNotes) {
        chordNotes.clear();
        melodyNotes.clear();
        bassNotes.clear();

        size_t ch1Count = 0, ch2Count = 0, ch3Count = 0;
        for (const auto& n : notes) {
            if (n.channel == 1) ++ch1Count;
            else if (n.channel == 2) ++ch2Count;
            else if (n.channel == 3) ++ch3Count;
        }
        const bool hasExplicitMelody = (ch2Count >= 4) &&
                                       (static_cast<double>(ch2Count) >= 0.05 * static_cast<double>(ch1Count + ch2Count));

        if (hasExplicitMelody) {
            for (const auto& n : notes) {
                if (n.channel == 2) melodyNotes.push_back(n);
                else if (n.channel == 3) bassNotes.push_back(n);
                else chordNotes.push_back(n);
            }
        } else {
            // Separate any explicit channel-3 or bass-program notes first, then skyline-separate the rest
            std::vector<NoteEvent> remaining;
            remaining.reserve(notes.size());
            for (const auto& n : notes) {
                if (n.channel == 3 || (n.program >= 32 && n.program <= 39)) {
                    bassNotes.push_back(n);
                } else if (n.channel == 2) {
                    melodyNotes.push_back(n);
                } else {
                    remaining.push_back(n);
                }
            }

            size_t i = 0;
            while (i < remaining.size()) {
                size_t j = i + 1;
                while (j < remaining.size() && (remaining[j].onset_beats - remaining[i].onset_beats) <= 0.15) {
                    ++j;
                }
                const size_t clusterCount = j - i;
                if (clusterCount >= 3) {
                    size_t startIdx = i;
                    if (ch3Count == 0 && remaining[i].pitch < 48 && (remaining[i + 1].pitch - remaining[i].pitch) >= 5) {
                        bassNotes.push_back(remaining[i]);
                        startIdx = i + 1;
                    }
                    for (size_t k = startIdx; k < j; ++k) {
                        chordNotes.push_back(remaining[k]);
                    }
                    const auto& top = remaining[j - 1];
                    const int gapBelow = top.pitch - remaining[j - 2].pitch;
                    if (top.pitch >= 60 && (gapBelow >= 5 || top.pitch >= 68)) {
                        melodyNotes.push_back(top);
                    }
                } else if (clusterCount == 2) {
                    const auto& lo = remaining[i];
                    const auto& hi = remaining[i + 1];
                    if (ch3Count == 0 && lo.pitch < 50) {
                        bassNotes.push_back(lo);
                        if (hi.pitch >= 60) melodyNotes.push_back(hi);
                        else chordNotes.push_back(hi);
                    } else {
                        chordNotes.push_back(lo);
                        if (hi.pitch >= 60) {
                            melodyNotes.push_back(hi);
                            if (hi.pitch - lo.pitch <= 12) chordNotes.push_back(hi);
                        } else {
                            chordNotes.push_back(hi);
                        }
                    }
                } else {
                    const auto& single = remaining[i];
                    if (ch3Count == 0 && single.pitch < 48) {
                        bassNotes.push_back(single);
                    } else if (single.pitch >= 60 || single.duration_beats <= 1.25) {
                        melodyNotes.push_back(single);
                        if (single.duration_beats >= 0.5 && single.pitch < 76) {
                            chordNotes.push_back(single);
                        }
                    } else {
                        chordNotes.push_back(single);
                    }
                }
                i = j;
            }
        }

        // Skyline fallback guarantee so melodyNotes is never empty on tonal music
        if (melodyNotes.size() < 2 && notes.size() >= 4) {
            melodyNotes.clear();
            std::map<int, NoteEvent> bucketTop;
            for (const auto& n : notes) {
                if (n.channel == 3 || n.pitch < 52) continue;
                const int bucket = static_cast<int>(std::floor(n.onset_beats * 2.0));
                auto it = bucketTop.find(bucket);
                if (it == bucketTop.end() || n.pitch > it->second.pitch) {
                    bucketTop[bucket] = n;
                }
            }
            for (const auto& [b, ev] : bucketTop) {
                melodyNotes.push_back(ev);
            }
        }

        if (chordNotes.empty()) {
            for (const auto& n : notes) {
                if (n.pitch >= 45) chordNotes.push_back(n);
            }
            if (chordNotes.empty()) chordNotes = notes;
        }
    };

    std::vector<NoteEvent> chordNotes;
    std::vector<NoteEvent> melodyNotes;
    std::vector<NoteEvent> bassNotes;
    separateRoles(pitched, chordNotes, melodyNotes, bassNotes);

    // 1. Key & Mode detection
    auto [detectedPc, detectedMode] = detectKeyAndMode(pitched);
    const int tonicPc = options.key_override_pc.value_or(detectedPc);
    const Mode mode = options.mode_override.value_or(detectedMode);
    profile.harmony.tonic_pc = tonicPc;
    profile.harmony.mode = mode;

    auto analyzeSubsetAxes = [&](const std::vector<NoteEvent>& subNotes,
                                 HarmonyAxis& outHarmony,
                                 RhythmAxis& outRhythm,
                                 VoicingAxis& outVoicing,
                                 MelodyAxis& outMelody) {
        if (subNotes.empty()) return;
        std::vector<NoteEvent> cNotes, mNotes, bNotes;
        separateRoles(subNotes, cNotes, mNotes, bNotes);

        const auto sc = scaleIntervals(mode);
        std::array<bool, 12> inScale{};
        for (int iv : sc) inScale[static_cast<size_t>(normalizePitchClass(tonicPc + iv))] = true;
        double totalDur = 0.0, chromDur = 0.0;
        for (const auto& n : subNotes) {
            totalDur += n.duration_beats;
            if (!inScale[static_cast<size_t>(normalizePitchClass(n.pitch))]) {
                chromDur += n.duration_beats;
            }
        }
        outHarmony.tonic_pc = tonicPc;
        outHarmony.mode = mode;
        outHarmony.chromaticism = totalDur > 0.0 ? std::clamp(chromDur / totalDur, 0.0, 1.0) : 0.1;

        double maxBeat = 0.0;
        for (const auto& n : subNotes) {
            maxBeat = std::max(maxBeat, n.onset_beats + n.duration_beats);
        }
        const int subBars = std::max(1, static_cast<int>(std::ceil(maxBeat / beatsPerBar)));
        const double windowBeats = std::max(1.0, beatsPerBar * 0.5);
        const int numWindows = std::max(1, static_cast<int>(std::ceil((subBars * beatsPerBar) / windowBeats)));

        struct ChordSeg {
            std::string symbol;
            RomanChord chord;
            double startBeat = 0.0;
            double endBeat = 0.0;
            bool isPhraseEnd = false;
        };
        std::vector<ChordSeg> segments;

        for (int w = 0; w < numWindows; ++w) {
            const double wStart = w * windowBeats;
            const double wEnd = wStart + windowBeats;
            std::array<double, 12> chroma{};
            int lowestBassPitch = 128;
            int bassPc = -1;

            for (const auto& n : cNotes) {
                const double overlap = std::min(n.onset_beats + n.duration_beats, wEnd) - std::max(n.onset_beats, wStart);
                if (overlap > 0.0) {
                    chroma[static_cast<size_t>(normalizePitchClass(n.pitch))] += overlap;
                }
            }
            for (const auto& n : bNotes) {
                const double overlap = std::min(n.onset_beats + n.duration_beats, wEnd) - std::max(n.onset_beats, wStart);
                if (overlap > 0.0 && n.pitch < lowestBassPitch) {
                    lowestBassPitch = n.pitch;
                    bassPc = normalizePitchClass(n.pitch);
                }
            }
            if (bassPc >= 0) {
                chroma[static_cast<size_t>(bassPc)] += 0.65 * windowBeats;
            }
            const double mass = std::accumulate(chroma.begin(), chroma.end(), 0.0);
            if (mass <= 1e-6) continue;

            const RomanChord rc = matchChordToRoman(chroma, bassPc, tonicPc);
            const std::string sym = formatRomanChord(rc);
            const int barIdx = static_cast<int>(wStart / beatsPerBar);
            const bool phraseEnd = ((barIdx + 1) % 4 == 0) && (wEnd >= (barIdx + 1) * beatsPerBar - 0.1);

            if (!segments.empty() && segments.back().symbol == sym) {
                segments.back().endBeat = wEnd;
                segments.back().isPhraseEnd = segments.back().isPhraseEnd || phraseEnd;
            } else {
                segments.push_back(ChordSeg{sym, rc, wStart, wEnd, phraseEnd});
            }
        }

        // Task 3D: Smooth isolated single-window blips
        if (segments.size() >= 3) {
            std::vector<ChordSeg> smoothed;
            smoothed.reserve(segments.size());
            for (size_t s = 0; s < segments.size(); ++s) {
                const double dur = segments[s].endBeat - segments[s].startBeat;
                if (!smoothed.empty() && s + 1 < segments.size() && dur <= windowBeats + 1e-4) {
                    const auto& prev = smoothed.back();
                    const auto& next = segments[s + 1];
                    if (prev.symbol == next.symbol) {
                        smoothed.back().endBeat = segments[s].endBeat;
                        smoothed.back().isPhraseEnd = prev.isPhraseEnd || segments[s].isPhraseEnd;
                        continue;
                    }
                    const double prevDur = prev.endBeat - prev.startBeat;
                    const double nextDur = next.endBeat - next.startBeat;
                    if (prevDur >= nextDur && chordsShareAtLeastTwoPcs(segments[s].chord, prev.chord, tonicPc, mode)) {
                        smoothed.back().endBeat = segments[s].endBeat;
                        smoothed.back().isPhraseEnd = prev.isPhraseEnd || segments[s].isPhraseEnd;
                        continue;
                    }
                    if (nextDur > prevDur && chordsShareAtLeastTwoPcs(segments[s].chord, next.chord, tonicPc, mode)) {
                        segments[s + 1].startBeat = segments[s].startBeat;
                        continue;
                    }
                }
                if (!smoothed.empty() && smoothed.back().symbol == segments[s].symbol) {
                    smoothed.back().endBeat = segments[s].endBeat;
                    smoothed.back().isPhraseEnd = smoothed.back().isPhraseEnd || segments[s].isPhraseEnd;
                } else {
                    smoothed.push_back(segments[s]);
                }
            }
            segments = std::move(smoothed);
        }

        if (!segments.empty()) {
            std::map<std::string, ChordVocabEntry> vocabMap;
            std::map<std::string, std::map<std::string, double>> counts;

            for (size_t s = 0; s < segments.size(); ++s) {
                auto& entry = vocabMap[segments[s].symbol];
                entry.symbol = segments[s].symbol;
                entry.chord = segments[s].chord;
                entry.weight += std::max(0.5, segments[s].endBeat - segments[s].startBeat);
                if (segments[s].isPhraseEnd || s + 1 == segments.size()) {
                    entry.cadence_weight += 1.0;
                }
                if (s + 1 < segments.size()) {
                    counts[segments[s].symbol][segments[s + 1].symbol] += 1.0;
                }
            }
            if (segments.size() >= 2) {
                counts[segments.back().symbol][segments.front().symbol] += 0.75;
            }

            outHarmony.vocabulary.clear();
            for (const auto& [sym, entry] : vocabMap) {
                outHarmony.vocabulary.push_back(entry);
            }
            std::sort(outHarmony.vocabulary.begin(), outHarmony.vocabulary.end(),
                      [](const ChordVocabEntry& a, const ChordVocabEntry& b) { return a.weight > b.weight; });

            // Task 3D: Top-K (K=12) vocabulary pruning
            if (outHarmony.vocabulary.size() > 12) {
                outHarmony.vocabulary.resize(12);
            }

            outHarmony.transitions.clear();
            for (const auto& fromEntry : outHarmony.vocabulary) {
                std::map<std::string, double> row;
                const auto it = counts.find(fromEntry.symbol);
                for (const auto& toEntry : outHarmony.vocabulary) {
                    double c = 0.14 * toEntry.weight; // healthy Dirichlet smoothing
                    if (it != counts.end()) {
                        const auto jt = it->second.find(toEntry.symbol);
                        if (jt != it->second.end()) {
                            c += jt->second;
                        }
                    }
                    if (toEntry.symbol == fromEntry.symbol && outHarmony.vocabulary.size() > 1) {
                        c *= 0.20;
                    }
                    row[toEntry.symbol] = c;
                }
                outHarmony.transitions[fromEntry.symbol] = std::move(row);
            }
        }

        // Rhythm extraction
        {
            const auto defaultR = makeDefaultStyleProfile().rhythm;
            std::array<double, 16> chordGrid{};
            std::array<double, 16> melodyGrid{};
            std::array<double, 16> bassGrid{};

            auto accumulateGrid = [&](const std::vector<NoteEvent>& evs, std::array<double, 16>& grid, bool dedupClusters) {
                double lastOnset = -10.0;
                for (const auto& n : evs) {
                    if (dedupClusters && (n.onset_beats - lastOnset) <= 0.12) continue;
                    lastOnset = n.onset_beats;
                    const double barPos = std::fmod(std::max(0.0, n.onset_beats), beatsPerBar);
                    const int step = static_cast<int>(std::llround((barPos / beatsPerBar) * 16.0)) % 16;
                    grid[static_cast<size_t>(step)] += 1.0;
                }
            };

            accumulateGrid(cNotes, chordGrid, true);
            accumulateGrid(mNotes, melodyGrid, false);
            accumulateGrid(bNotes, bassGrid, false);

            normalizeGridMax(chordGrid, defaultR.chord_onset_grid);
            normalizeGridMax(melodyGrid, defaultR.melody_onset_grid);
            normalizeGridMax(bassGrid, defaultR.bass_onset_grid);

            outRhythm.chord_onset_grid = chordGrid;
            outRhythm.melody_onset_grid = melodyGrid;
            outRhythm.bass_onset_grid = bassGrid;

            double totalWeight = 0.0, offBeatWeight = 0.0, anticWeight = 0.0;
            for (int s = 0; s < 16; ++s) {
                const double w = chordGrid[static_cast<size_t>(s)] + 0.5 * bassGrid[static_cast<size_t>(s)];
                totalWeight += w;
                if (s % 4 != 0) offBeatWeight += w;
                if (s == 14 || s == 15) anticWeight += chordGrid[static_cast<size_t>(s)];
            }
            outRhythm.syncopation = totalWeight > 0.0 ? std::clamp(offBeatWeight / totalWeight, 0.0, 1.0) : 0.35;
            const double chordSum = std::accumulate(chordGrid.begin(), chordGrid.end(), 0.0);
            outRhythm.anticipation = chordSum > 0.0 ? std::clamp(anticWeight / chordSum * 2.5, 0.0, 1.0) : 0.2;

            double swingAccum = 0.0;
            int swingCount = 0;
            for (const auto& n : subNotes) {
                const double beatFrac = std::fmod(std::max(0.0, n.onset_beats), 1.0);
                if (beatFrac >= 0.42 && beatFrac <= 0.72) {
                    swingAccum += std::clamp((beatFrac - 0.5) / 0.167, 0.0, 1.0);
                    ++swingCount;
                }
            }
            outRhythm.swing = swingCount >= 3 ? std::clamp(swingAccum / static_cast<double>(swingCount), 0.0, 1.0) : 0.0;
            outRhythm.harmonic_pacing = std::clamp(
                static_cast<double>(std::max<size_t>(1, segments.size())) / static_cast<double>(subBars),
                0.25, 4.0);

            if (!cNotes.empty()) {
                std::vector<double> durs;
                for (const auto& n : cNotes) durs.push_back(n.duration_beats);
                std::sort(durs.begin(), durs.end());
                const double medDur = percentileSorted(durs, 0.5);
                const double expectedBarSlice = beatsPerBar / std::max(0.5, outRhythm.harmonic_pacing);
                outRhythm.sustain_ratio = std::clamp(medDur / expectedBarSlice, 0.2, 1.15);
            }
        }

        // Voicing extraction
        {
            std::vector<int> allChordPitches;
            std::vector<double> spreads;
            std::vector<int> counts;
            std::vector<int> lowestPerChord;
            int extendedChords = 0, invertedChords = 0, totalClusters = 0;
            std::vector<double> centroids;

            size_t i = 0;
            while (i < cNotes.size()) {
                size_t j = i + 1;
                while (j < cNotes.size() && (cNotes[j].onset_beats - cNotes[i].onset_beats) <= 0.15) {
                    ++j;
                }
                if (j - i >= 2) {
                    std::vector<int> pitches;
                    for (size_t k = i; k < j; ++k) {
                        pitches.push_back(cNotes[k].pitch);
                        allChordPitches.push_back(cNotes[k].pitch);
                    }
                    std::sort(pitches.begin(), pitches.end());
                    spreads.push_back(static_cast<double>(pitches.back() - pitches.front()));
                    counts.push_back(static_cast<int>(pitches.size()));
                    lowestPerChord.push_back(pitches.front());

                    double cSum = 0.0;
                    std::set<int> pcs;
                    for (int p : pitches) {
                        cSum += static_cast<double>(p);
                        pcs.insert(normalizePitchClass(p));
                    }
                    centroids.push_back(cSum / static_cast<double>(pitches.size()));
                    if (pcs.size() >= 4) ++extendedChords;

                    const double tBeat = cNotes[i].onset_beats;
                    for (const auto& seg : segments) {
                        if (tBeat >= seg.startBeat - 0.1 && tBeat <= seg.endBeat + 0.1) {
                            const int rootPc = romanChordRootPitchClass(seg.chord, tonicPc);
                            if (normalizePitchClass(pitches.front()) != rootPc) ++invertedChords;
                            break;
                        }
                    }
                    ++totalClusters;
                }
                i = j;
            }

            if (!allChordPitches.empty()) {
                std::sort(allChordPitches.begin(), allChordPitches.end());
                outVoicing.register_low = std::clamp(static_cast<int>(std::floor(percentileSorted(allChordPitches, 0.08))), 36, 72);
                outVoicing.register_high = std::clamp(static_cast<int>(std::ceil(percentileSorted(allChordPitches, 0.92))),
                                                      outVoicing.register_low + 8, 96);
                outVoicing.center_pitch = static_cast<int>(std::llround(percentileSorted(allChordPitches, 0.5)));
            }
            if (!spreads.empty()) {
                outVoicing.spread_semitones = std::clamp(
                    std::accumulate(spreads.begin(), spreads.end(), 0.0) / static_cast<double>(spreads.size()),
                    5.0, 30.0);
            }
            if (!counts.empty()) {
                std::sort(counts.begin(), counts.end());
                outVoicing.min_notes = std::clamp(counts.front(), 2, 5);
                outVoicing.max_notes = std::clamp(counts.back(), outVoicing.min_notes, 6);
            }
            if (totalClusters > 0) {
                outVoicing.extension_richness = std::clamp(static_cast<double>(extendedChords) / totalClusters, 0.0, 1.0);
                outVoicing.inversion_prob = std::clamp(static_cast<double>(invertedChords) / totalClusters, 0.0, 1.0);
                outVoicing.drop2_prob = std::clamp((outVoicing.spread_semitones - 11.0) / 14.0, 0.0, 1.0);
            }
            if (centroids.size() >= 2) {
                double jumpSum = 0.0;
                for (size_t k = 1; k < centroids.size(); ++k) {
                    jumpSum += std::abs(centroids[k] - centroids[k - 1]);
                }
                const double meanJump = jumpSum / static_cast<double>(centroids.size() - 1);
                outVoicing.voice_leading_tightness = std::clamp(1.0 - (meanJump / 14.0), 0.1, 0.98);
            }
            if (!lowestPerChord.empty()) {
                std::sort(lowestPerChord.begin(), lowestPerChord.end());
                const int p10 = static_cast<int>(std::floor(percentileSorted(lowestPerChord, 0.10)));
                outVoicing.sub_bass_clearance_midi = std::clamp(p10, 36, std::min(60, outVoicing.register_high - 6));
            }
            if (!bNotes.empty()) {
                std::vector<int> bp;
                for (const auto& n : bNotes) bp.push_back(n.pitch);
                std::sort(bp.begin(), bp.end());
                outVoicing.bass_register_low = std::clamp(static_cast<int>(percentileSorted(bp, 0.05)), 21, 50);
                outVoicing.bass_register_high = std::clamp(static_cast<int>(percentileSorted(bp, 0.95)),
                                                           outVoicing.bass_register_low + 5, 58);
            }
        }

        // Melody extraction
        if (mNotes.size() >= 2) {
            std::array<double, 25> hist{};
            std::vector<int> mpitches, intervals;
            int chordToneHits = 0, tensionHits = 0, legCount = 0;
            double legSum = 0.0;

            for (size_t k = 0; k < mNotes.size(); ++k) {
                const auto& mn = mNotes[k];
                mpitches.push_back(mn.pitch);
                if (k > 0) {
                    const int diff = std::clamp(mn.pitch - mNotes[k - 1].pitch, -12, 12);
                    hist[static_cast<size_t>(diff + 12)] += 1.0;
                    intervals.push_back(diff);
                    const double ioi = std::max(0.0625, mn.onset_beats - mNotes[k - 1].onset_beats);
                    legSum += std::clamp(mNotes[k - 1].duration_beats / ioi, 0.0, 1.1);
                    ++legCount;
                }
                const int pc = normalizePitchClass(mn.pitch);
                bool isChordTone = false;
                for (const auto& seg : segments) {
                    if (mn.onset_beats >= seg.startBeat - 0.1 && mn.onset_beats <= seg.endBeat + 0.1) {
                        const auto cpcs = chordPitchClasses(seg.chord, tonicPc, mode);
                        isChordTone = std::find(cpcs.begin(), cpcs.end(), pc) != cpcs.end();
                        break;
                    }
                }
                if (isChordTone) ++chordToneHits;
                else if (inScale[static_cast<size_t>(pc)]) ++tensionHits;
            }

            const double histTotal = std::accumulate(hist.begin(), hist.end(), 0.0);
            if (histTotal > 0.0) {
                for (double& v : hist) v /= histTotal;
                outMelody.interval_histogram = hist;
            }
            const double mTotal = static_cast<double>(mNotes.size());
            outMelody.chord_tone_ratio = std::clamp(static_cast<double>(chordToneHits) / mTotal, 0.15, 0.95);
            outMelody.tension_tone_ratio = std::clamp(static_cast<double>(tensionHits) / mTotal, 0.02, 0.65);
            outMelody.motif_density = std::clamp((mTotal / static_cast<double>(subBars)) / 8.0, 0.1, 1.0);
            outMelody.legato_ratio = legCount > 0 ? std::clamp(legSum / legCount, 0.15, 1.0) : 0.7;

            if (intervals.size() >= 4) {
                std::map<std::pair<int, int>, int> bigrams;
                for (size_t k = 1; k < intervals.size(); ++k) bigrams[{intervals[k - 1], intervals[k]}] += 1;
                int repeated = 0;
                for (const auto& [bg, c] : bigrams) if (c >= 2) repeated += c;
                outMelody.motif_repetition = std::clamp(
                    static_cast<double>(repeated) / static_cast<double>(intervals.size()), 0.1, 0.95);
            }
            if (!intervals.empty()) {
                const double meanStep = std::accumulate(intervals.begin(), intervals.end(), 0.0) / static_cast<double>(intervals.size());
                outMelody.arch_contour = std::clamp(0.5 + meanStep / 10.0, 0.1, 0.9);
            }
            std::sort(mpitches.begin(), mpitches.end());
            outMelody.register_low = std::clamp(static_cast<int>(percentileSorted(mpitches, 0.05)), 48, 84);
            outMelody.register_high = std::clamp(static_cast<int>(percentileSorted(mpitches, 0.95)),
                                                 outMelody.register_low + 7, 108);
        }
    };

    analyzeSubsetAxes(pitched, profile.harmony, profile.rhythm, profile.voicing, profile.melody);
    normalizeProfileDistributions(profile);

    // Task 4B: Automatic Intro / Chorus / Outro SectionDNA extraction
    double maxBeatAll = 0.0;
    for (const auto& n : pitched) maxBeatAll = std::max(maxBeatAll, n.onset_beats + n.duration_beats);
    const int totalBarsAll = std::max(1, static_cast<int>(std::ceil(maxBeatAll / beatsPerBar)));

    if (totalBarsAll >= 12) {
        std::vector<double> barEnergy(static_cast<size_t>(totalBarsAll), 0.0);
        for (const auto& n : pitched) {
            const int b = std::clamp(static_cast<int>(n.onset_beats / beatsPerBar), 0, totalBarsAll - 1);
            const double w = (n.channel == 3 || n.pitch < 48) ? 1.5 : ((n.channel == 2) ? 1.2 : 1.0);
            barEnergy[static_cast<size_t>(b)] += w * (0.5 + n.velocity / 127.0);
        }
        const double maxEnergy = *std::max_element(barEnergy.begin(), barEnergy.end());
        int introEnd = std::max(4, totalBarsAll / 5);
        for (int b = 4; b < introEnd; ++b) {
            if (maxEnergy > 0.0 && barEnergy[static_cast<size_t>(b)] > 0.75 * maxEnergy) {
                introEnd = b;
                break;
            }
        }
        const int outroStart = std::max(introEnd + 2, totalBarsAll - std::max(4, totalBarsAll / 5));

        std::vector<double> midEnergies;
        for (int b = introEnd; b < outroStart; ++b) midEnergies.push_back(barEnergy[static_cast<size_t>(b)]);
        std::sort(midEnergies.begin(), midEnergies.end());
        const double medMid = midEnergies.empty() ? 0.0 : midEnergies[midEnergies.size() / 2];

        auto collectBarsSlice = [&](const std::set<int>& allowedBars, double baseBeat) {
            std::vector<NoteEvent> slice;
            for (const auto& n : pitched) {
                const int b = std::clamp(static_cast<int>(n.onset_beats / beatsPerBar), 0, totalBarsAll - 1);
                if (allowedBars.count(b)) {
                    NoteEvent shifted = n;
                    shifted.onset_beats = std::max(0.0, n.onset_beats - baseBeat);
                    slice.push_back(shifted);
                }
            }
            return slice;
        };

        std::set<int> introBars, outroBars, chorusBars, midBodyBars;
        for (int b = 0; b < introEnd; ++b) introBars.insert(b);
        for (int b = outroStart; b < totalBarsAll; ++b) outroBars.insert(b);
        for (int b = introEnd; b < outroStart; ++b) {
            midBodyBars.insert(b);
            if (barEnergy[static_cast<size_t>(b)] >= medMid * 0.85) chorusBars.insert(b);
        }
        if (chorusBars.empty()) chorusBars = midBodyBars;

        const auto introSlice = collectBarsSlice(introBars, 0.0);
        const auto midSlice = collectBarsSlice(midBodyBars, static_cast<double>(introEnd) * beatsPerBar);
        const auto chorusSlice = collectBarsSlice(chorusBars, static_cast<double>(*chorusBars.begin()) * beatsPerBar);
        const auto outroSlice = collectBarsSlice(outroBars, static_cast<double>(outroStart) * beatsPerBar);

        if (midSlice.size() >= 12 && midSlice.size() * 5 >= pitched.size() * 2) {
            analyzeSubsetAxes(midSlice, profile.harmony, profile.rhythm, profile.voicing, profile.melody);
        }

        auto makeSection = [&](const std::vector<NoteEvent>& slice, SongSection secType) {
            if (slice.size() < 4) return deriveSectionDNA(profile, secType);
            SectionDNA sec;
            sec.harmony = profile.harmony;
            sec.rhythm = profile.rhythm;
            sec.voicing = profile.voicing;
            sec.melody = profile.melody;
            sec.populated = true;
            analyzeSubsetAxes(slice, sec.harmony, sec.rhythm, sec.voicing, sec.melody);
            return sec;
        };

        profile.sections["intro"] = makeSection(introSlice, SongSection::Intro);
        profile.sections["chorus"] = makeSection(chorusSlice, SongSection::Chorus);
        profile.sections["outro"] = makeSection(outroSlice, SongSection::Outro);
    } else {
        profile.sections["intro"] = deriveSectionDNA(profile, SongSection::Intro);
        profile.sections["chorus"] = deriveSectionDNA(profile, SongSection::Chorus);
        profile.sections["outro"] = deriveSectionDNA(profile, SongSection::Outro);
    }

    normalizeProfileDistributions(profile);
    return profile;
}

StyleProfile MidiStyleAnalyzer::analyzeCorpus(const std::vector<SongInput>& songs,
                                              const AnalysisOptions& options) {
    std::vector<StyleProfile> perSong;
    std::vector<double> bpms;
    for (const auto& s : songs) {
        if (s.notes.empty()) continue;
        AnalysisOptions songOpts = options;
        songOpts.time_sig_num = s.time_sig_num;
        songOpts.time_sig_den = s.time_sig_den;
        const double b = s.bpm > 20.0 ? s.bpm : (options.bpm_hint > 20.0 ? options.bpm_hint : 120.0);
        perSong.push_back(analyze(s.notes, b, songOpts));
        bpms.push_back(b);
    }

    if (perSong.empty()) {
        StyleProfile def = makeDefaultStyleProfile();
        if (!options.id.empty()) def.id = options.id;
        if (!options.name.empty()) def.name = options.name;
        if (!options.genre.empty()) def.genre = options.genre;
        if (!options.source.empty()) def.source = options.source;
        def.tags = options.tags;
        def.genre_source = options.genre_source;
        def.artist = options.artist;
        def.album = options.album;
        return def;
    }
    if (perSong.size() == 1) {
        return perSong.front();
    }

    StyleProfile out = perSong.front();
    out.track_count = static_cast<int>(perSong.size());
    std::sort(bpms.begin(), bpms.end());
    out.default_bpm = bpms[bpms.size() / 2];
    out.bpm_min = std::clamp(bpms.front() - 4.0, 40.0, out.default_bpm);
    out.bpm_max = std::clamp(bpms.back() + 4.0, out.default_bpm, 240.0);

    std::vector<std::pair<HarmonyAxis, double>> hItems;
    std::vector<std::pair<RhythmAxis, double>> rItems;
    std::vector<std::pair<VoicingAxis, double>> vItems;
    std::vector<std::pair<MelodyAxis, double>> mItems;
    for (const auto& sp : perSong) {
        hItems.emplace_back(sp.harmony, 1.0);
        rItems.emplace_back(sp.rhythm, 1.0);
        vItems.emplace_back(sp.voicing, 1.0);
        mItems.emplace_back(sp.melody, 1.0);
    }
    out.harmony = poolHarmonyAxes(hItems, options.key_override_pc, options.mode_override,
                                  perSong.front().harmony.tonic_pc, perSong.front().harmony.mode);
    out.rhythm = poolRhythmAxes(rItems);
    out.voicing = poolVoicingAxes(vItems);
    out.melody = poolMelodyAxes(mItems);

    // Pool Intro / Chorus / Outro SectionDNA across corpus
    for (SongSection secEnum : {SongSection::Intro, SongSection::Chorus, SongSection::Outro}) {
        const std::string key(songSectionName(secEnum));
        std::vector<std::pair<HarmonyAxis, double>> sh;
        std::vector<std::pair<RhythmAxis, double>> sr;
        std::vector<std::pair<VoicingAxis, double>> sv;
        std::vector<std::pair<MelodyAxis, double>> sm;
        for (const auto& sp : perSong) {
            const SectionDNA sd = resolveSectionDNA(sp, secEnum);
            sh.emplace_back(sd.harmony, 1.0);
            sr.emplace_back(sd.rhythm, 1.0);
            sv.emplace_back(sd.voicing, 1.0);
            sm.emplace_back(sd.melody, 1.0);
        }
        SectionDNA pooledSec;
        pooledSec.harmony = poolHarmonyAxes(sh, options.key_override_pc, options.mode_override,
                                            out.harmony.tonic_pc, out.harmony.mode);
        pooledSec.rhythm = poolRhythmAxes(sr);
        pooledSec.voicing = poolVoicingAxes(sv);
        pooledSec.melody = poolMelodyAxes(sm);
        pooledSec.populated = true;
        out.sections[key] = std::move(pooledSec);
    }

    normalizeProfileDistributions(out);
    return out;
}

StyleProfile MidiStyleAnalyzer::mergeProfiles(const StyleProfile& existing,
                                              const StyleProfile& incoming) {
    const double wA = static_cast<double>(std::max(1, existing.track_count));
    const double wB = static_cast<double>(std::max(1, incoming.track_count));

    StyleProfile out = existing;
    if (out.id.empty()) out.id = incoming.id;
    if (out.name.empty()) out.name = incoming.name;
    if (out.genre.empty()) out.genre = incoming.genre;
    out.track_count = std::max(1, existing.track_count) + std::max(1, incoming.track_count);

    out.bpm_min = std::min(existing.bpm_min, incoming.bpm_min);
    out.bpm_max = std::max(existing.bpm_max, incoming.bpm_max);
    out.default_bpm = (wA * existing.default_bpm + wB * incoming.default_bpm) / (wA + wB);

    // Deduplicated union of tags & processed_tracks
    auto unionStrings = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
        std::vector<std::string> res = a;
        for (const auto& s : b) {
            if (!s.empty() && std::find(res.begin(), res.end(), s) == res.end()) {
                res.push_back(s);
            }
        }
        return res;
    };
    out.tags = unionStrings(existing.tags, incoming.tags);
    out.processed_tracks = unionStrings(existing.processed_tracks, incoming.processed_tracks);

    out.harmony = poolHarmonyAxes({{existing.harmony, wA}, {incoming.harmony, wB}},
                                  std::nullopt, std::nullopt,
                                  existing.harmony.tonic_pc, existing.harmony.mode);
    out.rhythm = poolRhythmAxes({{existing.rhythm, wA}, {incoming.rhythm, wB}});
    out.voicing = poolVoicingAxes({{existing.voicing, wA}, {incoming.voicing, wB}});
    out.melody = poolMelodyAxes({{existing.melody, wA}, {incoming.melody, wB}});

    for (SongSection secEnum : {SongSection::Intro, SongSection::Chorus, SongSection::Outro}) {
        const std::string key(songSectionName(secEnum));
        const SectionDNA secA = resolveSectionDNA(existing, secEnum);
        const SectionDNA secB = resolveSectionDNA(incoming, secEnum);
        SectionDNA mergedSec;
        mergedSec.harmony = poolHarmonyAxes({{secA.harmony, wA}, {secB.harmony, wB}},
                                            std::nullopt, std::nullopt,
                                            out.harmony.tonic_pc, out.harmony.mode);
        mergedSec.rhythm = poolRhythmAxes({{secA.rhythm, wA}, {secB.rhythm, wB}});
        mergedSec.voicing = poolVoicingAxes({{secA.voicing, wA}, {secB.voicing, wB}});
        mergedSec.melody = poolMelodyAxes({{secA.melody, wA}, {secB.melody, wB}});
        mergedSec.populated = true;
        out.sections[key] = std::move(mergedSec);
    }

    normalizeProfileDistributions(out);
    return out;
}

} // namespace style_scaler
