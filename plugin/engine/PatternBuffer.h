#pragma once

#include <cstdint>
#include <vector>

namespace style_scaler {

struct SequencerEvent {
    double beat_pos = 0.0;
    double duration_beats = 0.25;
    uint8_t pitch = 60;
    uint8_t velocity = 100;
    uint8_t channel = 1; // 1=Chords, 2=Melody, 3=Bass
};

struct PatternBuffer {
    uint64_t generation = 0;
    double loop_beats = 16.0;
    std::vector<SequencerEvent> events; // sorted by beat_pos
};

struct AuditionEvent {
    uint8_t channel = 1; // 1..3
    uint8_t pitch = 60;
    uint8_t velocity = 100;
    bool note_on = true;
};

} // namespace style_scaler
