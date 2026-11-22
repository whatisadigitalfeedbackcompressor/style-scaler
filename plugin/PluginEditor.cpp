#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace style_scaler {

StyleScalerAudioProcessorEditor::ContentRoot::ContentRoot(StyleScalerAudioProcessor& processor)
    : top_panel(processor),
      pad_bank(processor),
      timeline(processor),
      perform_bar(processor) {
    addAndMakeVisible(top_panel);
    addAndMakeVisible(pad_bank);
    addAndMakeVisible(timeline);
    addAndMakeVisible(perform_bar);
}

void StyleScalerAudioProcessorEditor::ContentRoot::paint(juce::Graphics& g) {
    g.fillAll(ui::Theme::bgMain);
}

void StyleScalerAudioProcessorEditor::ContentRoot::resized() {
    auto b = getLocalBounds().reduced(10);
    const int gap = 8;
    const int availH = b.getHeight() - 3 * gap;

    const int topH = std::max(185, static_cast<int>(availH * 0.27f));
    const int midH = std::max(142, static_cast<int>(availH * 0.21f));
    const int timelineH = std::max(132, static_cast<int>(availH * 0.20f));

    top_panel.setBounds(b.removeFromTop(topH));
    b.removeFromTop(gap);
    pad_bank.setBounds(b.removeFromTop(midH));
    b.removeFromTop(gap);
    timeline.setBounds(b.removeFromTop(timelineH));
    b.removeFromTop(gap);
    perform_bar.setBounds(b);
}

StyleScalerAudioProcessorEditor::StyleScalerAudioProcessorEditor(StyleScalerAudioProcessor& processor)
    : juce::AudioProcessorEditor(&processor),
      processor_(processor),
      content_root_(processor) {
    setLookAndFeel(&look_and_feel_);
    addAndMakeVisible(content_root_);

    content_root_.timeline.onSlotSelected = [this](const std::string& sym) {
        content_root_.pad_bank.setAnchorSymbol(sym);
    };

    juce::Component::SafePointer<StyleScalerAudioProcessorEditor> safeThis(this);
    processor_.onStateChangedForUI = [safeThis]() {
        if (safeThis != nullptr) {
            safeThis->refreshAllPanels();
        }
    };

    const int targetW = processor_.getEditorWidth();
    const int targetH = processor_.getEditorHeight();

    initializing_ = true;
    setResizable(true, true);
    setResizeLimits(kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    initializing_ = false;
    setSize(targetW, targetH);

    refreshAllPanels();
    startTimerHz(30);
}

StyleScalerAudioProcessorEditor::~StyleScalerAudioProcessorEditor() {
    stopTimer();
    processor_.onStateChangedForUI = nullptr;
    setLookAndFeel(nullptr);
}

void StyleScalerAudioProcessorEditor::refreshAllPanels() {
    content_root_.top_panel.refreshFromProcessor();
    content_root_.pad_bank.refreshFromProcessor();
    content_root_.timeline.refreshFromProcessor();
    content_root_.perform_bar.refreshFromProcessor();
}

void StyleScalerAudioProcessorEditor::timerCallback() {
    content_root_.timeline.updatePlayhead();
    content_root_.perform_bar.updateLiveState();
    if (processor_.isCapturingAudio() ||
        processor_.transcriptionService().isRunning() ||
        processor_.isCatalogSyncRunning() ||
        processor_.modelManager().getDownloadState() == ModelManager::DownloadState::Downloading ||
        processor_.modelManager().getDownloadState() == ModelManager::DownloadState::Verifying) {
        content_root_.top_panel.refreshFromProcessor();
    }
}

void StyleScalerAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(ui::Theme::bgMain);
}

void StyleScalerAudioProcessorEditor::resized() {
    const int w = std::max(1, getWidth());
    const int h = std::max(1, getHeight());
    const float scale = std::clamp(
        std::min(static_cast<float>(w) / static_cast<float>(kBaseWidth),
                 static_cast<float>(h) / static_cast<float>(kBaseHeight)),
        0.35f, 2.0f);

    const int logicalW = std::max(kBaseWidth, static_cast<int>(std::ceil(static_cast<float>(w) / scale)));
    const int logicalH = std::max(kBaseHeight, static_cast<int>(std::ceil(static_cast<float>(h) / scale)));

    content_root_.setTransform(juce::AffineTransform::scale(scale));
    content_root_.setBounds(0, 0, logicalW, logicalH);

    if (resizableCorner != nullptr) {
        resizableCorner->setBounds(w - 16, h - 16, 16, 16);
        resizableCorner->toFront(false);
    }

    if (!initializing_) {
        processor_.setEditorSize(w, h);
    }
}

} // namespace style_scaler
