#pragma once

#include "PluginProcessor.h"
#include "ui/StyleScalerLookAndFeel.h"

#include <JuceHeader.h>
#include <array>
#include <memory>
#include <vector>

namespace style_scaler::ui {

class PerformAndKeyboardBar : public juce::Component {
public:
    explicit PerformAndKeyboardBar(StyleScalerAudioProcessor& processor);
    ~PerformAndKeyboardBar() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void refreshFromProcessor();
    void updateLiveState();

private:
    class CustomPianoKeyboard : public juce::Component {
    public:
        static constexpr int kMinMidi = 24; // C1
        static constexpr int kMaxMidi = 96; // C7

        explicit CustomPianoKeyboard(StyleScalerAudioProcessor& processor);

        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;

        void setScaleContext(int tonicPc, Mode mode, int subFloorMidi);

    private:
        struct KeyRect {
            int midi = 60;
            bool is_black = false;
            juce::Rectangle<float> rect;
        };

        int hitTestMidi(juce::Point<int> pt) const noexcept;
        void releaseHeldMouseNote();

        StyleScalerAudioProcessor& processor_;
        int tonic_pc_ = 0;
        Mode mode_ = Mode::Minor;
        int sub_floor_midi_ = 48;
        int held_mouse_midi_ = -1;
        std::vector<KeyRect> keys_;
    };

    class MidiDragChip : public juce::Component {
    public:
        MidiDragChip(StyleScalerAudioProcessor& processor,
                     juce::String label,
                     int channelFilter,
                     juce::Colour accent);

        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseEnter(const juce::MouseEvent&) override { hovered_ = true; repaint(); }
        void mouseExit(const juce::MouseEvent&) override { hovered_ = false; repaint(); }

    private:
        StyleScalerAudioProcessor& processor_;
        juce::String label_;
        int channel_filter_ = 0;
        juce::Colour accent_;
        juce::Point<int> down_pos_;
        bool drag_started_ = false;
        bool hovered_ = false;
    };

    void setTempoScaleIndex(int idx);
    void handleSaveMidi();

    StyleScalerAudioProcessor& processor_;

    // Perform controls
    juce::TextButton play_btn_{"Play"};
    juce::TextButton daw_sync_btn_{"DAW Sync"};
    juce::TextButton groove_sync_btn_{"Groove Sync"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> groove_att_;

    juce::TextButton scale_half_btn_{"0.5x"};
    juce::TextButton scale_norm_btn_{"1x"};
    juce::TextButton scale_dbl_btn_{"2x"};

    struct KnobBox {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    KnobBox spread_knob_;
    KnobBox density_knob_;
    KnobBox humanize_knob_;
    KnobBox gain_knob_;

    juce::TextButton ch1_btn_{"Ch1 Chords"};
    juce::TextButton ch2_btn_{"Ch2 Melody"};
    juce::TextButton ch3_btn_{"Ch3 Bass"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ch1_att_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ch2_att_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ch3_att_;

    CustomPianoKeyboard keyboard_;

    juce::Label export_lbl_;
    MidiDragChip drag_chords_;
    MidiDragChip drag_melody_;
    MidiDragChip drag_bass_;
    MidiDragChip drag_full_;
    juce::TextButton save_midi_btn_{"Save .mid..."};
    std::unique_ptr<juce::FileChooser> file_chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PerformAndKeyboardBar)
};

} // namespace style_scaler::ui
