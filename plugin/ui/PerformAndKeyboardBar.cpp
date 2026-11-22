#include "ui/PerformAndKeyboardBar.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace style_scaler::ui {

namespace {
bool isBlackKeyPc(int pc) noexcept {
    switch (pc % 12) {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}
} // namespace

// ============================================================================
// CustomPianoKeyboard
// ============================================================================

PerformAndKeyboardBar::CustomPianoKeyboard::CustomPianoKeyboard(StyleScalerAudioProcessor& processor)
    : processor_(processor) {}

void PerformAndKeyboardBar::CustomPianoKeyboard::setScaleContext(int tonicPc, Mode mode, int subFloorMidi) {
    if (tonic_pc_ != tonicPc || mode_ != mode || sub_floor_midi_ != subFloorMidi) {
        tonic_pc_ = tonicPc;
        mode_ = mode;
        sub_floor_midi_ = subFloorMidi;
        repaint();
    }
}

void PerformAndKeyboardBar::CustomPianoKeyboard::resized() {
    keys_.clear();
    const int w = getWidth();
    const int h = getHeight();
    if (w <= 0 || h <= 0) return;

    int whiteCount = 0;
    for (int m = kMinMidi; m <= kMaxMidi; ++m) {
        if (!isBlackKeyPc(m % 12)) ++whiteCount;
    }
    const float whiteW = static_cast<float>(w) / static_cast<float>(std::max(1, whiteCount));
    const float whiteH = static_cast<float>(h);
    const float blackW = whiteW * 0.62f;
    const float blackH = whiteH * 0.60f;

    int wIdx = 0;
    std::array<float, 128> whiteX{};
    for (int m = kMinMidi; m <= kMaxMidi; ++m) {
        if (!isBlackKeyPc(m % 12)) {
            const float x = static_cast<float>(wIdx) * whiteW;
            whiteX[static_cast<size_t>(m)] = x;
            keys_.push_back(KeyRect{m, false, juce::Rectangle<float>(x, 0.0f, whiteW, whiteH)});
            ++wIdx;
        }
    }
    for (int m = kMinMidi; m <= kMaxMidi; ++m) {
        if (isBlackKeyPc(m % 12)) {
            const float prevWhiteX = whiteX[static_cast<size_t>(std::max(kMinMidi, m - 1))];
            const float bx = prevWhiteX + whiteW - blackW * 0.5f;
            keys_.push_back(KeyRect{m, true, juce::Rectangle<float>(bx, 0.0f, blackW, blackH)});
        }
    }
}

void PerformAndKeyboardBar::CustomPianoKeyboard::paint(juce::Graphics& g) {
    const auto sc = scaleIntervals(mode_);
    std::array<bool, 12> inScale{};
    for (int iv : sc) {
        inScale[static_cast<size_t>(normalizePitchClass(tonic_pc_ + iv))] = true;
    }

    auto activeColorFor = [&](int midi) -> std::optional<juce::Colour> {
        if (midi == held_mouse_midi_ || processor_.isNoteActive(1, midi)) return Theme::slotACyan;
        if (processor_.isNoteActive(2, midi)) return Theme::lockAmber;
        if (processor_.isNoteActive(3, midi)) return Theme::slotBViolet;
        return std::nullopt;
    };

    float floorLineX = -1.0f;

    // White keys
    for (const auto& k : keys_) {
        if (k.is_black) continue;
        if (k.midi == sub_floor_midi_) floorLineX = k.rect.getX();

        const auto active = activeColorFor(k.midi);
        g.setColour(active.has_value() ? active->interpolatedWith(juce::Colours::white, 0.28f)
                                       : juce::Colour(0xffe2e8f0));
        g.fillRect(k.rect.reduced(0.5f, 0.0f));
        g.setColour(Theme::bgMain);
        g.drawRect(k.rect, 0.8f);

        if (k.midi % 12 == 0) {
            g.setColour(Theme::bgMain.withAlpha(0.7f));
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            g.drawText("C" + juce::String(k.midi / 12 - 1),
                       k.rect.withTop(k.rect.getBottom() - 16.0f).toNearestInt(),
                       juce::Justification::centred);
        } else if (inScale[static_cast<size_t>(k.midi % 12)]) {
            g.setColour(k.midi % 12 == tonic_pc_ ? Theme::slotACyan : Theme::textMuted.withAlpha(0.65f));
            g.fillEllipse(k.rect.getCentreX() - 2.5f, k.rect.getBottom() - 14.0f, 5.0f, 5.0f);
        }
    }

    // Black keys
    for (const auto& k : keys_) {
        if (!k.is_black) continue;
        if (k.midi == sub_floor_midi_) floorLineX = k.rect.getX();

        const auto active = activeColorFor(k.midi);
        g.setColour(active.has_value() ? *active : juce::Colour(0xff1e2430));
        g.fillRoundedRectangle(k.rect, 2.0f);
        g.setColour(Theme::bgMain);
        g.drawRoundedRectangle(k.rect, 2.0f, 1.0f);

        if (inScale[static_cast<size_t>(k.midi % 12)] && !active.has_value()) {
            g.setColour(k.midi % 12 == tonic_pc_ ? Theme::slotACyan : Theme::textMuted.withAlpha(0.55f));
            g.fillEllipse(k.rect.getCentreX() - 2.0f, k.rect.getBottom() - 10.0f, 4.0f, 4.0f);
        }
    }

    // Sub-bass clearance floor line
    if (floorLineX >= 0.0f) {
        g.setColour(Theme::slotBViolet.withAlpha(0.85f));
        g.drawVerticalLine(static_cast<int>(std::llround(floorLineX)), 0.0f, static_cast<float>(getHeight()));
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText("Sub Floor", static_cast<int>(floorLineX) + 3, 2, 68, 14, juce::Justification::centredLeft);
    }
}

int PerformAndKeyboardBar::CustomPianoKeyboard::hitTestMidi(juce::Point<int> pt) const noexcept {
    const auto fpt = pt.toFloat();
    // Check black keys first
    for (const auto& k : keys_) {
        if (k.is_black && k.rect.contains(fpt)) return k.midi;
    }
    for (const auto& k : keys_) {
        if (!k.is_black && k.rect.contains(fpt)) return k.midi;
    }
    return -1;
}

void PerformAndKeyboardBar::CustomPianoKeyboard::releaseHeldMouseNote() {
    if (held_mouse_midi_ >= 0) {
        processor_.auditionSingleNote(1, held_mouse_midi_, 0, false);
        held_mouse_midi_ = -1;
        repaint();
    }
}

void PerformAndKeyboardBar::CustomPianoKeyboard::mouseDown(const juce::MouseEvent& e) {
    const int m = hitTestMidi(e.getPosition());
    if (m >= 0) {
        held_mouse_midi_ = m;
        processor_.auditionSingleNote(1, m, 96, true);
        repaint();
    }
}

void PerformAndKeyboardBar::CustomPianoKeyboard::mouseDrag(const juce::MouseEvent& e) {
    const int m = hitTestMidi(e.getPosition());
    if (m != held_mouse_midi_) {
        releaseHeldMouseNote();
        if (m >= 0) {
            held_mouse_midi_ = m;
            processor_.auditionSingleNote(1, m, 96, true);
            repaint();
        }
    }
}

void PerformAndKeyboardBar::CustomPianoKeyboard::mouseUp(const juce::MouseEvent&) {
    releaseHeldMouseNote();
}

// ============================================================================
// MidiDragChip
// ============================================================================

PerformAndKeyboardBar::MidiDragChip::MidiDragChip(StyleScalerAudioProcessor& processor,
                                                  juce::String label,
                                                  int channelFilter,
                                                  juce::Colour accent)
    : processor_(processor),
      label_(std::move(label)),
      channel_filter_(channelFilter),
      accent_(accent) {
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
}

void PerformAndKeyboardBar::MidiDragChip::paint(juce::Graphics& g) {
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(hovered_ ? accent_.withAlpha(0.26f) : Theme::bgCard);
    g.fillRoundedRectangle(b, 5.0f);
    g.setColour(hovered_ ? accent_ : accent_.withAlpha(0.65f));
    g.drawRoundedRectangle(b, 5.0f, 1.2f);

    g.setColour(Theme::textPrimary);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    g.drawText(label_, getLocalBounds(), juce::Justification::centred);
}

void PerformAndKeyboardBar::MidiDragChip::mouseDown(const juce::MouseEvent& e) {
    down_pos_ = e.getPosition();
    drag_started_ = false;
}

void PerformAndKeyboardBar::MidiDragChip::mouseDrag(const juce::MouseEvent& e) {
    if (!drag_started_ && down_pos_.getDistanceFrom(e.getPosition()) > 4) {
        drag_started_ = true;
        const juce::File midFile = processor_.exportTempMidiForDrag(channel_filter_);
        if (midFile.existsAsFile()) {
            juce::DragAndDropContainer::performExternalDragDropOfFiles({midFile.getFullPathName()}, false, this);
        }
    }
}

// ============================================================================
// PerformAndKeyboardBar
// ============================================================================

PerformAndKeyboardBar::PerformAndKeyboardBar(StyleScalerAudioProcessor& processor)
    : processor_(processor),
      keyboard_(processor),
      drag_chords_(processor, "Chords (Ch1)", 1, Theme::slotACyan),
      drag_melody_(processor, "Melody (Ch2)", 2, Theme::lockAmber),
      drag_bass_(processor, "Bass (Ch3)", 3, Theme::slotBViolet),
      drag_full_(processor, "Full (Ch1-3)", 0, Theme::probEmerald) {

    addAndMakeVisible(play_btn_);
    play_btn_.onClick = [this]() {
        processor_.setInternalPlaying(!processor_.isTransportPlaying());
        updateLiveState();
    };

    addAndMakeVisible(daw_sync_btn_);
    daw_sync_btn_.setClickingTogglesState(true);
    daw_sync_btn_.setToggleState(processor_.isDawSync(), juce::dontSendNotification);
    daw_sync_btn_.onClick = [this]() {
        processor_.setDawSync(daw_sync_btn_.getToggleState());
    };

    addAndMakeVisible(groove_sync_btn_);
    groove_sync_btn_.setClickingTogglesState(true);
    groove_att_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor_.apvts(), ParamIDs::grooveSync, groove_sync_btn_);

