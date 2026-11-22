#include "style_scaler/GenerationEngine.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace style_scaler {

namespace {

uint64_t splitMix64(uint64_t& x) noexcept {
    uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

inline uint64_t rotl64(uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
}

size_t sampleWeightedIndex(const std::vector<double>& weights, DeterministicRng& rng) {
    if (weights.empty()) return 0;
    double total = 0.0;
    for (double w : weights) total += std::max(0.0, w);
    if (total <= 1e-12) {
        return static_cast<size_t>(rng.nextInt(0, static_cast<int>(weights.size() - 1)));
    }
    double target = rng.nextDouble() * total;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double w = std::max(0.0, weights[i]);
        if (target <= w) return i;
        target -= w;
    }
    return weights.size() - 1;
}

const ChordVocabEntry* findVocab(const StyleProfile& profile, const std::string& symbol) {
    for (const auto& entry : profile.harmony.vocabulary) {
        if (entry.symbol == symbol) return &entry;
    }
    return nullptr;
}

std::vector<int> computeBaseChordTones(const RomanChord& chord,
                                       int tonicPc,
                                       Mode mode,
                                       const VoicingAxis& voicing,
                                       double richnessOverride) {
    const int rootPc = romanChordRootPitchClass(chord, tonicPc);
    std::vector<int> ivs = chordIntervals(chord.quality);

    const double richness = (richnessOverride >= 0.0)
                                ? std::clamp(richnessOverride, 0.0, 1.0)
                                : voicing.extension_richness;

    // Explicit UI richness override can simplify to triads or expand to 7th/9th
    if (richnessOverride >= 0.0) {
        if (richness < 0.25) {
            if (ivs.size() > 3) ivs.resize(3);
        } else if (richness < 0.65) {
            if (ivs.size() == 3) {
                const bool minor = isMinorLikeQuality(chord.quality);
                ivs.push_back(minor ? 10 : (chord.degree == 5 ? 10 : 11));
            } else if (ivs.size() > 4) {
                ivs.resize(4);
            }
        } else {
            if (ivs.size() == 3) {
                const bool minor = isMinorLikeQuality(chord.quality);
                ivs.push_back(minor ? 10 : (chord.degree == 5 ? 10 : 11));
            }
            if (ivs.size() == 4) {
                const auto sc = scaleIntervals(mode);
                const int ninth = (chord.quality == ChordQuality::Min && mode == Mode::Phrygian && chord.degree == 1)
                                      ? 13
                                      : 14;
                (void)sc;
                ivs.push_back(ninth);
            }
        }
    } else if (richness >= 0.72 && ivs.size() == 3 && voicing.max_notes >= 4) {
        const bool minor = isMinorLikeQuality(chord.quality);
        ivs.push_back(minor ? 10 : (chord.degree == 5 ? 10 : 11));
    }

    const int maxTones = std::clamp(voicing.max_notes, 2, 6);
    if (static_cast<int>(ivs.size()) > maxTones) {
        ivs.resize(static_cast<size_t>(maxTones));
    }

    std::vector<int> pcs;
    pcs.reserve(ivs.size());
    for (int iv : ivs) {
        pcs.push_back(normalizePitchClass(rootPc + iv));
    }
    return pcs;
}

double computeVoiceLeadingDistance(const std::vector<int>& a, const std::vector<int>& b) {
    if (a.empty() || b.empty()) return 0.0;
    double total = 0.0;
    for (int p : b) {
        int best = 127;
        for (int q : a) {
            best = std::min(best, std::abs(p - q));
        }
        total += static_cast<double>(best);
    }
    return total / static_cast<double>(b.size());
}

} // namespace

DeterministicRng::DeterministicRng(uint64_t seed) noexcept {
    uint64_t sm = seed ^ 0xA5A5A5A5A5A5A5A5ULL;
    for (int i = 0; i < 4; ++i) {
        s_[i] = splitMix64(sm);
    }
}

uint64_t DeterministicRng::nextU64() noexcept {
    const uint64_t result = rotl64(s_[0] + s_[3], 23) + s_[0];
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl64(s_[3], 45);
    return result;
}

double DeterministicRng::nextDouble() noexcept {
    return static_cast<double>(nextU64() >> 11) * (1.0 / 9007199254740992.0);
}

double DeterministicRng::nextRange(double lo, double hi) noexcept {
    return lo + (hi - lo) * nextDouble();
}

