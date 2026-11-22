#include "ui/TimelineComponent.h"

#include <algorithm>
#include <cmath>

namespace style_scaler::ui {

TimelineComponent::SlotCard::SlotCard(TimelineComponent& owner, size_t slotIndex)
    : owner_(owner), slot_index_(slotIndex) {
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);

    addAndMakeVisible(lock_btn_);
    lock_btn_.onClick = [this]() {
        owner_.processor_.setSlotLocked(slot_index_, !slot.locked);
    };

    addAndMakeVisible(sub_btn_);
    sub_btn_.onClick = [this]() { showSubstituteMenu(); };

    addAndMakeVisible(voi_btn_);
    voi_btn_.onClick = [this]() { showVoicingMenu(); };
}

void TimelineComponent::SlotCard::paint(juce::Graphics& g) {
    const auto b = getLocalBounds().toFloat().reduced(1.5f);

    juce::Colour bg = Theme::bgCard;
    if (is_playing_now) {
        bg = Theme::slotACyan.withAlpha(0.22f);
    } else if (slot.locked) {
        bg = Theme::bgCard.interpolatedWith(Theme::lockAmber, 0.10f);
    }
    if (drag_over_) {
        bg = Theme::probEmerald.withAlpha(0.25f);
    }

    g.setColour(bg);
    g.fillRoundedRectangle(b, 6.0f);

    juce::Colour borderCol = Theme::border;
    float stroke = 1.0f;
    if (drag_over_) {
        borderCol = Theme::probEmerald;
        stroke = 2.0f;
    } else if (is_playing_now) {
        borderCol = Theme::slotACyan;
        stroke = 2.0f;
    } else if (slot.locked) {
        borderCol = Theme::lockAmber;
        stroke = 1.5f;
    } else if (is_selected) {
        borderCol = Theme::borderBright;
        stroke = 1.4f;
    }
    g.setColour(borderCol);
    g.drawRoundedRectangle(b, 6.0f, stroke);

    // Top row: Bar badge + Lock tag
    g.setColour(Theme::textMuted);
    g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    const double beatInBar = std::fmod(slot.start_beat, 4.0);
    const juce::String barTag = beatInBar > 0.1
                                    ? ("B" + juce::String(slot.bar_index + 1) + ".3")
                                    : ("Bar " + juce::String(slot.bar_index + 1));
    g.drawText(barTag, b.getX() + 6.0f, b.getY() + 4.0f, 44.0f, 15.0f, juce::Justification::centredLeft);

    if (slot.locked) {
        g.setColour(Theme::lockAmber);
        g.drawText("LOCK", b.getRight() - 42.0f, b.getY() + 4.0f, 36.0f, 15.0f, juce::Justification::centredRight);
    }

    // Concrete chord name
    g.setColour(Theme::textPrimary);
    g.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
    g.drawText(concrete_name, b.getX() + 4.0f, b.getY() + 21.0f, b.getWidth() - 8.0f, 22.0f,
               juce::Justification::centred, true);

    // Roman numeral + voicing modifier badges
    juce::String subLine(slot.roman_symbol);
    if (slot.octave_shift != 0) {
        subLine += (slot.octave_shift > 0 ? " +" : " ") + juce::String(slot.octave_shift) + "oct";
    }
    if (slot.inversion_override == 3) subLine += " D2";
    else if (slot.inversion_override > 0) subLine += " inv" + juce::String(slot.inversion_override);
    if (slot.richness_override >= 0.0) {
        subLine += (slot.richness_override < 0.25 ? " Tri" : (slot.richness_override < 0.65 ? " 7th" : " 9th"));
    }

    g.setColour(Theme::slotACyan);
    g.setFont(juce::Font(juce::FontOptions(12.5f)));
    g.drawText(subLine, b.getX() + 4.0f, b.getY() + 44.0f, b.getWidth() - 8.0f, 16.0f,
               juce::Justification::centred, true);
}

void TimelineComponent::SlotCard::resized() {
    auto b = getLocalBounds().reduced(5, 5);
    auto btnRow = b.removeFromBottom(21);
    const int btnW = (btnRow.getWidth() - 6) / 3;
    lock_btn_.setBounds(btnRow.removeFromLeft(btnW));
    btnRow.removeFromLeft(3);
    sub_btn_.setBounds(btnRow.removeFromLeft(btnW));
    btnRow.removeFromLeft(3);
    voi_btn_.setBounds(btnRow);

    lock_btn_.setColour(juce::TextButton::buttonColourId,
                        slot.locked ? Theme::lockAmber.withAlpha(0.28f) : Theme::bgElevated);
}

