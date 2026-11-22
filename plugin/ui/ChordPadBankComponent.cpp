#include "ui/ChordPadBankComponent.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace style_scaler::ui {

ChordPadBankComponent::ChordPadButton::ChordPadButton(ChordPadBankComponent& owner, size_t index)
    : owner_(owner), index_(index) {}

void ChordPadBankComponent::ChordPadButton::paint(juce::Graphics& g) {
    const auto b = getLocalBounds().toFloat().reduced(1.5f);
    const float p = static_cast<float>(std::clamp(next_probability, 0.0, 1.0));

    juce::Colour bg = Theme::bgCard;
    if (pressed_) {
        bg = Theme::slotACyan.withAlpha(0.28f);
    } else if (p >= 0.14f) {
        bg = Theme::bgCard.interpolatedWith(Theme::probEmerald, std::min(0.22f, p * 0.45f));
    }
    if (hovered_ && !pressed_) {
        bg = bg.brighter(0.08f);
    }

    g.setColour(bg);
    g.fillRoundedRectangle(b, 6.0f);

    juce::Colour borderCol = Theme::border;
    float stroke = 1.0f;
    if (pressed_) {
        borderCol = Theme::slotACyan;
        stroke = 1.8f;
    } else if (p >= 0.12f) {
        borderCol = Theme::probEmerald.withAlpha(std::clamp(0.45f + p, 0.45f, 0.95f));
        stroke = 1.4f;
    } else if (hovered_) {
        borderCol = Theme::borderBright;
    }
    g.setColour(borderCol);
    g.drawRoundedRectangle(b, 6.0f, stroke);

    // Top-right '+' add zone
    const auto addRect = juce::Rectangle<float>(b.getRight() - 18.0f, b.getY() + 3.0f, 14.0f, 14.0f);
    g.setColour(Theme::bgElevated);
    g.fillRoundedRectangle(addRect, 3.0f);
    g.setColour(Theme::textMuted);
    g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
    g.drawText("+", addRect.toNearestInt(), juce::Justification::centred);

    // Concrete chord name
    g.setColour(Theme::textPrimary);
    g.setFont(juce::Font(juce::FontOptions(14.5f, juce::Font::bold)));
    g.drawText(concrete_name, b.getX() + 6.0f, b.getY() + 6.0f, b.getWidth() - 26.0f, 20.0f,
               juce::Justification::centredLeft, true);

    // Roman numeral + probability %
    g.setColour(in_vocabulary ? Theme::slotACyan : Theme::textMuted);
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText(roman_symbol, b.getX() + 6.0f, b.getY() + 26.0f, b.getWidth() * 0.55f, 16.0f,
               juce::Justification::centredLeft, true);

    if (p > 0.005f) {
        g.setColour(Theme::probEmerald);
        g.drawText(juce::String(static_cast<int>(std::llround(p * 100.0f))) + "%",
                   b.getX() + b.getWidth() * 0.5f, b.getY() + 26.0f, b.getWidth() * 0.5f - 6.0f, 16.0f,
                   juce::Justification::centredRight);
    }

    // Bottom probability meter strip
    const auto barRect = juce::Rectangle<float>(b.getX() + 6.0f, b.getBottom() - 7.0f, b.getWidth() - 12.0f, 3.5f);
    g.setColour(Theme::bgMain);
    g.fillRoundedRectangle(barRect, 1.5f);
    if (p > 0.005f) {
        g.setColour(Theme::probEmerald);
        g.fillRoundedRectangle(barRect.withWidth(barRect.getWidth() * std::min(1.0f, p * 1.6f)), 1.5f);
    }
}

void ChordPadBankComponent::ChordPadButton::mouseDown(const juce::MouseEvent& e) {
    down_pos_ = e.getPosition();
    drag_started_ = false;

    // Check '+' quick-assign hit area
    if (e.x >= getWidth() - 22 && e.y <= 22) {
        owner_.processor_.appendOrReplaceSlotFromPad(roman_symbol, chord);
        owner_.setAnchorSymbol(roman_symbol);
        return;
    }

    pressed_ = true;
    owner_.processor_.auditionChordPress(chord);
    owner_.setAnchorSymbol(roman_symbol);
    repaint();
}

void ChordPadBankComponent::ChordPadButton::mouseDrag(const juce::MouseEvent& e) {
    if (!drag_started_ && down_pos_.getDistanceFrom(e.getPosition()) > 6) {
        if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
            drag_started_ = true;
            dnd->startDragging("PAD:" + juce::String(roman_symbol), this);
        }
    }
}

void ChordPadBankComponent::ChordPadButton::mouseUp(const juce::MouseEvent&) {
    if (pressed_) {
        pressed_ = false;
        owner_.processor_.auditionChordRelease();
        repaint();
    }
}