int DeterministicRng::nextInt(int loInclusive, int hiInclusive) noexcept {
    if (loInclusive >= hiInclusive) return loInclusive;
    const uint64_t span = static_cast<uint64_t>(hiInclusive - loInclusive + 1);
    return loInclusive + static_cast<int>(nextU64() % span);
}

std::vector<std::pair<std::string, double>> ProgressionGenerator::nextChordProbabilities(
    const StyleProfile& profile,
    const std::string& currentSymbol) {
    std::map<std::string, double> probs;
    const auto it = profile.harmony.transitions.find(currentSymbol);
    if (it != profile.harmony.transitions.end() && !it->second.empty()) {
        probs = it->second;
    } else {
        for (const auto& v : profile.harmony.vocabulary) {
            probs[v.symbol] = std::max(0.01, v.weight);
        }
    }

    // Ensure every vocabulary item has a small non-zero floor if desired? Keep exact transitions, normalize sum.
    double sum = 0.0;
    for (const auto& [sym, p] : probs) sum += std::max(0.0, p);

    std::vector<std::pair<std::string, double>> out;
    out.reserve(probs.size());
    if (sum > 0.0) {
        for (const auto& [sym, p] : probs) {
            out.emplace_back(sym, std::max(0.0, p) / sum);
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    return out;
}

std::vector<ProgressionSlot> ProgressionGenerator::generateProgression(
    const StyleProfile& profile,
    int numBars,
    uint64_t seed,
    const std::vector<ProgressionSlot>& existingSlots,
    TempoScale tempoScale) {

    const int safeBars = std::clamp(numBars, 1, 32);
    const double beatsPerBar = static_cast<double>(profile.time_sig_num) * (4.0 / static_cast<double>(profile.time_sig_den));
    const double totalBeats = static_cast<double>(safeBars) * beatsPerBar;

    // Build slot timing layout
    std::vector<ProgressionSlot> slots;
    bool reuseExistingLayout = false;
    if (!existingSlots.empty()) {
        double maxEnd = 0.0;
        for (const auto& s : existingSlots) {
            maxEnd = std::max(maxEnd, s.start_beat + s.duration_beats);
        }
        if (std::abs(maxEnd - totalBeats) < 0.25) {
            reuseExistingLayout = true;
            slots = existingSlots;
        }
    }

    if (!reuseExistingLayout) {
        const double effectivePacing = profile.rhythm.harmonic_pacing * tempoScaleFactor(tempoScale);
        double slotDur = beatsPerBar;
        if (effectivePacing <= 0.65 && safeBars >= 2) {
            slotDur = beatsPerBar * 2.0;
        } else if (effectivePacing >= 1.65) {
            slotDur = beatsPerBar * 0.5;
        }

        // Preserve any locked slots by bar_index when resizing bar count
        std::map<int, ProgressionSlot> lockedByBar;
        for (const auto& es : existingSlots) {
            if (es.locked && es.bar_index >= 0 && es.bar_index < safeBars) {
                lockedByBar[es.bar_index] = es;
            }
        }

        const int numSlots = std::max(1, static_cast<int>(std::llround(totalBeats / slotDur)));
        const double actualDur = totalBeats / static_cast<double>(numSlots);
        for (int i = 0; i < numSlots; ++i) {
            ProgressionSlot s;
            s.start_beat = static_cast<double>(i) * actualDur;
            s.duration_beats = actualDur;
            s.bar_index = std::min(safeBars - 1, static_cast<int>(s.start_beat / beatsPerBar));

            if (auto it = lockedByBar.find(s.bar_index); it != lockedByBar.end() &&
                std::abs(s.start_beat - static_cast<double>(s.bar_index) * beatsPerBar) < 0.1) {
                s.roman_symbol = it->second.roman_symbol;
                s.chord = it->second.chord;
                s.locked = true;
                s.octave_shift = it->second.octave_shift;
                s.inversion_override = it->second.inversion_override;
                s.richness_override = it->second.richness_override;
                lockedByBar.erase(it);
            }
            slots.push_back(std::move(s));
        }
    }

    if (profile.harmony.vocabulary.empty()) {
        return slots;
    }

    DeterministicRng rng(seed);

    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].locked) continue;

        const bool isFirst = (i == 0);
        const bool isPhraseEnd = (i + 1 == slots.size()) ||
                                 (static_cast<int>((slots[i].start_beat + slots[i].duration_beats) / beatsPerBar) % 4 == 0 &&
                                  slots[i].duration_beats >= beatsPerBar * 0.5);

        std::vector<std::string> candidates;
        std::vector<double> weights;

        if (isFirst) {
            for (const auto& v : profile.harmony.vocabulary) {
                candidates.push_back(v.symbol);
                double w = std::max(0.01, v.weight);
                if (v.chord.degree == 1 && v.chord.accidental == 0) {
                    w *= 2.5; // anchor bar 1 strongly on tonic
                }
                weights.push_back(w);
            }
        } else {
            const auto probs = nextChordProbabilities(profile, slots[i - 1].roman_symbol);
            for (const auto& [sym, p] : probs) {
                double w = p;
                if (const auto* vc = findVocab(profile, sym)) {
                    if (isPhraseEnd) {
                        w *= (1.0 + 2.5 * vc->cadence_weight);
                    }
                    // Look ahead if the very next slot is locked so we voice-lead harmonically into it
                    if (i + 1 < slots.size() && slots[i + 1].locked) {
                        const auto nextProbs = nextChordProbabilities(profile, sym);
                        double linkBoost = 0.05;
                        for (const auto& [ns, np] : nextProbs) {
                            if (ns == slots[i + 1].roman_symbol) {
                                linkBoost += np;
                                break;
                            }
                        }
                        w *= linkBoost;
                    }
                }
                candidates.push_back(sym);
                weights.push_back(std::max(1e-4, w));
            }
        }

        if (candidates.empty()) {
            slots[i].roman_symbol = profile.harmony.vocabulary.front().symbol;
            slots[i].chord = profile.harmony.vocabulary.front().chord;
            continue;
        }

        const size_t picked = sampleWeightedIndex(weights, rng);
        const std::string& chosenSym = candidates[picked];
        slots[i].roman_symbol = chosenSym;
        if (const auto* vc = findVocab(profile, chosenSym)) {
            slots[i].chord = vc->chord;
        } else if (auto parsed = parseRomanChord(chosenSym)) {
            slots[i].chord = *parsed;
        }
    }

    return slots;
}