    auto setupScale = [&](juce::TextButton& btn, int idx) {
        addAndMakeVisible(btn);
        btn.onClick = [this, idx]() { setTempoScaleIndex(idx); };
    };
    setupScale(scale_half_btn_, 0);
    setupScale(scale_norm_btn_, 1);
    setupScale(scale_dbl_btn_, 2);

    auto initKnob = [&](KnobBox& kb, const char* title, const char* paramId) {
        kb.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        kb.slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible(kb.slider);

        kb.label.setText(title, juce::dontSendNotification);
        kb.label.setFont(juce::Font(juce::FontOptions(11.5f)));
        kb.label.setJustificationType(juce::Justification::centred);
        kb.label.setColour(juce::Label::textColourId, Theme::textMuted);
        addAndMakeVisible(kb.label);

        kb.att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor_.apvts(), paramId, kb.slider);
    };
    initKnob(spread_knob_, "Spread", ParamIDs::voicingSpread);
    initKnob(density_knob_, "Density", ParamIDs::melodyDensity);
    initKnob(humanize_knob_, "Humanize", ParamIDs::humanize);
    initKnob(gain_knob_, "Preview", ParamIDs::previewGain);

    auto initChBtn = [&](juce::TextButton& btn, const char* paramId,
                         std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>& att,
                         juce::Colour col) {
        btn.setClickingTogglesState(true);
        btn.setColour(juce::TextButton::buttonOnColourId, col.withAlpha(0.28f));
        addAndMakeVisible(btn);
        att = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor_.apvts(), paramId, btn);
    };
    initChBtn(ch1_btn_, ParamIDs::enableChords, ch1_att_, Theme::slotACyan);
    initChBtn(ch2_btn_, ParamIDs::enableMelody, ch2_att_, Theme::lockAmber);
    initChBtn(ch3_btn_, ParamIDs::enableBass, ch3_att_, Theme::slotBViolet);

    addAndMakeVisible(keyboard_);

    addAndMakeVisible(export_lbl_);
    export_lbl_.setText("DRAG MIDI TO DAW:", juce::dontSendNotification);
    export_lbl_.setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
    export_lbl_.setColour(juce::Label::textColourId, Theme::textMuted);

    addAndMakeVisible(drag_chords_);
    addAndMakeVisible(drag_melody_);
    addAndMakeVisible(drag_bass_);
    addAndMakeVisible(drag_full_);

    addAndMakeVisible(save_midi_btn_);
    save_midi_btn_.onClick = [this]() { handleSaveMidi(); };

    refreshFromProcessor();
}

