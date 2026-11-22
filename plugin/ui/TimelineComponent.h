#pragma once

#include "PluginProcessor.h"
#include "ui/StyleScalerLookAndFeel.h"

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace style_scaler::ui {

class TimelineComponent : public juce::Component {
public:
    explicit TimelineComponent(StyleScalerAudioProcessor& processor);
    ~TimelineComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void refreshFromProcessor();
    void updatePlayhead();

    std::function<void(const std::string& romanSymbol)> onSlotSelected;

private:
    class SlotCard : public juce::Component,
                     public juce::DragAndDropTarget {
    public:
        SlotCard(TimelineComponent& owner, size_t slotIndex);

        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;

        bool isInterestedInDragSource(const SourceDetails& details) override;
        void itemDragEnter(const SourceDetails&) override { drag_over_ = true; repaint(); }
        void itemDragExit(const SourceDetails&) override { drag_over_ = false; repaint(); }
        void itemDropped(const SourceDetails& details) override;

        ProgressionSlot slot;
        std::string concrete_name;
        bool is_playing_now = false;
        bool is_selected = false;

    private:
        void showSubstituteMenu();
        void showVoicingMenu();

        TimelineComponent& owner_;
        size_t slot_index_ = 0;
        bool auditioning_ = false;
        bool drag_over_ = false;

        juce::TextButton lock_btn_{"Lock"};
        juce::TextButton sub_btn_{"Sub"};
        juce::TextButton voi_btn_{"Voi"};
    };

    void setBarChoiceIndex(int choiceIndex);

    StyleScalerAudioProcessor& processor_;
    int selected_slot_index_ = 0;

    juce::Label header_lbl_;
    juce::Label playhead_lbl_;
    juce::TextButton bars4_btn_{"4 Bars"};
    juce::TextButton bars8_btn_{"8 Bars"};
    juce::TextButton bars16_btn_{"16 Bars"};
    juce::TextButton clear_locks_btn_{"Clear Locks"};
    juce::TextButton regen_unlocked_btn_{"Regen Unlocked"};

    juce::Viewport viewport_;
    juce::Component slots_container_;
    std::vector<std::unique_ptr<SlotCard>> cards_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineComponent)
};

} // namespace style_scaler::ui