std::vector<ChordVocabEntry> ProgressionGenerator::suggestSubstitutes(
    const StyleProfile& profile,
    const ProgressionSlot& slot,
    size_t maxResults) {

    std::vector<ChordVocabEntry> subs;
    std::set<std::string> seen;
    seen.insert(slot.roman_symbol);

    auto addCandidate = [&](const RomanChord& rc, double w) {
        const std::string sym = formatRomanChord(rc);
        if (seen.insert(sym).second) {
            subs.push_back(ChordVocabEntry{sym, rc, w, 0.0});
        }
    };

    const auto& cur = slot.chord;
    // 1. Quality variations on the same root
    if (isMinorLikeQuality(cur.quality)) {
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::Min7, 0}, 0.95);
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::Min9, 0}, 0.92);
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::Min11, 0}, 0.88);
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::MinAdd9, 0}, 0.85);
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::Sus2, 0}, 0.80);
    } else {
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::Maj7, 0}, 0.95);
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::Maj9, 0}, 0.92);
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::Add9, 0}, 0.88);
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::Dom7, 0}, 0.84);
        addCandidate(RomanChord{cur.degree, cur.accidental, ChordQuality::Sus4, 0}, 0.80);
    }

    // 2. Relative / mediant / tritone functional substitutes
    if (cur.degree == 1) {
        addCandidate(RomanChord{3, -1, ChordQuality::Maj7, 0}, 0.78); // bIIImaj7
        addCandidate(RomanChord{6, -1, ChordQuality::Maj7, 0}, 0.76); // bVImaj7
    } else if (cur.degree == 5) {
        addCandidate(RomanChord{2, -1, ChordQuality::Dom7, 0}, 0.82); // tritone sub bII7
        addCandidate(RomanChord{7, -1, ChordQuality::Maj, 0}, 0.78);  // bVII modal dominant
        addCandidate(RomanChord{5, 0, ChordQuality::Dom7, 0}, 0.76);
    } else if (cur.degree == 4) {
        addCandidate(RomanChord{2, 0, ChordQuality::HDim7, 0}, 0.78);
        addCandidate(RomanChord{6, -1, ChordQuality::Maj9, 0}, 0.76);
    } else if (cur.degree == 6) {
        addCandidate(RomanChord{4, 0, ChordQuality::Min7, 0}, 0.78);
    }

    // 3. Remaining style vocabulary chords
    for (const auto& vc : profile.harmony.vocabulary) {
        if (seen.insert(vc.symbol).second) {
            subs.push_back(vc);
        }
    }

    if (subs.size() > maxResults) {
        subs.resize(maxResults);
    }
    return subs;
}

