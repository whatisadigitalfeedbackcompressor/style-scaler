#include "ui/StyleScalerLookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace style_scaler::ui {

StyleScalerLookAndFeel::StyleScalerLookAndFeel() {
    setColour(juce::ResizableWindow::backgroundColourId, Theme::bgMain);
    setColour(juce::Label::textColourId, Theme::textPrimary);
    setColour(juce::TextButton::buttonColourId, Theme::bgCard);
    setColour(juce::TextButton::buttonOnColourId, Theme::slotACyan.withAlpha(0.25f));
    setColour(juce::TextButton::textColourOffId, Theme::textPrimary);
    setColour(juce::TextButton::textColourOnId, Theme::textPrimary);
    setColour(juce::ComboBox::backgroundColourId, Theme::bgCard);
    setColour(juce::ComboBox::textColourId, Theme::textPrimary);
    setColour(juce::ComboBox::outlineColourId, Theme::border);
    setColour(juce::ComboBox::arrowColourId, Theme::textMuted);
    setColour(juce::PopupMenu::backgroundColourId, Theme::bgPanel);
    setColour(juce::PopupMenu::textColourId, Theme::textPrimary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, Theme::bgElevated);
    setColour(juce::PopupMenu::highlightedTextColourId, Theme::slotACyan);
}

juce::Font StyleScalerLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight) {
    return juce::Font(juce::FontOptions(std::clamp(static_cast<float>(buttonHeight) * 0.50f, 11.5f, 15.0f)));
}

juce::Font StyleScalerLookAndFeel::getComboBoxFont(juce::ComboBox&) {
    return juce::Font(juce::FontOptions(13.5f));
}

void StyleScalerLookAndFeel::drawCornerResizer(juce::Graphics& g,
                                               int w,
                                               int h,
                                               bool isMouseOver,
                                               bool isMouseDragging) {
    const auto col = (isMouseDragging || isMouseOver) ? Theme::slotACyan : Theme::borderBright;
    g.setColour(col);
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    for (float offset : {4.0f, 8.0f, 12.0f}) {
        g.drawLine(fw - offset, fh - 1.5f, fw - 1.5f, fh - offset, 1.4f);
    }
}

void StyleScalerLookAndFeel::drawButtonBackground(juce::Graphics& g,
                                                  juce::Button& button,
                                                  const juce::Colour& backgroundColour,
                                                  bool shouldDrawButtonAsHighlighted,
                                                  bool shouldDrawButtonAsDown) {
    const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    auto base = backgroundColour;
    if (button.getToggleState()) {
        base = button.findColour(juce::TextButton::buttonOnColourId);
    }
    if (shouldDrawButtonAsDown) {
        base = base.brighter(0.18f);
    } else if (shouldDrawButtonAsHighlighted) {
        base = base.brighter(0.08f);
    }

    g.setColour(base);
    g.fillRoundedRectangle(bounds, 5.0f);

    const auto borderCol = button.getToggleState()
                               ? Theme::slotACyan.withAlpha(0.85f)
                               : (shouldDrawButtonAsHighlighted ? Theme::borderBright : Theme::border);
    g.setColour(borderCol);
    g.drawRoundedRectangle(bounds, 5.0f, 1.0f);
}

void StyleScalerLookAndFeel::drawComboBox(juce::Graphics& g,
                                          int width,
                                          int height,
                                          bool,
                                          int,
                                          int,
                                          int,
                                          int,
                                          juce::ComboBox& box) {
    const auto bounds = juce::Rectangle<float>(0.5f, 0.5f, static_cast<float>(width) - 1.0f,
                                               static_cast<float>(height) - 1.0f);
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(bounds, 5.0f);
    g.setColour(box.findColour(juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(bounds, 5.0f, 1.0f);

    juce::Path arrow;
    const float cx = static_cast<float>(width) - 13.0f;
    const float cy = static_cast<float>(height) * 0.5f;
    arrow.addTriangle(cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.fillPath(arrow);
}

void StyleScalerLookAndFeel::drawLinearSlider(juce::Graphics& g,
                                              int x,
                                              int y,
                                              int width,
                                              int height,
                                              float sliderPos,
                                              float,
                                              float,
                                              juce::Slider::SliderStyle style,
                                              juce::Slider& slider) {
    if (style != juce::Slider::LinearHorizontal) {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, 0.0f, 0.0f, style, slider);
        return;
    }

    const float cy = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const float trackH = 6.0f;
    const auto trackRect = juce::Rectangle<float>(static_cast<float>(x), cy - trackH * 0.5f,
                                                  static_cast<float>(width), trackH);

    const bool isDna = static_cast<bool>(slider.getProperties().getWithDefault("isDnaBlend", false));
    if (isDna) {
        juce::ColourGradient grad(Theme::slotACyan.withAlpha(0.35f), trackRect.getX(), cy,
                                  Theme::slotBViolet.withAlpha(0.35f), trackRect.getRight(), cy, false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(trackRect, 3.0f);

        const float fillW = std::clamp(sliderPos - static_cast<float>(x), 0.0f, static_cast<float>(width));
        const float ratio = width > 0 ? fillW / static_cast<float>(width) : 0.0f;
        const auto activeCol = Theme::slotACyan.interpolatedWith(Theme::slotBViolet, ratio);
        g.setColour(activeCol);
        g.fillRoundedRectangle(trackRect.withWidth(std::max(4.0f, fillW)), 3.0f);

        g.setColour(Theme::textPrimary);
        g.fillRoundedRectangle(sliderPos - 5.0f, cy - 7.5f, 10.0f, 15.0f, 3.0f);
        g.setColour(activeCol);
        g.drawRoundedRectangle(sliderPos - 5.0f, cy - 7.5f, 10.0f, 15.0f, 3.0f, 1.5f);
    } else {
        g.setColour(Theme::bgMain);
        g.fillRoundedRectangle(trackRect, 3.0f);
        const float fillW = std::clamp(sliderPos - static_cast<float>(x), 0.0f, static_cast<float>(width));
        g.setColour(Theme::slotACyan);
        g.fillRoundedRectangle(trackRect.withWidth(fillW), 3.0f);
        g.setColour(Theme::textPrimary);
        g.fillEllipse(sliderPos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
    }
}

void StyleScalerLookAndFeel::drawRotarySlider(juce::Graphics& g,
                                              int x,
                                              int y,
                                              int width,
                                              int height,
                                              float sliderPosProportional,
                                              float rotaryStartAngle,
                                              float rotaryEndAngle,
                                              juce::Slider&) {
    const float radius = static_cast<float>(std::min(width, height)) * 0.5f - 3.0f;
    const float cx = static_cast<float>(x) + static_cast<float>(width) * 0.5f;
    const float cy = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    g.setColour(Theme::bgMain);
    g.fillEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    juce::Path bgArc;
    bgArc.addCentredArc(cx, cy, radius, radius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(Theme::border);
    g.strokePath(bgArc, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path valArc;
    valArc.addCentredArc(cx, cy, radius, radius, 0.0f, rotaryStartAngle, angle, true);
    g.setColour(Theme::slotACyan);
    g.strokePath(valArc, juce::PathStrokeType(2.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float px = cx + std::sin(angle) * (radius * 0.65f);
    const float py = cy - std::cos(angle) * (radius * 0.65f);
    g.setColour(Theme::textPrimary);
    g.drawLine(cx, cy, px, py, 2.0f);
}

} // namespace style_scaler::ui
