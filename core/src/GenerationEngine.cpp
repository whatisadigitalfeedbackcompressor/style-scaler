#include "style_scaler/GenerationEngine.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <optional>
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
    const size_t m = std::max(a.size(), b.size());
    double total = 0.0;
    for (size_t k = 0; k < m; ++k) {
        const int pa = a[(k * a.size()) / m];
        const int pb = b[(k * b.size()) / m];
        total += static_cast<double>(std::abs(pa - pb));
    }
    return total / static_cast<double>(m);
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
        const bool hasMovingRefProg = [&]() {
            if (profile.harmony.reference_progression.size() < 2) return false;
            const auto& first = profile.harmony.reference_progression.front();
            return std::any_of(profile.harmony.reference_progression.begin(),
                               profile.harmony.reference_progression.end(),
                               [&](const std::string& s) { return s != first; });
        }();
        double slotDur = beatsPerBar;
        if (effectivePacing >= 1.65) {
            slotDur = beatsPerBar * 0.5;
        } else if (effectivePacing <= 0.65 && safeBars >= 2 &&
                   !(tempoScale == TempoScale::Normal && hasMovingRefProg)) {
            slotDur = beatsPerBar * 2.0;
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

    auto assignSlotSymbol = [&](ProgressionSlot& dst, const std::string& sym) {
        dst.roman_symbol = sym;
        if (const auto* vc = findVocab(profile, sym)) {
            dst.chord = vc->chord;
        } else if (auto parsed = parseRomanChord(sym)) {
            dst.chord = *parsed;
        } else {
            dst.roman_symbol = profile.harmony.vocabulary.front().symbol;
            dst.chord = profile.harmony.vocabulary.front().chord;
        }
    };

    // seed == 0 -> Song Original mode: play extracted reference_progression verbatim (or greedy best path)
    if (seed == 0ULL) {
        if (!profile.harmony.reference_progression.empty()) {
            const auto& ref = profile.harmony.reference_progression;
            for (size_t i = 0; i < slots.size(); ++i) {
                if (slots[i].locked) continue;
                assignSlotSymbol(slots[i], ref[i % ref.size()]);
            }
            return slots;
        }
        const size_t phraseLen = std::min<size_t>(4, slots.size());
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].locked) continue;
            if (i >= phraseLen && (i % phraseLen) < phraseLen && !slots[i % phraseLen].roman_symbol.empty()) {
                assignSlotSymbol(slots[i], slots[i % phraseLen].roman_symbol);
                continue;
            }
            if (i == 0) {
                std::string bestSym = profile.harmony.vocabulary.front().symbol;
                double bestW = -1.0;
                for (const auto& v : profile.harmony.vocabulary) {
                    double w = v.weight * ((v.chord.degree == 1 && v.chord.accidental == 0) ? 2.5 : 1.0);
                    if (w > bestW) { bestW = w; bestSym = v.symbol; }
                }
                assignSlotSymbol(slots[i], bestSym);
            } else {
                const auto probs = nextChordProbabilities(profile, slots[i - 1].roman_symbol);
                std::string chosen = profile.harmony.vocabulary.front().symbol;
                for (const auto& [sym, _p] : probs) {
                    chosen = sym;
                    if (sym != slots[i - 1].roman_symbol || probs.size() == 1) break;
                }
                assignSlotSymbol(slots[i], chosen);
            }
        }
        return slots;
    }

    enum class HarmFunc { Tonic, Predominant, Dominant };
    auto chordFuncOf = [](const RomanChord& rc) noexcept -> HarmFunc {
        switch (rc.degree) {
            case 2:
            case 4:
                return HarmFunc::Predominant;
            case 5:
            case 7:
                return HarmFunc::Dominant;
            default:
                return HarmFunc::Tonic;
        }
    };

    const auto& refProg = profile.harmony.reference_progression;
    const std::set<std::string> refSet(refProg.begin(), refProg.end());
    std::set<std::pair<std::string, std::string>> refBigrams;
    for (size_t i = 0; i + 1 < refProg.size(); ++i) {
        refBigrams.emplace(refProg[i], refProg[i + 1]);
    }
    if (refProg.size() >= 2) {
        refBigrams.emplace(refProg.back(), refProg.front());
    }
    const size_t phraseLen = std::min<size_t>(4, slots.size());

    auto transProbOf = [&](const std::string& fromSym, const std::string& toSym) -> double {
        const auto probs = nextChordProbabilities(profile, fromSym);
        for (const auto& [ns, np] : probs) {
            if (ns == toSym) return np;
        }
        return 0.05;
    };

    auto generateCandidate = [&](uint64_t subSeed, bool mutateFromRef) {
        std::vector<ProgressionSlot> cand = slots;
        DeterministicRng candRng(subSeed);

        if (mutateFromRef && !refProg.empty()) {
            for (size_t i = 0; i < cand.size(); ++i) {
                if (!cand[i].locked) {
                    assignSlotSymbol(cand[i], refProg[i % refProg.size()]);
                }
            }
            if (profile.harmony.vocabulary.size() >= 2 && cand.size() >= 2) {
                const size_t stepPhrase = std::max<size_t>(1, phraseLen);
                for (size_t pStart = 0; pStart < cand.size(); pStart += stepPhrase) {
                    const size_t pEnd = std::min(cand.size(), pStart + stepPhrase);
                    std::vector<size_t> mutableIdx;
                    for (size_t idx = pStart; idx < pEnd; ++idx) {
                        if (!cand[idx].locked && !(idx == 0 && cand.size() >= 3)) {
                            mutableIdx.push_back(idx);
                        }
                    }
                    if (mutableIdx.empty()) continue;

                    const size_t numMut = (mutableIdx.size() >= 3 && candRng.nextDouble() < 0.65) ? 2 : 1;
                    // Shuffle mutable indices via Fisher-Yates so we don't always hit the same bar
                    for (size_t m = mutableIdx.size(); m > 1; --m) {
                        const size_t j = static_cast<size_t>(candRng.nextInt(0, static_cast<int>(m - 1)));
                        std::swap(mutableIdx[m - 1], mutableIdx[j]);
                    }

                    std::vector<size_t> chosenIdx;
                    for (size_t idx : mutableIdx) {
                        if (chosenIdx.size() >= numMut) break;
                        // Avoid wiping both chords of an adjacent signature bigram unless forced
                        bool adjacentToPicked = false;
                        for (size_t prevIdx : chosenIdx) {
                            if (idx == prevIdx + 1 || prevIdx == idx + 1) adjacentToPicked = true;
                        }
                        if (adjacentToPicked && mutableIdx.size() >= 3) continue;
                        chosenIdx.push_back(idx);
                    }
                    if (chosenIdx.empty()) chosenIdx.push_back(mutableIdx.front());

                    for (size_t idx : chosenIdx) {
                        std::vector<ChordVocabEntry> pool;
                        std::set<std::string> seen;
                        for (const auto& vc : profile.harmony.vocabulary) {
                            if (seen.insert(vc.symbol).second) pool.push_back(vc);
                        }
                        for (const auto& sub : suggestSubstitutes(profile, cand[idx], 6)) {
                            if (seen.insert(sub.symbol).second) pool.push_back(sub);
                        }
                        if (pool.empty()) continue;

                        const size_t posInPhrase = (phraseLen > 0) ? (idx % phraseLen) : 0;
                        const bool isPhraseEnd = (idx + 1 == cand.size()) ||
                                                 (phraseLen > 0 && ((idx + 1) % phraseLen == 0));
                        const bool isFinalTurnaround = (idx + 1 == cand.size()) || (pStart > 0);
                        const bool nextIsLocked = (idx + 1 < cand.size() && cand[idx + 1].locked);
                        const HarmFunc origFunc = chordFuncOf(cand[idx].chord);

                        std::vector<double> weights;
                        weights.reserve(pool.size());
                        for (const auto& entry : pool) {
                            double w = std::max(0.05, entry.weight);
                            if (idx > 0) {
                                w *= (0.25 + transProbOf(cand[idx - 1].roman_symbol, entry.symbol));
                                if (refBigrams.count({cand[idx - 1].roman_symbol, entry.symbol})) {
                                    w *= 2.4;
                                }
                            }
                            if (idx + 1 < cand.size() && refBigrams.count({entry.symbol, cand[idx + 1].roman_symbol})) {
                                w *= 2.4;
                            }
                            const HarmFunc fn = chordFuncOf(entry.chord);
                            if (fn == origFunc) w *= 1.8;

                            if (isPhraseEnd) {
                                if (isFinalTurnaround) {
                                    if (fn == HarmFunc::Dominant) w *= 2.1;
                                    else if (fn == HarmFunc::Predominant) w *= 1.3;
                                    w *= (1.0 + 2.0 * std::clamp(entry.cadence_weight, 0.0, 1.0));
                                } else {
                                    if (fn == HarmFunc::Dominant) w *= 1.6;
                                    else if (fn == HarmFunc::Predominant) w *= 1.4;
                                }
                            } else if ((posInPhrase == 1 || posInPhrase == 2) && fn == HarmFunc::Predominant) {
                                w *= 1.30;
                            }

                            if (idx > 0 && entry.symbol == cand[idx - 1].roman_symbol && pool.size() > 1) {
                                w *= 0.12;
                            }
                            if (idx + 1 < cand.size() && entry.symbol == cand[idx + 1].roman_symbol && pool.size() > 1) {
                                w *= 0.18;
                            }
                            if (entry.symbol == cand[idx].roman_symbol && pool.size() > 1) {
                                w *= 0.05;
                            }
                            if (nextIsLocked) {
                                w *= (0.05 + transProbOf(entry.symbol, cand[idx + 1].roman_symbol));
                            }
                            weights.push_back(std::max(1e-4, w));
                        }

                        const size_t picked = sampleWeightedIndex(weights, candRng);
                        assignSlotSymbol(cand[idx], pool[picked].symbol);
                    }
                }
            }
            return cand;
        }

        for (size_t i = 0; i < cand.size(); ++i) {
            if (cand[i].locked) continue;

            const size_t posInPhrase = (phraseLen > 0) ? (i % phraseLen) : 0;
            const bool nextIsLocked = (i + 1 < cand.size() && cand[i + 1].locked);

            // A - A' harmonic form across 8/16-bar timelines
            if (i >= phraseLen && posInPhrase < std::max<size_t>(1, phraseLen / 2) &&
                !nextIsLocked && !cand[posInPhrase].roman_symbol.empty() && candRng.nextDouble() < 0.68) {
                assignSlotSymbol(cand[i], cand[posInPhrase].roman_symbol);
                continue;
            }

            const bool isFirst = (i == 0);
            const bool isPhraseEnd = (i + 1 == cand.size()) ||
                                     (phraseLen > 0 && ((i + 1) % phraseLen == 0)) ||
                                     (static_cast<int>((cand[i].start_beat + cand[i].duration_beats) / beatsPerBar) % 4 == 0 &&
                                      cand[i].duration_beats >= beatsPerBar * 0.5);

            std::vector<std::string> candidates;
            std::vector<double> weights;

            if (isFirst) {
                for (const auto& v : profile.harmony.vocabulary) {
                    candidates.push_back(v.symbol);
                    double w = std::max(0.01, v.weight);
                    if (v.chord.degree == 1 && v.chord.accidental == 0) {
                        w *= 6.0; // anchor bar 1 strongly on tonic
                    }
                    if (!refProg.empty() && v.symbol == refProg.front()) {
                        w *= 2.0;
                    }
                    weights.push_back(w);
                }
            } else {
                const auto probs = nextChordProbabilities(profile, cand[i - 1].roman_symbol);
                for (const auto& [sym, p] : probs) {
                    double w = p;
                    if (sym == cand[i - 1].roman_symbol && probs.size() > 1) {
                        w *= 0.15;
                    }
                    if (!refSet.empty() && refSet.count(sym)) {
                        w *= 1.35;
                    }
                    if (refBigrams.count({cand[i - 1].roman_symbol, sym})) {
                        w *= 2.4;
                    }
                    if (const auto* vc = findVocab(profile, sym)) {
                        const HarmFunc fn = chordFuncOf(vc->chord);
                        if (isPhraseEnd) {
                            w *= (1.0 + 2.0 * std::clamp(vc->cadence_weight, 0.0, 1.0));
                            if (fn == HarmFunc::Dominant) w *= 1.25;
                            else if (fn == HarmFunc::Predominant) w *= 1.15;
                        } else if (posInPhrase == 0 && vc->chord.degree == 1 && vc->chord.accidental == 0) {
                            w *= 1.40;
                        } else if ((posInPhrase == 1 || posInPhrase == 2) && fn == HarmFunc::Predominant) {
                            w *= 1.25;
                        }
                        if (nextIsLocked) {
                            w *= (0.05 + transProbOf(sym, cand[i + 1].roman_symbol));
                        }
                    }
                    candidates.push_back(sym);
                    weights.push_back(std::max(1e-4, w));
                }
            }

            if (candidates.empty()) {
                cand[i].roman_symbol = profile.harmony.vocabulary.front().symbol;
                cand[i].chord = profile.harmony.vocabulary.front().chord;
                continue;
            }

            const size_t picked = sampleWeightedIndex(weights, candRng);
            assignSlotSymbol(cand[i], candidates[picked]);
        }

        return cand;
    };

    auto scoreProgression = [&](const std::vector<ProgressionSlot>& cand) -> double {
        if (cand.empty()) return -1e9;
        double score = 0.0;
        std::set<std::string> uniq;
        int bigramHits = 0;
        int tonicSlots = 0;
        bool differsFromRef = false;
        bool hasMutableSlot = false;

        for (size_t i = 0; i < cand.size(); ++i) {
            uniq.insert(cand[i].roman_symbol);
            if (!cand[i].locked) hasMutableSlot = true;
            if (cand[i].chord.degree == 1 && cand[i].chord.accidental == 0) ++tonicSlots;
            if (!refProg.empty() && !cand[i].locked && cand[i].roman_symbol != refProg[i % refProg.size()]) {
                differsFromRef = true;
            }
            if (i > 0) {
                if (cand[i].roman_symbol == cand[i - 1].roman_symbol && profile.harmony.vocabulary.size() > 1) {
                    score -= 2.5;
                }
                if (refBigrams.count({cand[i - 1].roman_symbol, cand[i].roman_symbol})) {
                    ++bigramHits;
                }
            }
            const bool isPhraseEnd = (i + 1 == cand.size()) || (phraseLen > 0 && ((i + 1) % phraseLen == 0));
            if (isPhraseEnd) {
                const HarmFunc fn = chordFuncOf(cand[i].chord);
                const auto* vc = findVocab(profile, cand[i].roman_symbol);
                if (fn == HarmFunc::Dominant || fn == HarmFunc::Predominant || (vc && vc->cadence_weight >= 0.25)) {
                    score += 1.5;
                }
            }
        }

        score += 1.5 * static_cast<double>(std::min<size_t>(uniq.size(), 4));
        if (cand.front().chord.degree == 1 && cand.front().chord.accidental == 0) score += 3.0;
        score += 2.2 * static_cast<double>(std::min(tonicSlots, std::max(1, static_cast<int>(cand.size()) / 3)));
        if (!refProg.empty() && cand.front().roman_symbol == refProg.front()) score += 2.0;
        score += 1.4 * static_cast<double>(std::min(bigramHits, 3));
        if (cand.size() >= 8 && cand[4].roman_symbol == cand[0].roman_symbol) score += 1.2;
        if (!refProg.empty() && profile.harmony.vocabulary.size() >= 3 && hasMutableSlot) {
            score += differsFromRef ? 4.5 : -5.0;
        }
        if (uniq.size() == 1 && profile.harmony.vocabulary.size() >= 2) score -= 8.0;
        return score;
    };

    std::vector<ProgressionSlot> bestCand;
    double bestScore = -1e18;
    for (int k = 0; k < 8; ++k) {
        const bool mutateFromRef = (k < 4 && !refProg.empty());
        const uint64_t subSeed = seed ^ (static_cast<uint64_t>(k + 1) * 0x9E3779B97F4A7C15ULL);
        auto cand = generateCandidate(subSeed, mutateFromRef);
        const double sc = scoreProgression(cand);
        if (sc > bestScore) {
            bestScore = sc;
            bestCand = std::move(cand);
        }
    }
    return bestCand;
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
                                    ? std::clamp(voicing.spread_semitones + (std::clamp(spreadOverride, 0.0, 1.0) - 0.5) * 18.0, 5.0, 30.0)
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

            // Guide-tone resolution bonus: reward 7th->3rd downward step (1..2 semitones) and tight inner voices
            if (pcs.size() >= 2) {
                const int thirdPc = pcs[1];
                const int seventhPc = (pcs.size() >= 4) ? pcs[3] : -1;
                for (int p : c) {
                    const int pc = normalizePitchClass(p);
                    if (pc == thirdPc || (seventhPc >= 0 && pc == seventhPc)) {
                        for (int qp : prevVoicing) {
                            const int stepDown = qp - p;
                            if (pc == thirdPc && stepDown >= 1 && stepDown <= 2) {
                                cost -= 1.4;
                                break;
                            }
                            if (std::abs(p - qp) <= 2) {
                                cost -= 0.8;
                                break;
                            }
                        }
                    }
                }
            }

            // Soprano (top voice) continuity across chord changes
            const int topJump = std::abs(c.back() - prevVoicing.back());
            if (topJump <= 3) {
                cost -= 0.9;
            } else if (topJump > 7) {
                cost += 1.8;
            }
        }

        // Low-interval clarity penalty: avoid adjacent voices < 4 semitones below MIDI 60
        for (size_t k = 0; k + 1 < c.size(); ++k) {
            const int gap = c[k + 1] - c[k];
            if (c[k + 1] < 60 && gap < 4) {
                cost += 6.0 * static_cast<double>(4 - gap);
            } else if (c[k] < 54 && gap < 5) {
                cost += 3.5;
            }
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

namespace {

GeneratedPattern generatePatternSingle(
    const StyleProfile& profile,
    const std::vector<ProgressionSlot>& progression,
    const PerformParams& perform,
    uint64_t seed,
    bool isOriginalMode) {

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

    auto hasNonZeroPat = [](const std::array<double, 16>& p) {
        for (double v : p) {
            if (v > 1e-6) return true;
        }
        return false;
    };

    // Precompute anticipation shifts so previous slot's chords truncate cleanly before anticipated downbeats
    std::vector<double> slotAnticipationShift(progression.size(), 0.0);
    if (perform.groove_sync && !isOriginalMode) {
        for (size_t s = 1; s < progression.size(); ++s) {
            if ((perform.section == SongSection::Intro && progression[s].start_beat < totalBeats * 0.5) ||
                (perform.section == SongSection::Outro && s + 1 == progression.size())) {
                continue;
            }
            if (profile.rhythm.anticipation > 0.15 && rng.nextDouble() < profile.rhythm.anticipation * 0.75) {
                const double maxPull = std::min(stepBeats * 2.0, progression[s].duration_beats * 0.35);
                slotAnticipationShift[s] = -std::min(maxPull, progression[s].start_beat * 0.5);
            }
        }
    }

    // --- 1. Channel 2: Melody (generated first so Chords & Bass can interlock around it) ---
    if (perform.enable_melody) {
        const auto scaleIvs = scaleIntervals(profile.harmony.mode);

        int avgChordTop = profile.voicing.center_pitch + 6;
        if (!slotVoicings.empty()) {
            int sumTop = 0, cntTop = 0;
            for (const auto& sv : slotVoicings) {
                if (!sv.empty()) { sumTop += sv.back(); ++cntTop; }
            }
            if (cntTop > 0) avgChordTop = sumTop / cntTop;
        }
        const int minMelLo = std::max(profile.melody.register_low, profile.voicing.sub_bass_clearance_midi + 7);
        const int clearAboveChords = std::min(avgChordTop - 1, profile.voicing.sub_bass_clearance_midi + 18);
        const int melLo = std::clamp(std::max(minMelLo, clearAboveChords), minMelLo, 84);
        const int melHi = std::max(melLo + 12, profile.melody.register_high);

        auto buildChordCompatiblePcs = [&](const RomanChord& chord,
                                           std::vector<int>& outChordPcs,
                                           std::vector<int>& outSafeScalePcs,
                                           std::vector<int>& outTensionPcs) {
            outChordPcs = chordPitchClasses(chord, profile.harmony.tonic_pc, profile.harmony.mode);
            const int rootPc = romanChordRootPitchClass(chord, profile.harmony.tonic_pc);
            const auto civs = chordIntervals(chord.quality);
            std::vector<int> sensitivePcs = {rootPc};
            for (size_t k = 1; k < civs.size(); ++k) {
                if (civs[k] == 3 || civs[k] == 4 || civs[k] == 10 || civs[k] == 11) {
                    sensitivePcs.push_back(normalizePitchClass(rootPc + civs[k]));
                }
            }

            outSafeScalePcs = outChordPcs;
            outTensionPcs.clear();
            for (int iv : scaleIvs) {
                const int pc = normalizePitchClass(profile.harmony.tonic_pc + iv);
                if (std::find(outChordPcs.begin(), outChordPcs.end(), pc) != outChordPcs.end()) continue;
                bool avoid = false;
                for (int spc : sensitivePcs) {
                    const int d = normalizePitchClass(pc - spc);
                    if (d == 1 || d == 11) {
                        avoid = true;
                        break;
                    }
                }
                if (!avoid) {
                    outSafeScalePcs.push_back(pc);
                    outTensionPcs.push_back(pc);
                }
            }
            if (outSafeScalePcs.empty()) outSafeScalePcs = outChordPcs;
            if (outTensionPcs.empty()) outTensionPcs = outSafeScalePcs;
        };

        std::vector<int> scaleMidiNotes;
        for (int p = melLo; p <= melHi; ++p) {
            const int rel = normalizePitchClass(p - profile.harmony.tonic_pc);
            if (std::find(scaleIvs.begin(), scaleIvs.end(), rel) != scaleIvs.end()) {
                scaleMidiNotes.push_back(p);
            }
        }
        if (scaleMidiNotes.empty()) scaleMidiNotes.push_back((melLo + melHi) / 2);
        const int anchorScaleIdx = static_cast<int>(scaleMidiNotes.size()) / 2;

        struct PlannedMelNote {
            double beat = 0.0;
            int step16 = 0;
            int degOffset = 0;
            double durSteps = 2.0;
            bool wantChordTone = true;
            bool isCadence = false;
            bool isOpenCadence = false;
            bool isClosedCadence = false;
        };
        std::vector<PlannedMelNote> planned;

        const double cycleBeats = 32.0 * stepBeats;
        const int numCycles = std::max(1, static_cast<int>(std::ceil((totalBeats - 1e-4) / std::max(0.25, cycleBeats))));

        if (!profile.melody.reference_motif.empty()) {
            const size_t motifSz = profile.melody.reference_motif.size();
            for (int c = 0; c < numCycles; ++c) {
                const double cStart = static_cast<double>(c) * cycleBeats;
                const bool isVarCycle = (!isOriginalMode && (c % 2 == 1));
                const int phraseTranspose = (!isOriginalMode && (c % 2 == 1)) ? (rng.nextDouble() < 0.5 ? 1 : -1) : 0;

                for (size_t k = 0; k < motifSz; ++k) {
                    const auto& ms = profile.melody.reference_motif[k];
                    const double beat = cStart + static_cast<double>(ms.step16) * stepBeats;
                    if (beat >= totalBeats - 0.08) break;
                    if (perform.section == SongSection::Intro && beat < totalBeats * 0.5 &&
                        motifSz >= 4 && (ms.step16 % 4 != 0)) {
                        continue;
                    }
                    if (perform.section == SongSection::Outro && beat >= totalBeats - beatsPerBar &&
                        ((ms.step16 % 4 != 0) && (k + 1 < motifSz))) {
                        continue;
                    }
                    if (!isOriginalMode) {
                        if (perform.melody_density < 0.40 && (ms.step16 % 4 != 0) &&
                            rng.nextDouble() < (0.40 - perform.melody_density) * 1.4) {
                            continue;
                        }
                    }

                    int degOff = ms.scale_degree_offset + phraseTranspose;
                    if (isVarCycle && k > 0 && rng.nextDouble() < 0.30) {
                        degOff += (rng.nextDouble() < 0.5) ? 1 : -1;
                    }
                    const bool isLastInCycle = (k + 1 == motifSz);
                    const bool isFinalRegion = (c + 1 == numCycles) || (beat >= totalBeats - beatsPerBar);
                    const bool isOpen = isLastInCycle && !isFinalRegion && ((c % 2) == 0);
                    const bool isClosed = isLastInCycle && (isFinalRegion || ((c % 2) == 1));
                    const bool isCad = isLastInCycle && (isFinalRegion || ((c + 1) % 2 == 0));
                    planned.push_back(PlannedMelNote{
                        beat,
                        ms.step16 % 16,
                        degOff,
                        ms.duration_steps,
                        ms.is_chord_tone || isCad || (ms.step16 % 4 == 0),
                        isCad,
                        isOpen,
                        isClosed
                    });
                }
            }
        } else {
            // Structured 2-bar Motif A synthesis when no reference_motif is present (starter presets)
            const bool useCrispMelPat = hasNonZeroPat(profile.rhythm.melody_pattern);
            const auto& melGrid = useCrispMelPat ? profile.rhythm.melody_pattern : profile.rhythm.melody_onset_grid;
            const double densityFactor = std::clamp(0.35 + 1.15 * (0.5 * profile.melody.motif_density + 0.5 * perform.melody_density), 0.15, 1.4);

            std::vector<double> hw(profile.melody.interval_histogram.begin(), profile.melody.interval_histogram.end());
            auto buildMotifSteps = [&](bool allowVar) {
                std::vector<MelodyMotifStep> motif;
                int curDeg = 0;
                int lastStepDeg = 0;
                for (int s32 = 0; s32 < 32; ++s32) {
                    if (s32 >= 28 && (s32 % 4 != 0)) continue; // phrase breath at bar 2 end
                    const double gridP = melGrid[static_cast<size_t>(s32 % 16)] * densityFactor;
                    bool fire = false;
                    if (s32 == 0) {
                        fire = true;
                    } else if (isOriginalMode && !allowVar) {
                        fire = (gridP >= 0.52);
                    } else {
                        fire = (rng.nextDouble() < std::clamp(gridP * 0.65, 0.0, 0.88));
                    }
                    if (!fire) continue;

                    int stepDeg = 0;
                    if (!motif.empty()) {
                        if (std::abs(lastStepDeg) >= 3) {
                            stepDeg = (lastStepDeg > 0) ? -1 : 1; // contrary recovery after leap
                        } else if (isOriginalMode && !allowVar) {
                            stepDeg = ((s32 / 4) % 2 == 0) ? 1 : -1;
                        } else {
                            const int rawSemi = static_cast<int>(sampleWeightedIndex(hw, rng)) - 12;
                            stepDeg = std::clamp(static_cast<int>(std::llround(static_cast<double>(rawSemi) / 1.75)), -4, 4);
                        }
                        const double archTarget = (profile.melody.arch_contour - 0.45) * 6.0 *
                                                  std::sin(3.14159265358979323846 * (static_cast<double>(s32) / 31.0));
                        curDeg = std::clamp(static_cast<int>(std::llround(0.70 * (curDeg + stepDeg) + 0.30 * archTarget)), -6, 6);
                        lastStepDeg = curDeg - motif.back().scale_degree_offset;
                    }
                    const bool isCt = (s32 % 4 == 0) || (rng.nextDouble() < profile.melody.chord_tone_ratio);
                    motif.push_back(MelodyMotifStep{s32, curDeg, 2.0, isCt});
                    if (motif.size() >= 10) break;
                }
                for (size_t k = 0; k < motif.size(); ++k) {
                    const int nextS = (k + 1 < motif.size()) ? motif[k + 1].step16 : 32;
                    motif[k].duration_steps = std::clamp(static_cast<double>(nextS - motif[k].step16), 1.0, 6.0);
                }
                return motif;
            };

            const auto motifA = buildMotifSteps(false);
            const auto motifB = isOriginalMode ? motifA : buildMotifSteps(true);

            for (int c = 0; c < numCycles; ++c) {
                const double cStart = static_cast<double>(c) * cycleBeats;
                const bool useA = (c % 2 == 0) || (rng.nextDouble() < profile.melody.motif_repetition);
                const auto& srcMotif = useA ? motifA : motifB;
                const size_t srcSz = srcMotif.size();
                for (size_t k = 0; k < srcSz; ++k) {
                    const auto& ms = srcMotif[k];
                    const double beat = cStart + static_cast<double>(ms.step16) * stepBeats;
                    if (beat >= totalBeats - 0.08) break;
                    if (perform.section == SongSection::Intro && beat < totalBeats * 0.5 &&
                        srcSz >= 4 && (ms.step16 % 4 != 0)) {
                        continue;
                    }
                    if (perform.section == SongSection::Outro && beat >= totalBeats - beatsPerBar &&
                        ((ms.step16 % 4 != 0) && (k + 1 < srcSz))) {
                        continue;
                    }
                    const bool isLastInCycle = (k + 1 == srcSz);
                    const bool isFinalRegion = (c + 1 == numCycles) || (beat >= totalBeats - beatsPerBar);
                    const bool isOpen = isLastInCycle && !isFinalRegion && ((c % 2) == 0);
                    const bool isClosed = isLastInCycle && (isFinalRegion || ((c % 2) == 1));
                    const bool isCad = isLastInCycle && (isFinalRegion || ((c + 1) % 2 == 0));
                    planned.push_back(PlannedMelNote{
                        beat,
                        ms.step16 % 16,
                        ms.scale_degree_offset,
                        ms.duration_steps,
                        ms.is_chord_tone || isCad,
                        isCad,
                        isOpen,
                        isClosed
                    });
                }
            }
        }

        if (planned.empty()) {
            planned.push_back(PlannedMelNote{0.0, 0, 0, 4.0, true, false, false, true});
        }

        int prevPitch = std::clamp(scaleMidiNotes[static_cast<size_t>(anchorScaleIdx)], melLo, melHi);
        int prevInterval = 0;
        int samePitchCount = 0;
        bool prevWasNonChord = false;

        for (size_t i = 0; i < planned.size(); ++i) {
            const auto& pm = planned[i];
            const size_t slotIdx = findSlotAtBeat(pm.beat);
            const auto& slot = progression[slotIdx];

            std::vector<int> chordPcs, safeScalePcs, tensionPcs;
            buildChordCompatiblePcs(slot.chord, chordPcs, safeScalePcs, tensionPcs);

            const int targetIdx = std::clamp(anchorScaleIdx + pm.degOffset, 0, static_cast<int>(scaleMidiNotes.size()) - 1);
            const int targetPitch = scaleMidiNotes[static_cast<size_t>(targetIdx)];

            const bool forceChordResolve = prevWasNonChord || pm.isCadence || pm.isOpenCadence || pm.isClosedCadence;
            const std::vector<int>* pool = &chordPcs;
            if (!forceChordResolve && !pm.wantChordTone) {
                const double nonCtRatio = std::max(0.05, 1.0 - profile.melody.chord_tone_ratio);
                const double tensProb = std::clamp(profile.melody.tension_tone_ratio / nonCtRatio, 0.0, 1.0);
                pool = (!isOriginalMode && rng.nextDouble() < tensProb) ? &tensionPcs : &safeScalePcs;
            }

            std::vector<int> allowedPitches;
            for (int p = melLo; p <= melHi; ++p) {
                if (std::find(pool->begin(), pool->end(), normalizePitchClass(p)) != pool->end()) {
                    allowedPitches.push_back(p);
                }
            }
            if (allowedPitches.empty()) {
                for (int p = melLo; p <= melHi; ++p) {
                    if (std::find(safeScalePcs.begin(), safeScalePcs.end(), normalizePitchClass(p)) != safeScalePcs.end()) {
                        allowedPitches.push_back(p);
                    }
                }
            }
            if (allowedPitches.empty()) allowedPitches.push_back(prevPitch);

            const int refPitch = prevWasNonChord ? prevPitch : targetPitch;
            int chosen = allowedPitches.front();
            double bestScore = 1e9;
            for (int p : allowedPitches) {
                const int pPc = normalizePitchClass(p);
                double dist = static_cast<double>(std::abs(p - refPitch));
                if (!prevWasNonChord) {
                    dist += 0.25 * static_cast<double>(std::abs(p - prevPitch));
                } else {
                    dist += 1.8 * static_cast<double>(std::max(0, std::abs(p - prevPitch) - 2));
                }

                // Leap-then-step-back recovery after >= 5 semitone leap
                if (std::abs(prevInterval) >= 5 && i > 0) {
                    const int delta = p - prevPitch;
                    if (delta != 0 && ((delta > 0) != (prevInterval > 0)) && std::abs(delta) <= 3) {
                        dist -= 2.2;
                    }
                }

                // Antecedent (open) vs Consequent (closed) phrasing
                if (!chordPcs.empty()) {
                    if (pm.isOpenCadence && chordPcs.size() >= 2 && pPc == chordPcs.front()) {
                        dist += 2.2;
                    }
                    if (pm.isClosedCadence) {
                        const bool isRootOrThird = (pPc == chordPcs[0]) || (chordPcs.size() >= 2 && pPc == chordPcs[1]);
                        if (isRootOrThird) dist -= 2.4;
                    }
                }

                if (i > 0 && p == prevPitch && samePitchCount >= 2) {
                    dist += 2.2;
                }
                if (p == melLo || p == melHi) dist += 1.5; // discourage edge pinning
                if (dist < bestScore) {
                    bestScore = dist;
                    chosen = p;
                }
            }

            const int chosenPc = normalizePitchClass(chosen);
            prevWasNonChord = (std::find(chordPcs.begin(), chordPcs.end(), chosenPc) == chordPcs.end());

            const double nextBeat = (i + 1 < planned.size()) ? planned[i + 1].beat : totalBeats;
            const double gap = std::max(0.10, nextBeat - pm.beat);
            const double desiredDur = pm.durSteps * stepBeats * std::clamp(profile.melody.legato_ratio, 0.40, 1.05);
            const double maxDurCap = pm.isClosedCadence ? 3.2 : 2.5;
            const double dur = std::clamp(desiredDur, 0.08, std::min(gap * 0.95, maxDurCap));

            double onset = isOriginalMode ? std::clamp(pm.beat, 0.0, totalBeats - 0.03)
                                          : applySwingAndHumanize(pm.beat, pm.step16);
            if (!isOriginalMode && humanize > 0.01 && dur >= 0.75 && pm.step16 > 0) {
                onset = std::clamp(onset + 0.008 * humanize, 0.0, std::max(0.0, nextBeat - 0.06));
            }

            const double metricVel = (pm.step16 % 8 == 0)
                                         ? 96.0
                                         : ((pm.step16 % 4 == 0) ? 90.0 : ((pm.step16 % 2 == 0) ? 85.0 : 74.0));
            const double contourSwell = (melHi > melLo)
                                            ? (8.0 * static_cast<double>(chosen - melLo) / static_cast<double>(melHi - melLo))
                                            : 0.0;
            double cadenceScale = 1.0;
            if (perform.section == SongSection::Intro) {
                cadenceScale = 0.74 + 0.26 * (pm.beat / std::max(1.0, totalBeats));
            } else if (perform.section == SongSection::Outro && pm.beat >= totalBeats - beatsPerBar) {
                cadenceScale = 0.84;
            }
            const int baseVel = static_cast<int>(std::llround((metricVel + contourSwell - (pm.isClosedCadence ? 5.0 : 0.0)) * cadenceScale));
            const int vel = isOriginalMode
                                ? std::clamp(baseVel, 40, 125)
                                : std::clamp(static_cast<int>(std::llround(baseVel + rng.nextRange(-7.0, 7.0) * humanize)), 40, 125);

            NoteEvent ev;
            ev.onset_beats = onset;
            ev.duration_beats = std::max(0.06, std::min(dur, totalBeats - onset));
            ev.pitch = chosen;
            ev.velocity = vel;
            ev.channel = 2;
            out.melody.push_back(ev);

            if (i > 0 && chosen == prevPitch) {
                ++samePitchCount;
            } else {
                samePitchCount = 0;
            }
            if (i > 0) prevInterval = chosen - prevPitch;
            prevPitch = chosen;
        }
    }

    auto melodyAttackNear = [&](double beat) -> std::optional<int> {
        for (const auto& m : out.melody) {
            if (std::abs(m.onset_beats - beat) <= 0.08) return m.pitch;
        }
        return std::nullopt;
    };

    auto melodyRestingAt = [&](double beat) -> bool {
        for (const auto& m : out.melody) {
            if (m.onset_beats <= beat + 0.35 && (m.onset_beats + m.duration_beats) >= beat - 0.15) {
                return false;
            }
        }
        return true;
    };

    // --- 2. Channel 1: Chords (melody-aware interlocking + 2-bar groove + section conductor) ---
    if (perform.enable_chords) {
        const bool useCrispChordPat = hasNonZeroPat(profile.rhythm.chord_pattern);
        const bool hasChordPat2 = useCrispChordPat && hasNonZeroPat(profile.rhythm.chord_pattern_bar2);

        for (size_t s = 0; s < progression.size(); ++s) {
            const auto& slot = progression[s];
            const auto& notes = slotVoicings[s];
            if (notes.empty()) continue;

            const bool isLastSlot = (s + 1 == progression.size());
            const bool isIntroFirstHalf = (perform.section == SongSection::Intro && slot.start_beat < totalBeats * 0.5);
            const double effectiveStart = std::max(0.0, slot.start_beat + slotAnticipationShift[s]);
            const double slotEnd = (!isLastSlot)
                                       ? std::max(effectiveStart + 0.12, progression[s + 1].start_beat + slotAnticipationShift[s + 1])
                                       : totalBeats;
            const double effectiveSlotDur = std::max(0.12, slotEnd - effectiveStart);

            const double introGain = (perform.section == SongSection::Intro)
                                         ? (0.76 + 0.24 * (slot.start_beat / std::max(1.0, totalBeats)))
                                         : ((perform.section == SongSection::Outro && isLastSlot) ? 0.80 : 1.0);

            if (!perform.groove_sync || (perform.section == SongSection::Outro && isLastSlot)) {
                const double sustainMult = (perform.section == SongSection::Outro && isLastSlot)
                                               ? 0.96
                                               : std::clamp(profile.rhythm.sustain_ratio, 0.45, 0.98);
                const double dur = std::clamp(effectiveSlotDur * sustainMult, 0.12, std::max(0.12, effectiveSlotDur - 0.01));
                for (size_t v = 0; v < notes.size(); ++v) {
                    const double strum = static_cast<double>(v) * 0.014 * humanize;
                    const int vel = std::clamp(static_cast<int>(std::llround((92.0 + rng.nextRange(-7.0, 7.0) * humanize) * introGain)), 40, 120);
                    NoteEvent ev;
                    ev.onset_beats = std::min(slotEnd - 0.05, effectiveStart + strum);
                    ev.duration_beats = std::max(0.06, std::min(dur, slotEnd - ev.onset_beats - 0.005));
                    ev.pitch = notes[v];
                    ev.velocity = vel;
                    ev.channel = 1;
                    out.chords.push_back(ev);
                }
                continue;
            }

            std::vector<std::pair<double, int>> onsets; // (beat, stepIdx16)
            const int nSteps = std::max(1, static_cast<int>(std::floor(slot.duration_beats / stepBeats + 1e-6)));

            for (int k = 0; k < nSteps; ++k) {
                const double rawBeat = (k == 0) ? effectiveStart : (slot.start_beat + static_cast<double>(k) * stepBeats);
                if (rawBeat >= slotEnd - 0.06) break;
                const int barIdx = static_cast<int>(std::floor(rawBeat / beatsPerBar));
                const double scaledBeat = rawBeat * scale;
                const int step16 = static_cast<int>(std::llround(std::fmod(scaledBeat, beatsPerBar) / 0.25)) % 16;

                const auto& chordGrid = (useCrispChordPat && hasChordPat2 && (barIdx % 2 == 1))
                                            ? profile.rhythm.chord_pattern_bar2
                                            : (useCrispChordPat ? profile.rhythm.chord_pattern : profile.rhythm.chord_onset_grid);
                const double gridProb = chordGrid[static_cast<size_t>(step16)];

                if (perform.section == SongSection::Outro && rawBeat >= totalBeats - 2.0 * beatsPerBar &&
                    k > 0 && (step16 % 4 != 0)) {
                    continue;
                }

                bool trigger = false;
                if (isIntroFirstHalf) {
                    trigger = (k == 0) || (step16 == 8 && slot.duration_beats >= beatsPerBar && gridProb >= 0.65);
                } else if (k == 0) {
                    trigger = true; // guarantee harmonic anchor on chord change
                } else if (useCrispChordPat) {
                    if (isOriginalMode) {
                        trigger = (gridProb >= 0.45);
                    } else if (gridProb >= 0.72) {
                        trigger = true;
                    } else if (gridProb >= 0.28 && rng.nextDouble() < gridProb) {
                        trigger = true;
                    }
                } else {
                    if (isOriginalMode) {
                        trigger = (gridProb >= 0.62);
                    } else {
                        const double threshold = 0.55 - 0.30 * profile.rhythm.syncopation;
                        if (gridProb >= threshold && rng.nextDouble() < std::clamp(gridProb + 0.15, 0.0, 0.98)) {
                            trigger = true;
                        }
                    }
                }
                if (trigger) {
                    onsets.emplace_back(rawBeat, step16);
                }
            }

            for (size_t idx = 0; idx < onsets.size(); ++idx) {
                const double rawOnset = onsets[idx].first;
                const int step16 = onsets[idx].second;
                const double nextOnset = (idx + 1 < onsets.size()) ? onsets[idx + 1].first : slotEnd;
                const double maxAvail = std::max(0.10, nextOnset - rawOnset);
                const double sustainMult = isIntroFirstHalf ? 0.90 : std::clamp(profile.rhythm.sustain_ratio, 0.42, 0.96);
                const double dur = std::clamp(maxAvail * sustainMult, 0.08, maxAvail * 0.95);

                const double onset = std::min(nextOnset - 0.04, applySwingAndHumanize(rawOnset, step16));
                double metricVel = (step16 == 0)
                                       ? 98.0
                                       : ((step16 % 4 == 0) ? 88.0 : ((step16 % 2 == 0) ? 84.0 : 74.0));
                if (idx > 0) {
                    if (melodyAttackNear(rawOnset).has_value() && (step16 % 4 != 0)) {
                        metricVel *= 0.86;
                    } else if (melodyRestingAt(rawOnset)) {
                        metricVel *= 1.06;
                    }
                }
                const double secGain = (perform.section == SongSection::Intro)
                                           ? (0.78 + 0.22 * (rawOnset / std::max(1.0, totalBeats)))
                                           : ((perform.section == SongSection::Outro && rawOnset >= totalBeats - 2.0 * beatsPerBar) ? 0.88 : 1.0);
                const int baseVel = static_cast<int>(std::llround(metricVel * secGain));
                const double strumScale = (dur >= 0.75) ? (isIntroFirstHalf ? 0.016 : 0.012) : 0.005;

                for (size_t v = 0; v < notes.size(); ++v) {
                    const double strum = static_cast<double>(v) * strumScale * humanize;
                    const int vel = std::clamp(static_cast<int>(std::llround(baseVel + rng.nextRange(-7.0, 7.0) * humanize)), 35, 124);
                    NoteEvent ev;
                    ev.onset_beats = std::min(nextOnset - 0.03, onset + strum);
                    ev.duration_beats = std::max(0.06, std::min(dur, nextOnset - ev.onset_beats - 0.005));
                    ev.pitch = notes[v];
                    ev.velocity = vel;
                    ev.channel = 1;
                    out.chords.push_back(ev);
                }
            }
        }
    }

    // --- 3. Channel 3: Bass (2-bar groove + counterpoint + turnaround + section conductor) ---
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

        const auto scaleIvs = scaleIntervals(profile.harmony.mode);
        auto isInScale = [&](int pitch) {
            const int rel = normalizePitchClass(pitch - profile.harmony.tonic_pc);
            return std::find(scaleIvs.begin(), scaleIvs.end(), rel) != scaleIvs.end();
        };

        const bool useCrispBassPat = hasNonZeroPat(profile.rhythm.bass_pattern);
        const bool hasBassPat2 = useCrispBassPat && hasNonZeroPat(profile.rhythm.bass_pattern_bar2);
        const double bassSustainFactor = std::clamp(0.45 + 0.50 * profile.rhythm.sustain_ratio, 0.35, 0.96);

        for (size_t s = 0; s < progression.size(); ++s) {
            const auto& slot = progression[s];
            const bool isLastSlot = (s + 1 == progression.size());
            const bool isIntroFirstHalf = (perform.section == SongSection::Intro && slot.start_beat < totalBeats * 0.5);
            const double effectiveStart = std::max(0.0, slot.start_beat + slotAnticipationShift[s]);
            const double slotEnd = (!isLastSlot)
                                       ? std::max(effectiveStart + 0.12, progression[s + 1].start_beat + slotAnticipationShift[s + 1])
                                       : totalBeats;

            const int rootPc = romanChordRootPitchClass(slot.chord, profile.harmony.tonic_pc);
            const int bassPc = normalizePitchClass(rootPc + slot.chord.bass_degree_offset);
            const int rootPitch = placeInBassRegister(bassPc);

            int fifthIv = 7;
            if (slot.chord.quality == ChordQuality::Dim ||
                slot.chord.quality == ChordQuality::HDim7 ||
                slot.chord.quality == ChordQuality::Dim7) {
                fifthIv = 6;
            } else if (slot.chord.quality == ChordQuality::Aug) {
                fifthIv = 8;
            }
            const int fifthPitch = placeInBassRegister(bassPc + fifthIv);
            const int octPitch = (rootPitch + 12 <= bassHi) ? (rootPitch + 12) : rootPitch;

            const int nSteps = std::max(1, static_cast<int>(std::floor(slot.duration_beats / stepBeats + 1e-6)));
            std::vector<std::pair<double, int>> bassOnsets;
            for (int k = 0; k < nSteps; ++k) {
                const double rawBeat = (k == 0) ? effectiveStart : (slot.start_beat + static_cast<double>(k) * stepBeats);
                if (rawBeat >= slotEnd - 0.06) break;
                if ((isIntroFirstHalf || (perform.section == SongSection::Outro && isLastSlot)) && k > 0) {
                    continue;
                }
                const int barIdx = static_cast<int>(std::floor(rawBeat / beatsPerBar));
                const double scaledBeat = rawBeat * scale;
                const int step16 = static_cast<int>(std::llround(std::fmod(scaledBeat, beatsPerBar) / 0.25)) % 16;

                if (perform.section == SongSection::Outro && rawBeat >= totalBeats - 2.0 * beatsPerBar &&
                    k > 0 && (step16 % 4 != 0)) {
                    continue;
                }

                const auto& bassGrid = (useCrispBassPat && hasBassPat2 && (barIdx % 2 == 1))
                                           ? profile.rhythm.bass_pattern_bar2
                                           : (useCrispBassPat ? profile.rhythm.bass_pattern : profile.rhythm.bass_onset_grid);
                const double gridP = bassGrid[static_cast<size_t>(step16)];

                bool trigger = false;
                if (k == 0) {
                    trigger = true;
                } else if (perform.groove_sync) {
                    if (useCrispBassPat) {
                        if (isOriginalMode) trigger = (gridP >= 0.45);
                        else if (gridP >= 0.72) trigger = true;
                        else if (gridP >= 0.25 && rng.nextDouble() < gridP) trigger = true;
                    } else {
                        if (isOriginalMode) trigger = (gridP >= 0.55);
                        else if (gridP >= 0.25 && rng.nextDouble() < gridP * 0.85) trigger = true;
                    }
                }
                if (trigger) {
                    bassOnsets.emplace_back(rawBeat, step16);
                }
            }

            const double slotBassSustain = (isIntroFirstHalf || (perform.section == SongSection::Outro && isLastSlot))
                                               ? 0.91
                                               : bassSustainFactor;

            for (size_t idx = 0; idx < bassOnsets.size(); ++idx) {
                const double rawBeat = bassOnsets[idx].first;
                const int step16 = bassOnsets[idx].second;
                const double nextBeat = (idx + 1 < bassOnsets.size()) ? bassOnsets[idx + 1].first : slotEnd;
                const double gap = std::max(0.10, nextBeat - rawBeat);
                const double dur = std::clamp(gap * slotBassSustain, 0.08, gap * 0.96);

                int pitch = rootPitch;
                if (idx > 0 && !isOriginalMode) {
                    const bool isLastBeforeChange = (idx + 1 == bassOnsets.size()) &&
                                                    (s + 1 < progression.size() || (isLastSlot && bassOnsets.size() >= 2)) &&
                                                    perform.section != SongSection::Outro;
                    if (isLastBeforeChange && rng.nextDouble() < 0.45) {
                        const size_t targetSlotIdx = (s + 1 < progression.size()) ? (s + 1) : 0;
                        const int nextRootPc = romanChordRootPitchClass(progression[targetSlotIdx].chord, profile.harmony.tonic_pc);
                        const int nextBassPc = normalizePitchClass(nextRootPc + progression[targetSlotIdx].chord.bass_degree_offset);
                        const int nextRootPitch = placeInBassRegister(nextBassPc);
                        std::vector<int> approaches;
                        for (int delta : {-2, -1, 1, 2}) {
                            const int cand = nextRootPitch + delta;
                            if (cand < bassLo || cand > bassHi) continue;
                            if (isInScale(cand) || (profile.harmony.chromaticism > 0.35 && delta == -1)) {
                                approaches.push_back(cand);
                            }
                        }
                        if (!approaches.empty()) {
                            pitch = approaches[static_cast<size_t>(rng.nextInt(0, static_cast<int>(approaches.size() - 1)))];
                        }
                    } else {
                        const double r = rng.nextDouble();
                        if (r < 0.22) pitch = fifthPitch;
                        else if (r < 0.34) pitch = octPitch;
                    }

                    // Counterpoint guard: avoid parallel unison/octave on pocket bass hits coinciding with melody attacks
                    if (const auto melPitch = melodyAttackNear(rawBeat)) {
                        if (normalizePitchClass(pitch) == normalizePitchClass(*melPitch) &&
                            normalizePitchClass(fifthPitch) != normalizePitchClass(*melPitch)) {
                            pitch = fifthPitch;
                        }
                    }
                } else if (idx > 0 && isOriginalMode && idx + 1 == bassOnsets.size() && bassOnsets.size() >= 3) {
                    pitch = fifthPitch;
                }

                const double onset = isOriginalMode ? std::clamp(rawBeat, 0.0, totalBeats - 0.03)
                                                    : applySwingAndHumanize(rawBeat, step16);
                const double introGain = (perform.section == SongSection::Intro)
                                             ? (0.80 + 0.20 * (rawBeat / std::max(1.0, totalBeats)))
                                             : ((perform.section == SongSection::Outro && isLastSlot) ? 0.85 : 1.0);
                const double metricBassVel = (idx == 0) ? 108.0 : ((step16 % 2 == 0) ? 92.0 : 80.0);
                const int vel = isOriginalMode
                                    ? std::clamp(static_cast<int>(std::llround(metricBassVel * introGain)), 45, 127)
                                    : std::clamp(static_cast<int>(std::llround((metricBassVel + rng.nextRange(-6.0, 6.0) * humanize) * introGain)), 45, 127);

                NoteEvent ev;
                ev.onset_beats = onset;
                ev.duration_beats = std::max(0.06, std::min(dur, nextBeat - onset - 0.005));
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

double scorePattern(const StyleProfile& profile, const GeneratedPattern& pat, const PerformParams& perform) {
    if (!perform.enable_melody) {
        return static_cast<double>(pat.all_events.size());
    }
    if (pat.melody.empty()) {
        return -100.0;
    }

    double score = 0.0;
    std::set<int> uniqPitches;
    int sameCount = 0;
    int singableCount = 0;
    int bigJumpCount = 0;
    bool hasBreath = false;
    int minPitch = 127, maxPitch = 0;

    for (size_t i = 0; i < pat.melody.size(); ++i) {
        const int p = pat.melody[i].pitch;
        uniqPitches.insert(p);
        minPitch = std::min(minPitch, p);
        maxPitch = std::max(maxPitch, p);
        if (i > 0) {
            const int diff = std::abs(p - pat.melody[i - 1].pitch);
            if (diff == 0) ++sameCount;
            else if (diff >= 1 && diff <= 4) ++singableCount;
            else if (diff > 7) ++bigJumpCount;

            const double restGap = pat.melody[i].onset_beats - (pat.melody[i - 1].onset_beats + pat.melody[i - 1].duration_beats);
            const double ioi = pat.melody[i].onset_beats - pat.melody[i - 1].onset_beats;
            if (restGap >= 0.35 || ioi >= 0.75) hasBreath = true;
        }
    }

    score += 1.6 * static_cast<double>(std::min<size_t>(uniqPitches.size(), 6));
    if (uniqPitches.size() < 3 && pat.melody.size() >= 4) score -= 6.0;

    const double sameRatio = static_cast<double>(sameCount) / static_cast<double>(std::max<size_t>(1, pat.melody.size() - 1));
    if (sameRatio > 0.35) score -= 6.0 * (sameRatio - 0.35);
    score += 0.45 * static_cast<double>(std::min(singableCount, 12));
    score -= 1.4 * static_cast<double>(bigJumpCount);
    if (hasBreath) score += 2.5;

    // Chord-tone accuracy vs target chord_tone_ratio + final note chord-tone resolution
    int ctHits = 0;
    bool finalIsChordTone = false;
    for (size_t i = 0; i < pat.melody.size(); ++i) {
        const double b = pat.melody[i].onset_beats;
        size_t sIdx = pat.progression.empty() ? 0 : pat.progression.size() - 1;
        for (size_t s = 0; s < pat.progression.size(); ++s) {
            if (b >= pat.progression[s].start_beat - 1e-6 &&
                b < pat.progression[s].start_beat + pat.progression[s].duration_beats - 1e-6) {
                sIdx = s;
                break;
            }
        }
        if (!pat.progression.empty()) {
            const auto pcs = chordPitchClasses(pat.progression[sIdx].chord, profile.harmony.tonic_pc, profile.harmony.mode);
            const int pc = normalizePitchClass(pat.melody[i].pitch);
            const bool isCt = (std::find(pcs.begin(), pcs.end(), pc) != pcs.end());
            if (isCt) ++ctHits;
            if (i + 1 == pat.melody.size()) finalIsChordTone = isCt;
        }
    }
    const double actualCtRatio = static_cast<double>(ctHits) / static_cast<double>(pat.melody.size());
    score += 3.0 * (1.0 - std::abs(actualCtRatio - profile.melody.chord_tone_ratio));
    if (finalIsChordTone) score += 2.0;

    if (maxPitch > minPitch && pat.melody.size() >= 6) {
        int pinned = 0;
        for (const auto& m : pat.melody) {
            if (m.pitch == minPitch || m.pitch == maxPitch) ++pinned;
        }
        if (pinned * 2 > static_cast<int>(pat.melody.size())) score -= 2.5;
    }

    return score;
}

} // namespace

GeneratedPattern PatternGenerator::generatePattern(
    const StyleProfile& profile,
    const std::vector<ProgressionSlot>& progression,
    const PerformParams& perform,
    uint64_t seed) {

    if (seed == 0ULL) {
        return generatePatternSingle(profile, progression, perform, 0ULL, true);
    }

    GeneratedPattern bestPat;
    double bestScore = -1e18;
    for (int k = 0; k < 6; ++k) {
        const uint64_t candSeed = seed ^ (static_cast<uint64_t>(k + 1) * 0x517CC1B727220A95ULL);
        auto cand = generatePatternSingle(profile, progression, perform, candSeed, false);
        const double sc = scorePattern(profile, cand, perform);
        if (sc > bestScore) {
            bestScore = sc;
            bestPat = std::move(cand);
        }
    }
    return bestPat;
}

} // namespace style_scaler