std::vector<int> VoicingEngine::voiceChord(
    const RomanChord& chord,
    int tonicPc,
    Mode mode,
    const VoicingAxis& voicing,
    const std::vector<int>& prevVoicing,
    double spreadOverride,
    int octaveShift,
    int inversionOverride,
    double richnessOverride) {

    const auto pcs = computeBaseChordTones(chord, tonicPc, mode, voicing, richnessOverride);
    if (pcs.empty()) return {};

    const int floorMidi = std::clamp(voicing.sub_bass_clearance_midi, 24, 72);
    const int targetCenter = std::clamp(voicing.center_pitch + octaveShift * 12, floorMidi + 4, 96);
    const double targetSpread = (spreadOverride >= 0.0)
                                    ? (6.0 + std::clamp(spreadOverride, 0.0, 1.0) * 20.0)
                                    : voicing.spread_semitones;

    const int nTones = static_cast<int>(pcs.size());
    std::vector<std::vector<int>> candidates;

    auto enforceClearanceAndSort = [&](std::vector<int> notes) -> std::vector<int> {
        for (int& n : notes) {
            while (n < floorMidi) n += 12;
            while (n > 108) n -= 12;
            if (n < floorMidi) n = floorMidi;
        }
        std::sort(notes.begin(), notes.end());
        notes.erase(std::unique(notes.begin(), notes.end()), notes.end());

        // If clearance collapsed an octave duplicate and dropped below min_notes, restore upper doubling
        const int minNotes = std::min(voicing.min_notes, nTones);
        size_t idx = 0;
        while (static_cast<int>(notes.size()) < minNotes && idx < pcs.size()) {
            int candidate = notes.empty() ? floorMidi + pcs[idx] : notes.back() + 12;
            const int targetPc = pcs[idx];
            // Pick octave near top of chord
            int base = (notes.empty() ? targetCenter : notes.back()) - 6;
            while (normalizePitchClass(base) != targetPc) ++base;
            while (base < floorMidi) base += 12;
            if (base <= 108 && std::find(notes.begin(), notes.end(), base) == notes.end()) {
                notes.push_back(base);
                std::sort(notes.begin(), notes.end());
            } else {
                candidate = base + 12;
                if (candidate <= 108 && std::find(notes.begin(), notes.end(), candidate) == notes.end()) {
                    notes.push_back(candidate);
                    std::sort(notes.begin(), notes.end());
                }
            }
            ++idx;
        }
        return notes;
    };

    // Build close inversions + drop-2 + open spread across octave centers
    for (int inv = 0; inv < nTones; ++inv) {
        if (inversionOverride >= 0 && inversionOverride <= 2 && inv != (inversionOverride % nTones)) {
            continue;
        }
        for (int baseOct = 2; baseOct <= 5; ++baseOct) {
            std::vector<int> close;
            int prevPitch = baseOct * 12 + pcs[static_cast<size_t>(inv)];
            close.push_back(prevPitch);
            for (int k = 1; k < nTones; ++k) {
                const int pc = pcs[static_cast<size_t>((inv + k) % nTones)];
                int p = prevPitch + 1;
                while (normalizePitchClass(p) != pc) ++p;
                close.push_back(p);
                prevPitch = p;
            }

            if (inversionOverride != 3) {
                candidates.push_back(enforceClearanceAndSort(close));
            }

            // Drop-2 voicing variant (drop second-highest voice down an octave)
            if (close.size() >= 4 && (inversionOverride == 3 || inversionOverride < 0)) {
                auto drop2 = close;
                drop2[drop2.size() - 2] -= 12;
                candidates.push_back(enforceClearanceAndSort(std::move(drop2)));
            }

            // Open / spread variant (lift 3rd / 5th up an octave for wide Liquid/Cinematic voicings)
            if (close.size() >= 3 && inversionOverride < 0) {
                auto openV = close;
                openV[1] += 12;
                candidates.push_back(enforceClearanceAndSort(std::move(openV)));
            }
        }
    }

    if (candidates.empty()) {
        std::vector<int> fallback;
        int base = targetCenter - 6;
        for (int pc : pcs) {
            int p = base;
            while (normalizePitchClass(p) != pc) ++p;
            while (p < floorMidi) p += 12;
            fallback.push_back(p);
            base = p + 1;
        }
        return enforceClearanceAndSort(std::move(fallback));
    }

    size_t bestIdx = 0;
    double bestCost = 1e18;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (c.empty()) continue;

        double sum = 0.0;
        for (int p : c) sum += static_cast<double>(p);
        const double centroid = sum / static_cast<double>(c.size());
        const double spread = static_cast<double>(c.back() - c.front());

        double cost = 0.9 * std::abs(centroid - static_cast<double>(targetCenter));
        cost += 0.55 * std::abs(spread - targetSpread);

        if (c.back() > std::max(voicing.register_high + octaveShift * 12, floorMidi + 24)) {
            cost += 1.5 * static_cast<double>(c.back() - voicing.register_high);
        }

        if (!prevVoicing.empty()) {
            const double vl = computeVoiceLeadingDistance(prevVoicing, c);
            cost += (0.5 + 2.2 * voicing.voice_leading_tightness) * vl;
        }

        // Encourage root or non-root according to inversion_prob when Auto
        if (inversionOverride < 0) {
            const bool isRootLowest = (normalizePitchClass(c.front()) == pcs.front());
            if (voicing.inversion_prob < 0.3 && !isRootLowest) cost += 2.5;
            if (voicing.drop2_prob > 0.45 && spread >= 13.0) cost -= 1.2;
        }

        if (cost < bestCost) {
            bestCost = cost;
            bestIdx = i;
        }
    }

    return candidates[bestIdx];
}

