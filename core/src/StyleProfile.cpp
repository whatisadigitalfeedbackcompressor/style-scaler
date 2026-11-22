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

void normalizeProfileDistributions(StyleProfile& profile) {
    // Normalize vocabulary weights
    double vocabSum = 0.0;
    for (auto& entry : profile.harmony.vocabulary) {
        entry.weight = std::max(0.0, entry.weight);
        entry.cadence_weight = std::max(0.0, entry.cadence_weight);
        vocabSum += entry.weight;
    }
    if (!profile.harmony.vocabulary.empty()) {
        if (vocabSum <= 0.0) {
            const double uniform = 1.0 / static_cast<double>(profile.harmony.vocabulary.size());
            for (auto& entry : profile.harmony.vocabulary) entry.weight = uniform;
        } else {
            for (auto& entry : profile.harmony.vocabulary) entry.weight /= vocabSum;
        }
    }

    // Normalize Markov transition rows
    for (auto& [fromSym, row] : profile.harmony.transitions) {
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
    for (double& v : profile.melody.interval_histogram) {
        v = std::max(0.0, v);
        histSum += v;
    }
    if (histSum > 0.0) {
        for (double& v : profile.melody.interval_histogram) v /= histSum;
    } else {
        profile.melody.interval_histogram = defaultIntervalHistogram();
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

    // Harmony
    nlohmann::json vocabJson = nlohmann::json::array();
    for (const auto& entry : p.harmony.vocabulary) {
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
    j["harmony"] = {
        {"tonic_pc", p.harmony.tonic_pc},
        {"mode", std::string(modeName(p.harmony.mode))},
        {"vocabulary", vocabJson},
        {"transitions", p.harmony.transitions},
        {"chromaticism", p.harmony.chromaticism}
    };

    // Rhythm
    j["rhythm"] = {
        {"subdivision", p.rhythm.subdivision},
        {"chord_onset_grid", p.rhythm.chord_onset_grid},
        {"melody_onset_grid", p.rhythm.melody_onset_grid},
        {"bass_onset_grid", p.rhythm.bass_onset_grid},
        {"syncopation", p.rhythm.syncopation},
        {"anticipation", p.rhythm.anticipation},
        {"swing", p.rhythm.swing},
        {"harmonic_pacing", p.rhythm.harmonic_pacing},
        {"sustain_ratio", p.rhythm.sustain_ratio}
    };

    // Voicing
    j["voicing"] = {
        {"register_low", p.voicing.register_low},
        {"register_high", p.voicing.register_high},
        {"center_pitch", p.voicing.center_pitch},
        {"spread_semitones", p.voicing.spread_semitones},
        {"min_notes", p.voicing.min_notes},
        {"max_notes", p.voicing.max_notes},
        {"extension_richness", p.voicing.extension_richness},
        {"drop2_prob", p.voicing.drop2_prob},
        {"inversion_prob", p.voicing.inversion_prob},
        {"voice_leading_tightness", p.voicing.voice_leading_tightness},
        {"sub_bass_clearance_midi", p.voicing.sub_bass_clearance_midi},
        {"bass_register_low", p.voicing.bass_register_low},
        {"bass_register_high", p.voicing.bass_register_high}
    };

    // Melody
    j["melody"] = {
        {"interval_histogram", p.melody.interval_histogram},
        {"chord_tone_ratio", p.melody.chord_tone_ratio},
        {"tension_tone_ratio", p.melody.tension_tone_ratio},
        {"motif_density", p.melody.motif_density},
        {"motif_repetition", p.melody.motif_repetition},
        {"arch_contour", p.melody.arch_contour},
        {"register_low", p.melody.register_low},
        {"register_high", p.melody.register_high},
        {"legato_ratio", p.melody.legato_ratio}
    };

    return j;
}

std::expected<StyleProfile, std::string> fromJson(const nlohmann::json& j) {
    if (!j.is_object()) {
        return std::unexpected("Root JSON must be an object");
    }

    StyleProfile p;
    std::vector<std::string> errs;

    auto reqNum = [&](const nlohmann::json& obj, const char* key, double fallback) -> double {
        if (!obj.contains(key)) {
            errs.push_back(std::string("Missing field: ") + key);
            return fallback;
        }
        if (!obj[key].is_number()) {
            errs.push_back(std::string("Field must be numeric: ") + key);
            return fallback;
        }
        return obj[key].get<double>();
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

    if (j.contains("time_signature") && j["time_signature"].is_object()) {
        p.time_sig_num = j["time_signature"].value("numerator", 4);
        p.time_sig_den = j["time_signature"].value("denominator", 4);
    }

    // Harmony
    if (!j.contains("harmony") || !j["harmony"].is_object()) {
        return std::unexpected("Missing 'harmony' object");
    }
    {
        const auto& h = j["harmony"];
        p.harmony.tonic_pc = h.value("tonic_pc", 0);
        if (const auto m = parseMode(h.value("mode", "minor"))) {
            p.harmony.mode = *m;
        } else {
            errs.push_back("Invalid harmony.mode");
        }
        p.harmony.chromaticism = h.value("chromaticism", 0.15);

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
                p.harmony.vocabulary.push_back(std::move(entry));
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
                p.harmony.transitions[it.key()] = std::move(row);
            }
        }
    }

    // Rhythm
    if (!j.contains("rhythm") || !j["rhythm"].is_object()) {
        return std::unexpected("Missing 'rhythm' object");
    }
    {
        const auto& r = j["rhythm"];
        p.rhythm.subdivision = r.value("subdivision", 16);
        if (r.contains("chord_onset_grid")) {
            p.rhythm.chord_onset_grid = parseDoubleArray<16>(r["chord_onset_grid"], "rhythm.chord_onset_grid", errs);
        } else {
            errs.push_back("Missing rhythm.chord_onset_grid");
        }
        if (r.contains("melody_onset_grid")) {
            p.rhythm.melody_onset_grid = parseDoubleArray<16>(r["melody_onset_grid"], "rhythm.melody_onset_grid", errs);
        } else {
            errs.push_back("Missing rhythm.melody_onset_grid");
        }
        if (r.contains("bass_onset_grid")) {
            p.rhythm.bass_onset_grid = parseDoubleArray<16>(r["bass_onset_grid"], "rhythm.bass_onset_grid", errs);
        } else {
            errs.push_back("Missing rhythm.bass_onset_grid");
        }
        p.rhythm.syncopation = reqNum(r, "syncopation", 0.35);
        p.rhythm.anticipation = reqNum(r, "anticipation", 0.2);
        p.rhythm.swing = reqNum(r, "swing", 0.0);
        p.rhythm.harmonic_pacing = reqNum(r, "harmonic_pacing", 1.0);
        p.rhythm.sustain_ratio = reqNum(r, "sustain_ratio", 0.85);
    }

    // Voicing
    if (!j.contains("voicing") || !j["voicing"].is_object()) {
        return std::unexpected("Missing 'voicing' object");
    }
    {
        const auto& v = j["voicing"];
        p.voicing.register_low = v.value("register_low", 48);
        p.voicing.register_high = v.value("register_high", 76);
        p.voicing.center_pitch = v.value("center_pitch", 62);
        p.voicing.spread_semitones = reqNum(v, "spread_semitones", 14.0);
        p.voicing.min_notes = v.value("min_notes", 3);
        p.voicing.max_notes = v.value("max_notes", 5);
        p.voicing.extension_richness = reqNum(v, "extension_richness", 0.6);
        p.voicing.drop2_prob = reqNum(v, "drop2_prob", 0.25);
        p.voicing.inversion_prob = reqNum(v, "inversion_prob", 0.4);
        p.voicing.voice_leading_tightness = reqNum(v, "voice_leading_tightness", 0.8);
        p.voicing.sub_bass_clearance_midi = v.value("sub_bass_clearance_midi", 48);
        p.voicing.bass_register_low = v.value("bass_register_low", 28);
        p.voicing.bass_register_high = v.value("bass_register_high", 45);
    }

    // Melody
    if (!j.contains("melody") || !j["melody"].is_object()) {
        return std::unexpected("Missing 'melody' object");
    }
    {
        const auto& m = j["melody"];
        if (m.contains("interval_histogram")) {
            p.melody.interval_histogram = parseDoubleArray<25>(m["interval_histogram"], "melody.interval_histogram", errs);
        } else {
            errs.push_back("Missing melody.interval_histogram");
        }
        p.melody.chord_tone_ratio = reqNum(m, "chord_tone_ratio", 0.72);
        p.melody.tension_tone_ratio = reqNum(m, "tension_tone_ratio", 0.18);
        p.melody.motif_density = reqNum(m, "motif_density", 0.45);
        p.melody.motif_repetition = reqNum(m, "motif_repetition", 0.6);
        p.melody.arch_contour = reqNum(m, "arch_contour", 0.5);
        p.melody.register_low = m.value("register_low", 64);
        p.melody.register_high = m.value("register_high", 86);
        p.melody.legato_ratio = reqNum(m, "legato_ratio", 0.7);
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
        return std::unexpected("Unable to open style profile file: " + path.string());
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
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return std::unexpected("Unable to open file for writing: " + path.string());
    }
    ofs << toJson(profile).dump(2) << '\n';
    if (!ofs) {
        return std::unexpected("Failed while writing file: " + path.string());
    }
    return {};
}

} // namespace style_scaler
