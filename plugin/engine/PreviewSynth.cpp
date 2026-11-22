#include "engine/PreviewSynth.h"

#include <algorithm>
#include <cmath>

namespace style_scaler {

namespace {
constexpr double kTwoPi = 6.28318530717958647692;

inline double midiToHz(int pitch) noexcept {
    return 440.0 * std::pow(2.0, (static_cast<double>(pitch) - 69.0) / 12.0);
}
} // namespace

void PreviewSynth::prepare(double sampleRate) noexcept {
    sample_rate_ = std::max(8000.0, sampleRate);
    reset();
}

void PreviewSynth::reset() noexcept {
    for (auto& v : voices_) {
        v = Voice{};
    }
    voice_counter_ = 0;
}

int PreviewSynth::allocateVoiceIndex() noexcept {
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!voices_[static_cast<size_t>(i)].active) return i;
    }
    // Steal oldest releasing voice first, else oldest active voice
    int bestIdx = 0;
    bool bestReleasing = false;
    uint64_t oldestAge = UINT64_MAX;
    for (int i = 0; i < kMaxVoices; ++i) {
        const auto& v = voices_[static_cast<size_t>(i)];
        if (v.releasing && !bestReleasing) {
            bestReleasing = true;
            oldestAge = v.age;
            bestIdx = i;
        } else if (v.releasing == bestReleasing && v.age < oldestAge) {
            oldestAge = v.age;
            bestIdx = i;
        }
    }
    return bestIdx;
}

void PreviewSynth::noteOn(int channel1Based, int pitch, float velocityNorm) noexcept {
    const int ch = std::clamp(channel1Based, 1, 16);
    const int p = std::clamp(pitch, 0, 127);

    // Retrigger matching voice if already sounding
    int idx = -1;
    for (int i = 0; i < kMaxVoices; ++i) {
        const auto& v = voices_[static_cast<size_t>(i)];
        if (v.active && v.channel == ch && v.pitch == p) {
            idx = i;
            break;
        }
    }
    if (idx < 0) idx = allocateVoiceIndex();

    auto& v = voices_[static_cast<size_t>(idx)];
    v.active = true;
    v.releasing = false;
    v.channel = ch;
    v.pitch = p;
    v.velocity = std::clamp(velocityNorm, 0.05f, 1.0f);
    v.phase = 0.0;
    v.phase_inc = midiToHz(p) / sample_rate_;
    v.env = 0.001;
    v.lp_state = 0.0;
    v.age = ++voice_counter_;
}

void PreviewSynth::noteOff(int channel1Based, int pitch) noexcept {
    const int ch = std::clamp(channel1Based, 1, 16);
    const int p = std::clamp(pitch, 0, 127);
    for (auto& v : voices_) {
        if (v.active && v.channel == ch && v.pitch == p) {
            v.releasing = true;
        }
    }
}

void PreviewSynth::allNotesOff() noexcept {
    for (auto& v : voices_) {
        if (v.active) v.releasing = true;
    }
}

void PreviewSynth::renderBlock(juce::AudioBuffer<float>& audioBuffer,
                               const juce::MidiBuffer& midiMessages,
                               float masterGain,
                               bool previewChords,
                               bool previewMelody,
                               bool previewBass) noexcept {
    const int numSamples = audioBuffer.getNumSamples();
    const int numChannels = audioBuffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0) return;

    auto handleMidi = [&](const juce::MidiMessage& m) {
        const int ch = m.getChannel();
        if (m.isAllNotesOff() || m.isAllSoundOff()) {
            allNotesOff();
            return;
        }
        if (m.isNoteOn()) {
            if ((ch == 1 && !previewChords) || (ch == 2 && !previewMelody) || (ch == 3 && !previewBass)) {
                return;
            }
            noteOn(ch, m.getNoteNumber(), m.getFloatVelocity());
        } else if (m.isNoteOff()) {
            noteOff(ch, m.getNoteNumber());
        }
    };

    auto midiIt = midiMessages.cbegin();
    const double attackRate = 1.0 / (0.005 * sample_rate_);
    const double releaseChord = std::exp(-1.0 / (0.28 * sample_rate_));
    const double releaseMelody = std::exp(-1.0 / (0.18 * sample_rate_));
    const double releaseBass = std::exp(-1.0 / (0.09 * sample_rate_));
    const double decayChord = std::exp(-1.0 / (1.4 * sample_rate_));
    const double decayMelody = std::exp(-1.0 / (0.55 * sample_rate_));

    for (int s = 0; s < numSamples; ++s) {
        while (midiIt != midiMessages.cend() && (*midiIt).samplePosition <= s) {
            handleMidi((*midiIt).getMessage());
            ++midiIt;
        }

        double mix = 0.0;
        for (auto& v : voices_) {
            if (!v.active) continue;

            if (!v.releasing) {
                if (v.env < 1.0) {
                    v.env = std::min(1.0, v.env + attackRate);
                }
                if (v.channel == 1) {
                    v.env = std::max(0.35, v.env * decayChord);
                } else if (v.channel == 2) {
                    v.env = std::max(0.18, v.env * decayMelody);
                }
            } else {
                const double rel = (v.channel == 3) ? releaseBass : (v.channel == 2 ? releaseMelody : releaseChord);
                v.env *= rel;
                if (v.env < 1e-4) {
                    v.active = false;
                    v.env = 0.0;
                    continue;
                }
            }

            const double p = v.phase * kTwoPi;
            double sample = 0.0;
            if (v.channel == 3) {
                // Punchy clean sub-bass: fundamental + gentle 2nd harmonic warmth
                const double raw = std::sin(p) + 0.14 * std::sin(2.0 * p);
                sample = std::tanh(raw * 1.15) * 0.42;
            } else if (v.channel == 2) {
                // Soft glassy bell / pluck melody
                const double envBri = v.env * v.velocity;
                const double fm = std::sin(p + 0.45 * envBri * std::sin(2.0 * p));
                sample = (0.72 * fm + 0.22 * std::sin(3.0 * p) * envBri) * 0.24;
            } else {
                // Warm Rhodes-style electric piano for chords
                const double tine = 0.22 * v.velocity * std::sin(2.0 * p) + 0.08 * v.velocity * std::sin(3.0 * p);
                const double raw = std::sin(p) + tine * (0.4 + 0.6 * v.env);
                v.lp_state += 0.28 * (raw - v.lp_state);
                sample = v.lp_state * 0.20;
            }

            mix += sample * v.env * static_cast<double>(v.velocity);
            v.phase += v.phase_inc;
            if (v.phase >= 1.0) v.phase -= 1.0;
        }

        const float outSample = static_cast<float>(std::tanh(mix * static_cast<double>(masterGain)));
        for (int ch = 0; ch < numChannels; ++ch) {
            audioBuffer.addSample(ch, s, outSample);
        }
    }

    // Drain any trailing MIDI events past numSamples
    while (midiIt != midiMessages.cend()) {
        handleMidi((*midiIt).getMessage());
        ++midiIt;
    }
}

} // namespace style_scaler
