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
    static constexpr int kBaseWidth = 1160;
    static constexpr int kBaseHeight = 760;
    static constexpr int kDefaultWidth = 580;
    static constexpr int kDefaultHeight = 380;
    static constexpr int kMinWidth = 460;
    static constexpr int kMinHeight = 300;
    static constexpr int kMaxWidth = 1920;
    static constexpr int kMaxHeight = 1260;

    explicit StyleScalerAudioProcessorEditor(StyleScalerAudioProcessor& processor);
    ~StyleScalerAudioProcessorEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void refreshAllPanels();

protected:
    bool shouldDropFilesWhenDraggedExternally(const juce::DragAndDropTarget::SourceDetails& sourceDetails,
                                              juce::StringArray& files,
                                              bool& canMoveFiles) override;

private:
    class ContentRoot : public juce::Component {
    public:
        explicit ContentRoot(StyleScalerAudioProcessor& processor);

        void paint(juce::Graphics& g) override;
        void resized() override;

        ui::TopDetectDnaPanel top_panel;
        ui::ChordPadBankComponent pad_bank;
        ui::TimelineComponent timeline;
        ui::PerformAndKeyboardBar perform_bar;
    };

    void timerCallback() override;

    StyleScalerAudioProcessor& processor_;
    ui::StyleScalerLookAndFeel look_and_feel_;
    ContentRoot content_root_;
    bool initializing_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StyleScalerAudioProcessorEditor)
};

} // namespace style_scaler