ChordPadBankComponent::ChordPadBankComponent(StyleScalerAudioProcessor& processor)
    : processor_(processor) {
    addAndMakeVisible(header_lbl_);
    header_lbl_.setText("PLAYABLE CHORD PAD BANK  (Click to audition, '+' or drag to timeline)",
                        juce::dontSendNotification);
    header_lbl_.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
    header_lbl_.setColour(juce::Label::textColourId, Theme::textPrimary);

    addAndMakeVisible(anchor_lbl_);
    anchor_lbl_.setFont(juce::Font(juce::FontOptions(11.5f)));
    anchor_lbl_.setJustificationType(juce::Justification::centredRight);
    anchor_lbl_.setColour(juce::Label::textColourId, Theme::probEmerald);

    for (size_t i = 0; i < 16; ++i) {
        auto pad = std::make_unique<ChordPadButton>(*this, i);
        addAndMakeVisible(*pad);
        pads_.push_back(std::move(pad));
    }

    refreshFromProcessor();
}

void ChordPadBankComponent::setAnchorSymbol(const std::string& romanSymbol) {
    anchor_symbol_ = romanSymbol;
    refreshFromProcessor();
}

void ChordPadBankComponent::refreshFromProcessor() {
    const auto snap = processor_.getSnapshot();
    const auto& prof = snap.blended;

    if (anchor_symbol_.empty()) {
        if (!snap.progression.empty()) {
            anchor_symbol_ = snap.progression.front().roman_symbol;
        } else if (!prof.harmony.vocabulary.empty()) {
            anchor_symbol_ = prof.harmony.vocabulary.front().symbol;
        }
    }

    const auto probsList = ProgressionGenerator::nextChordProbabilities(prof, anchor_symbol_);
    std::map<std::string, double> probMap;
    for (const auto& [sym, p] : probsList) probMap[sym] = p;

    std::vector<ChordVocabEntry> items = prof.harmony.vocabulary;
    std::set<std::string> seen;
    for (const auto& v : items) seen.insert(v.symbol);

    // Fill up to 16 pads with complementary diatonic chords
    const bool useSeventh = prof.voicing.extension_richness >= 0.35;
    for (int deg = 1; deg <= 7 && items.size() < 16; ++deg) {
        const auto dc = diatonicChordForMode(prof.harmony.mode, deg, useSeventh);
        const auto sym = formatRomanChord(dc);
        if (seen.insert(sym).second) {
            items.push_back(ChordVocabEntry{sym, dc, 0.0, 0.0});
        }
    }
    for (int deg = 1; deg <= 7 && items.size() < 16; ++deg) {
        const auto dc = diatonicChordForMode(prof.harmony.mode, deg, !useSeventh);
        const auto sym = formatRomanChord(dc);
        if (seen.insert(sym).second) {
            items.push_back(ChordVocabEntry{sym, dc, 0.0, 0.0});
        }
    }

    for (size_t i = 0; i < pads_.size(); ++i) {
        auto& pad = *pads_[i];
        if (i < items.size()) {
            pad.setVisible(true);
            pad.roman_symbol = items[i].symbol;
            pad.chord = items[i].chord;
            pad.concrete_name = formatRomanAsConcreteChord(items[i].chord, prof.harmony.tonic_pc, prof.harmony.mode);
            pad.in_vocabulary = (items[i].weight > 0.0);
            pad.next_probability = probMap.count(items[i].symbol) ? probMap[items[i].symbol] : 0.0;
        } else {
            pad.setVisible(false);
        }
        pad.repaint();
    }

    if (const auto anchorParsed = parseRomanChord(anchor_symbol_)) {
        const auto concreteAnchor = formatRomanAsConcreteChord(*anchorParsed, prof.harmony.tonic_pc, prof.harmony.mode);
        anchor_lbl_.setText("Likely Next after: " + juce::String(concreteAnchor) + " (" + juce::String(anchor_symbol_) + ")",
                            juce::dontSendNotification);
    }
}

void ChordPadBankComponent::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat();
    g.setColour(Theme::bgPanel);
    g.fillRoundedRectangle(r, 8.0f);
    g.setColour(Theme::border);
    g.drawRoundedRectangle(r, 8.0f, 1.0f);
}

void ChordPadBankComponent::resized() {
    auto b = getLocalBounds().reduced(10, 6);
    auto top = b.removeFromTop(20);
    anchor_lbl_.setBounds(top.removeFromRight(290));
    header_lbl_.setBounds(top);

    b.removeFromTop(4);
    const int cols = 8;
    const int rows = 2;
    const int cellW = b.getWidth() / cols;
    const int cellH = b.getHeight() / rows;
    for (size_t i = 0; i < pads_.size(); ++i) {
        const int r = static_cast<int>(i) / cols;
        const int c = static_cast<int>(i) % cols;
        pads_[i]->setBounds(b.getX() + c * cellW, b.getY() + r * cellH, cellW, cellH);
    }
}

} // namespace style_scaler::ui
