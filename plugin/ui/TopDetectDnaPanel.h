#pragma once

#include "PluginProcessor.h"
#include "ui/StyleScalerLookAndFeel.h"

#include <JuceHeader.h>
#include <memory>

namespace style_scaler::ui {

class TopDetectDnaPanel : public juce::Component,
                          public juce::FileDragAndDropTarget {
public:
    explicit TopDetectDnaPanel(StyleScalerAudioProcessor& processor);
    ~TopDetectDnaPanel() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    void refreshFromProcessor();

private:
    void populateGenreFilter();
    void populatePresetCombos();
    void handleBrowseAudioOrMidi();
    void handleLoadJsonProfile(bool isSlotB);
    void handleSaveBlendedProfile();
    void handleLoadCustomGguf();
    void handleEditCatalogUrl();

    StyleScalerAudioProcessor& processor_;
    std::string last_library_sig_;
    std::string active_genre_filter_{"All Genres"};
    bool drag_hover_ = false;
    TranscriptionService::TargetSlot target_slot_ = TranscriptionService::TargetSlot::SlotA;
    float capture_peak_ = 0.0f;
    double capture_seconds_ = 0.0;
    bool capturing_ = false;
    double tx_progress_ = 0.0;

    // Left: Detect / Drop / Capture / Model
    juce::TextButton browse_btn_{"Browse Audio/MIDI"};
    juce::TextButton capture_btn_{"Capture DAW"};
    juce::TextButton target_a_btn_{"-> Slot A"};
    juce::TextButton target_b_btn_{"-> Slot B"};
    juce::TextButton download_model_btn_{"Download Model"};
    juce::TextButton load_gguf_btn_{"Load GGUF"};
    juce::Label status_lbl_;
    juce::Label model_badge_lbl_;

    // Center: Slot A <-> Slot B + 4 axis blend sliders
    juce::ComboBox genre_filter_combo_;
    juce::ComboBox slot_a_combo_;
    juce::ComboBox slot_b_combo_;
    juce::TextButton load_a_json_btn_{"JSON"};
    juce::TextButton load_b_json_btn_{"JSON"};
    juce::TextButton save_blend_btn_{"Save Blend"};

    struct AxisSliderRow {
        juce::Label title;
        juce::Slider slider;
        juce::Label readout;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    AxisSliderRow harmony_row_;
    AxisSliderRow rhythm_row_;
    AxisSliderRow voicing_row_;
    AxisSliderRow melody_row_;

    // Right: Key / Scale & Seed
    juce::ComboBox key_root_combo_;
    juce::ComboBox key_mode_combo_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> key_root_att_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> key_mode_att_;
    juce::TextButton regen_btn_{"Regenerate"};
    juce::TextButton lock_seed_btn_{"Lock Seed"};
    juce::TextButton sync_github_btn_{"Sync GitHub"};
    juce::TextButton catalog_url_btn_{"URL"};
    juce::Label key_info_lbl_;
    juce::Label seed_lbl_;

    juce::Rectangle<int> drop_zone_rect_;
    juce::Rectangle<int> meter_rect_;
    juce::Rectangle<int> progress_rect_;
    juce::Rectangle<int> left_card_;
    juce::Rectangle<int> center_card_;
    juce::Rectangle<int> right_card_;

    std::unique_ptr<juce::FileChooser> file_chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TopDetectDnaPanel)
};

} // namespace style_scaler::ui