void TimelineComponent::SlotCard::mouseDown(const juce::MouseEvent& e) {
    down_pos_ = e.getPosition();
    drag_started_ = false;
    auditioning_ = true;
    owner_.selected_slot_index_ = static_cast<int>(slot_index_);
    owner_.processor_.auditionChordPress(
        slot.chord, slot.octave_shift, slot.inversion_override, slot.richness_override);
    if (owner_.onSlotSelected) {
        owner_.onSlotSelected(slot.roman_symbol);
    }
    owner_.updatePlayhead();
}

void TimelineComponent::SlotCard::mouseDrag(const juce::MouseEvent& e) {
    if (!drag_started_ && down_pos_.getDistanceFrom(e.getPosition()) > 4) {
        drag_started_ = true;
        if (auditioning_) {
            auditioning_ = false;
            owner_.processor_.auditionChordRelease();
        }
        const juce::File midFile = owner_.processor_.exportTempSlotMidiForDrag(slot_index_);
        if (midFile.existsAsFile()) {
            juce::DragAndDropContainer::performExternalDragDropOfFiles({midFile.getFullPathName()}, false, this);
        }
    }
}

void TimelineComponent::SlotCard::mouseUp(const juce::MouseEvent&) {
    if (auditioning_) {
        auditioning_ = false;
        owner_.processor_.auditionChordRelease();
    }
}

bool TimelineComponent::SlotCard::isInterestedInDragSource(const SourceDetails& details) {
    return details.description.toString().startsWith("PAD:");
}

void TimelineComponent::SlotCard::itemDropped(const SourceDetails& details) {
    drag_over_ = false;
    const juce::String desc = details.description.toString();
    if (desc.startsWith("PAD:")) {
        const std::string sym = desc.fromFirstOccurrenceOf("PAD:", false, false).toStdString();
        if (auto parsed = parseRomanChord(sym)) {
            owner_.processor_.replaceSlotChord(slot_index_, sym, *parsed);
            if (owner_.onSlotSelected) owner_.onSlotSelected(sym);
        }
    }
    repaint();
}

namespace {
std::string extractPadSymbolFromTempMidi(const juce::String& filePath) {
    const juce::String name = juce::File(filePath).getFileName();
    if (!name.startsWith("StyleScaler_Pad__") || !name.endsWith("__.mid")) return {};
    juce::String sym = name.fromFirstOccurrenceOf("StyleScaler_Pad__", false, false);
    sym = sym.upToLastOccurrenceOf("__.mid", false, false);
    return sym.replace("_SL_", "/").toStdString();
}
} // namespace

bool TimelineComponent::SlotCard::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& f : files) {
        if (!extractPadSymbolFromTempMidi(f).empty()) return true;
    }
    return false;
}

void TimelineComponent::SlotCard::filesDropped(const juce::StringArray& files, int, int) {
    drag_over_ = false;
    for (const auto& f : files) {
        const std::string sym = extractPadSymbolFromTempMidi(f);
        if (!sym.empty()) {
            if (auto parsed = parseRomanChord(sym)) {
                owner_.processor_.replaceSlotChord(slot_index_, sym, *parsed);
                if (owner_.onSlotSelected) owner_.onSlotSelected(sym);
                break;
            }
        }
    }
    repaint();
}

void TimelineComponent::SlotCard::showSubstituteMenu() {
    const auto snap = owner_.processor_.getSnapshot();
    const auto subs = ProgressionGenerator::suggestSubstitutes(snap.blended, slot, 10);

    juce::PopupMenu menu;
    menu.addSectionHeader("Substitute " + juce::String(concrete_name));
    for (size_t i = 0; i < subs.size(); ++i) {
        const auto cName = formatRomanAsConcreteChord(
            subs[i].chord, snap.blended.harmony.tonic_pc, snap.blended.harmony.mode);
        menu.addItem(static_cast<int>(i + 1),
                     juce::String(cName) + "  (" + juce::String(subs[i].symbol) + ")");
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(sub_btn_),
                       [this, subs](int choice) {
                           if (choice > 0 && static_cast<size_t>(choice - 1) < subs.size()) {
                               const auto& chosen = subs[static_cast<size_t>(choice - 1)];
                               owner_.processor_.replaceSlotChord(slot_index_, chosen.symbol, chosen.chord);
                               if (owner_.onSlotSelected) owner_.onSlotSelected(chosen.symbol);
                           }
                       });
}

