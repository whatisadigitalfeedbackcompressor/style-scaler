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

StyleProfile MidiStyleAnalyzer::analyzeTimed(const std::vector<TimedNoteSec>& notes,
                                             const AnalysisOptions& options) {
    const double bpm = (options.bpm_hint > 20.0) ? options.bpm_hint : estimateBpm(notes);
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
        // Infer preliminary role channel from GM program / pitch when single-channel transcription
        if (ev.is_drum) {
            ev.channel = 10;
        } else if (ev.program >= 32 && ev.program <= 39) {
            ev.channel = 3; // Bass
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
    const double safeBpm = std::clamp(bpm > 20.0 ? bpm : 120.0, 40.0, 260.0);
    profile.default_bpm = safeBpm;
    profile.bpm_min = std::max(40.0, safeBpm - 8.0);
    profile.bpm_max = std::min(240.0, safeBpm + 8.0);

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

    // Separate roles into chords, melody, bass
    bool hasExplicitMultiChannel = false;
    {
        std::set<int> chans;
        for (const auto& n : pitched) {
            if (n.channel >= 1 && n.channel <= 3) chans.insert(n.channel);
        }
        hasExplicitMultiChannel = chans.size() >= 2;
    }

    std::vector<NoteEvent> chordNotes;
    std::vector<NoteEvent> melodyNotes;
    std::vector<NoteEvent> bassNotes;

    if (hasExplicitMultiChannel) {
        for (const auto& n : pitched) {
            if (n.channel == 2) melodyNotes.push_back(n);
            else if (n.channel == 3) bassNotes.push_back(n);
            else chordNotes.push_back(n);
        }
    } else {
        // Cluster simultaneous attacks (within 0.15 beats) to infer bass / chords / top melody
        size_t i = 0;
        while (i < pitched.size()) {
            size_t j = i + 1;
            while (j < pitched.size() && (pitched[j].onset_beats - pitched[i].onset_beats) <= 0.15) {
                ++j;
            }
            const size_t clusterCount = j - i;
            if (clusterCount >= 3) {
                // Lowest note below MIDI 48 goes to bass if wide gap, remaining form chord, optional top note to melody if high
                size_t startIdx = i;
                if (pitched[i].pitch < 48 && (pitched[i + 1].pitch - pitched[i].pitch) >= 5) {
                    bassNotes.push_back(pitched[i]);
                    startIdx = i + 1;
                }
                for (size_t k = startIdx; k < j; ++k) {
                    chordNotes.push_back(pitched[k]);
                }
                if (pitched[j - 1].pitch >= 68 && clusterCount >= 4) {
                    melodyNotes.push_back(pitched[j - 1]);
                }
            } else if (clusterCount == 2) {
                if (pitched[i].pitch < 50) {
                    bassNotes.push_back(pitched[i]);
                    if (pitched[i + 1].pitch >= 64) melodyNotes.push_back(pitched[i + 1]);
                    else chordNotes.push_back(pitched[i + 1]);
                } else {
                    chordNotes.push_back(pitched[i]);
                    chordNotes.push_back(pitched[i + 1]);
                }
            } else {
                const auto& single = pitched[i];
                if (single.pitch < 48 || (single.program >= 32 && single.program <= 39)) {
                    bassNotes.push_back(single);
                } else if (single.pitch >= 64 || single.duration_beats <= 1.0) {
                    melodyNotes.push_back(single);
                } else {
                    chordNotes.push_back(single);
                }
            }
            i = j;
        }
    }

    if (chordNotes.empty()) {
        for (const auto& n : pitched) {
            if (n.pitch >= 45) chordNotes.push_back(n);
        }
        if (chordNotes.empty()) chordNotes = pitched;
    }

    // 1. Key & Mode detection
    auto [detectedPc, detectedMode] = detectKeyAndMode(pitched);
    const int tonicPc = options.key_override_pc.value_or(detectedPc);
    const Mode mode = options.mode_override.value_or(detectedMode);
    profile.harmony.tonic_pc = tonicPc;
    profile.harmony.mode = mode;

    // Chromaticism: duration-weighted fraction outside scale
    {
        const auto sc = scaleIntervals(mode);
        std::array<bool, 12> inScale{};
        for (int iv : sc) inScale[static_cast<size_t>(normalizePitchClass(tonicPc + iv))] = true;
        double totalDur = 0.0, chromDur = 0.0;
        for (const auto& n : pitched) {
            totalDur += n.duration_beats;
            if (!inScale[static_cast<size_t>(normalizePitchClass(n.pitch))]) {
                chromDur += n.duration_beats;
            }
        }
        profile.harmony.chromaticism = totalDur > 0.0 ? std::clamp(chromDur / totalDur, 0.0, 1.0) : 0.1;
    }

    // 2. Chord segmentation & Roman Markov matrix
    const double beatsPerBar = static_cast<double>(profile.time_sig_num) * (4.0 / static_cast<double>(profile.time_sig_den));
    double maxBeat = 0.0;
    for (const auto& n : pitched) {
        maxBeat = std::max(maxBeat, n.onset_beats + n.duration_beats);
    }
    const int totalBars = std::max(1, static_cast<int>(std::ceil(maxBeat / beatsPerBar)));

    // Use 2-beat (half-bar) harmonic analysis windows
    const double windowBeats = std::max(1.0, beatsPerBar * 0.5);
    const int numWindows = std::max(1, static_cast<int>(std::ceil((totalBars * beatsPerBar) / windowBeats)));

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

        for (const auto& n : chordNotes) {
            const double overlap = std::min(n.onset_beats + n.duration_beats, wEnd) - std::max(n.onset_beats, wStart);
            if (overlap > 0.0) {
                chroma[static_cast<size_t>(normalizePitchClass(n.pitch))] += overlap;
            }
        }
        for (const auto& n : bassNotes) {
            const double overlap = std::min(n.onset_beats + n.duration_beats, wEnd) - std::max(n.onset_beats, wStart);
            if (overlap > 0.0 && n.pitch < lowestBassPitch) {
                lowestBassPitch = n.pitch;
                bassPc = normalizePitchClass(n.pitch);
            }
        }
        // Include bass slightly in chroma for root stability
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
        // Wrap-around loop transition from last chord back to first
        if (segments.size() >= 2) {
            counts[segments.back().symbol][segments.front().symbol] += 0.75;
        }

        profile.harmony.vocabulary.clear();
        for (const auto& [sym, entry] : vocabMap) {
            profile.harmony.vocabulary.push_back(entry);
        }
        std::sort(profile.harmony.vocabulary.begin(), profile.harmony.vocabulary.end(),
                  [](const ChordVocabEntry& a, const ChordVocabEntry& b) { return a.weight > b.weight; });

        // Build row-normalized transitions with light Dirichlet smoothing over observed vocabulary
        profile.harmony.transitions.clear();
        for (const auto& fromEntry : profile.harmony.vocabulary) {
            std::map<std::string, double> row;
            const auto it = counts.find(fromEntry.symbol);
            for (const auto& toEntry : profile.harmony.vocabulary) {
                double c = 0.05 * toEntry.weight; // prior smoothing
                if (it != counts.end()) {
                    const auto jt = it->second.find(toEntry.symbol);
                    if (jt != it->second.end()) {
                        c += jt->second;
                    }
                }
                if (toEntry.symbol == fromEntry.symbol && profile.harmony.vocabulary.size() > 1) {
                    c *= 0.25; // discourage stagnant self-loops unless dominant
                }
                row[toEntry.symbol] = c;
            }
            profile.harmony.transitions[fromEntry.symbol] = std::move(row);
        }
    }

    // 3. Rhythm extraction
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

        accumulateGrid(chordNotes, chordGrid, true);
        accumulateGrid(melodyNotes, melodyGrid, false);
        accumulateGrid(bassNotes, bassGrid, false);

        normalizeGridMax(chordGrid, defaultR.chord_onset_grid);
        normalizeGridMax(melodyGrid, defaultR.melody_onset_grid);
        normalizeGridMax(bassGrid, defaultR.bass_onset_grid);

        profile.rhythm.chord_onset_grid = chordGrid;
        profile.rhythm.melody_onset_grid = melodyGrid;
        profile.rhythm.bass_onset_grid = bassGrid;

        // Syncopation & anticipation across all pitched attack clusters
        double totalWeight = 0.0;
        double offBeatWeight = 0.0;
        double anticWeight = 0.0;
        for (int s = 0; s < 16; ++s) {
            const double w = chordGrid[static_cast<size_t>(s)] + 0.5 * bassGrid[static_cast<size_t>(s)];
            totalWeight += w;
            if (s % 4 != 0) offBeatWeight += w;
            if (s == 14 || s == 15) anticWeight += chordGrid[static_cast<size_t>(s)];
        }
        profile.rhythm.syncopation = totalWeight > 0.0 ? std::clamp(offBeatWeight / totalWeight, 0.0, 1.0) : 0.35;
        const double chordSum = std::accumulate(chordGrid.begin(), chordGrid.end(), 0.0);
        profile.rhythm.anticipation = chordSum > 0.0 ? std::clamp(anticWeight / chordSum * 2.5, 0.0, 1.0) : 0.2;

        // Swing estimate from off-beat 8ths (beat frac in [0.38, 0.72])
        double swingAccum = 0.0;
        int swingCount = 0;
        for (const auto& n : pitched) {
            const double beatFrac = std::fmod(std::max(0.0, n.onset_beats), 1.0);
            if (beatFrac >= 0.42 && beatFrac <= 0.72) {
                swingAccum += std::clamp((beatFrac - 0.5) / 0.167, 0.0, 1.0);
                ++swingCount;
            }
        }
        profile.rhythm.swing = swingCount >= 3 ? std::clamp(swingAccum / static_cast<double>(swingCount), 0.0, 1.0) : 0.0;

        // Harmonic pacing: chord changes per bar
        profile.rhythm.harmonic_pacing = std::clamp(
            static_cast<double>(std::max<size_t>(1, segments.size())) / static_cast<double>(totalBars),
            0.25, 4.0);

        // Chord sustain ratio
        if (!chordNotes.empty()) {
            std::vector<double> durs;
            for (const auto& n : chordNotes) durs.push_back(n.duration_beats);
            std::sort(durs.begin(), durs.end());
            const double medDur = percentileSorted(durs, 0.5);
            const double expectedBarSlice = beatsPerBar / std::max(0.5, profile.rhythm.harmonic_pacing);
            profile.rhythm.sustain_ratio = std::clamp(medDur / expectedBarSlice, 0.2, 1.15);
        }
    }

    // 4. Voicing extraction
    {
        std::vector<int> allChordPitches;
        std::vector<double> spreads;
        std::vector<int> counts;
        std::vector<int> lowestPerChord;
        int extendedChords = 0;
        int invertedChords = 0;
        int totalClusters = 0;
        std::vector<double> centroids;

        size_t i = 0;
        while (i < chordNotes.size()) {
            size_t j = i + 1;
            while (j < chordNotes.size() && (chordNotes[j].onset_beats - chordNotes[i].onset_beats) <= 0.15) {
                ++j;
            }
            if (j - i >= 2) {
                std::vector<int> pitches;
                for (size_t k = i; k < j; ++k) {
                    pitches.push_back(chordNotes[k].pitch);
                    allChordPitches.push_back(chordNotes[k].pitch);
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

                // Compare lowest chord pitch class against segment chord root at that onset
                const double tBeat = chordNotes[i].onset_beats;
                for (const auto& seg : segments) {
                    if (tBeat >= seg.startBeat - 0.1 && tBeat <= seg.endBeat + 0.1) {
                        const int rootPc = romanChordRootPitchClass(seg.chord, tonicPc);
                        if (normalizePitchClass(pitches.front()) != rootPc) {
                            ++invertedChords;
                        }
                        break;
                    }
                }
                ++totalClusters;
            }
            i = j;
        }

        if (!allChordPitches.empty()) {
            std::sort(allChordPitches.begin(), allChordPitches.end());
            profile.voicing.register_low = std::clamp(static_cast<int>(std::floor(percentileSorted(allChordPitches, 0.08))), 36, 72);
            profile.voicing.register_high = std::clamp(static_cast<int>(std::ceil(percentileSorted(allChordPitches, 0.92))),
                                                       profile.voicing.register_low + 8, 96);
            profile.voicing.center_pitch = static_cast<int>(std::llround(percentileSorted(allChordPitches, 0.5)));
        }
        if (!spreads.empty()) {
            profile.voicing.spread_semitones = std::clamp(
                std::accumulate(spreads.begin(), spreads.end(), 0.0) / static_cast<double>(spreads.size()),
                5.0, 30.0);
        }
        if (!counts.empty()) {
            std::sort(counts.begin(), counts.end());
            profile.voicing.min_notes = std::clamp(counts.front(), 2, 5);
            profile.voicing.max_notes = std::clamp(counts.back(), profile.voicing.min_notes, 6);
        }
        if (totalClusters > 0) {
            profile.voicing.extension_richness = std::clamp(static_cast<double>(extendedChords) / totalClusters, 0.0, 1.0);
            profile.voicing.inversion_prob = std::clamp(static_cast<double>(invertedChords) / totalClusters, 0.0, 1.0);
            profile.voicing.drop2_prob = std::clamp((profile.voicing.spread_semitones - 11.0) / 14.0, 0.0, 1.0);
        }
        if (centroids.size() >= 2) {
            double jumpSum = 0.0;
            for (size_t k = 1; k < centroids.size(); ++k) {
                jumpSum += std::abs(centroids[k] - centroids[k - 1]);
            }
            const double meanJump = jumpSum / static_cast<double>(centroids.size() - 1);
            profile.voicing.voice_leading_tightness = std::clamp(1.0 - (meanJump / 14.0), 0.1, 0.98);
        }
        if (!lowestPerChord.empty()) {
            std::sort(lowestPerChord.begin(), lowestPerChord.end());
            const int p10 = static_cast<int>(std::floor(percentileSorted(lowestPerChord, 0.10)));
            profile.voicing.sub_bass_clearance_midi = std::clamp(p10, 36, std::min(60, profile.voicing.register_high - 6));
        }

        if (!bassNotes.empty()) {
            std::vector<int> bp;
            for (const auto& n : bassNotes) bp.push_back(n.pitch);
            std::sort(bp.begin(), bp.end());
            profile.voicing.bass_register_low = std::clamp(static_cast<int>(percentileSorted(bp, 0.05)), 21, 50);
            profile.voicing.bass_register_high = std::clamp(static_cast<int>(percentileSorted(bp, 0.95)),
                                                            profile.voicing.bass_register_low + 5, 58);
        }
    }

    // 5. Melody extraction
    if (melodyNotes.size() >= 2) {
        std::array<double, 25> hist{};
        std::vector<int> mpitches;
        std::vector<int> intervals;
        int chordToneHits = 0;
        int tensionHits = 0;
        double legSum = 0.0;
        int legCount = 0;

        const auto sc = scaleIntervals(mode);
        std::array<bool, 12> inScale{};
        for (int iv : sc) inScale[static_cast<size_t>(normalizePitchClass(tonicPc + iv))] = true;

        for (size_t k = 0; k < melodyNotes.size(); ++k) {
            const auto& mn = melodyNotes[k];
            mpitches.push_back(mn.pitch);

            if (k > 0) {
                const int diff = std::clamp(mn.pitch - melodyNotes[k - 1].pitch, -12, 12);
                hist[static_cast<size_t>(diff + 12)] += 1.0;
                intervals.push_back(diff);

                const double ioi = std::max(0.0625, mn.onset_beats - melodyNotes[k - 1].onset_beats);
                legSum += std::clamp(melodyNotes[k - 1].duration_beats / ioi, 0.0, 1.1);
                ++legCount;
            }

            // Check chord tone vs scale tension tone
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
            profile.melody.interval_histogram = hist;
        }

        const double mTotal = static_cast<double>(melodyNotes.size());
        profile.melody.chord_tone_ratio = std::clamp(static_cast<double>(chordToneHits) / mTotal, 0.15, 0.95);
        profile.melody.tension_tone_ratio = std::clamp(static_cast<double>(tensionHits) / mTotal, 0.02, 0.65);

        const double notesPerBar = mTotal / static_cast<double>(totalBars);
        profile.melody.motif_density = std::clamp(notesPerBar / 8.0, 0.1, 1.0);
        profile.melody.legato_ratio = legCount > 0 ? std::clamp(legSum / legCount, 0.15, 1.0) : 0.7;

        // Motif repetition via 2-interval bigram reuse
        if (intervals.size() >= 4) {
            std::map<std::pair<int, int>, int> bigrams;
            for (size_t k = 1; k < intervals.size(); ++k) {
                bigrams[{intervals[k - 1], intervals[k]}] += 1;
            }
            int repeated = 0;
            for (const auto& [bg, c] : bigrams) {
                if (c >= 2) repeated += c;
            }
            profile.melody.motif_repetition = std::clamp(
                static_cast<double>(repeated) / static_cast<double>(intervals.size()), 0.1, 0.95);
        }

        if (!intervals.empty()) {
            const double meanStep = std::accumulate(intervals.begin(), intervals.end(), 0.0) / static_cast<double>(intervals.size());
            profile.melody.arch_contour = std::clamp(0.5 + meanStep / 10.0, 0.1, 0.9);
        }

        std::sort(mpitches.begin(), mpitches.end());
        profile.melody.register_low = std::clamp(static_cast<int>(percentileSorted(mpitches, 0.05)), 48, 84);
        profile.melody.register_high = std::clamp(static_cast<int>(percentileSorted(mpitches, 0.95)),
                                                  profile.melody.register_low + 7, 108);
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
    const double n = static_cast<double>(perSong.size());
    std::sort(bpms.begin(), bpms.end());
    out.default_bpm = bpms[bpms.size() / 2];
    out.bpm_min = std::clamp(bpms.front() - 4.0, 40.0, out.default_bpm);
    out.bpm_max = std::clamp(bpms.back() + 4.0, out.default_bpm, 240.0);

    // Harmony: pool Roman vocabulary and transitions across songs
    std::map<std::string, ChordVocabEntry> vocabMap;
    std::map<std::string, std::map<std::string, double>> transAccum;
    std::map<Mode, int> modeCounts;
    double chromSum = 0.0;

    for (const auto& sp : perSong) {
        modeCounts[sp.harmony.mode] += 1;
        chromSum += sp.harmony.chromaticism;
        for (const auto& v : sp.harmony.vocabulary) {
            auto& dst = vocabMap[v.symbol];
            dst.symbol = v.symbol;
            dst.chord = v.chord;
            dst.weight += v.weight;
            dst.cadence_weight += v.cadence_weight;
        }
        for (const auto& [fromSym, row] : sp.harmony.transitions) {
            for (const auto& [toSym, p] : row) {
                transAccum[fromSym][toSym] += p;
            }
        }
    }

    if (options.mode_override.has_value()) {
        out.harmony.mode = *options.mode_override;
    } else {
        int bestModeCount = -1;
        for (const auto& [m, cnt] : modeCounts) {
            if (cnt > bestModeCount) {
                bestModeCount = cnt;
                out.harmony.mode = m;
            }
        }
    }
    if (options.key_override_pc.has_value()) {
        out.harmony.tonic_pc = normalizePitchClass(*options.key_override_pc);
    }
    out.harmony.chromaticism = std::clamp(chromSum / n, 0.0, 1.0);

    out.harmony.vocabulary.clear();
    for (const auto& [sym, entry] : vocabMap) {
        out.harmony.vocabulary.push_back(entry);
    }
    std::sort(out.harmony.vocabulary.begin(), out.harmony.vocabulary.end(),
              [](const ChordVocabEntry& a, const ChordVocabEntry& b) { return a.weight > b.weight; });

    out.harmony.transitions.clear();
    for (const auto& fromEntry : out.harmony.vocabulary) {
        std::map<std::string, double> row;
        const auto it = transAccum.find(fromEntry.symbol);
        for (const auto& toEntry : out.harmony.vocabulary) {
            double val = 0.02 * toEntry.weight;
            if (it != transAccum.end()) {
                if (auto jt = it->second.find(toEntry.symbol); jt != it->second.end()) {
                    val += jt->second;
                }
            }
            row[toEntry.symbol] = val;
        }
        out.harmony.transitions[fromEntry.symbol] = std::move(row);
    }

    // Rhythm pooling
    std::array<double, 16> cg{}, mg{}, bg{};
    double syncSum = 0.0, antSum = 0.0, swingSum = 0.0, paceSum = 0.0, sustSum = 0.0;
    for (const auto& sp : perSong) {
        for (size_t i = 0; i < 16; ++i) {
            cg[i] += sp.rhythm.chord_onset_grid[i];
            mg[i] += sp.rhythm.melody_onset_grid[i];
            bg[i] += sp.rhythm.bass_onset_grid[i];
        }
        syncSum += sp.rhythm.syncopation;
        antSum += sp.rhythm.anticipation;
        swingSum += sp.rhythm.swing;
        paceSum += sp.rhythm.harmonic_pacing;
        sustSum += sp.rhythm.sustain_ratio;
    }
    const auto defR = makeDefaultStyleProfile().rhythm;
    normalizeGridMax(cg, defR.chord_onset_grid);
    normalizeGridMax(mg, defR.melody_onset_grid);
    normalizeGridMax(bg, defR.bass_onset_grid);
    out.rhythm.chord_onset_grid = cg;
    out.rhythm.melody_onset_grid = mg;
    out.rhythm.bass_onset_grid = bg;
    out.rhythm.syncopation = std::clamp(syncSum / n, 0.0, 1.0);
    out.rhythm.anticipation = std::clamp(antSum / n, 0.0, 1.0);
    out.rhythm.swing = std::clamp(swingSum / n, 0.0, 1.0);
    out.rhythm.harmonic_pacing = std::clamp(paceSum / n, 0.25, 4.0);
    out.rhythm.sustain_ratio = std::clamp(sustSum / n, 0.2, 1.15);

    // Voicing pooling
    double regLo = 0.0, regHi = 0.0, cent = 0.0, spread = 0.0;
    double ext = 0.0, drop2 = 0.0, inv = 0.0, vl = 0.0, clear = 0.0;
    double bLo = 0.0, bHi = 0.0;
    int minN = 6, maxN = 1;
    for (const auto& sp : perSong) {
        regLo += sp.voicing.register_low;
        regHi += sp.voicing.register_high;
        cent += sp.voicing.center_pitch;
        spread += sp.voicing.spread_semitones;
        ext += sp.voicing.extension_richness;
        drop2 += sp.voicing.drop2_prob;
        inv += sp.voicing.inversion_prob;
        vl += sp.voicing.voice_leading_tightness;
        clear += sp.voicing.sub_bass_clearance_midi;
        bLo += sp.voicing.bass_register_low;
        bHi += sp.voicing.bass_register_high;
        minN = std::min(minN, sp.voicing.min_notes);
        maxN = std::max(maxN, sp.voicing.max_notes);
    }
    out.voicing.register_low = std::clamp(static_cast<int>(std::llround(regLo / n)), 36, 72);
    out.voicing.register_high = std::clamp(static_cast<int>(std::llround(regHi / n)), out.voicing.register_low + 8, 96);
    out.voicing.center_pitch = std::clamp(static_cast<int>(std::llround(cent / n)),
                                          out.voicing.register_low, out.voicing.register_high);
    out.voicing.spread_semitones = std::clamp(spread / n, 5.0, 30.0);
    out.voicing.min_notes = std::clamp(minN, 2, 5);
    out.voicing.max_notes = std::clamp(maxN, out.voicing.min_notes, 6);
    out.voicing.extension_richness = std::clamp(ext / n, 0.0, 1.0);
    out.voicing.drop2_prob = std::clamp(drop2 / n, 0.0, 1.0);
    out.voicing.inversion_prob = std::clamp(inv / n, 0.0, 1.0);
    out.voicing.voice_leading_tightness = std::clamp(vl / n, 0.1, 0.98);
    out.voicing.sub_bass_clearance_midi = std::clamp(
        static_cast<int>(std::llround(clear / n)), 36, std::min(60, out.voicing.register_high - 6));
    out.voicing.bass_register_low = std::clamp(static_cast<int>(std::llround(bLo / n)), 21, 50);
    out.voicing.bass_register_high = std::clamp(static_cast<int>(std::llround(bHi / n)),
                                                out.voicing.bass_register_low + 5, 58);

    // Melody pooling
    std::array<double, 25> hist{};
    double ct = 0.0, tt = 0.0, md = 0.0, mr = 0.0, ac = 0.0, leg = 0.0, mLo = 0.0, mHi = 0.0;
    for (const auto& sp : perSong) {
        for (size_t i = 0; i < 25; ++i) hist[i] += sp.melody.interval_histogram[i];
        ct += sp.melody.chord_tone_ratio;
        tt += sp.melody.tension_tone_ratio;
        md += sp.melody.motif_density;
        mr += sp.melody.motif_repetition;
        ac += sp.melody.arch_contour;
        leg += sp.melody.legato_ratio;
        mLo += sp.melody.register_low;
        mHi += sp.melody.register_high;
    }
    out.melody.interval_histogram = hist;
    out.melody.chord_tone_ratio = std::clamp(ct / n, 0.15, 0.95);
    out.melody.tension_tone_ratio = std::clamp(tt / n, 0.02, 0.65);
    out.melody.motif_density = std::clamp(md / n, 0.1, 1.0);
    out.melody.motif_repetition = std::clamp(mr / n, 0.1, 0.95);
    out.melody.arch_contour = std::clamp(ac / n, 0.1, 0.9);
    out.melody.legato_ratio = std::clamp(leg / n, 0.15, 1.0);
    out.melody.register_low = std::clamp(static_cast<int>(std::llround(mLo / n)), 48, 84);
    out.melody.register_high = std::clamp(static_cast<int>(std::llround(mHi / n)), out.melody.register_low + 7, 108);

    normalizeProfileDistributions(out);
    return out;
}

} // namespace style_scaler
