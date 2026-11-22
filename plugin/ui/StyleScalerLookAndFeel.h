#pragma once

#include <JuceHeader.h>

namespace style_scaler::ui {

struct Theme {
    static inline const juce::Colour bgMain       {0xff11141a};
    static inline const juce::Colour bgPanel      {0xff1a1f29};
    static inline const juce::Colour bgCard       {0xff222936};
    static inline const juce::Colour bgElevated   {0xff2a3343};
    static inline const juce::Colour border       {0xff2e3748};
    static inline const juce::Colour borderBright {0xff475569};
    static inline const juce::Colour textPrimary  {0xfff1f5f9};
    static inline const juce::Colour textMuted    {0xff94a3b8};
    static inline const juce::Colour slotACyan    {0xff38bdf8};
    static inline const juce::Colour slotBViolet  {0xffa855f7};
    static inline const juce::Colour lockAmber    {0xfff59e0b};
    static inline const juce::Colour probEmerald  {0xff10b981};
    static inline const juce::Colour recCrimson   {0xffef4444};
};

class StyleScalerLookAndFeel : public juce::LookAndFeel_V4 {
public:
    StyleScalerLookAndFeel();

    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawComboBox(juce::Graphics& g,
                      int width,
                      int height,
                      bool isButtonDown,
                      int buttonX,
                      int buttonY,
                      int buttonW,
                      int buttonH,
                      juce::ComboBox& box) override;

    void drawLinearSlider(juce::Graphics& g,
                          int x,
                          int y,
                          int width,
                          int height,
                          float sliderPos,
                          float minSliderPos,
                          float maxSliderPos,
                          juce::Slider::SliderStyle style,
                          juce::Slider& slider) override;

    void drawRotarySlider(juce::Graphics& g,
                          int x,
                          int y,
                          int width,
                          int height,
                          float sliderPosProportional,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider& slider) override;

    void drawCornerResizer(juce::Graphics& g,
                           int w,
                           int h,
                           bool isMouseOver,
                           bool isMouseDragging) override;

    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
};

} // namespace style_scaler::ui
