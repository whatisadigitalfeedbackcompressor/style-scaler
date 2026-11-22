#pragma once

#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <optional>
#include <string>
#include <vector>

namespace style_scaler {

struct AnalysisOptions {
    std::string id = "analyzed_style";
    std::string name = "Analyzed Style";
    std::string genre = "Analyzed";
    std::string source = "in_plugin_analysis";
    double bpm_hint = 0.0;                  // <= 0 triggers automatic tempo estimation
    int time_sig_num = 4;
    int time_sig_den = 4;
    std::optional<int> key_override_pc;
    std::optional<Mode> mode_override;
};

struct SongInput {
    std::vector<NoteEvent> notes;
    double bpm = 120.0;
    int time_sig_num = 4;
    int time_sig_den = 4;
};

class MidiStyleAnalyzer {
public:
    static double estimateBpm(const std::vector<TimedNoteSec>& notes);

    static StyleProfile analyze(const std::vector<NoteEvent>& events,
                                double bpm,
                                const AnalysisOptions& options = {});

    static StyleProfile analyzeTimed(const std::vector<TimedNoteSec>& notes,
                                     const AnalysisOptions& options = {});

    static StyleProfile analyzeCorpus(const std::vector<SongInput>& songs,
                                      const AnalysisOptions& options = {});
};

} // namespace style_scaler
