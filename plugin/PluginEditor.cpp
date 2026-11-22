#include "PluginEditor.h"

namespace style_scaler {

StyleScalerAudioProcessorEditor::StyleScalerAudioProcessorEditor(StyleScalerAudioProcessor& processor)
    : juce::AudioProcessorEditor(&processor),
      processor_(processor),
      top_panel_(processor),
      pad_bank_(processor),
      timeline_(processor),
      perform_bar_(processor) {
    setLookAndFeel(&look_and_feel_);

    addAndMakeVisible(top_panel_);
    addAndMakeVisible(pad_bank_);
    addAndMakeVisible(timeline_);
    addAndMakeVisible(perform_bar_);

    timeline_.onSlotSelected = [this](const std::string& sym) {
        pad_bank_.setAnchorSymbol(sym);
    };

    juce::Component::SafePointer<StyleScalerAudioProcessorEditor> safeThis(this);
    processor_.onStateChangedForUI = [safeThis]() {
        if (safeThis != nullptr) {
            safeThis->refreshAllPanels();
        }
    };

    setResizable(true, true);
    setResizeLimits(960, 640, 1920, 1200);
    setSize(1160, 760);

    refreshAllPanels();
    startTimerHz(30);
}

StyleScalerAudioProcessorEditor::~StyleScalerAudioProcessorEditor() {
    stopTimer();
    processor_.onStateChangedForUI = nullptr;
    setLookAndFeel(nullptr);
}

void StyleScalerAudioProcessorEditor::refreshAllPanels() {
    top_panel_.refreshFromProcessor();
    pad_bank_.refreshFromProcessor();
    timeline_.refreshFromProcessor();
    perform_bar_.refreshFromProcessor();
}

void StyleScalerAudioProcessorEditor::timerCallback() {
    timeline_.updatePlayhead();
    perform_bar_.updateLiveState();
    if (processor_.isCapturingAudio() ||
        processor_.transcriptionService().isRunning() ||
        processor_.modelManager().getDownloadState() == ModelManager::DownloadState::Downloading ||
        processor_.modelManager().getDownloadState() == ModelManager::DownloadState::Verifying) {
        top_panel_.refreshFromProcessor();
    }
}

void StyleScalerAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(ui::Theme::bgMain);
}

void StyleScalerAudioProcessorEditor::resized() {
    auto b = getLocalBounds().reduced(10);
    const int gap = 8;
    const int availH = b.getHeight() - 3 * gap;

    const int topH = std::max(185, static_cast<int>(availH * 0.27f));
    const int midH = std::max(142, static_cast<int>(availH * 0.21f));
    const int timelineH = std::max(132, static_cast<int>(availH * 0.20f));

    top_panel_.setBounds(b.removeFromTop(topH));
    b.removeFromTop(gap);
    pad_bank_.setBounds(b.removeFromTop(midH));
    b.removeFromTop(gap);
    timeline_.setBounds(b.removeFromTop(timelineH));
    b.removeFromTop(gap);
    perform_bar_.setBounds(b);
}

} // namespace style_scaler
