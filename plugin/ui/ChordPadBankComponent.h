#pragma once

#include "PluginProcessor.h"
#include "ui/StyleScalerLookAndFeel.h"

#include <JuceHeader.h>
#include <string>
#include <vector>

namespace style_scaler::ui {

class ChordPadBankComponent : public juce::Component {
public:
    explicit ChordPadBankComponent(StyleScalerAudioProcessor& processor);
    ~ChordPadBankComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setAnchorSymbol(const std::string& romanSymbol);
    void refreshFromProcessor();

private:
    class ChordPadButton : public juce::Component {
    public:
        ChordPadButton(ChordPadBankComponent& owner, size_t index);

        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseEnter(const juce::MouseEvent&) override { hovered_ = true; repaint(); }
        void mouseExit(const juce::MouseEvent&) override { hovered_ = false; repaint(); }

        std::string roman_symbol;
        RomanChord chord;
        std::string concrete_name;
        double next_probability = 0.0;
        bool in_vocabulary = true;

    private:
        ChordPadBankComponent& owner_;
        size_t index_ = 0;
        bool pressed_ = false;
        bool hovered_ = false;
        bool drag_started_ = false;
        juce::Point<int> down_pos_;
    };

    StyleScalerAudioProcessor& processor_;
    std::string anchor_symbol_;
    juce::Label header_lbl_;
    juce::Label anchor_lbl_;
    std::vector<std::unique_ptr<ChordPadButton>> pads_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChordPadBankComponent)
};

} // namespace style_scaler::ui