void TimelineComponent::SlotCard::showVoicingMenu() {
    juce::PopupMenu menu;
    menu.addSectionHeader("Octave Shift");
    menu.addItem(101, "-1 Octave", true, slot.octave_shift == -1);
    menu.addItem(102, "0 (Default)", true, slot.octave_shift == 0);
    menu.addItem(103, "+1 Octave", true, slot.octave_shift == 1);

    menu.addSeparator();
    menu.addSectionHeader("Inversion / Shape");
    menu.addItem(200, "Auto", true, slot.inversion_override == -1);
    menu.addItem(201, "Root Position", true, slot.inversion_override == 0);
    menu.addItem(202, "1st Inversion", true, slot.inversion_override == 1);
    menu.addItem(203, "2nd Inversion", true, slot.inversion_override == 2);
    menu.addItem(204, "Drop-2 Open", true, slot.inversion_override == 3);

    menu.addSeparator();
    menu.addSectionHeader("Extensions");
    menu.addItem(300, "Auto (Style DNA)", true, slot.richness_override < 0.0);
    menu.addItem(301, "Triad", true, slot.richness_override >= 0.0 && slot.richness_override < 0.25);
    menu.addItem(302, "7th", true, slot.richness_override >= 0.25 && slot.richness_override < 0.65);
    menu.addItem(303, "9th / 11th", true, slot.richness_override >= 0.65);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(voi_btn_),
                       [this](int choice) {
                           if (choice == 0) return;
                           int oct = slot.octave_shift;
                           int inv = slot.inversion_override;
                           double rich = slot.richness_override;
                           if (choice == 101) oct = -1;
                           else if (choice == 102) oct = 0;
                           else if (choice == 103) oct = 1;
                           else if (choice == 200) inv = -1;
                           else if (choice >= 201 && choice <= 204) inv = choice - 201;
                           else if (choice == 300) rich = -1.0;
                           else if (choice == 301) rich = 0.1;
                           else if (choice == 302) rich = 0.5;
                           else if (choice == 303) rich = 0.95;
                           owner_.processor_.setSlotVoicing(slot_index_, oct, inv, rich);
                       });
}

TimelineComponent::TimelineComponent(StyleScalerAudioProcessor& processor)
    : processor_(processor) {
    addAndMakeVisible(header_lbl_);
    header_lbl_.setText("PROGRESSION TIMELINE", juce::dontSendNotification);
    header_lbl_.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    header_lbl_.setColour(juce::Label::textColourId, Theme::textPrimary);

    addAndMakeVisible(playhead_lbl_);
    playhead_lbl_.setFont(juce::Font(juce::FontOptions(13.0f)));
    playhead_lbl_.setColour(juce::Label::textColourId, Theme::slotACyan);

    auto setupSecBtn = [&](juce::TextButton& btn, int choiceIdx) {
        addAndMakeVisible(btn);
        btn.setClickingTogglesState(false);
        btn.onClick = [this, choiceIdx]() { setSectionChoiceIndex(choiceIdx); };
    };
    setupSecBtn(sec_intro_btn_, 0);
    setupSecBtn(sec_chorus_btn_, 1);
    setupSecBtn(sec_outro_btn_, 2);

    auto setupBarsBtn = [&](juce::TextButton& btn, int choiceIdx) {
        addAndMakeVisible(btn);
        btn.onClick = [this, choiceIdx]() { setBarChoiceIndex(choiceIdx); };
    };
    setupBarsBtn(bars4_btn_, 0);
    setupBarsBtn(bars8_btn_, 1);
    setupBarsBtn(bars16_btn_, 2);

    addAndMakeVisible(clear_locks_btn_);
    clear_locks_btn_.onClick = [this]() { processor_.clearAllLocks(); };

    addAndMakeVisible(regen_unlocked_btn_);
    regen_unlocked_btn_.setColour(juce::TextButton::buttonColourId, Theme::slotACyan.withAlpha(0.22f));
    regen_unlocked_btn_.onClick = [this]() { processor_.regenerateProgression(true, true); };

    viewport_.setViewedComponent(&slots_container_, false);
    viewport_.setScrollBarsShown(false, true);
    addAndMakeVisible(viewport_);

    refreshFromProcessor();
}

void TimelineComponent::setBarChoiceIndex(int choiceIndex) {
    if (auto* p = processor_.apvts().getParameter(ParamIDs::progressionBars)) {
        p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(choiceIndex)));
    }
}

void TimelineComponent::setSectionChoiceIndex(int choiceIndex) {
    if (auto* p = processor_.apvts().getParameter(ParamIDs::songSection)) {
        p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(choiceIndex)));
    }
}

