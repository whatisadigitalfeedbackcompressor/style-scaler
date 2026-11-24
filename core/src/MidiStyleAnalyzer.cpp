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
    std::array<double, 12> bassChroma{};
    std::array<double, 12> downbeatBassChroma{};
    double bassSum = 0.0;
    double downbeatBassSum = 0.0;
    int firstBassPc = -1;
    double earliestBassOnset = 1e9;
    int earliestBassPitch = 128;

    for (const auto& n : notes) {
        if (n.is_drum) continue;
        const int pc = normalizePitchClass(n.pitch);
        const double weight = std::max(0.05, n.duration_beats) * (n.channel == 3 ? 1.4 : 1.0);
        chroma[static_cast<size_t>(pc)] += weight;

        const bool isBassLike = (n.channel == 3 || n.pitch < 50);
        if (isBassLike) {
            const double bW = std::max(0.05, n.duration_beats);
            bassChroma[static_cast<size_t>(pc)] += bW;
            bassSum += bW;
            if (n.onset_beats < earliestBassOnset - 0.05 ||
                (std::abs(n.onset_beats - earliestBassOnset) <= 0.05 && n.pitch < earliestBassPitch)) {
                earliestBassOnset = n.onset_beats;
                earliestBassPitch = n.pitch;
                firstBassPc = pc;
            }
            const double barPos = std::fmod(std::max(0.0, n.onset_beats), 4.0);
            if (barPos < 0.35 || barPos > 3.35) {
                downbeatBassChroma[static_cast<size_t>(pc)] += std::max(0.25, n.duration_beats);
                downbeatBassSum += std::max(0.25, n.duration_beats);
            }
        }
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
            if (tmpl.mode == Mode::Minor || tmpl.mode == Mode::Major) {
                score += 0.025;
            }
            if (pc == firstBassPc && earliestBassOnset < 2.0) {
                score += 0.08;
            }
            if (downbeatBassSum > 1e-6) {
                score += 0.08 * (downbeatBassChroma[static_cast<size_t>(pc)] / downbeatBassSum);
            }
            if (bassSum > 1e-6) {
                score += 0.10 * (bassChroma[static_cast<size_t>(pc)] / bassSum);
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
                               int tonicPc,
                               Mode mode) {
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

    const auto sc = scaleIntervals(mode);
    std::array<bool, 12> inScale{};
    for (int iv : sc) {
        inScale[static_cast<size_t>(normalizePitchClass(tonicPc + iv))] = true;
    }

    int bestRoot = tonicPc;
    ChordQuality bestQual = ChordQuality::Min;
    double bestScore = -1e9;

    for (int root = 0; root < 12; ++root) {
        for (ChordQuality q : kCandidateQualities) {
            const auto ivs = chordIntervals(q);
            std::array<bool, 12> inChord{};
            bool allDiatonic = true;
            for (int iv : ivs) {
                const int pc = normalizePitchClass(root + iv);
                inChord[static_cast<size_t>(pc)] = true;
                if (!inScale[static_cast<size_t>(pc)]) allDiatonic = false;
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

            int missingCoreTones = 0;
            for (size_t k = 0; k < std::min<size_t>(2, ivs.size()); ++k) {
                const int pc = normalizePitchClass(root + ivs[k]);
                if (windowChroma[static_cast<size_t>(pc)] < totalMass * 0.05) {
                    ++missingCoreTones;
                }
            }
            int missingExtTones = 0;
            for (size_t k = 3; k < ivs.size(); ++k) {
                const int pc = normalizePitchClass(root + ivs[k]);
                if (windowChroma[static_cast<size_t>(pc)] < totalMass * 0.08) {
                    ++missingExtTones;
                }
            }

            double score = (hit - 0.85 * miss) / totalMass;
            if (bassPc >= 0 && bassPc == root) {
                score += 0.32;
            }
            score += 0.04 * static_cast<double>(coveredTones);
            score -= 0.28 * static_cast<double>(missingCoreTones);
            score -= 0.22 * static_cast<double>(missingExtTones);
            score -= 0.065 * static_cast<double>(std::max<int>(0, static_cast<int>(ivs.size()) - 3));
            if (allDiatonic) {
                score += 0.10;
            }

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
    const bool isRnbOrGroove = containsAnyToken(meta, {"r&b", "rnb", "neo-soul", "soul", "funk", "disco"});
    if (isDnb && out < 115.0) {
        out = std::clamp(out * 2.0, 40.0, 240.0);
    } else if (isHiphop && out > 142.0) {
        out = std::clamp(out * 0.5, 40.0, 240.0);
    } else if (isRnbOrGroove && out > 146.0) {
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
    double bestRefWeight = -1.0;
    std::vector<std::string> bestRefProg;

    for (const auto& [h, wRaw] : items) {
        const double w = std::max(0.01, wRaw);
        wSum += w;
        modeWeights[h.mode] += w;
        chromSum += h.chromaticism * w;
        if (!h.reference_progression.empty() && w > bestRefWeight) {
            bestRefWeight = w;
            bestRefProg = h.reference_progression;
        }
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

    for (auto& [sym, entry] : vocabMap) {
        if (wSum > 0.0) {
            entry.weight /= wSum;
            entry.cadence_weight /= wSum;
        }
        out.vocabulary.push_back(entry);
    }
    std::sort(out.vocabulary.begin(), out.vocabulary.end(),
              [](const ChordVocabEntry& a, const ChordVocabEntry& b) { return a.weight > b.weight; });
    if (out.vocabulary.size() > 12) {
        out.vocabulary.resize(12);
    }
    double vocabW = 0.0, vocabC = 0.0;
    for (const auto& e : out.vocabulary) {
        vocabW += std::max(0.0, e.weight);
        vocabC += std::max(0.0, e.cadence_weight);
    }
    if (vocabW > 0.0) {
        for (auto& e : out.vocabulary) e.weight /= vocabW;
    }
    if (vocabC > 0.0) {
        for (auto& e : out.vocabulary) e.cadence_weight /= vocabC;
    }

    if (!bestRefProg.empty()) {
        bool allInVocab = true;
        for (const auto& rs : bestRefProg) {
            bool found = false;
            for (const auto& ve : out.vocabulary) {
                if (ve.symbol == rs) { found = true; break; }
            }
            if (!found) { allInVocab = false; break; }
        }
        if (allInVocab) {
            out.reference_progression = std::move(bestRefProg);
        }
    }

    for (const auto& fromEntry : out.vocabulary) {
        std::map<std::string, double> row;
        const auto it = transAccum.find(fromEntry.symbol);
        for (const auto& toEntry : out.vocabulary) {
            double val = 0.02 * toEntry.weight;
            if (it != transAccum.end()) {
                if (auto jt = it->second.find(toEntry.symbol); jt != it->second.end()) {
                    val += (wSum > 0.0 ? (jt->second / wSum) : jt->second);
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
    std::array<double, 16> cp{}, mp{}, bp{}, cp2{}, bp2{};
    double syncSum = 0.0, antSum = 0.0, swingSum = 0.0, paceSum = 0.0, sustSum = 0.0, wSum = 0.0;
    double cpWSum = 0.0, mpWSum = 0.0, bpWSum = 0.0, cp2WSum = 0.0, bp2WSum = 0.0;
    auto hasPat = [](const std::array<double, 16>& p) {
        for (double v : p) if (v > 1e-6) return true;
        return false;
    };
    for (const auto& [r, wRaw] : items) {
        const double w = std::max(0.01, wRaw);
        wSum += w;
        for (size_t i = 0; i < 16; ++i) {
            cg[i] += r.chord_onset_grid[i] * w;
            mg[i] += r.melody_onset_grid[i] * w;
            bg[i] += r.bass_onset_grid[i] * w;
        }
        if (hasPat(r.chord_pattern)) {
            cpWSum += w;
            for (size_t i = 0; i < 16; ++i) cp[i] += r.chord_pattern[i] * w;
        }
        if (hasPat(r.melody_pattern)) {
            mpWSum += w;
            for (size_t i = 0; i < 16; ++i) mp[i] += r.melody_pattern[i] * w;
        }
        if (hasPat(r.bass_pattern)) {
            bpWSum += w;
            for (size_t i = 0; i < 16; ++i) bp[i] += r.bass_pattern[i] * w;
        }
        if (hasPat(r.chord_pattern_bar2)) {
            cp2WSum += w;
            for (size_t i = 0; i < 16; ++i) cp2[i] += r.chord_pattern_bar2[i] * w;
        }
        if (hasPat(r.bass_pattern_bar2)) {
            bp2WSum += w;
            for (size_t i = 0; i < 16; ++i) bp2[i] += r.bass_pattern_bar2[i] * w;
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

    auto finalizePat = [](std::array<double, 16>& pat, double patWSum) {
        if (patWSum <= 0.0) return;
        double mx = 0.0;
        for (double v : pat) mx = std::max(mx, v);
        if (mx > 1e-9) {
            for (double& v : pat) {
                v = std::clamp(v / mx, 0.0, 1.0);
                if (v < 0.28) v = 0.0;
            }
        }
    };
    finalizePat(cp, cpWSum);
    finalizePat(mp, mpWSum);
    finalizePat(bp, bpWSum);
    finalizePat(cp2, cp2WSum);
    finalizePat(bp2, bp2WSum);
    out.chord_pattern = cp;
    out.melody_pattern = mp;
    out.bass_pattern = bp;
    out.chord_pattern_bar2 = cp2;
    out.bass_pattern_bar2 = bp2;

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
    double bestMotifW = -1.0;
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
        if (!m.reference_motif.empty() && w > bestMotifW) {
            bestMotifW = w;
            out.reference_motif = m.reference_motif;
        }
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

    // Score candidate integer BPMs from 70 to 185 using beat-grid alignment + natural tempo prior
    int bestBpm = 120;
    double bestWeighted = -1.0;
    std::array<double, 186> rawScores{};
    for (int bpm = 70; bpm <= 185; ++bpm) {
        const double spb = 60.0 / static_cast<double>(bpm);
        double score = 0.0;
        for (size_t i = 0; i < uniqueOnsets.size(); ++i) {
            const double beatPhase = std::fmod(uniqueOnsets[i] / spb, 1.0);
            const double eighthErr = std::abs(beatPhase * 2.0 - std::round(beatPhase * 2.0));
            score += std::exp(-eighthErr * eighthErr * 18.0);
            if (i > 0) {
                const double ioi = uniqueOnsets[i] - uniqueOnsets[i - 1];
                if (ioi >= 0.16 && ioi <= 2.2) {
                    const double beats = ioi / spb;
                    const double qErr = std::abs(beats * 2.0 - std::round(beats * 2.0));
                    score += 0.55 * std::exp(-qErr * qErr * 16.0);
                    if (beats >= 0.7 && beats <= 2.3) {
                        const double bErr = std::abs(beats - std::round(beats));
                        score += 0.35 * std::exp(-bErr * bErr * 14.0);
                    }
                }
            }
        }
        rawScores[static_cast<size_t>(bpm)] = score;
        const double prior = 1.0 + 0.10 * std::exp(-std::pow((static_cast<double>(bpm) - 110.0) / 36.0, 2.0));
        const double weighted = score * prior;
        if (weighted > bestWeighted) {
            bestWeighted = weighted;
            bestBpm = bpm;
        }
    }

    if (bestBpm > 148) {
        const int halfCenter = static_cast<int>(std::llround(static_cast<double>(bestBpm) * 0.5));
        int bestHalf = -1;
        double bestHalfRaw = -1.0;
        for (int b = std::max(70, halfCenter - 1); b <= std::min(185, halfCenter + 1); ++b) {
            if (rawScores[static_cast<size_t>(b)] > bestHalfRaw) {
                bestHalfRaw = rawScores[static_cast<size_t>(b)];
                bestHalf = b;
            }
        }
        if (bestHalf >= 70 && bestHalfRaw >= 0.80 * rawScores[static_cast<size_t>(bestBpm)]) {
            bestBpm = bestHalf;
        }
    }
    return static_cast<double>(bestBpm);
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
        } else if ((ev.program >= 52 && ev.program <= 54) || ev.program == 40 ||
                   (ev.program >= 56 && ev.program <= 87) || (ev.program >= 104 && ev.program <= 111)) {
            const bool isVoice = (ev.program >= 52 && ev.program <= 54);
            if (isVoice && (ev.pitch < 45 || ev.pitch > 92)) continue;
            if (!isVoice && (ev.pitch < 48 || ev.pitch > 96)) continue;
            ev.channel = 2; // Voice / Solo / Lead / Reed / Pipe / Pluck
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

        auto isVoiceProg = [](int prog) { return prog >= 52 && prog <= 54; };
        auto isLeadProg = [](int prog) {
            return prog == 40 || (prog >= 56 && prog <= 87) || (prog >= 104 && prog <= 111);
        };
        auto isValidMelNote = [&](const NoteEvent& n) {
            if (isVoiceProg(n.program)) return n.pitch >= 45 && n.pitch <= 92;
            if (n.program > 0 && isLeadProg(n.program)) return n.pitch >= 48 && n.pitch <= 96;
            return n.pitch >= 48 && n.pitch <= 104;
        };

        std::vector<NoteEvent> voiceNotes;
        std::vector<NoteEvent> explicitCh2Notes;
        std::vector<NoteEvent> explicitBassNotes;
        std::vector<NoteEvent> otherNotes;

        for (const auto& n : notes) {
            if (n.channel == 3 || (n.program >= 32 && n.program <= 39)) {
                explicitBassNotes.push_back(n);
            } else if (isVoiceProg(n.program)) {
                if (isValidMelNote(n)) {
                    NoteEvent vn = n;
                    vn.channel = 2;
                    voiceNotes.push_back(vn);
                }
            } else if (n.channel == 2 || isLeadProg(n.program)) {
                if (isValidMelNote(n)) {
                    NoteEvent mn = n;
                    mn.channel = 2;
                    explicitCh2Notes.push_back(mn);
                }
            } else {
                otherNotes.push_back(n);
            }
        }

        bassNotes = explicitBassNotes;
        const bool hasExplicitBass = !explicitBassNotes.empty();
        const size_t minExplicitCount = (notes.size() < 24) ? 2 : 4;

        if (voiceNotes.size() >= minExplicitCount) {
            melodyNotes = voiceNotes;
            for (const auto& en : explicitCh2Notes) {
                if (en.program == 80) melodyNotes.push_back(en);
                else otherNotes.push_back(en);
            }
            std::sort(melodyNotes.begin(), melodyNotes.end(), [](const NoteEvent& a, const NoteEvent& b) {
                if (a.onset_beats != b.onset_beats) return a.onset_beats < b.onset_beats;
                return a.pitch < b.pitch;
            });
            for (const auto& n : otherNotes) {
                if (!hasExplicitBass && n.pitch < 48) bassNotes.push_back(n);
                else chordNotes.push_back(n);
            }
        } else if (explicitCh2Notes.size() >= minExplicitCount) {
            melodyNotes = explicitCh2Notes;
            for (const auto& vn : voiceNotes) melodyNotes.push_back(vn);
            std::sort(melodyNotes.begin(), melodyNotes.end(), [](const NoteEvent& a, const NoteEvent& b) {
                if (a.onset_beats != b.onset_beats) return a.onset_beats < b.onset_beats;
                return a.pitch < b.pitch;
            });
            for (const auto& n : otherNotes) {
                if (!hasExplicitBass && n.pitch < 48) bassNotes.push_back(n);
                else chordNotes.push_back(n);
            }
        } else {
            // No explicit vocal/melody channel: keep harmony intact and extract a clean monophonic skyline
            std::vector<NoteEvent> remaining = otherNotes;
            remaining.insert(remaining.end(), explicitCh2Notes.begin(), explicitCh2Notes.end());
            remaining.insert(remaining.end(), voiceNotes.begin(), voiceNotes.end());
            std::sort(remaining.begin(), remaining.end(), [](const NoteEvent& a, const NoteEvent& b) {
                if (a.onset_beats != b.onset_beats) return a.onset_beats < b.onset_beats;
                return a.pitch < b.pitch;
            });

            size_t i = 0;
            while (i < remaining.size()) {
                size_t j = i + 1;
                while (j < remaining.size() && (remaining[j].onset_beats - remaining[i].onset_beats) <= 0.15) {
                    ++j;
                }
                const size_t clusterCount = j - i;
                if (clusterCount >= 3) {
                    size_t startIdx = i;
                    if (!hasExplicitBass && remaining[i].pitch < 48 && (remaining[i + 1].pitch - remaining[i].pitch) >= 5) {
                        bassNotes.push_back(remaining[i]);
                        startIdx = i + 1;
                    }
                    for (size_t k = startIdx; k < j; ++k) {
                        chordNotes.push_back(remaining[k]);
                    }
                } else if (clusterCount == 2) {
                    if (!hasExplicitBass && remaining[i].pitch < 50) {
                        bassNotes.push_back(remaining[i]);
                        chordNotes.push_back(remaining[i + 1]);
                    } else {
                        chordNotes.push_back(remaining[i]);
                        chordNotes.push_back(remaining[i + 1]);
                    }
                } else {
                    if (!hasExplicitBass && remaining[i].pitch < 48) {
                        bassNotes.push_back(remaining[i]);
                    } else {
                        chordNotes.push_back(remaining[i]);
                    }
                }
                i = j;
            }

            std::map<int, NoteEvent> bucketTop;
            for (const auto& n : remaining) {
                if (n.pitch < 58) continue;
                const int bucket = static_cast<int>(std::floor(n.onset_beats * 4.0 + 1e-6));
                auto it = bucketTop.find(bucket);
                if (it == bucketTop.end() || n.pitch > it->second.pitch) {
                    bucketTop[bucket] = n;
                }
            }
            for (const auto& [b, ev] : bucketTop) {
                if (!melodyNotes.empty() &&
                    melodyNotes.back().pitch == ev.pitch &&
                    (ev.onset_beats - melodyNotes.back().onset_beats) < 0.35) {
                    continue;
                }
                melodyNotes.push_back(ev);
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
                    const double durMult = (n.duration_beats >= 0.45) ? 1.35 : 0.60;
                    chroma[static_cast<size_t>(normalizePitchClass(n.pitch))] += overlap * durMult;
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
                chroma[static_cast<size_t>(bassPc)] += 0.95 * windowBeats;
            }
            const double mass = std::accumulate(chroma.begin(), chroma.end(), 0.0);
            if (mass <= 1e-6) continue;

            const RomanChord rc = matchChordToRoman(chroma, bassPc, tonicPc, mode);
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

            if (outHarmony.vocabulary.size() > 8) {
                outHarmony.vocabulary.resize(8);
            }

            auto normalizeHarmVocab = [&]() {
                double wSum = 0.0, cSum = 0.0;
                for (const auto& e : outHarmony.vocabulary) {
                    wSum += std::max(0.0, e.weight);
                    cSum += std::max(0.0, e.cadence_weight);
                }
                if (wSum > 0.0) {
                    for (auto& e : outHarmony.vocabulary) e.weight /= wSum;
                }
                if (cSum > 0.0) {
                    for (auto& e : outHarmony.vocabulary) e.cadence_weight /= cSum;
                }
            };
            normalizeHarmVocab();

            // Extract representative 4-bar / 8-bar reference_progression
            const double rawPacing = static_cast<double>(segments.size()) / static_cast<double>(subBars);
            const int slotsPerBar = (rawPacing >= 1.65) ? 2 : 1;
            const double slotDur = beatsPerBar / static_cast<double>(slotsPerBar);
            const int numProgSlots = subBars * slotsPerBar;
            std::vector<std::string> slotSyms;
            slotSyms.reserve(static_cast<size_t>(numProgSlots));
            for (int k = 0; k < numProgSlots; ++k) {
                const double s0 = static_cast<double>(k) * slotDur;
                const double s1 = s0 + slotDur;
                std::string bestSym = segments.front().symbol;
                double bestOv = -1.0;
                for (const auto& seg : segments) {
                    const double ov = std::min(seg.endBeat, s1) - std::max(seg.startBeat, s0);
                    if (ov > bestOv) {
                        bestOv = ov;
                        bestSym = seg.symbol;
                    }
                }
                slotSyms.push_back(std::move(bestSym));
            }

            if (!slotSyms.empty()) {
                auto vocabWeightOf = [&](const std::string& sym) -> double {
                    for (const auto& e : outHarmony.vocabulary) {
                        if (e.symbol == sym) return e.weight;
                    }
                    return 0.0;
                };
                auto scoreWindow = [&](size_t start, size_t len) -> std::pair<double, int> {
                    std::set<std::string> uniq;
                    double wAccum = 0.0;
                    bool hasTonic = false;
                    int changes = 0;
                    for (size_t k = 0; k < len; ++k) {
                        const auto& sym = slotSyms[start + k];
                        uniq.insert(sym);
                        wAccum += vocabWeightOf(sym);
                        if (k > 0 && sym != slotSyms[start + k - 1]) {
                            ++changes;
                        }
                        if (auto it = vocabMap.find(sym); it != vocabMap.end() &&
                            it->second.chord.degree == 1 && it->second.chord.accidental == 0) {
                            hasTonic = true;
                        }
                    }
                    int repeatHits = 0;
                    for (size_t other = 0; other + len <= slotSyms.size(); other += static_cast<size_t>(slotsPerBar)) {
                        if (other + len > start && other < start + len) continue;
                        int matches = 0;
                        for (size_t k = 0; k < len; ++k) {
                            if (slotSyms[start + k] == slotSyms[other + k]) ++matches;
                        }
                        if (static_cast<double>(matches) >= 0.75 * static_cast<double>(len)) {
                            ++repeatHits;
                        }
                    }
                    double scVal = 1.6 * static_cast<double>(std::min<size_t>(uniq.size(), 4));
                    scVal += 1.1 * static_cast<double>(std::min(changes, 3));
                    if (uniq.size() == 1 && vocabMap.size() > 1) scVal -= 12.0;
                    if (hasTonic) scVal += 1.0;
                    if (auto it = vocabMap.find(slotSyms[start]); it != vocabMap.end() &&
                        it->second.chord.degree == 1 && it->second.chord.accidental == 0) {
                        scVal += 0.8;
                    }
                    scVal += 1.2 * static_cast<double>(std::min(repeatHits, 4));
                    scVal += 0.35 * wAccum;
                    return {scVal, repeatHits};
                };

                const size_t winLen4 = std::min<size_t>(static_cast<size_t>(4 * slotsPerBar), slotSyms.size());
                size_t bestStart = 0;
                size_t bestLen = winLen4;
                double bestScore = -1e9;
                size_t bestDistinct = 0;

                for (size_t s = 0; s + winLen4 <= slotSyms.size(); s += static_cast<size_t>(slotsPerBar)) {
                    const auto [scVal, _rep] = scoreWindow(s, winLen4);
                    if (scVal > bestScore) {
                        bestScore = scVal;
                        bestStart = s;
                        std::set<std::string> u(slotSyms.begin() + static_cast<ptrdiff_t>(s),
                                                slotSyms.begin() + static_cast<ptrdiff_t>(s + winLen4));
                        bestDistinct = u.size();
                    }
                }
                if (slotsPerBar == 1 && slotSyms.size() >= 16) {
                    for (size_t s = 0; s + 8 <= slotSyms.size(); s += 4) {
                        const auto [scVal, rep] = scoreWindow(s, 8);
                        std::set<std::string> u(slotSyms.begin() + static_cast<ptrdiff_t>(s),
                                                slotSyms.begin() + static_cast<ptrdiff_t>(s + 8));
                        if (rep >= 1 && u.size() > bestDistinct && scVal >= bestScore - 0.5) {
                            bestStart = s;
                            bestLen = 8;
                            bestScore = scVal;
                            bestDistinct = u.size();
                        }
                    }
                }

                outHarmony.reference_progression.assign(
                    slotSyms.begin() + static_cast<ptrdiff_t>(bestStart),
                    slotSyms.begin() + static_cast<ptrdiff_t>(bestStart + bestLen));
                while (outHarmony.reference_progression.size() < 4 && !slotSyms.empty()) {
                    outHarmony.reference_progression.push_back(
                        outHarmony.reference_progression[outHarmony.reference_progression.size() % bestLen]);
                }

                // Guarantee every reference_progression chord exists in outHarmony.vocabulary
                bool addedRefChord = false;
                for (const auto& rs : outHarmony.reference_progression) {
                    const bool exists = std::any_of(
                        outHarmony.vocabulary.begin(), outHarmony.vocabulary.end(),
                        [&](const ChordVocabEntry& e) { return e.symbol == rs; });
                    if (!exists) {
                        if (auto it = vocabMap.find(rs); it != vocabMap.end()) {
                            outHarmony.vocabulary.push_back(it->second);
                            addedRefChord = true;
                        }
                    }
                }
                if (addedRefChord) {
                    normalizeHarmVocab();
                }
            }

            outHarmony.transitions.clear();
            for (const auto& fromEntry : outHarmony.vocabulary) {
                std::map<std::string, double> row;
                const auto it = counts.find(fromEntry.symbol);
                for (const auto& toEntry : outHarmony.vocabulary) {
                    double c = 0.15 * toEntry.weight; // light normalized prior preserves real transitions
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

            auto extractCrispBarPattern = [&](const std::vector<NoteEvent>& evs, bool dedupClusters, int maxHits, int barParity = -1) {
                std::array<double, 16> pat{};
                if (evs.empty()) return pat;

                std::map<int, uint16_t> barMasks;
                std::array<double, 16> stepCounts{};
                double lastOnset = -10.0;
                for (const auto& n : evs) {
                    if (dedupClusters && (n.onset_beats - lastOnset) <= 0.12) continue;
                    lastOnset = n.onset_beats;
                    const double pos = std::max(0.0, n.onset_beats);
                    const int barIdx = static_cast<int>(std::floor(pos / beatsPerBar));
                    if (barParity >= 0 && (barIdx % 2) != barParity) continue;
                    const double barPos = std::fmod(pos, beatsPerBar);
                    const int step = static_cast<int>(std::llround((barPos / beatsPerBar) * 16.0)) % 16;
                    barMasks[barIdx] |= static_cast<uint16_t>(1u << step);
                }
                if (barMasks.empty() || (barParity >= 0 && barMasks.size() < 2)) return pat;

                std::map<uint16_t, int> maskFreq;
                for (const auto& [b, mask] : barMasks) {
                    if (mask == 0) continue;
                    maskFreq[mask] += 1;
                    for (int s = 0; s < 16; ++s) {
                        if ((mask & (1u << s)) != 0) stepCounts[static_cast<size_t>(s)] += 1.0;
                    }
                }
                if (maskFreq.empty()) return pat;

                uint16_t modeMask = 0;
                int bestFreq = -1;
                for (const auto& [mask, cnt] : maskFreq) {
                    const int scScore = cnt * 10 + ((mask & 1u) ? 1 : 0);
                    if (scScore > bestFreq) {
                        bestFreq = scScore;
                        modeMask = mask;
                    }
                }

                const double activeBars = static_cast<double>(barMasks.size());
                double mx = 0.0;
                for (int s = 0; s < 16; ++s) {
                    const double freq = stepCounts[static_cast<size_t>(s)] / activeBars;
                    const double modeBit = ((modeMask & (1u << s)) != 0) ? 1.0 : 0.0;
                    pat[static_cast<size_t>(s)] = 0.60 * modeBit + 0.40 * freq;
                    mx = std::max(mx, pat[static_cast<size_t>(s)]);
                }
                if (mx > 1e-6) {
                    for (double& v : pat) v /= mx;
                }
                if (stepCounts[0] > 0.0 && pat[0] < 0.85) {
                    pat[0] = 0.85;
                }
                for (double& v : pat) {
                    if (v < 0.30) v = 0.0;
                }
                std::vector<std::pair<double, int>> ranked;
                for (int s = 0; s < 16; ++s) {
                    if (pat[static_cast<size_t>(s)] > 0.0) ranked.emplace_back(pat[static_cast<size_t>(s)], s);
                }
                if (static_cast<int>(ranked.size()) > maxHits) {
                    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                    std::array<bool, 16> keep{};
                    for (int k = 0; k < maxHits; ++k) keep[static_cast<size_t>(ranked[static_cast<size_t>(k)].second)] = true;
                    for (int s = 0; s < 16; ++s) {
                        if (!keep[static_cast<size_t>(s)]) pat[static_cast<size_t>(s)] = 0.0;
                    }
                }
                return pat;
            };

            accumulateGrid(cNotes, chordGrid, true);
            accumulateGrid(mNotes, melodyGrid, false);
            accumulateGrid(bNotes, bassGrid, false);

            normalizeGridMax(chordGrid, defaultR.chord_onset_grid);
            normalizeGridMax(melodyGrid, defaultR.melody_onset_grid);
            normalizeGridMax(bassGrid, defaultR.bass_onset_grid);

            auto patternsDiffer = [](const std::array<double, 16>& a, const std::array<double, 16>& b) {
                double sumA = 0.0, sumB = 0.0;
                bool differs = false;
                for (size_t s = 0; s < 16; ++s) {
                    sumA += a[s];
                    sumB += b[s];
                    if (std::abs(a[s] - b[s]) > 0.25) differs = true;
                }
                return sumA > 0.5 && sumB > 0.5 && differs;
            };

            outRhythm.chord_onset_grid = chordGrid;
            outRhythm.melody_onset_grid = melodyGrid;
            outRhythm.bass_onset_grid = bassGrid;
            outRhythm.chord_pattern = extractCrispBarPattern(cNotes, true, 6);
            outRhythm.bass_pattern = extractCrispBarPattern(bNotes, false, 6);
            outRhythm.melody_pattern = extractCrispBarPattern(mNotes, false, 8);

            const auto evenChordPat = extractCrispBarPattern(cNotes, true, 6, 0);
            const auto oddChordPat = extractCrispBarPattern(cNotes, true, 6, 1);
            if (patternsDiffer(evenChordPat, oddChordPat)) {
                outRhythm.chord_pattern = evenChordPat;
                outRhythm.chord_pattern_bar2 = oddChordPat;
            }
            const auto evenBassPat = extractCrispBarPattern(bNotes, false, 6, 0);
            const auto oddBassPat = extractCrispBarPattern(bNotes, false, 6, 1);
            if (patternsDiffer(evenBassPat, oddBassPat)) {
                outRhythm.bass_pattern = evenBassPat;
                outRhythm.bass_pattern_bar2 = oddBassPat;
            }

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
                std::vector<double> clusterOnsets;
                double lastOn = -10.0;
                for (const auto& n : cNotes) {
                    durs.push_back(n.duration_beats);
                    if ((n.onset_beats - lastOn) > 0.12) {
                        clusterOnsets.push_back(n.onset_beats);
                        lastOn = n.onset_beats;
                    }
                }
                std::sort(durs.begin(), durs.end());
                const double medDur = percentileSorted(durs, 0.5);
                double medIoi = beatsPerBar / std::max(0.5, outRhythm.harmonic_pacing);
                if (clusterOnsets.size() >= 2) {
                    std::vector<double> iois;
                    for (size_t k = 1; k < clusterOnsets.size(); ++k) {
                        const double dt = clusterOnsets[k] - clusterOnsets[k - 1];
                        if (dt >= 0.12 && dt <= beatsPerBar * 2.0) iois.push_back(dt);
                    }
                    if (!iois.empty()) {
                        std::sort(iois.begin(), iois.end());
                        medIoi = std::clamp(percentileSorted(iois, 0.5), 0.5, beatsPerBar);
                    }
                }
                outRhythm.sustain_ratio = std::clamp(medDur / std::max(0.25, medIoi), 0.45, 0.96);
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

            // Phrase arch contour: middle of 2-bar phrases vs edges
            if (mNotes.size() >= 4) {
                double archAccum = 0.0;
                int archPhrases = 0;
                const double winBeats = 2.0 * beatsPerBar;
                for (int b = 0; b < subBars; b += 2) {
                    const double w0 = static_cast<double>(b) * beatsPerBar;
                    const double w1 = w0 + winBeats;
                    double midS = 0.0, edgeS = 0.0;
                    int midC = 0, edgeC = 0;
                    for (const auto& mn : mNotes) {
                        if (mn.onset_beats < w0 - 1e-4 || mn.onset_beats >= w1 - 1e-4) continue;
                        const double rel = (mn.onset_beats - w0) / winBeats;
                        if (rel >= 0.25 && rel <= 0.75) { midS += mn.pitch; ++midC; }
                        else { edgeS += mn.pitch; ++edgeC; }
                    }
                    if (midC > 0 && edgeC > 0) {
                        archAccum += (midS / midC) - (edgeS / edgeC);
                        ++archPhrases;
                    }
                }
                if (archPhrases > 0) {
                    outMelody.arch_contour = std::clamp(0.5 + (archAccum / archPhrases) / 12.0, 0.1, 0.9);
                }
            }

            // Extract representative 2-bar melodic contour motif (reference_motif)
            {
                std::vector<int> scaleMidi;
                for (int oct = 0; oct <= 10; ++oct) {
                    for (int iv : sc) {
                        const int mp = oct * 12 + normalizePitchClass(tonicPc + iv);
                        if (mp >= 0 && mp <= 127) scaleMidi.push_back(mp);
                    }
                }
                std::sort(scaleMidi.begin(), scaleMidi.end());
                auto pitchToScaleDeg = [&](int p) -> int {
                    int bestIdx = 0;
                    int bestDist = 999;
                    for (size_t idx = 0; idx < scaleMidi.size(); ++idx) {
                        const int d = std::abs(scaleMidi[idx] - p);
                        if (d < bestDist) {
                            bestDist = d;
                            bestIdx = static_cast<int>(idx);
                        }
                    }
                    return bestIdx;
                };

                struct QuantMelNote {
                    int step16 = 0;
                    int pitch = 60;
                    double durSteps = 2.0;
                    bool isCt = true;
                };

                const double winBeats = 2.0 * beatsPerBar;
                const int maxStartBar = std::max(0, subBars - 2);
                std::vector<QuantMelNote> bestWin;
                double bestWinScore = -1e9;

                for (int b = 0; b <= maxStartBar; ++b) {
                    const double w0 = static_cast<double>(b) * beatsPerBar;
                    const double w1 = w0 + winBeats;
                    std::map<int, QuantMelNote> byStep;
                    for (const auto& mn : mNotes) {
                        if (mn.onset_beats < w0 - 1e-4 || mn.onset_beats >= w1 - 1e-4) continue;
                        const int step32 = std::clamp(
                            static_cast<int>(std::llround(((mn.onset_beats - w0) / winBeats) * 32.0)),
                            0, 31);
                        bool isCt = true;
                        const int pc = normalizePitchClass(mn.pitch);
                        for (const auto& seg : segments) {
                            if (mn.onset_beats >= seg.startBeat - 0.1 && mn.onset_beats <= seg.endBeat + 0.1) {
                                const auto cpcs = chordPitchClasses(seg.chord, tonicPc, mode);
                                isCt = std::find(cpcs.begin(), cpcs.end(), pc) != cpcs.end();
                                break;
                            }
                        }
                        const double dSteps = std::clamp(mn.duration_beats * (16.0 / beatsPerBar), 1.0, 8.0);
                        auto it = byStep.find(step32);
                        if (it == byStep.end() || mn.pitch > it->second.pitch) {
                            byStep[step32] = QuantMelNote{step32, mn.pitch, dSteps, isCt};
                        }
                    }
                    if (byStep.size() < 3) continue;

                    std::vector<QuantMelNote> cand;
                    std::set<int> uniqP;
                    int minP = 128, maxP = -1, unisons = 0, inSc = 0, cts = 0;
                    int prevP = -1;
                    for (const auto& [st, qn] : byStep) {
                        cand.push_back(qn);
                        uniqP.insert(qn.pitch);
                        minP = std::min(minP, qn.pitch);
                        maxP = std::max(maxP, qn.pitch);
                        if (prevP == qn.pitch) ++unisons;
                        prevP = qn.pitch;
                        if (inScale[static_cast<size_t>(normalizePitchClass(qn.pitch))]) ++inSc;
                        if (qn.isCt) ++cts;
                    }
                    const double nCand = static_cast<double>(cand.size());
                    const double span = static_cast<double>(maxP - minP);
                    const double unisonRatio = static_cast<double>(unisons) / std::max(1.0, nCand - 1.0);
                    double scVal = 0.8 * static_cast<double>(std::min<size_t>(uniqP.size(), 6));
                    if (cand.size() >= 4 && cand.size() <= 14) scVal += 2.0;
                    if (span >= 3.0 && span <= 16.0) scVal += 2.0;
                    scVal += 2.5 * (static_cast<double>(inSc) / nCand);
                    scVal += 1.5 * (static_cast<double>(cts) / nCand);
                    scVal -= 2.2 * unisonRatio;

                    if (scVal > bestWinScore) {
                        bestWinScore = scVal;
                        bestWin = std::move(cand);
                    }
                }

                if (bestWin.size() >= 3) {
                    if (bestWin.size() > 16) bestWin.resize(16);
                    std::vector<int> degs;
                    degs.reserve(bestWin.size());
                    for (const auto& qn : bestWin) degs.push_back(pitchToScaleDeg(qn.pitch));
                    std::vector<int> sortedDegs = degs;
                    std::sort(sortedDegs.begin(), sortedDegs.end());
                    const int anchorDeg = sortedDegs[sortedDegs.size() / 2];

                    outMelody.reference_motif.clear();
                    for (size_t k = 0; k < bestWin.size(); ++k) {
                        MelodyMotifStep ms;
                        ms.step16 = bestWin[k].step16;
                        ms.scale_degree_offset = std::clamp(degs[k] - anchorDeg, -10, 10);
                        ms.duration_steps = bestWin[k].durSteps;
                        ms.is_chord_tone = bestWin[k].isCt;
                        outMelody.reference_motif.push_back(ms);
                    }
                }
            }

            std::sort(mpitches.begin(), mpitches.end());
            outMelody.register_low = std::clamp(static_cast<int>(percentileSorted(mpitches, 0.05)), 48, 84);
            outMelody.register_high = std::clamp(static_cast<int>(percentileSorted(mpitches, 0.95)),
                                                 outMelody.register_low + 7, 108);
        }

        // Prevent inverted high-stab chord voicings (e.g. brass/synth hits at 70..86 above baritone vocals at 60..71)
        if (outVoicing.register_low >= outMelody.register_high - 4 && outVoicing.register_low >= 56) {
            const int drop = std::min(12, outVoicing.register_low - std::max(44, outVoicing.bass_register_high + 4));
            if (drop > 0) {
                outVoicing.register_low -= drop;
                outVoicing.register_high = std::max(outVoicing.register_low + 8, outVoicing.register_high - drop);
                outVoicing.center_pitch = std::clamp(outVoicing.center_pitch - drop, outVoicing.register_low, outVoicing.register_high);
                outVoicing.sub_bass_clearance_midi = std::clamp(
                    outVoicing.sub_bass_clearance_midi - drop,
                    std::max(36, outVoicing.bass_register_high + 1),
                    std::min(60, outVoicing.register_high - 6));
            }
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
            if (secType == SongSection::Chorus) {
                SectionDNA sec;
                sec.harmony = profile.harmony;
                sec.rhythm = profile.rhythm;
                sec.voicing = profile.voicing;
                sec.melody = profile.melody;
                sec.populated = true;
                if (slice.size() >= 12) {
                    analyzeSubsetAxes(slice, sec.harmony, sec.rhythm, sec.voicing, sec.melody);
                }
                if (sec.voicing.register_high > sec.melody.register_high - 3) {
                    sec.voicing.register_high = std::max(sec.voicing.register_low + 8, sec.melody.register_high - 3);
                    sec.voicing.center_pitch = std::clamp(sec.voicing.center_pitch, sec.voicing.register_low, sec.voicing.register_high);
                }
                return sec;
            }

            SectionDNA sec = deriveSectionDNA(profile, secType);
            if (slice.size() >= 8) {
                HarmonyAxis tmpH = profile.harmony;
                RhythmAxis tmpR = profile.rhythm;
                VoicingAxis tmpV = profile.voicing;
                MelodyAxis tmpM = profile.melody;
                analyzeSubsetAxes(slice, tmpH, tmpR, tmpV, tmpM);
                if (secType == SongSection::Intro) {
                    sec.rhythm.sustain_ratio = std::clamp(std::max(sec.rhythm.sustain_ratio, tmpR.sustain_ratio), 0.65, 0.96);
                    sec.melody.motif_density = std::clamp(
                        std::min(sec.melody.motif_density, tmpM.motif_density),
                        0.12,
                        std::max(0.12, profile.melody.motif_density * 0.75));
                } else if (secType == SongSection::Outro) {
                    sec.rhythm.sustain_ratio = std::clamp(std::max(sec.rhythm.sustain_ratio, tmpR.sustain_ratio), 0.72, 0.96);
                    sec.melody.motif_density = std::clamp(
                        std::min(sec.melody.motif_density, tmpM.motif_density),
                        0.12,
                        std::max(0.12, profile.melody.motif_density * 0.80));
                }
            }
            return sec;
        };

        profile.sections["intro"] = makeSection(introSlice, SongSection::Intro);
        profile.sections["chorus"] = makeSection(chorusSlice, SongSection::Chorus);
        profile.sections["outro"] = makeSection(outroSlice, SongSection::Outro);
        if (profile.sections["chorus"].melody.motif_density < profile.sections["intro"].melody.motif_density) {
            profile.sections["intro"].melody.motif_density =
                std::clamp(profile.sections["chorus"].melody.motif_density * 0.75, 0.1, 1.0);
        }
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