void PerformAndKeyboardBar::setTempoScaleIndex(int idx) {
    if (auto* p = processor_.apvts().getParameter(ParamIDs::tempoScale)) {
        p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(idx)));
    }
    refreshFromProcessor();
}

void PerformAndKeyboardBar::refreshFromProcessor() {
    const auto snap = processor_.getSnapshot();
    scale_half_btn_.setToggleState(snap.tempo_scale == TempoScale::Half, juce::dontSendNotification);
    scale_norm_btn_.setToggleState(snap.tempo_scale == TempoScale::Normal, juce::dontSendNotification);
    scale_dbl_btn_.setToggleState(snap.tempo_scale == TempoScale::Double, juce::dontSendNotification);

    keyboard_.setScaleContext(
        snap.blended.harmony.tonic_pc,
        snap.blended.harmony.mode,
        snap.blended.voicing.sub_bass_clearance_midi);
    updateLiveState();
}

void PerformAndKeyboardBar::updateLiveState() {
    const bool playing = processor_.isTransportPlaying();
    play_btn_.setButtonText(playing ? "Stop" : "Play");
    play_btn_.setColour(juce::TextButton::buttonColourId,
                        playing ? Theme::probEmerald.withAlpha(0.45f) : Theme::bgCard);
    keyboard_.repaint();
}

