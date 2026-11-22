#pragma once

#include "style_scaler/Types.h"

#include <array>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace style_scaler {

struct SmfReadResult {
    std::vector<NoteEvent> notes;
    double bpm = 120.0;
    int time_sig_num = 4;
    int time_sig_den = 4;
};

class SmfWriter {
public:
    static constexpr uint16_t kDefaultPpq = 480;

    static std::expected<void, std::string> writeType1(
        const std::filesystem::path& path,
        double bpm,
        int timeSigNum,
        int timeSigDen,
        const std::vector<NoteEvent>& events,
        const std::array<std::string, 3>& channelTrackNames = {"Chords", "Melody", "Bass"});

    // Convenience helper to export only one channel (e.g., 1=Chords, 2=Melody, 3=Bass) or 0=All
    static std::expected<void, std::string> writeFilteredType1(
        const std::filesystem::path& path,
        double bpm,
        int timeSigNum,
        int timeSigDen,
        const std::vector<NoteEvent>& events,
        int channelFilter);
};

class SmfReader {
public:
    static std::expected<SmfReadResult, std::string> readFile(const std::filesystem::path& path);
    static std::expected<SmfReadResult, std::string> readMemory(const uint8_t* data, size_t size);
};

} // namespace style_scaler