void TimelineComponent::refreshFromProcessor() {
    const auto snap = processor_.getSnapshot();
    sec_intro_btn_.setToggleState(snap.song_section == SongSection::Intro, juce::dontSendNotification);
    sec_chorus_btn_.setToggleState(snap.song_section == SongSection::Chorus, juce::dontSendNotification);
    sec_outro_btn_.setToggleState(snap.song_section == SongSection::Outro, juce::dontSendNotification);
    sec_intro_btn_.setColour(juce::TextButton::buttonColourId,
                             snap.song_section == SongSection::Intro ? Theme::slotACyan.withAlpha(0.28f) : Theme::bgElevated);
    sec_chorus_btn_.setColour(juce::TextButton::buttonColourId,
                              snap.song_section == SongSection::Chorus ? Theme::slotACyan.withAlpha(0.28f) : Theme::bgElevated);
    sec_outro_btn_.setColour(juce::TextButton::buttonColourId,
                             snap.song_section == SongSection::Outro ? Theme::slotACyan.withAlpha(0.28f) : Theme::bgElevated);
    bars4_btn_.setToggleState(snap.num_bars == 4, juce::dontSendNotification);
    bars8_btn_.setToggleState(snap.num_bars == 8, juce::dontSendNotification);
    bars16_btn_.setToggleState(snap.num_bars == 16, juce::dontSendNotification);

    const size_t n = snap.progression.size();
    if (cards_.size() != n) {
        slots_container_.removeAllChildren();
        cards_.clear();
        for (size_t i = 0; i < n; ++i) {
            auto c = std::make_unique<SlotCard>(*this, i);
            slots_container_.addAndMakeVisible(*c);
            cards_.push_back(std::move(c));
        }
    }

    for (size_t i = 0; i < n; ++i) {
        cards_[i]->slot = snap.progression[i];
        cards_[i]->concrete_name = formatRomanAsConcreteChord(
            snap.progression[i].chord, snap.blended.harmony.tonic_pc, snap.blended.harmony.mode);
        cards_[i]->is_selected = (static_cast<int>(i) == selected_slot_index_);
        cards_[i]->resized();
        cards_[i]->repaint();
    }

    resized();
    updatePlayhead();
}

void TimelineComponent::updatePlayhead() {
    const bool playing = processor_.isTransportPlaying();
    const double beat = processor_.currentBeatInLoop();
    const double loopBeats = processor_.currentLoopBeats();

    if (playing) {
        const int bar = static_cast<int>(beat / 4.0) + 1;
        const int beatInBar = static_cast<int>(std::fmod(beat, 4.0)) + 1;
        playhead_lbl_.setText("PLAYING  Bar " + juce::String(bar) + "." + juce::String(beatInBar) +
                                  "  (" + juce::String(processor_.currentBpm(), 1) + " BPM)",
                              juce::dontSendNotification);
    } else {
        playhead_lbl_.setText("Loop: " + juce::String(loopBeats, 0) + " beats", juce::dontSendNotification);
    }

    for (size_t i = 0; i < cards_.size(); ++i) {
        const auto& s = cards_[i]->slot;
        const bool active = playing && (beat >= s.start_beat - 1e-4) && (beat < s.start_beat + s.duration_beats - 1e-4);
        const bool sel = (static_cast<int>(i) == selected_slot_index_);
        if (cards_[i]->is_playing_now != active || cards_[i]->is_selected != sel) {
            cards_[i]->is_playing_now = active;
            cards_[i]->is_selected = sel;
            cards_[i]->repaint();
        }
    }
}

void TimelineComponent::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat();
    g.setColour(Theme::bgPanel);
    g.fillRoundedRectangle(r, 8.0f);
    g.setColour(Theme::border);
    g.drawRoundedRectangle(r, 8.0f, 1.0f);
}

void TimelineComponent::resized() {
    auto b = getLocalBounds().reduced(10, 6);
    auto top = b.removeFromTop(24);
    header_lbl_.setBounds(top.removeFromLeft(152));
    playhead_lbl_.setBounds(top.removeFromLeft(195));

    regen_unlocked_btn_.setBounds(top.removeFromRight(106));
    top.removeFromRight(5);
    clear_locks_btn_.setBounds(top.removeFromRight(80));
    top.removeFromRight(8);
    bars16_btn_.setBounds(top.removeFromRight(52));
    top.removeFromRight(2);
    bars8_btn_.setBounds(top.removeFromRight(48));
    top.removeFromRight(2);
    bars4_btn_.setBounds(top.removeFromRight(48));
    top.removeFromRight(8);
    sec_outro_btn_.setBounds(top.removeFromRight(46));
    top.removeFromRight(2);
    sec_chorus_btn_.setBounds(top.removeFromRight(52));
    top.removeFromRight(2);
    sec_intro_btn_.setBounds(top.removeFromRight(42));

    b.removeFromTop(5);
    viewport_.setBounds(b);

    const int n = std::max(1, static_cast<int>(cards_.size()));
    const int viewW = std::max(100, viewport_.getWidth());
    const int cardW = std::max(106, viewW / n);
    const int cardH = std::max(72, b.getHeight() - (cardW * n > viewW ? 10 : 0));
    slots_container_.setBounds(0, 0, cardW * n, cardH);

    for (size_t i = 0; i < cards_.size(); ++i) {
        cards_[i]->setBounds(static_cast<int>(i) * cardW, 0, cardW, cardH);
    }
}

} // namespace style_scaler::ui
