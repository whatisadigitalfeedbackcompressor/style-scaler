#pragma once

#include "PluginProcessor.h"
#include "ui/ChordPadBankComponent.h"
#include "ui/PerformAndKeyboardBar.h"
#include "ui/StyleScalerLookAndFeel.h"
#include "ui/TimelineComponent.h"
#include "ui/TopDetectDnaPanel.h"

#include <JuceHeader.h>

namespace style_scaler {

class StyleScalerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        public juce::DragAndDropContainer,
                                        private juce::Timer {
public:
    explicit StyleScalerAudioProcessorEditor(StyleScalerAudioProcessor& processor);
    ~StyleScalerAudioProcessorEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void refreshAllPanels();

private:
    void timerCallback() override;

    StyleScalerAudioProcessor& processor_;
    ui::StyleScalerLookAndFeel look_and_feel_;

    ui::TopDetectDnaPanel top_panel_;
    ui::ChordPadBankComponent pad_bank_;
    ui::TimelineComponent timeline_;
    ui::PerformAndKeyboardBar perform_bar_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StyleScalerAudioProcessorEditor)
};

} // namespace style_scaler
