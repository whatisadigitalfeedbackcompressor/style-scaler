#include "engine/MidiSequencer.h"

#include <algorithm>
#include <cmath>

namespace style_scaler {

namespace {
double wrapBeat(double b, double loopBeats) noexcept {
    if (loopBeats <= 0.0) return 0.0;
    double w = std::fmod(b, loopBeats);
    if (w < 0.0) w += loopBeats;
    return w;
}
} // namespace

void MidiSequencer::prepare(double sampleRate) noexcept {
    sample_rate_ = std::max(8000.0, sampleRate);
    reset();
}

void MidiSequencer::reset() noexcept {
    internal_beat_ = 0.0;
    expected_next_beat_ = 0.0;
    was_playing_ = false;
    last_pattern_gen_ = 0;
    prev_channel_enabled_ = {true, true, true};
    for (auto& chArr : seq_note_rem_beats_) chArr.fill(-1.0);
    for (auto& chArr : aud_note_active_) chArr.fill(false);
    for (auto& m : active_masks_) m.store(0ULL, std::memory_order_relaxed);
    ui_beat_in_loop_.store(0.0, std::memory_order_relaxed);
    ui_playing_.store(false, std::memory_order_relaxed);
}

void MidiSequencer::setInternalPlaying(bool playing) noexcept {
    if (playing && !internal_playing_.load(std::memory_order_relaxed)) {
        internal_beat_ = 0.0;
    }
    internal_playing_.store(playing, std::memory_order_release);
}

void MidiSequencer::flushChannelNotes(int ch0, juce::MidiBuffer& outMidi, int samplePos) noexcept {
    if (ch0 < 0 || ch0 > 2) return;
    auto& arr = seq_note_rem_beats_[static_cast<size_t>(ch0)];
    for (int p = 0; p < 128; ++p) {
        if (arr[static_cast<size_t>(p)] >= 0.0) {
            arr[static_cast<size_t>(p)] = -1.0;
            if (!aud_note_active_[static_cast<size_t>(ch0)][static_cast<size_t>(p)]) {
                outMidi.addEvent(juce::MidiMessage::noteOff(ch0 + 1, p), samplePos);
            }
        }
    }
}

void MidiSequencer::flushSequenceNotes(juce::MidiBuffer& outMidi, int samplePos) noexcept {
    for (int ch = 0; ch < 3; ++ch) {
        flushChannelNotes(ch, outMidi, samplePos);
    }
}

void MidiSequencer::publishActiveMasks() noexcept {
    for (int ch = 0; ch < 3; ++ch) {
        uint64_t lo = 0ULL;
        uint64_t hi = 0ULL;
        for (int p = 0; p < 64; ++p) {
            if (seq_note_rem_beats_[static_cast<size_t>(ch)][static_cast<size_t>(p)] >= 0.0 ||
                aud_note_active_[static_cast<size_t>(ch)][static_cast<size_t>(p)]) {
                lo |= (1ULL << p);
            }
        }
        for (int p = 64; p < 128; ++p) {
            if (seq_note_rem_beats_[static_cast<size_t>(ch)][static_cast<size_t>(p)] >= 0.0 ||
                aud_note_active_[static_cast<size_t>(ch)][static_cast<size_t>(p)]) {
                hi |= (1ULL << (p - 64));
            }
        }
        active_masks_[static_cast<size_t>(ch * 2)].store(lo, std::memory_order_relaxed);
        active_masks_[static_cast<size_t>(ch * 2 + 1)].store(hi, std::memory_order_relaxed);
    }
}

void MidiSequencer::processBlock(int numSamples,
                                 juce::AudioPlayHead* playHead,
                                 LockFreePatternSlot& patternSlot,
                                 juce::MidiBuffer& outMidi,
                                 bool enableChords,
                                 bool enableMelody,
                                 bool enableBass) noexcept {
    if (numSamples <= 0) return;

    // 1. Drain lock-free audition queue at sample 0 (instant pad/keyboard feedback)
    AuditionEvent aud{};
    while (audition_queue_.pop(aud)) {
        const int ch = std::clamp(static_cast<int>(aud.channel), 1, 3);
        const int ch0 = ch - 1;
        const int pitch = std::clamp(static_cast<int>(aud.pitch), 0, 127);
        if (aud.note_on) {
            if (aud_note_active_[static_cast<size_t>(ch0)][static_cast<size_t>(pitch)]) {
                outMidi.addEvent(juce::MidiMessage::noteOff(ch, pitch), 0);
            }
            aud_note_active_[static_cast<size_t>(ch0)][static_cast<size_t>(pitch)] = true;
            outMidi.addEvent(juce::MidiMessage::noteOn(ch, pitch, static_cast<juce::uint8>(std::clamp<int>(aud.velocity, 1, 127))), 0);
        } else {
            aud_note_active_[static_cast<size_t>(ch0)][static_cast<size_t>(pitch)] = false;
            if (seq_note_rem_beats_[static_cast<size_t>(ch0)][static_cast<size_t>(pitch)] < 0.0) {
                outMidi.addEvent(juce::MidiMessage::noteOff(ch, pitch), 0);
            }
        }
    }

    const std::array<bool, 3> chEnabled = {enableChords, enableMelody, enableBass};
    for (int ch = 0; ch < 3; ++ch) {
        if (prev_channel_enabled_[static_cast<size_t>(ch)] && !chEnabled[static_cast<size_t>(ch)]) {
            flushChannelNotes(ch, outMidi, 0);
        }
        prev_channel_enabled_[static_cast<size_t>(ch)] = chEnabled[static_cast<size_t>(ch)];
    }

    // 2. Resolve transport state (DAW PlayHead vs internal Standalone/audition clock)
    double bpm = std::clamp(internal_bpm_.load(std::memory_order_relaxed), 30.0, 300.0);
    bool hostPlaying = false;
    double hostPpq = 0.0;
    bool hasHostPpq = false;

    if (daw_sync_.load(std::memory_order_relaxed) && playHead != nullptr) {
        if (const auto pos = playHead->getPosition()) {
            if (const auto hostBpm = pos->getBpm(); hostBpm.hasValue() && *hostBpm > 20.0) {
                bpm = std::clamp(*hostBpm, 30.0, 300.0);
            }
            hostPlaying = pos->getIsPlaying();
            if (const auto ppq = pos->getPpqPosition(); ppq.hasValue()) {
                hostPpq = std::max(0.0, *ppq);
                hasHostPpq = true;
            }
        }
    }

    const bool useHostClock = daw_sync_.load(std::memory_order_relaxed) && hostPlaying && hasHostPpq;
    const bool isPlaying = useHostClock || internal_playing_.load(std::memory_order_relaxed);
    const double rateScale = std::clamp(playback_rate_scale_.load(std::memory_order_relaxed), 0.25, 4.0);
    const double beatsPerSample = (bpm / 60.0) * rateScale / sample_rate_;
    const double blockBeats = beatsPerSample * static_cast<double>(numSamples);

    ui_bpm_.store(bpm, std::memory_order_relaxed);
    ui_playing_.store(isPlaying, std::memory_order_relaxed);

    LockFreePatternSlot::ScopedAudioRead guard(patternSlot);
    const PatternBuffer* pat = guard.get();

    if (!isPlaying || pat == nullptr || pat->loop_beats <= 0.0) {
        if (was_playing_) {
            flushSequenceNotes(outMidi, 0);
        }
        was_playing_ = false;
        publishActiveMasks();
        return;
    }

    ui_loop_beats_.store(pat->loop_beats, std::memory_order_relaxed);

    // Flush active notes on pattern swap
    if (pat->generation != last_pattern_gen_) {
        if (was_playing_) {
            flushSequenceNotes(outMidi, 0);
        }
        last_pattern_gen_ = pat->generation;
    }

    const double blockStartRaw = useHostClock ? (hostPpq * rateScale) : internal_beat_;
    const double blockEndRaw = blockStartRaw + blockBeats;

    // Detect DAW seek / jump
    if (was_playing_ && std::abs(blockStartRaw - expected_next_beat_) > 0.15) {
        flushSequenceNotes(outMidi, 0);
    }

    // 3. Advance active sequence note-offs inside this block
    for (int ch = 0; ch < 3; ++ch) {
        auto& arr = seq_note_rem_beats_[static_cast<size_t>(ch)];
        for (int p = 0; p < 128; ++p) {
            const double rem = arr[static_cast<size_t>(p)];
            if (rem < 0.0) continue;
            if (rem <= blockBeats) {
                const int offSample = std::clamp(static_cast<int>(std::llround(rem / beatsPerSample)), 0, numSamples - 1);
                arr[static_cast<size_t>(p)] = -1.0;
                if (!aud_note_active_[static_cast<size_t>(ch)][static_cast<size_t>(p)]) {
                    outMidi.addEvent(juce::MidiMessage::noteOff(ch + 1, p), offSample);
                }
            } else {
                arr[static_cast<size_t>(p)] = rem - blockBeats;
            }
        }
    }

    // 4. Schedule new Note-Ons within [blockStartRaw, blockEndRaw) wrapped modulo pat->loop_beats
    const double loopBeats = pat->loop_beats;
    auto scheduleWindow = [&](double winStart, double winEnd, double baseOffsetBeats) {
        for (const auto& ev : pat->events) {
            if (ev.beat_pos < winStart - 1e-9) continue;
            if (ev.beat_pos >= winEnd - 1e-9) break;

            const int ch = std::clamp(static_cast<int>(ev.channel), 1, 3);
            const int ch0 = ch - 1;
            if (!chEnabled[static_cast<size_t>(ch0)]) continue;

            const double relBeats = std::max(0.0, baseOffsetBeats + (ev.beat_pos - winStart));
            const int onSample = std::clamp(static_cast<int>(std::llround(relBeats / beatsPerSample)), 0, numSamples - 1);
            const int pitch = std::clamp(static_cast<int>(ev.pitch), 0, 127);

            // Close existing note on identical channel+pitch cleanly before retriggering
            if (seq_note_rem_beats_[static_cast<size_t>(ch0)][static_cast<size_t>(pitch)] >= 0.0) {
                outMidi.addEvent(juce::MidiMessage::noteOff(ch, pitch), onSample);
                seq_note_rem_beats_[static_cast<size_t>(ch0)][static_cast<size_t>(pitch)] = -1.0;
            }

            outMidi.addEvent(
                juce::MidiMessage::noteOn(ch, pitch, static_cast<juce::uint8>(std::clamp<int>(ev.velocity, 1, 127))),
                onSample);

            const double remainingFromBlockEnd = (ev.duration_beats + relBeats) - blockBeats;
            if (remainingFromBlockEnd <= 0.0) {
                const int offSample = std::clamp(
                    static_cast<int>(std::llround((relBeats + ev.duration_beats) / beatsPerSample)),
                    onSample + 1 <= numSamples - 1 ? onSample + 1 : onSample,
                    numSamples - 1);
                outMidi.addEvent(juce::MidiMessage::noteOff(ch, pitch), offSample);
            } else {
                seq_note_rem_beats_[static_cast<size_t>(ch0)][static_cast<size_t>(pitch)] = remainingFromBlockEnd;
            }
        }
    };

    const double startWrapped = wrapBeat(blockStartRaw, loopBeats);
    const double span = blockBeats;
    if (startWrapped + span <= loopBeats) {
        scheduleWindow(startWrapped, startWrapped + span, 0.0);
    } else {
        const double firstPart = loopBeats - startWrapped;
        scheduleWindow(startWrapped, loopBeats, 0.0);
        scheduleWindow(0.0, span - firstPart, firstPart);
    }

    if (!useHostClock) {
        internal_beat_ = wrapBeat(blockEndRaw, loopBeats);
    }
    expected_next_beat_ = blockEndRaw;
    was_playing_ = true;
    ui_beat_in_loop_.store(wrapBeat(blockEndRaw, loopBeats), std::memory_order_relaxed);
    publishActiveMasks();
}

} // namespace style_scaler
