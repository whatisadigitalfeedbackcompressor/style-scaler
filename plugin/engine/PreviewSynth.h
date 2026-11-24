#pragma once

#include <JuceHeader.h>
#include <array>
#include <cstdint>

namespace style_scaler {

/**
 * Lightweight, zero-allocation 24-voice polyphonic preview synthesizer.
 * Renders distinct timbres per MIDI channel:
 *  - Ch 1 (Chords): Warm electric piano / Rhodes
 *  - Ch 2 (Melody): Soft glassy bell / pluck
 *  - Ch 3 (Bass):   Clean punchy sub-bass
 */
class PreviewSynth {
public:
    static constexpr int kMaxVoices = 24;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void renderBlock(juce::AudioBuffer<float>& audioBuffer,
                     const juce::MidiBuffer& midiMessages,
                     float masterGain,
                     bool previewChords,
                     bool previewMelody,
                     bool previewBass) noexcept;

    void noteOn(int channel1Based, int pitch, float velocityNorm) noexcept;
    void noteOff(int channel1Based, int pitch) noexcept;
    void allNotesOff() noexcept;

private:
    struct Voice {
        bool active = false;
        bool attacking = false;
        bool releasing = false;
        int channel = 1; // 1=Chords, 2=Melody, 3=Bass
        int pitch = 60;
        float velocity = 0.8f;
        double phase = 0.0;
        double phase_inc = 0.0;
        double env = 0.0;
        double lp_state = 0.0;
        uint64_t age = 0;
    };

    int allocateVoiceIndex() noexcept;

    double sample_rate_ = 44100.0;
    uint64_t voice_counter_ = 0;
    std::array<Voice, kMaxVoices> voices_{};
};

} // namespace style_scaler
