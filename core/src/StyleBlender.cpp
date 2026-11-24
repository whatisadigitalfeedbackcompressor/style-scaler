#include "style_scaler/StyleBlender.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace style_scaler {

namespace {

double lerp(double a, double b, double t) noexcept {
    return a + (b - a) * t;
}

int lerpRoundInt(int a, int b, double t) noexcept {
    return static_cast<int>(std::llround(lerp(static_cast<double>(a), static_cast<double>(b), t)));
}

std::map<std::string, double> buildStationaryPrior(const HarmonyAxis& h) {
    std::map<std::string, double> prior;
    double sum = 0.0;
    for (const auto& entry : h.vocabulary) {
        prior[entry.symbol] += std::max(0.0, entry.weight);
        sum += std::max(0.0, entry.weight);
    }
    if (sum > 0.0) {
        for (auto& [k, v] : prior) v /= sum;
    }
    return prior;
}

HarmonyAxis blendHarmony(const HarmonyAxis& a, const HarmonyAxis& b, double w) {
    if (w <= 0.0) return a;
    if (w >= 1.0) return b;

    HarmonyAxis out;
    out.tonic_pc = (w < 0.5) ? a.tonic_pc : b.tonic_pc;
    out.mode = (w < 0.5) ? a.mode : b.mode;
    out.chromaticism = lerp(a.chromaticism, b.chromaticism, w);

    std::map<std::string, ChordVocabEntry> mergedVocab;
    for (const auto& e : a.vocabulary) {
        auto& dst = mergedVocab[e.symbol];
        dst.symbol = e.symbol;
        dst.chord = e.chord;
        dst.weight += (1.0 - w) * e.weight;
        dst.cadence_weight += (1.0 - w) * e.cadence_weight;
    }
    for (const auto& e : b.vocabulary) {
        auto& dst = mergedVocab[e.symbol];
        dst.symbol = e.symbol;
        dst.chord = e.chord;
        dst.weight += w * e.weight;
        dst.cadence_weight += w * e.cadence_weight;
    }

    for (const auto& [sym, entry] : mergedVocab) {
        out.vocabulary.push_back(entry);
    }
    std::sort(out.vocabulary.begin(), out.vocabulary.end(),
              [](const ChordVocabEntry& x, const ChordVocabEntry& y) { return x.weight > y.weight; });

    double vSum = 0.0, cSum = 0.0;
    for (const auto& e : out.vocabulary) {
        vSum += std::max(0.0, e.weight);
        cSum += std::max(0.0, e.cadence_weight);
    }
    if (vSum > 0.0) {
        for (auto& e : out.vocabulary) e.weight /= vSum;
    }
    if (cSum > 0.0) {
        for (auto& e : out.vocabulary) e.cadence_weight /= cSum;
    }

    if (!a.reference_progression.empty() && !b.reference_progression.empty()) {
        if (w < 0.35) {
            out.reference_progression = a.reference_progression;
        } else if (w > 0.65) {
            out.reference_progression = b.reference_progression;
        } else if (a.reference_progression.size() >= 4 && b.reference_progression.size() >= 4) {
            const size_t halfA = a.reference_progression.size() / 2;
            out.reference_progression.assign(
                a.reference_progression.begin(),
                a.reference_progression.begin() + static_cast<ptrdiff_t>(halfA));
            const size_t startB = std::min(halfA, b.reference_progression.size());
            out.reference_progression.insert(
                out.reference_progression.end(),
                b.reference_progression.begin() + static_cast<ptrdiff_t>(startB),
                b.reference_progression.end());
        } else {
            out.reference_progression = (w < 0.5) ? a.reference_progression : b.reference_progression;
        }
    } else if (!a.reference_progression.empty()) {
        out.reference_progression = a.reference_progression;
    } else if (!b.reference_progression.empty()) {
        out.reference_progression = b.reference_progression;
    }

    const auto priorA = buildStationaryPrior(a);
    const auto priorB = buildStationaryPrior(b);

    auto getRow = [](const HarmonyAxis& h, const std::map<std::string, double>& prior, const std::string& fromSym) {
        const auto it = h.transitions.find(fromSym);
        if (it != h.transitions.end() && !it->second.empty()) {
            return it->second;
        }
        return prior;
    };

    for (const auto& fromEntry : out.vocabulary) {
        const auto rowA = getRow(a, priorA, fromEntry.symbol);
        const auto rowB = getRow(b, priorB, fromEntry.symbol);

        std::map<std::string, double> blendedRow;
        double sum = 0.0;
        for (const auto& toEntry : out.vocabulary) {
            double pA = 0.0;
            double pB = 0.0;
            if (auto it = rowA.find(toEntry.symbol); it != rowA.end()) pA = it->second;
            if (auto it = rowB.find(toEntry.symbol); it != rowB.end()) pB = it->second;
            const double p = (1.0 - w) * pA + w * pB;
            if (p > 1e-6) {
                blendedRow[toEntry.symbol] = p;
                sum += p;
            }
        }
        if (sum > 0.0) {
            for (auto& [k, v] : blendedRow) v /= sum;
        } else {
            for (const auto& toEntry : out.vocabulary) {
                blendedRow[toEntry.symbol] = toEntry.weight;
                sum += toEntry.weight;
            }
            if (sum > 0.0) {
                for (auto& [k, v] : blendedRow) v /= sum;
            }
        }
        out.transitions[fromEntry.symbol] = std::move(blendedRow);
    }

    return out;
}

RhythmAxis blendRhythm(const RhythmAxis& a, const RhythmAxis& b, double w) {
    if (w <= 0.0) return a;
    if (w >= 1.0) return b;

    auto hasPat = [](const std::array<double, 16>& p) {
        for (double v : p) if (v > 1e-6) return true;
        return false;
    };
    auto blendPat = [&](const std::array<double, 16>& pa, const std::array<double, 16>& ga,
                        const std::array<double, 16>& pb, const std::array<double, 16>& gb) {
        std::array<double, 16> pat{};
        const bool ha = hasPat(pa), hb = hasPat(pb);
        if (!ha && !hb) return pat;
        const auto& srcA = ha ? pa : ga;
        const auto& srcB = hb ? pb : gb;
        double mx = 0.0;
        for (size_t i = 0; i < 16; ++i) {
            pat[i] = std::clamp(lerp(srcA[i], srcB[i], w), 0.0, 1.0);
            mx = std::max(mx, pat[i]);
        }
        if (mx > 1e-6) {
            for (double& v : pat) {
                v /= mx;
                if (v < 0.25) v = 0.0;
            }
        }
        return pat;
    };

    RhythmAxis out;
    out.subdivision = 16;
    for (size_t i = 0; i < 16; ++i) {
        out.chord_onset_grid[i] = std::clamp(lerp(a.chord_onset_grid[i], b.chord_onset_grid[i], w), 0.0, 1.0);
        out.melody_onset_grid[i] = std::clamp(lerp(a.melody_onset_grid[i], b.melody_onset_grid[i], w), 0.0, 1.0);
        out.bass_onset_grid[i] = std::clamp(lerp(a.bass_onset_grid[i], b.bass_onset_grid[i], w), 0.0, 1.0);
    }
    out.chord_pattern = blendPat(a.chord_pattern, a.chord_onset_grid, b.chord_pattern, b.chord_onset_grid);
    out.bass_pattern = blendPat(a.bass_pattern, a.bass_onset_grid, b.bass_pattern, b.bass_onset_grid);
    out.melody_pattern = blendPat(a.melody_pattern, a.melody_onset_grid, b.melody_pattern, b.melody_onset_grid);
    if (hasPat(a.chord_pattern_bar2) || hasPat(b.chord_pattern_bar2)) {
        out.chord_pattern_bar2 = blendPat(
            hasPat(a.chord_pattern_bar2) ? a.chord_pattern_bar2 : a.chord_pattern, a.chord_onset_grid,
            hasPat(b.chord_pattern_bar2) ? b.chord_pattern_bar2 : b.chord_pattern, b.chord_onset_grid);
    }
    if (hasPat(a.bass_pattern_bar2) || hasPat(b.bass_pattern_bar2)) {
        out.bass_pattern_bar2 = blendPat(
            hasPat(a.bass_pattern_bar2) ? a.bass_pattern_bar2 : a.bass_pattern, a.bass_onset_grid,
            hasPat(b.bass_pattern_bar2) ? b.bass_pattern_bar2 : b.bass_pattern, b.bass_onset_grid);
    }
    out.syncopation = std::clamp(lerp(a.syncopation, b.syncopation, w), 0.0, 1.0);
    out.anticipation = std::clamp(lerp(a.anticipation, b.anticipation, w), 0.0, 1.0);
    out.swing = std::clamp(lerp(a.swing, b.swing, w), 0.0, 1.0);
    out.harmonic_pacing = std::clamp(lerp(a.harmonic_pacing, b.harmonic_pacing, w), 0.25, 4.0);
    out.sustain_ratio = std::clamp(lerp(a.sustain_ratio, b.sustain_ratio, w), 0.1, 1.5);
    return out;
}

VoicingAxis blendVoicing(const VoicingAxis& a, const VoicingAxis& b, double w) {
    if (w <= 0.0) return a;
    if (w >= 1.0) return b;

    VoicingAxis out;
    out.sub_bass_clearance_midi = lerpRoundInt(a.sub_bass_clearance_midi, b.sub_bass_clearance_midi, w);
    out.register_low = lerpRoundInt(a.register_low, b.register_low, w);
    out.register_high = lerpRoundInt(a.register_high, b.register_high, w);
    out.register_low = std::max(out.register_low, out.sub_bass_clearance_midi);
    if (out.register_high <= out.register_low + 4) {
        out.register_high = std::min(120, out.register_low + 12);
    }
    out.center_pitch = std::clamp(lerpRoundInt(a.center_pitch, b.center_pitch, w),
                                  out.register_low, out.register_high);
    out.spread_semitones = lerp(a.spread_semitones, b.spread_semitones, w);
    out.min_notes = std::clamp(lerpRoundInt(a.min_notes, b.min_notes, w), 1, 6);
    out.max_notes = std::clamp(lerpRoundInt(a.max_notes, b.max_notes, w), out.min_notes, 7);
    out.extension_richness = std::clamp(lerp(a.extension_richness, b.extension_richness, w), 0.0, 1.0);
    out.drop2_prob = std::clamp(lerp(a.drop2_prob, b.drop2_prob, w), 0.0, 1.0);
    out.inversion_prob = std::clamp(lerp(a.inversion_prob, b.inversion_prob, w), 0.0, 1.0);
    out.voice_leading_tightness = std::clamp(lerp(a.voice_leading_tightness, b.voice_leading_tightness, w), 0.0, 1.0);
    out.bass_register_low = lerpRoundInt(a.bass_register_low, b.bass_register_low, w);
    out.bass_register_high = std::max(out.bass_register_low + 4, lerpRoundInt(a.bass_register_high, b.bass_register_high, w));
    return out;
}

MelodyAxis blendMelody(const MelodyAxis& a, const MelodyAxis& b, double w) {
    if (w <= 0.0) return a;
    if (w >= 1.0) return b;

    MelodyAxis out;
    double sum = 0.0;
    for (size_t i = 0; i < 25; ++i) {
        out.interval_histogram[i] = std::max(0.0, lerp(a.interval_histogram[i], b.interval_histogram[i], w));
        sum += out.interval_histogram[i];
    }
    if (sum > 0.0) {
        for (double& v : out.interval_histogram) v /= sum;
    }
    out.chord_tone_ratio = std::clamp(lerp(a.chord_tone_ratio, b.chord_tone_ratio, w), 0.0, 1.0);
    out.tension_tone_ratio = std::clamp(lerp(a.tension_tone_ratio, b.tension_tone_ratio, w), 0.0, 1.0);
    out.motif_density = std::clamp(lerp(a.motif_density, b.motif_density, w), 0.0, 1.0);
    out.motif_repetition = std::clamp(lerp(a.motif_repetition, b.motif_repetition, w), 0.0, 1.0);
    out.arch_contour = std::clamp(lerp(a.arch_contour, b.arch_contour, w), 0.0, 1.0);
    out.register_low = lerpRoundInt(a.register_low, b.register_low, w);
    out.register_high = std::max(out.register_low + 6, lerpRoundInt(a.register_high, b.register_high, w));
    out.legato_ratio = std::clamp(lerp(a.legato_ratio, b.legato_ratio, w), 0.1, 1.2);
    if (!a.reference_motif.empty() && !b.reference_motif.empty()) {
        if (w < 0.35) {
            out.reference_motif = a.reference_motif;
        } else if (w > 0.65) {
            out.reference_motif = b.reference_motif;
        } else {
            for (const auto& st : a.reference_motif) {
                if (st.step16 < 16) out.reference_motif.push_back(st);
            }
            for (const auto& st : b.reference_motif) {
                if (st.step16 >= 16) out.reference_motif.push_back(st);
            }
            if (out.reference_motif.empty()) {
                out.reference_motif = (w < 0.5) ? a.reference_motif : b.reference_motif;
            }
        }
    } else if (!a.reference_motif.empty()) {
        out.reference_motif = a.reference_motif;
    } else if (!b.reference_motif.empty()) {
        out.reference_motif = b.reference_motif;
    }
    return out;
}

} // namespace

