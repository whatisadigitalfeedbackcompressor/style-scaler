#pragma once

#include "engine/LockFreePatternSlot.h"
#include "engine/PatternBuffer.h"

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cstdint>

namespace style_scaler {

class PadAuditionQueue {
public:
    static constexpr size_t kCapacity = 256;

    bool push(const AuditionEvent& ev) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % kCapacity;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }
        ring_[head] = ev;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(AuditionEvent& out) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = ring_[tail];
        tail_.store((tail + 1) % kCapacity, std::memory_order_release);
        return true;
    }

private:
    std::array<AuditionEvent, kCapacity> ring_{};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

class MidiSequencer {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void setInternalPlaying(bool playing) noexcept;
    bool isInternalPlaying() const noexcept { return internal_playing_.load(std::memory_order_relaxed); }

    void setDawSync(bool enabled) noexcept { daw_sync_.store(enabled, std::memory_order_relaxed); }
    bool isDawSync() const noexcept { return daw_sync_.load(std::memory_order_relaxed); }

    void setInternalBpm(double bpm) noexcept { internal_bpm_.store(bpm, std::memory_order_relaxed); }
    void setPlaybackRateScale(double scale) noexcept { playback_rate_scale_.store(scale, std::memory_order_relaxed); }

    bool enqueueAudition(const AuditionEvent& ev) noexcept { return audition_queue_.push(ev); }

    void processBlock(int numSamples,
                      juce::AudioPlayHead* playHead,
                      LockFreePatternSlot& patternSlot,
                      juce::MidiBuffer& outMidi,
                      bool enableChords,
                      bool enableMelody,
                      bool enableBass) noexcept;

    // Lock-free UI queries
    double currentBeatInLoop() const noexcept { return ui_beat_in_loop_.load(std::memory_order_relaxed); }
    double currentLoopBeats() const noexcept { return ui_loop_beats_.load(std::memory_order_relaxed); }
    bool isTransportPlaying() const noexcept { return ui_playing_.load(std::memory_order_relaxed); }
    double currentBpm() const noexcept { return ui_bpm_.load(std::memory_order_relaxed); }

    bool isNoteActive(int channel1Based, int pitch) const noexcept {
        if (channel1Based < 1 || channel1Based > 3 || pitch < 0 || pitch > 127) return false;
        const int ch = channel1Based - 1;
        const size_t wordIdx = static_cast<size_t>(ch * 2 + (pitch >= 64 ? 1 : 0));
        const uint64_t mask = 1ULL << (pitch & 63);
        return (active_masks_[wordIdx].load(std::memory_order_relaxed) & mask) != 0ULL;
    }

private:
    void flushSequenceNotes(juce::MidiBuffer& outMidi, int samplePos) noexcept;
    void flushChannelNotes(int channelIdx0, juce::MidiBuffer& outMidi, int samplePos) noexcept;
    void publishActiveMasks() noexcept;

    double sample_rate_ = 44100.0;
    double internal_beat_ = 0.0;
    double expected_next_beat_ = 0.0;
    bool was_playing_ = false;
    uint64_t last_pattern_gen_ = 0;
    std::array<bool, 3> prev_channel_enabled_ = {true, true, true};

    // Remaining duration in beats for active sequenced notes (-1.0 = inactive)
    std::array<std::array<double, 128>, 3> seq_note_rem_beats_{};
    // Active audition notes
    std::array<std::array<bool, 128>, 3> aud_note_active_{};

    std::atomic<bool> internal_playing_{false};
    std::atomic<bool> daw_sync_{true};
    std::atomic<double> internal_bpm_{120.0};
    std::atomic<double> playback_rate_scale_{1.0};

    std::atomic<double> ui_beat_in_loop_{0.0};
    std::atomic<double> ui_loop_beats_{16.0};
    std::atomic<bool> ui_playing_{false};
    std::atomic<double> ui_bpm_{120.0};
    std::array<std::atomic<uint64_t>, 6> active_masks_{};

    PadAuditionQueue audition_queue_;
};

} // namespace style_scaler
