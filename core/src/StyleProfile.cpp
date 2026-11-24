#include "style_scaler/StyleProfile.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <set>
#include <sstream>

namespace style_scaler {

namespace {

std::array<double, 25> defaultIntervalHistogram() {
    // Centered around small stepwise intervals (-2, -1, 0, +1, +2) and 3rds/5ths
    std::array<double, 25> h = {
        0.005, 0.005, 0.005, 0.010, 0.015, 0.025, 0.015, 0.035, 0.055, 0.080, 0.140, 0.080,
        0.080, // 0 semitones (index 12)
        0.080, 0.140, 0.080, 0.055, 0.035, 0.015, 0.025, 0.015, 0.010, 0.005, 0.005, 0.005
    };
    const double sum = std::accumulate(h.begin(), h.end(), 0.0);
    if (sum > 0.0) {
        for (double& v : h) v /= sum;
    }
    return h;
}

template <size_t N>
std::array<double, N> parseDoubleArray(const nlohmann::json& arr, const std::string& fieldName, std::vector<std::string>& errs) {
    std::array<double, N> out{};
    if (!arr.is_array() || arr.size() != N) {
        errs.push_back("Field '" + fieldName + "' must be an array of length " + std::to_string(N));
        return out;
    }
    for (size_t i = 0; i < N; ++i) {
        if (!arr[i].is_number()) {
            errs.push_back("Element " + std::to_string(i) + " of '" + fieldName + "' must be numeric");
            return out;
        }
        out[i] = arr[i].get<double>();
    }
    return out;
}

} // namespace

StyleProfile makeDefaultStyleProfile() {
    StyleProfile p;
    p.id = "default_minor";
    p.name = "Default Minor";
    p.genre = "Reference";
    p.description = "Balanced natural minor template";
    p.harmony.tonic_pc = 0;
    p.harmony.mode = Mode::Minor;

    const std::vector<std::tuple<std::string, double, double>> seedChords = {
        {"i7",      0.35, 0.30},
        {"bVImaj7", 0.22, 0.10},
        {"iv7",     0.18, 0.15},
        {"bVII",    0.15, 0.30},
        {"v7",      0.10, 0.15}
    };
    for (const auto& [sym, w, cw] : seedChords) {
        auto parsed = parseRomanChord(sym);
        if (parsed) {
            p.harmony.vocabulary.push_back(ChordVocabEntry{sym, *parsed, w, cw});
        }
    }
    p.harmony.transitions = {
        {"i7",      {{"bVImaj7", 0.38}, {"iv7", 0.30}, {"bVII", 0.22}, {"v7", 0.10}}},
        {"bVImaj7", {{"bVII", 0.42}, {"iv7", 0.33}, {"i7", 0.15}, {"v7", 0.10}}},
        {"iv7",     {{"bVII", 0.35}, {"v7", 0.30}, {"i7", 0.25}, {"bVImaj7", 0.10}}},
        {"bVII",    {{"i7", 0.55}, {"bVImaj7", 0.25}, {"iv7", 0.20}}},
        {"v7",      {{"i7", 0.55}, {"bVImaj7", 0.30}, {"iv7", 0.15}}}
    };
    p.melody.interval_histogram = defaultIntervalHistogram();
    normalizeProfileDistributions(p);
    return p;
}

void normalizeHarmonyAndMelody(HarmonyAxis& harmony, MelodyAxis& melody) {
    // Normalize vocabulary weights and cadence_weight
    double vocabSum = 0.0;
    double cadenceSum = 0.0;
    for (auto& entry : harmony.vocabulary) {
        entry.weight = std::max(0.0, entry.weight);
        entry.cadence_weight = std::max(0.0, entry.cadence_weight);
        vocabSum += entry.weight;
        cadenceSum += entry.cadence_weight;
    }
    if (!harmony.vocabulary.empty()) {
        if (vocabSum <= 0.0) {
            const double uniform = 1.0 / static_cast<double>(harmony.vocabulary.size());
            for (auto& entry : harmony.vocabulary) entry.weight = uniform;
        } else {
            for (auto& entry : harmony.vocabulary) entry.weight /= vocabSum;
        }
        if (cadenceSum > 0.0) {
            for (auto& entry : harmony.vocabulary) entry.cadence_weight /= cadenceSum;
        }
    }

    // Normalize Markov transition rows
    for (auto& [fromSym, row] : harmony.transitions) {
        double rowSum = 0.0;
        for (auto& [toSym, prob] : row) {
            prob = std::max(0.0, prob);
            rowSum += prob;
        }
        if (rowSum > 0.0) {
            for (auto& [toSym, prob] : row) prob /= rowSum;
        } else if (!row.empty()) {
            const double u = 1.0 / static_cast<double>(row.size());
            for (auto& [toSym, prob] : row) prob = u;
        }
    }

    // Normalize melody interval histogram
    double histSum = 0.0;
    for (double& v : melody.interval_histogram) {
        v = std::max(0.0, v);
        histSum += v;
    }
    if (histSum > 0.0) {
        for (double& v : melody.interval_histogram) v /= histSum;
    } else {
        melody.interval_histogram = defaultIntervalHistogram();
    }
}

void normalizeProfileDistributions(StyleProfile& profile) {
    normalizeHarmonyAndMelody(profile.harmony, profile.melody);
    for (auto& [secName, sec] : profile.sections) {
        if (sec.populated) {
            normalizeHarmonyAndMelody(sec.harmony, sec.melody);
        }
    }
}

ValidationResult validate(const StyleProfile& profile) {
    ValidationResult res;
    auto err = [&](const std::string& msg) {
        res.valid = false;
        res.errors.push_back(msg);
    };

    if (profile.schema_version != StyleProfile::kCurrentSchemaVersion) {
        err("Unsupported schema_version: " + std::to_string(profile.schema_version));
    }
    if (profile.id.empty()) err("Profile id must not be empty");
    if (profile.name.empty()) err("Profile name must not be empty");
    if (profile.bpm_min <= 20.0 || profile.bpm_max > 350.0 || profile.bpm_min > profile.bpm_max) {
        err("Invalid BPM range");
    }
    if (profile.default_bpm <= 20.0 || profile.default_bpm > 350.0) {
        err("Invalid default_bpm");
    }
    if (profile.time_sig_num <= 0 || profile.time_sig_den <= 0) {
        err("Invalid time signature");
    }

    // Harmony checks
    if (profile.harmony.tonic_pc < 0 || profile.harmony.tonic_pc > 11) {
        err("harmony.tonic_pc must be in 0..11");
    }
    if (profile.harmony.vocabulary.empty()) {
        err("harmony.vocabulary must not be empty");
    }
    std::set<std::string> vocabSymbols;
    for (const auto& v : profile.harmony.vocabulary) {
        if (v.symbol.empty()) {
            err("harmony.vocabulary contains empty symbol");
        } else {
            vocabSymbols.insert(v.symbol);
        }
        if (v.weight < 0.0 || !std::isfinite(v.weight)) {
            err("Invalid vocabulary weight for '" + v.symbol + "'");
        }
        if (v.chord.degree < 1 || v.chord.degree > 7) {
            err("Invalid chord degree for '" + v.symbol + "'");
        }
    }
    for (const auto& [fromSym, row] : profile.harmony.transitions) {
        if (row.empty()) {
            err("Transition row for '" + fromSym + "' is empty");
            continue;
        }
        double sum = 0.0;
        for (const auto& [toSym, p] : row) {
            if (!std::isfinite(p) || p < 0.0 || p > 1.0 + 1e-6) {
                err("Invalid transition probability from '" + fromSym + "' to '" + toSym + "'");
            }
            if (!vocabSymbols.empty() && !vocabSymbols.count(toSym)) {
                err("Transition target '" + toSym + "' missing from harmony.vocabulary");
            }
            sum += p;
        }
        if (std::abs(sum - 1.0) > 1e-2) {
            err("Transition row '" + fromSym + "' must sum to 1.0 (got " + std::to_string(sum) + ")");
        }
    }

    // Rhythm checks
    if (profile.rhythm.subdivision != 16) {
        err("rhythm.subdivision must be 16");
    }
    auto checkGrid = [&](const std::array<double, 16>& g, const std::string& gName) {
        double maxVal = 0.0;
        for (size_t i = 0; i < g.size(); ++i) {
            if (!std::isfinite(g[i]) || g[i] < 0.0 || g[i] > 1.0 + 1e-6) {
                err(gName + "[" + std::to_string(i) + "] out of [0,1]");
            }
            maxVal = std::max(maxVal, g[i]);
        }
        if (maxVal <= 0.0) {
            err(gName + " must have at least one non-zero probability");
        }
    };
    checkGrid(profile.rhythm.chord_onset_grid, "rhythm.chord_onset_grid");
    checkGrid(profile.rhythm.melody_onset_grid, "rhythm.melody_onset_grid");
    checkGrid(profile.rhythm.bass_onset_grid, "rhythm.bass_onset_grid");
    if (profile.rhythm.harmonic_pacing <= 0.0 || profile.rhythm.harmonic_pacing > 8.0) {
        err("rhythm.harmonic_pacing out of range (0, 8]");
    }
    if (profile.rhythm.swing < 0.0 || profile.rhythm.swing > 1.0) {
        err("rhythm.swing out of [0,1]");
    }
    if (profile.rhythm.sustain_ratio <= 0.0 || profile.rhythm.sustain_ratio > 2.0) {
        err("rhythm.sustain_ratio out of (0, 2]");
    }

    // Voicing checks
    const auto& vo = profile.voicing;
    if (vo.register_low < 0 || vo.register_high > 127 || vo.register_low >= vo.register_high) {
        err("voicing register_low/register_high invalid");
    }
    if (vo.sub_bass_clearance_midi < 0 || vo.sub_bass_clearance_midi > vo.register_high) {
        err("voicing.sub_bass_clearance_midi out of valid range");
    }
    if (vo.bass_register_low < 0 || vo.bass_register_high > 127 || vo.bass_register_low >= vo.bass_register_high) {
        err("voicing bass register invalid");
    }
    if (vo.min_notes < 1 || vo.max_notes < vo.min_notes || vo.max_notes > 8) {
        err("voicing min_notes/max_notes invalid");
    }

    // Melody checks
    const auto& mel = profile.melody;
    if (mel.register_low < 0 || mel.register_high > 127 || mel.register_low >= mel.register_high) {
        err("melody register_low/register_high invalid");
    }
    double histSum = 0.0;
    for (double v : mel.interval_histogram) {
        if (!std::isfinite(v) || v < 0.0) {
            err("melody.interval_histogram contains negative or non-finite value");
            break;
        }
        histSum += v;
    }
    if (std::abs(histSum - 1.0) > 1e-2) {
        err("melody.interval_histogram must sum to 1.0 (got " + std::to_string(histSum) + ")");
    }

    return res;
}

namespace {

bool hasNonZeroPattern(const std::array<double, 16>& p) noexcept {
    for (double v : p) {
        if (v > 1e-6) return true;
    }
    return false;
}

nlohmann::json harmonyToJson(const HarmonyAxis& h) {
    nlohmann::json vocabJson = nlohmann::json::array();
    for (const auto& entry : h.vocabulary) {
        vocabJson.push_back({
            {"symbol", entry.symbol},
            {"degree", entry.chord.degree},
            {"accidental", entry.chord.accidental},
            {"quality", std::string(qualityName(entry.chord.quality))},
            {"bass_degree_offset", entry.chord.bass_degree_offset},
            {"weight", entry.weight},
            {"cadence_weight", entry.cadence_weight}
        });
    }
    nlohmann::json j = {
        {"tonic_pc", h.tonic_pc},
        {"mode", std::string(modeName(h.mode))},
        {"vocabulary", vocabJson},
        {"transitions", h.transitions},
        {"chromaticism", h.chromaticism}
    };
    if (!h.reference_progression.empty()) {
        j["reference_progression"] = h.reference_progression;
    }
    return j;
}

nlohmann::json rhythmToJson(const RhythmAxis& r) {
    nlohmann::json j = {
        {"subdivision", r.subdivision},
        {"chord_onset_grid", r.chord_onset_grid},
        {"melody_onset_grid", r.melody_onset_grid},
        {"bass_onset_grid", r.bass_onset_grid},
        {"syncopation", r.syncopation},
        {"anticipation", r.anticipation},
        {"swing", r.swing},
        {"harmonic_pacing", r.harmonic_pacing},
        {"sustain_ratio", r.sustain_ratio}
    };
    if (hasNonZeroPattern(r.chord_pattern)) j["chord_pattern"] = r.chord_pattern;
    if (hasNonZeroPattern(r.bass_pattern)) j["bass_pattern"] = r.bass_pattern;
    if (hasNonZeroPattern(r.melody_pattern)) j["melody_pattern"] = r.melody_pattern;
    return j;
}

nlohmann::json voicingToJson(const VoicingAxis& v) {
    return {
        {"register_low", v.register_low},
        {"register_high", v.register_high},
        {"center_pitch", v.center_pitch},
        {"spread_semitones", v.spread_semitones},
        {"min_notes", v.min_notes},
        {"max_notes", v.max_notes},
        {"extension_richness", v.extension_richness},
        {"drop2_prob", v.drop2_prob},
        {"inversion_prob", v.inversion_prob},
        {"voice_leading_tightness", v.voice_leading_tightness},
        {"sub_bass_clearance_midi", v.sub_bass_clearance_midi},
        {"bass_register_low", v.bass_register_low},
        {"bass_register_high", v.bass_register_high}
    };
}

nlohmann::json melodyToJson(const MelodyAxis& m) {
    nlohmann::json j = {
        {"interval_histogram", m.interval_histogram},
        {"chord_tone_ratio", m.chord_tone_ratio},
        {"tension_tone_ratio", m.tension_tone_ratio},
        {"motif_density", m.motif_density},
        {"motif_repetition", m.motif_repetition},
        {"arch_contour", m.arch_contour},
        {"register_low", m.register_low},
        {"register_high", m.register_high},
        {"legato_ratio", m.legato_ratio}
    };
    if (!m.reference_motif.empty()) {
        nlohmann::json motifArr = nlohmann::json::array();
        for (const auto& step : m.reference_motif) {
            motifArr.push_back({
                {"step16", step.step16},
                {"deg", step.scale_degree_offset},
                {"dur", step.duration_steps},
                {"ct", step.is_chord_tone}
            });
        }
        j["reference_motif"] = std::move(motifArr);
    }
    return j;
}

double reqNumField(const nlohmann::json& obj, const char* key, double fallback, std::vector<std::string>& errs) {
    if (!obj.contains(key)) {
        errs.push_back(std::string("Missing field: ") + key);
        return fallback;
    }
    if (!obj[key].is_number()) {
        errs.push_back(std::string("Field must be numeric: ") + key);
        return fallback;
    }
    return obj[key].get<double>();
}

HarmonyAxis harmonyFromJson(const nlohmann::json& h, std::vector<std::string>& errs) {
    HarmonyAxis out;
    out.tonic_pc = h.value("tonic_pc", 0);
    if (const auto m = parseMode(h.value("mode", "minor"))) {
        out.mode = *m;
    } else {
        errs.push_back("Invalid harmony.mode");
    }
    out.chromaticism = h.value("chromaticism", 0.15);

    if (!h.contains("vocabulary") || !h["vocabulary"].is_array()) {
        errs.push_back("Missing harmony.vocabulary array");
    } else {
        for (const auto& item : h["vocabulary"]) {
            ChordVocabEntry entry;
            entry.symbol = item.value("symbol", "");
            if (item.contains("degree") && item.contains("quality")) {
                entry.chord.degree = item.value("degree", 1);
                entry.chord.accidental = item.value("accidental", 0);
                if (auto q = parseQuality(item.value("quality", "min"))) {
                    entry.chord.quality = *q;
                } else {
                    errs.push_back("Invalid chord quality in vocabulary for '" + entry.symbol + "'");
                }
                entry.chord.bass_degree_offset = item.value("bass_degree_offset", 0);
            } else if (auto parsed = parseRomanChord(entry.symbol)) {
                entry.chord = *parsed;
            } else {
                errs.push_back("Cannot parse Roman symbol '" + entry.symbol + "'");
            }
            if (entry.symbol.empty()) {
                entry.symbol = formatRomanChord(entry.chord);
            }
            entry.weight = item.value("weight", 1.0);
            entry.cadence_weight = item.value("cadence_weight", 0.0);
            out.vocabulary.push_back(std::move(entry));
        }
    }

    if (h.contains("transitions") && h["transitions"].is_object()) {
        for (auto it = h["transitions"].begin(); it != h["transitions"].end(); ++it) {
            if (!it.value().is_object()) {
                errs.push_back("Transition row '" + it.key() + "' must be an object");
                continue;
            }
            std::map<std::string, double> row;
            for (auto jt = it.value().begin(); jt != it.value().end(); ++jt) {
                if (!jt.value().is_number()) {
                    errs.push_back("Non-numeric transition probability in '" + it.key() + "'");
                    continue;
                }
                row[jt.key()] = jt.value().get<double>();
            }
            out.transitions[it.key()] = std::move(row);
        }
    }
    if (h.contains("reference_progression") && h["reference_progression"].is_array()) {
        for (const auto& s : h["reference_progression"]) {
            if (s.is_string()) {
                auto sym = s.get<std::string>();
                if (!sym.empty()) out.reference_progression.push_back(std::move(sym));
            }
        }
    }
    return out;
}

RhythmAxis rhythmFromJson(const nlohmann::json& r, std::vector<std::string>& errs) {
    RhythmAxis out;
    out.subdivision = r.value("subdivision", 16);
    if (r.contains("chord_onset_grid")) {
        out.chord_onset_grid = parseDoubleArray<16>(r["chord_onset_grid"], "rhythm.chord_onset_grid", errs);
    } else {
        errs.push_back("Missing rhythm.chord_onset_grid");
    }
    if (r.contains("melody_onset_grid")) {
        out.melody_onset_grid = parseDoubleArray<16>(r["melody_onset_grid"], "rhythm.melody_onset_grid", errs);
    } else {
        errs.push_back("Missing rhythm.melody_onset_grid");
    }
    if (r.contains("bass_onset_grid")) {
        out.bass_onset_grid = parseDoubleArray<16>(r["bass_onset_grid"], "rhythm.bass_onset_grid", errs);
    } else {
        errs.push_back("Missing rhythm.bass_onset_grid");
    }
    if (r.contains("chord_pattern")) {
        out.chord_pattern = parseDoubleArray<16>(r["chord_pattern"], "rhythm.chord_pattern", errs);
    }
    if (r.contains("bass_pattern")) {
        out.bass_pattern = parseDoubleArray<16>(r["bass_pattern"], "rhythm.bass_pattern", errs);
    }
    if (r.contains("melody_pattern")) {
        out.melody_pattern = parseDoubleArray<16>(r["melody_pattern"], "rhythm.melody_pattern", errs);
    }
    out.syncopation = reqNumField(r, "syncopation", 0.35, errs);
    out.anticipation = reqNumField(r, "anticipation", 0.2, errs);
    out.swing = reqNumField(r, "swing", 0.0, errs);
    out.harmonic_pacing = reqNumField(r, "harmonic_pacing", 1.0, errs);
    out.sustain_ratio = reqNumField(r, "sustain_ratio", 0.85, errs);
    return out;
}

VoicingAxis voicingFromJson(const nlohmann::json& v, std::vector<std::string>& errs) {
    VoicingAxis out;
    out.register_low = v.value("register_low", 48);
    out.register_high = v.value("register_high", 76);
    out.center_pitch = v.value("center_pitch", 62);
    out.spread_semitones = reqNumField(v, "spread_semitones", 14.0, errs);
    out.min_notes = v.value("min_notes", 3);
    out.max_notes = v.value("max_notes", 5);
    out.extension_richness = reqNumField(v, "extension_richness", 0.6, errs);
    out.drop2_prob = reqNumField(v, "drop2_prob", 0.25, errs);
    out.inversion_prob = reqNumField(v, "inversion_prob", 0.4, errs);
    out.voice_leading_tightness = reqNumField(v, "voice_leading_tightness", 0.8, errs);
    out.sub_bass_clearance_midi = v.value("sub_bass_clearance_midi", 48);
    out.bass_register_low = v.value("bass_register_low", 28);
    out.bass_register_high = v.value("bass_register_high", 45);
    return out;
}

MelodyAxis melodyFromJson(const nlohmann::json& m, std::vector<std::string>& errs) {
    MelodyAxis out;
    if (m.contains("interval_histogram")) {
        out.interval_histogram = parseDoubleArray<25>(m["interval_histogram"], "melody.interval_histogram", errs);
    } else {
        errs.push_back("Missing melody.interval_histogram");
    }
    out.chord_tone_ratio = reqNumField(m, "chord_tone_ratio", 0.72, errs);
    out.tension_tone_ratio = reqNumField(m, "tension_tone_ratio", 0.18, errs);
    out.motif_density = reqNumField(m, "motif_density", 0.45, errs);
    out.motif_repetition = reqNumField(m, "motif_repetition", 0.6, errs);
    out.arch_contour = reqNumField(m, "arch_contour", 0.5, errs);
    out.register_low = m.value("register_low", 64);
    out.register_high = m.value("register_high", 86);
    out.legato_ratio = reqNumField(m, "legato_ratio", 0.7, errs);
    if (m.contains("reference_motif") && m["reference_motif"].is_array()) {
        for (const auto& item : m["reference_motif"]) {
            if (!item.is_object()) continue;
            MelodyMotifStep step;
            step.step16 = std::clamp(item.value("step16", 0), 0, 31);
            step.scale_degree_offset = std::clamp(item.value("deg", 0), -14, 14);
            step.duration_steps = std::clamp(item.value("dur", 2.0), 0.5, 16.0);
            step.is_chord_tone = item.value("ct", true);
            out.reference_motif.push_back(step);
        }
    }
    return out;
}

} // namespace

nlohmann::json toJson(const StyleProfile& p) {
    nlohmann::json j;
    j["schema_version"] = p.schema_version;
    j["id"] = p.id;
    j["name"] = p.name;
    j["genre"] = p.genre;
    j["description"] = p.description;
    j["bpm_min"] = p.bpm_min;
    j["bpm_max"] = p.bpm_max;
    j["default_bpm"] = p.default_bpm;
    j["time_signature"] = {{"numerator", p.time_sig_num}, {"denominator", p.time_sig_den}};
    j["source"] = p.source;
    j["track_count"] = std::max(1, p.track_count);
    if (!p.processed_tracks.empty()) j["processed_tracks"] = p.processed_tracks;
    if (!p.tags.empty()) j["tags"] = p.tags;
    if (!p.genre_source.empty()) j["genre_source"] = p.genre_source;
    if (!p.artist.empty()) j["artist"] = p.artist;
    if (!p.album.empty()) j["album"] = p.album;

    j["harmony"] = harmonyToJson(p.harmony);
    j["rhythm"] = rhythmToJson(p.rhythm);
    j["voicing"] = voicingToJson(p.voicing);
    j["melody"] = melodyToJson(p.melody);

    if (!p.sections.empty()) {
        nlohmann::json secObj = nlohmann::json::object();
        for (const auto& [secKey, sec] : p.sections) {
            if (!sec.populated) continue;
            secObj[secKey] = {
                {"harmony", harmonyToJson(sec.harmony)},
                {"rhythm", rhythmToJson(sec.rhythm)},
                {"voicing", voicingToJson(sec.voicing)},
                {"melody", melodyToJson(sec.melody)}
            };
        }
        if (!secObj.empty()) {
            j["sections"] = std::move(secObj);
        }
    }

    return j;
}

std::expected<StyleProfile, std::string> fromJson(const nlohmann::json& j) {
    if (!j.is_object()) {
        return std::unexpected("Root JSON must be an object");
    }

    StyleProfile p;
    std::vector<std::string> errs;

    auto reqNum = [&](const nlohmann::json& obj, const char* key, double fallback) -> double {
        return reqNumField(obj, key, fallback, errs);
    };

    if (!j.contains("schema_version") || !j["schema_version"].is_number_integer()) {
        return std::unexpected("Missing or invalid integer 'schema_version'");
    }
    p.schema_version = j["schema_version"].get<int>();
    if (p.schema_version != StyleProfile::kCurrentSchemaVersion) {
        return std::unexpected("Unsupported schema_version: " + std::to_string(p.schema_version));
    }

    p.id = j.value("id", "");
    p.name = j.value("name", "");
    p.genre = j.value("genre", "");
    p.description = j.value("description", "");
    p.bpm_min = reqNum(j, "bpm_min", 85.0);
    p.bpm_max = reqNum(j, "bpm_max", 175.0);
    p.default_bpm = reqNum(j, "default_bpm", 120.0);
    p.source = j.value("source", "preset");
    p.track_count = std::max(1, j.value("track_count", 1));
    if (j.contains("processed_tracks") && j["processed_tracks"].is_array()) {
        for (const auto& t : j["processed_tracks"]) {
            if (t.is_string()) {
                auto s = t.get<std::string>();
                if (!s.empty()) p.processed_tracks.push_back(std::move(s));
            }
        }
    }
    if (j.contains("tags") && j["tags"].is_array()) {
        for (const auto& t : j["tags"]) {
            if (t.is_string()) {
                auto s = t.get<std::string>();
                if (!s.empty()) p.tags.push_back(std::move(s));
            }
        }
    }
    p.genre_source = j.value("genre_source", "");
    p.artist = j.value("artist", "");
    p.album = j.value("album", "");

    if (j.contains("time_signature") && j["time_signature"].is_object()) {
        p.time_sig_num = j["time_signature"].value("numerator", 4);
        p.time_sig_den = j["time_signature"].value("denominator", 4);
    }

    if (!j.contains("harmony") || !j["harmony"].is_object()) {
        return std::unexpected("Missing 'harmony' object");
    }
    p.harmony = harmonyFromJson(j["harmony"], errs);

    if (!j.contains("rhythm") || !j["rhythm"].is_object()) {
        return std::unexpected("Missing 'rhythm' object");
    }
    p.rhythm = rhythmFromJson(j["rhythm"], errs);

    if (!j.contains("voicing") || !j["voicing"].is_object()) {
        return std::unexpected("Missing 'voicing' object");
    }
    p.voicing = voicingFromJson(j["voicing"], errs);

    if (!j.contains("melody") || !j["melody"].is_object()) {
        return std::unexpected("Missing 'melody' object");
    }
    p.melody = melodyFromJson(j["melody"], errs);

    if (j.contains("sections") && j["sections"].is_object()) {
        for (auto it = j["sections"].begin(); it != j["sections"].end(); ++it) {
            if (!it.value().is_object()) continue;
            const auto& secObj = it.value();
            SectionDNA sec;
            sec.harmony = (secObj.contains("harmony") && secObj["harmony"].is_object())
                              ? harmonyFromJson(secObj["harmony"], errs)
                              : p.harmony;
            sec.rhythm = (secObj.contains("rhythm") && secObj["rhythm"].is_object())
                             ? rhythmFromJson(secObj["rhythm"], errs)
                             : p.rhythm;
            sec.voicing = (secObj.contains("voicing") && secObj["voicing"].is_object())
                              ? voicingFromJson(secObj["voicing"], errs)
                              : p.voicing;
            sec.melody = (secObj.contains("melody") && secObj["melody"].is_object())
                             ? melodyFromJson(secObj["melody"], errs)
                             : p.melody;
            sec.populated = true;
            p.sections[it.key()] = std::move(sec);
        }
    }

    if (!errs.empty()) {
        std::ostringstream oss;
        for (size_t i = 0; i < errs.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << errs[i];
        }
        return std::unexpected(oss.str());
    }

    const auto vr = validate(p);
    if (!vr.valid) {
        std::ostringstream oss;
        for (size_t i = 0; i < vr.errors.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << vr.errors[i];
        }
        return std::unexpected(oss.str());
    }

    return p;
}

std::expected<StyleProfile, std::string> loadFromJsonString(std::string_view jsonText) {
    try {
        auto parsed = nlohmann::json::parse(jsonText.begin(), jsonText.end());
        return fromJson(parsed);
    } catch (const std::exception& e) {
        return std::unexpected(std::string("JSON parse error: ") + e.what());
    }
}

std::expected<StyleProfile, std::string> loadFromFile(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        const auto u8 = path.u8string();
        return std::unexpected("Unable to open style profile file: " +
                               std::string(reinterpret_cast<const char*>(u8.data()), u8.size()));
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return loadFromJsonString(ss.str());
}

std::expected<void, std::string> saveToFile(const StyleProfile& profile, const std::filesystem::path& path) {
    const auto vr = validate(profile);
    if (!vr.valid) {
        return std::unexpected("Cannot save invalid StyleProfile: " + vr.errors.front());
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const auto u8 = path.u8string();
    const std::string pathStr(reinterpret_cast<const char*>(u8.data()), u8.size());
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return std::unexpected("Unable to open file for writing: " + pathStr);
    }
    ofs << toJson(profile).dump(2) << '\n';
    if (!ofs) {
        return std::unexpected("Failed while writing file: " + pathStr);
    }
    return {};
}

SectionDNA deriveSectionDNA(const StyleProfile& profile, SongSection section) {
    SectionDNA sec;
    sec.harmony = profile.harmony;
    sec.rhythm = profile.rhythm;
    sec.voicing = profile.voicing;
    sec.melody = profile.melody;
    sec.populated = true;

    if (section == SongSection::Intro) {
        sec.rhythm.harmonic_pacing = std::clamp(sec.rhythm.harmonic_pacing * 0.55, 0.25, 4.0);
        sec.rhythm.sustain_ratio = std::clamp(sec.rhythm.sustain_ratio * 1.15, 0.2, 1.5);
        sec.rhythm.syncopation = std::clamp(sec.rhythm.syncopation * 0.65, 0.0, 1.0);
        for (size_t i = 0; i < 16; ++i) {
            sec.rhythm.bass_onset_grid[i] *= (i % 8 == 0) ? 1.0 : 0.35;
            sec.rhythm.melody_onset_grid[i] *= 0.55;
        }
        sec.voicing.register_low = std::min(sec.voicing.register_high - 6, sec.voicing.register_low + 4);
        sec.voicing.center_pitch = std::clamp(sec.voicing.center_pitch + 2, sec.voicing.register_low, sec.voicing.register_high);
        sec.voicing.spread_semitones = std::clamp(sec.voicing.spread_semitones * 0.85, 5.0, 30.0);
        sec.melody.motif_density = std::clamp(sec.melody.motif_density * 0.60, 0.1, 1.0);
        for (auto& v : sec.harmony.vocabulary) {
            if (v.chord.degree == 1 && v.chord.accidental == 0) {
                v.weight *= 1.5;
            }
        }
        normalizeHarmonyAndMelody(sec.harmony, sec.melody);
    } else if (section == SongSection::Outro) {
        sec.rhythm.sustain_ratio = std::clamp(sec.rhythm.sustain_ratio * 1.20, 0.2, 1.5);
        sec.rhythm.syncopation = std::clamp(sec.rhythm.syncopation * 0.75, 0.0, 1.0);
        sec.melody.motif_density = std::clamp(sec.melody.motif_density * 0.70, 0.1, 1.0);
        for (auto& v : sec.harmony.vocabulary) {
            if (v.chord.degree == 1 && v.chord.accidental == 0) {
                v.cadence_weight += 0.5;
                v.weight *= 1.35;
            }
        }
        normalizeHarmonyAndMelody(sec.harmony, sec.melody);
    }
    return sec;
}

SectionDNA resolveSectionDNA(const StyleProfile& profile, SongSection section) {
    const std::string key(songSectionName(section));
    if (const auto it = profile.sections.find(key);
        it != profile.sections.end() && it->second.populated && !it->second.harmony.vocabulary.empty()) {
        return it->second;
    }
    return deriveSectionDNA(profile, section);
}

} // namespace style_scaler