StyleProfile StyleBlender::blend(const StyleProfile& slotA,
                                 const StyleProfile& slotB,
                                 const BlendParams& params) {
    const double wH = std::clamp(params.harmony, 0.0, 1.0);
    const double wR = std::clamp(params.rhythm, 0.0, 1.0);
    const double wV = std::clamp(params.voicing, 0.0, 1.0);
    const double wM = std::clamp(params.melody, 0.0, 1.0);

    StyleProfile out;
    out.schema_version = StyleProfile::kCurrentSchemaVersion;
    out.id = slotA.id + "__x__" + slotB.id;
    out.name = slotA.name + " x " + slotB.name;
    out.genre = (wH < 0.5) ? slotA.genre : slotB.genre;
    out.description = "Cross-genre blend: " + slotA.name + " foundation with " + slotB.name + " styling";
    out.source = "blended";

    out.bpm_min = lerp(slotA.bpm_min, slotB.bpm_min, wR);
    out.bpm_max = lerp(slotA.bpm_max, slotB.bpm_max, wR);
    out.default_bpm = lerp(slotA.default_bpm, slotB.default_bpm, wR);
    out.time_sig_num = slotA.time_sig_num;
    out.time_sig_den = slotA.time_sig_den;

    if (params.section == SongSection::Chorus) {
        out.harmony = blendHarmony(slotA.harmony, slotB.harmony, wH);
        out.rhythm = blendRhythm(slotA.rhythm, slotB.rhythm, wR);
        out.voicing = blendVoicing(slotA.voicing, slotB.voicing, wV);
        out.melody = blendMelody(slotA.melody, slotB.melody, wM);
    } else {
        const SectionDNA secA = resolveSectionDNA(slotA, params.section);
        const SectionDNA secB = resolveSectionDNA(slotB, params.section);
        out.harmony = blendHarmony(secA.harmony, secB.harmony, wH);
        out.rhythm = blendRhythm(secA.rhythm, secB.rhythm, wR);
        out.voicing = blendVoicing(secA.voicing, secB.voicing, wV);
        out.melody = blendMelody(secA.melody, secB.melody, wM);
    }

    if (params.key_override_pc.has_value()) {
        out.harmony.tonic_pc = normalizePitchClass(*params.key_override_pc);
    }
    if (params.mode_override.has_value()) {
        out.harmony.mode = *params.mode_override;
    }

    for (SongSection secEnum : {SongSection::Intro, SongSection::Chorus, SongSection::Outro}) {
        const SectionDNA sA = resolveSectionDNA(slotA, secEnum);
        const SectionDNA sB = resolveSectionDNA(slotB, secEnum);
        SectionDNA blendedSec;
        blendedSec.harmony = blendHarmony(sA.harmony, sB.harmony, wH);
        if (params.key_override_pc.has_value()) {
            blendedSec.harmony.tonic_pc = normalizePitchClass(*params.key_override_pc);
        }
        if (params.mode_override.has_value()) {
            blendedSec.harmony.mode = *params.mode_override;
        }
        blendedSec.rhythm = blendRhythm(sA.rhythm, sB.rhythm, wR);
        blendedSec.voicing = blendVoicing(sA.voicing, sB.voicing, wV);
        blendedSec.melody = blendMelody(sA.melody, sB.melody, wM);
        blendedSec.populated = true;
        out.sections[std::string(songSectionName(secEnum))] = std::move(blendedSec);
    }

    if ((wH > 0.0 && wH < 1.0) || (wM > 0.0 && wM < 1.0)) {
        normalizeProfileDistributions(out);
    }
    return out;
}

} // namespace style_scaler