GeneratedPattern PatternGenerator::generatePattern(
    const StyleProfile& profile,
    const std::vector<ProgressionSlot>& progression,
    const PerformParams& perform,
    uint64_t seed) {

    GeneratedPattern out;
    out.progression = progression;
    const double beatsPerBar = static_cast<double>(profile.time_sig_num) * (4.0 / static_cast<double>(profile.time_sig_den));

    double totalBeats = 0.0;
    for (const auto& s : progression) {
        totalBeats = std::max(totalBeats, s.start_beat + s.duration_beats);
    }
    if (totalBeats <= 0.0) {
        totalBeats = 4.0 * beatsPerBar;
    }
    out.loop_length_beats = totalBeats;

    if (progression.empty()) {
        return out;
    }

    DeterministicRng rng(seed ^ 0x9988776655443322ULL);
    const double scale = tempoScaleFactor(perform.tempo_scale);
    const double stepBeats = 0.25 / scale; // 16th note at 1x, 8th at 0.5x, 32nd at 2x
    const double humanize = std::clamp(perform.humanize, 0.0, 1.0);

    // Pre-compute chord voicings for every slot with smooth voice leading
    std::vector<std::vector<int>> slotVoicings(progression.size());
    std::vector<int> prevVoicing;
    for (size_t s = 0; s < progression.size(); ++s) {
        slotVoicings[s] = VoicingEngine::voiceChord(
            progression[s].chord,
            profile.harmony.tonic_pc,
            profile.harmony.mode,
            profile.voicing,
            prevVoicing,
            perform.voicing_spread,
            progression[s].octave_shift,
            progression[s].inversion_override,
            progression[s].richness_override);
        prevVoicing = slotVoicings[s];
    }

    auto applySwingAndHumanize = [&](double rawBeat, int stepIdx16) -> double {
        double t = rawBeat;
        // Swing shifts odd 8th/16th subdivisions late
        if ((stepIdx16 % 4) == 2 && profile.rhythm.swing > 0.01) {
            t += profile.rhythm.swing * stepBeats * 0.66;
        } else if ((stepIdx16 % 2) == 1 && profile.rhythm.swing > 0.01) {
            t += profile.rhythm.swing * stepBeats * 0.25;
        }
        if (humanize > 1e-4) {
            t += rng.nextRange(-0.022, 0.022) * humanize;
        }
        return std::clamp(t, 0.0, totalBeats - 0.03);
    };

    auto findSlotAtBeat = [&](double beat) -> size_t {
        for (size_t s = 0; s < progression.size(); ++s) {
            if (beat >= progression[s].start_beat - 1e-6 &&
                beat < progression[s].start_beat + progression[s].duration_beats - 1e-6) {
                return s;
            }
        }
        return progression.size() - 1;
    };

    // --- 1. Channel 1: Chords ---
    if (perform.enable_chords) {
        for (size_t s = 0; s < progression.size(); ++s) {
            const auto& slot = progression[s];
            const auto& notes = slotVoicings[s];
            if (notes.empty()) continue;

            if (!perform.groove_sync) {
                const double dur = std::max(0.25, slot.duration_beats * std::clamp(profile.rhythm.sustain_ratio, 0.35, 0.98));
                for (size_t v = 0; v < notes.size(); ++v) {
                    const double strum = static_cast<double>(v) * 0.012 * humanize;
                    const int vel = std::clamp(static_cast<int>(std::llround(92.0 + rng.nextRange(-8.0, 8.0) * humanize)), 40, 120);
                    NoteEvent ev;
                    ev.onset_beats = std::min(totalBeats - 0.05, slot.start_beat + strum);
                    ev.duration_beats = std::min(dur, totalBeats - ev.onset_beats);
                    ev.pitch = notes[v];
                    ev.velocity = vel;
                    ev.channel = 1;
                    out.chords.push_back(ev);
                }
                continue;
            }

            // Groove-synced chord triggers over the slot's duration using 16-step grid scaled by tempoScale
            std::vector<std::pair<double, int>> onsets; // (beat, stepIdx16)
            const int nSteps = std::max(1, static_cast<int>(std::floor(slot.duration_beats / stepBeats + 1e-6)));

            // Optional anticipation push for slots after slot 0
            double firstOffsetShift = 0.0;
            if (s > 0 && profile.rhythm.anticipation > 0.15 && rng.nextDouble() < profile.rhythm.anticipation * 0.75) {
                firstOffsetShift = -std::min(stepBeats * 2.0, slot.start_beat * 0.5);
            }

            for (int k = 0; k < nSteps; ++k) {
                const double rawBeat = slot.start_beat + static_cast<double>(k) * stepBeats;
                // Map beat position to 16-step bar cycle taking tempo scale into account
                const double scaledBeat = rawBeat * scale;
                const int step16 = static_cast<int>(std::llround(std::fmod(scaledBeat, beatsPerBar) / 0.25)) % 16;
                const double gridProb = profile.rhythm.chord_onset_grid[static_cast<size_t>(step16)];

                bool trigger = false;
                if (k == 0) {
                    trigger = true; // guarantee harmonic anchor on chord change
                } else {
                    const double threshold = 0.55 - 0.30 * profile.rhythm.syncopation;
                    if (gridProb >= threshold && rng.nextDouble() < std::clamp(gridProb + 0.15, 0.0, 0.98)) {
                        trigger = true;
                    }
                }
                if (trigger) {
                    const double b = (k == 0) ? std::max(0.0, rawBeat + firstOffsetShift) : rawBeat;
                    onsets.emplace_back(b, step16);
                }
            }

            const double slotEnd = slot.start_beat + slot.duration_beats;
            for (size_t idx = 0; idx < onsets.size(); ++idx) {
                const double rawOnset = onsets[idx].first;
                const int step16 = onsets[idx].second;
                const double nextOnset = (idx + 1 < onsets.size()) ? onsets[idx + 1].first : slotEnd;
                const double maxAvail = std::max(0.10, nextOnset - rawOnset);
                const double dur = std::clamp(maxAvail * profile.rhythm.sustain_ratio, 0.10, maxAvail * 0.96);

                const double onset = applySwingAndHumanize(rawOnset, step16);
                const int baseVel = (step16 % 4 == 0) ? 98 : 84;
                for (size_t v = 0; v < notes.size(); ++v) {
                    const double strum = static_cast<double>(v) * 0.01 * humanize;
                    const int vel = std::clamp(static_cast<int>(std::llround(baseVel + rng.nextRange(-9.0, 9.0) * humanize)), 35, 124);
                    NoteEvent ev;
                    ev.onset_beats = std::min(totalBeats - 0.04, onset + strum);
                    ev.duration_beats = std::max(0.06, std::min(dur, totalBeats - ev.onset_beats));
                    ev.pitch = notes[v];
                    ev.velocity = vel;
                    ev.channel = 1;
                    out.chords.push_back(ev);
                }
            }
        }
    }

    // --- 2. Channel 2: Melody ---
    if (perform.enable_melody) {
        const auto scaleIvs = scaleIntervals(profile.harmony.mode);
        const int melLo = std::max(profile.melody.register_low, profile.voicing.sub_bass_clearance_midi + 7);
        const int melHi = std::max(melLo + 12, profile.melody.register_high);

        // Density combines profile motif_density and perform.melody_density
        const double densityFactor = std::clamp(0.35 + 1.15 * (0.5 * profile.melody.motif_density + 0.5 * perform.melody_density), 0.15, 1.4);

        // Build a 16-step motif pattern (onsets + interval direction hints)
        const int motifSteps = 16;
        std::vector<bool> motifMask(motifSteps, false);
        std::vector<int> motifStepsSigned(motifSteps, 0);
        for (int k = 0; k < motifSteps; ++k) {
            const double p = profile.rhythm.melody_onset_grid[static_cast<size_t>(k)] * densityFactor;
            if ((k == 0 && rng.nextDouble() < 0.75) || rng.nextDouble() < std::clamp(p * 0.72, 0.0, 0.92)) {
                motifMask[static_cast<size_t>(k)] = true;
            }
            // Draw interval step from interval_histogram
            std::vector<double> hw(profile.melody.interval_histogram.begin(), profile.melody.interval_histogram.end());
            const int rawInterval = static_cast<int>(sampleWeightedIndex(hw, rng)) - 12;
            motifStepsSigned[static_cast<size_t>(k)] = rawInterval;
        }

        // Build full timeline of melody onset times using motif repetition
        const int totalSteps = std::max(1, static_cast<int>(std::floor(totalBeats / stepBeats + 1e-6)));
        std::vector<std::pair<double, int>> melOnsets; // (beat, motifStepIdx)
        for (int g = 0; g < totalSteps; ++g) {
            const int motifIdx = g % motifSteps;
            bool fire = false;
            if (g < motifSteps) {
                fire = motifMask[static_cast<size_t>(motifIdx)];
            } else {
                const bool reuseMotif = rng.nextDouble() < profile.melody.motif_repetition;
                if (reuseMotif) {
                    fire = motifMask[static_cast<size_t>(motifIdx)];
                } else {
                    const double p = profile.rhythm.melody_onset_grid[static_cast<size_t>(motifIdx)] * densityFactor;
                    fire = rng.nextDouble() < std::clamp(p * 0.70, 0.0, 0.90);
                }
            }
            if (fire) {
                melOnsets.emplace_back(static_cast<double>(g) * stepBeats, g);
            }
        }

        int prevPitch = std::clamp((melLo + melHi) / 2, melLo, melHi);
        for (size_t i = 0; i < melOnsets.size(); ++i) {
            const double rawBeat = melOnsets[i].first;
            const int stepGlobal = melOnsets[i].second;
            const int step16 = stepGlobal % 16;
            const size_t slotIdx = findSlotAtBeat(rawBeat);
            const auto& slot = progression[slotIdx];
            const auto cpcs = chordPitchClasses(slot.chord, profile.harmony.tonic_pc, profile.harmony.mode);

            // Determine candidate pitches inside [melLo, melHi]
            std::vector<int> allowedPitches;
            const bool isStrongBeat = (stepGlobal % 4 == 0);
            const double targetChordToneP = isStrongBeat
                                                ? std::clamp(profile.melody.chord_tone_ratio + 0.15, 0.0, 0.98)
                                                : profile.melody.chord_tone_ratio;
            const bool pickChordTone = rng.nextDouble() < targetChordToneP;

            if (pickChordTone && !cpcs.empty()) {
                for (int p = melLo; p <= melHi; ++p) {
                    if (std::find(cpcs.begin(), cpcs.end(), normalizePitchClass(p)) != cpcs.end()) {
                        allowedPitches.push_back(p);
                    }
                }
            } else {
                for (int p = melLo; p <= melHi; ++p) {
                    const int rel = normalizePitchClass(p - profile.harmony.tonic_pc);
                    if (std::find(scaleIvs.begin(), scaleIvs.end(), rel) != scaleIvs.end()) {
                        allowedPitches.push_back(p);
                    }
                }
            }

            if (allowedPitches.empty()) {
                allowedPitches.push_back(prevPitch);
            }

            const int motifDelta = motifStepsSigned[static_cast<size_t>(step16)];
            const int contourBias = static_cast<int>(std::llround((profile.melody.arch_contour - 0.5) * 4.0));
            const int desiredPitch = std::clamp(prevPitch + motifDelta + contourBias, melLo, melHi);

            // Pick closest candidate pitch to desiredPitch, with slight random tie-break
            int chosen = allowedPitches.front();
            int bestDist = 999;
            for (int p : allowedPitches) {
                const int d = std::abs(p - desiredPitch);
                if (d < bestDist || (d == bestDist && rng.nextDouble() < 0.4)) {
                    bestDist = d;
                    chosen = p;
                }
            }

            const double nextBeat = (i + 1 < melOnsets.size()) ? melOnsets[i + 1].first : totalBeats;
            const double gap = std::max(0.10, nextBeat - rawBeat);
            const double dur = std::clamp(gap * profile.melody.legato_ratio, 0.08, std::min(gap * 0.95, 2.0));
            const double onset = applySwingAndHumanize(rawBeat, step16);

            const int baseVel = isStrongBeat ? 96 : 82;
            const int vel = std::clamp(static_cast<int>(std::llround(baseVel + rng.nextRange(-10.0, 10.0) * humanize)), 40, 125);

            NoteEvent ev;
            ev.onset_beats = onset;
            ev.duration_beats = std::max(0.06, std::min(dur, totalBeats - onset));
            ev.pitch = chosen;
            ev.velocity = vel;
            ev.channel = 2;
            out.melody.push_back(ev);

            prevPitch = chosen;
        }
    }

    // --- 3. Channel 3: Bass ---
    if (perform.enable_bass) {
        const int bassCeiling = std::max(28, std::min(profile.voicing.bass_register_high, profile.voicing.sub_bass_clearance_midi - 1));
        const int bassLo = std::clamp(profile.voicing.bass_register_low, 21, std::max(21, bassCeiling - 5));
        const int bassHi = std::max(bassLo + 5, bassCeiling);

        auto placeInBassRegister = [&](int pc) -> int {
            int p = bassLo;
            while (normalizePitchClass(p) != normalizePitchClass(pc)) ++p;
            while (p + 12 <= bassHi && p < bassLo + 3) p += 12;
            return std::clamp(p, bassLo, bassHi);
        };

        for (size_t s = 0; s < progression.size(); ++s) {
            const auto& slot = progression[s];
            const int rootPc = romanChordRootPitchClass(slot.chord, profile.harmony.tonic_pc);
            const int bassPc = normalizePitchClass(rootPc + slot.chord.bass_degree_offset);
            const int rootPitch = placeInBassRegister(bassPc);
            const int fifthPitch = placeInBassRegister(bassPc + 7);
            const int octPitch = (rootPitch + 12 <= bassHi) ? (rootPitch + 12) : rootPitch;

            const int nSteps = std::max(1, static_cast<int>(std::floor(slot.duration_beats / stepBeats + 1e-6)));
            std::vector<std::pair<double, int>> bassOnsets;
            for (int k = 0; k < nSteps; ++k) {
                const double rawBeat = slot.start_beat + static_cast<double>(k) * stepBeats;
                const double scaledBeat = rawBeat * scale;
                const int step16 = static_cast<int>(std::llround(std::fmod(scaledBeat, beatsPerBar) / 0.25)) % 16;
                const double gridP = profile.rhythm.bass_onset_grid[static_cast<size_t>(step16)];

                bool trigger = false;
                if (k == 0) {
                    trigger = true;
                } else if (perform.groove_sync && gridP >= 0.25 && rng.nextDouble() < gridP * 0.85) {
                    trigger = true;
                }
                if (trigger) {
                    bassOnsets.emplace_back(rawBeat, step16);
                }
            }

            const double slotEnd = slot.start_beat + slot.duration_beats;
            for (size_t idx = 0; idx < bassOnsets.size(); ++idx) {
                const double rawBeat = bassOnsets[idx].first;
                const int step16 = bassOnsets[idx].second;
                const double nextBeat = (idx + 1 < bassOnsets.size()) ? bassOnsets[idx + 1].first : slotEnd;
                const double gap = std::max(0.10, nextBeat - rawBeat);
                const double dur = std::clamp(gap * 0.88, 0.08, gap * 0.96);

                int pitch = rootPitch;
                if (idx > 0) {
                    const double r = rng.nextDouble();
                    if (r < 0.22) pitch = fifthPitch;
                    else if (r < 0.34) pitch = octPitch;
                }

                const double onset = applySwingAndHumanize(rawBeat, step16);
                const int vel = std::clamp(static_cast<int>(std::llround((idx == 0 ? 108.0 : 92.0) + rng.nextRange(-7.0, 7.0) * humanize)), 45, 127);

                NoteEvent ev;
                ev.onset_beats = onset;
                ev.duration_beats = std::max(0.06, std::min(dur, totalBeats - onset));
                ev.pitch = pitch;
                ev.velocity = vel;
                ev.channel = 3;
                out.bass.push_back(ev);
            }
        }
    }

    out.all_events.reserve(out.chords.size() + out.melody.size() + out.bass.size());
    out.all_events.insert(out.all_events.end(), out.chords.begin(), out.chords.end());
    out.all_events.insert(out.all_events.end(), out.melody.begin(), out.melody.end());
    out.all_events.insert(out.all_events.end(), out.bass.begin(), out.bass.end());
    std::sort(out.all_events.begin(), out.all_events.end(), [](const NoteEvent& a, const NoteEvent& b) {
        if (a.onset_beats != b.onset_beats) return a.onset_beats < b.onset_beats;
        if (a.channel != b.channel) return a.channel < b.channel;
        return a.pitch < b.pitch;
    });

    return out;
}

} // namespace style_scaler