void PerformAndKeyboardBar::handleSaveMidi() {
    file_chooser_ = std::make_unique<juce::FileChooser>(
        "Save Full Multi-Track MIDI (Ch1 Chords / Ch2 Melody / Ch3 Bass)",
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("StyleScaler_Pattern.mid"),
        "*.mid");
    file_chooser_->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (file == juce::File()) return;
            if (!file.hasFileExtension(".mid")) file = file.withFileExtension(".mid");
            std::string err;
            processor_.exportMidiToFile(file, 0, err);
        });
}

void PerformAndKeyboardBar::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat();
    g.setColour(Theme::bgPanel);
    g.fillRoundedRectangle(r, 8.0f);
    g.setColour(Theme::border);
    g.drawRoundedRectangle(r, 8.0f, 1.0f);
}

void PerformAndKeyboardBar::resized() {
    auto b = getLocalBounds().reduced(10, 6);

    // Top Perform strip
    auto perfRow = b.removeFromTop(48);
    play_btn_.setBounds(perfRow.removeFromLeft(66).reduced(0, 8));
    perfRow.removeFromLeft(6);
    daw_sync_btn_.setBounds(perfRow.removeFromLeft(76).reduced(0, 8));
    perfRow.removeFromLeft(4);
    groove_sync_btn_.setBounds(perfRow.removeFromLeft(88).reduced(0, 8));
    perfRow.removeFromLeft(8);

    scale_half_btn_.setBounds(perfRow.removeFromLeft(42).reduced(0, 8));
    perfRow.removeFromLeft(2);
    scale_norm_btn_.setBounds(perfRow.removeFromLeft(36).reduced(0, 8));
    perfRow.removeFromLeft(2);
    scale_dbl_btn_.setBounds(perfRow.removeFromLeft(36).reduced(0, 8));
    perfRow.removeFromLeft(12);

    // Right side channel toggles
    ch3_btn_.setBounds(perfRow.removeFromRight(76).reduced(0, 8));
    perfRow.removeFromRight(4);
    ch2_btn_.setBounds(perfRow.removeFromRight(86).reduced(0, 8));
    perfRow.removeFromRight(4);
    ch1_btn_.setBounds(perfRow.removeFromRight(86).reduced(0, 8));
    perfRow.removeFromRight(10);

    // Center knobs
    auto layoutKnob = [](KnobBox& kb, juce::Rectangle<int> area) {
        kb.label.setBounds(area.removeFromBottom(13));
        kb.slider.setBounds(area);
    };
    const int knobW = std::min(64, std::max(44, perfRow.getWidth() / 4));
    layoutKnob(spread_knob_, perfRow.removeFromLeft(knobW));
    layoutKnob(density_knob_, perfRow.removeFromLeft(knobW));
    layoutKnob(humanize_knob_, perfRow.removeFromLeft(knobW));
    layoutKnob(gain_knob_, perfRow.removeFromLeft(knobW));

    b.removeFromTop(4);

    // Bottom Drag-and-Drop MIDI Export bar
    auto exportRow = b.removeFromBottom(28);
    b.removeFromBottom(6);
    keyboard_.setBounds(b);

    export_lbl_.setBounds(exportRow.removeFromLeft(132));
    save_midi_btn_.setBounds(exportRow.removeFromRight(96));
    exportRow.removeFromRight(8);

    const int chipW = std::min(126, (exportRow.getWidth() - 24) / 4);
    drag_chords_.setBounds(exportRow.removeFromLeft(chipW));
    exportRow.removeFromLeft(6);
    drag_melody_.setBounds(exportRow.removeFromLeft(chipW));
    exportRow.removeFromLeft(6);
    drag_bass_.setBounds(exportRow.removeFromLeft(chipW));
    exportRow.removeFromLeft(6);
    drag_full_.setBounds(exportRow.removeFromLeft(chipW));
}

} // namespace style_scaler::ui
