#include "ui/TopDetectDnaPanel.h"

#include <cmath>

namespace style_scaler::ui {

namespace {
bool isSupportedDropExtension(const juce::String& filePath) {
    const juce::File f(filePath);
    if (f.getFileName().startsWithIgnoreCase("StyleScaler_")) return false;
    return f.hasFileExtension(".wav;.aif;.aiff;.flac;.ogg;.mp3;.m4a;.aac;.mid;.midi");
}

juce::String formatPresetComboLabel(const StyleProfile& prof) {
    const std::string g = prof.genre.empty() ? std::string("Uncategorized") : prof.genre;
    auto lowEq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    };
    if (lowEq(prof.name, g)) {
        const int tc = std::max(1, prof.track_count);
        return juce::String(g) + " (" + juce::String(tc) + (tc == 1 ? " track)" : " tracks)");
    }
    return juce::String(prof.name) + " (" + juce::String(g) + ")";
}

std::string buildLibrarySignature(const std::vector<StyleProfile>& lib) {
    std::string sig = std::to_string(lib.size());
    for (const auto& p : lib) {
        sig.push_back(';');
        sig.append(p.id);
        sig.push_back('|');
        sig.append(p.genre);
        sig.push_back(':');
        sig.append(std::to_string(p.track_count));
    }
    return sig;
}
} // namespace

TopDetectDnaPanel::TopDetectDnaPanel(StyleScalerAudioProcessor& processor)
    : processor_(processor) {

    // Left: Detect / Capture / Model
    addAndMakeVisible(browse_btn_);
    browse_btn_.onClick = [this]() { handleBrowseAudioOrMidi(); };

    addAndMakeVisible(capture_btn_);
    capture_btn_.onClick = [this]() {
        if (processor_.isCapturingAudio()) {
            processor_.stopAndAnalyzeCapture(target_slot_);
        } else {
            processor_.startLiveCapture();
        }
        refreshFromProcessor();
    };

    auto setupTargetToggle = [&](juce::TextButton& btn, TranscriptionService::TargetSlot slot) {
        addAndMakeVisible(btn);
        btn.setClickingTogglesState(false);
        btn.onClick = [this, slot]() {
            target_slot_ = slot;
            refreshFromProcessor();
        };
    };
    setupTargetToggle(target_a_btn_, TranscriptionService::TargetSlot::SlotA);
    setupTargetToggle(target_b_btn_, TranscriptionService::TargetSlot::SlotB);

    addAndMakeVisible(download_model_btn_);
    download_model_btn_.onClick = [this]() {
        auto& mm = processor_.modelManager();
        if (mm.getDownloadState() == ModelManager::DownloadState::Downloading) {
            mm.cancelDownload();
        } else {
            mm.startDownloadAsync([this](ModelManager::DownloadState, float, std::string) {
                refreshFromProcessor();
            });
        }
        refreshFromProcessor();
    };

    addAndMakeVisible(load_gguf_btn_);
    load_gguf_btn_.onClick = [this]() { handleLoadCustomGguf(); };

    addAndMakeVisible(model_badge_lbl_);
    model_badge_lbl_.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));

    addAndMakeVisible(status_lbl_);
    status_lbl_.setFont(juce::Font(juce::FontOptions(12.5f)));
    status_lbl_.setColour(juce::Label::textColourId, Theme::textMuted);

    // Center: Slot A <-> Slot B + Genre Filter + 4 Axis Blend Sliders
    addAndMakeVisible(genre_filter_combo_);
    genre_filter_combo_.onChange = [this]() {
        const auto chosen = genre_filter_combo_.getText().toStdString();
        active_genre_filter_ = chosen.empty() ? std::string("All Genres") : chosen;
        populatePresetCombos();
    };

    addAndMakeVisible(slot_a_combo_);
    slot_a_combo_.onChange = [this]() {
        const int idx = slot_a_combo_.getSelectedId() - 1;
        if (idx >= 0) processor_.selectSlotAPreset(idx);
    };

    addAndMakeVisible(slot_b_combo_);
    slot_b_combo_.onChange = [this]() {
        const int idx = slot_b_combo_.getSelectedId() - 1;
        if (idx >= 0) processor_.selectSlotBPreset(idx);
    };

    addAndMakeVisible(load_a_json_btn_);
    load_a_json_btn_.onClick = [this]() { handleLoadJsonProfile(false); };

    addAndMakeVisible(load_b_json_btn_);
    load_b_json_btn_.onClick = [this]() { handleLoadJsonProfile(true); };

    addAndMakeVisible(save_blend_btn_);
    save_blend_btn_.onClick = [this]() { handleSaveBlendedProfile(); };

    auto initAxisRow = [&](AxisSliderRow& row, const char* titleText, const char* paramId) {
        row.title.setText(titleText, juce::dontSendNotification);
        row.title.setFont(juce::Font(juce::FontOptions(13.0f)));
        row.title.setColour(juce::Label::textColourId, Theme::textPrimary);
        addAndMakeVisible(row.title);

        row.slider.setSliderStyle(juce::Slider::LinearHorizontal);
        row.slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        row.slider.getProperties().set("isDnaBlend", true);
        row.slider.onValueChange = [this, &row]() {
            const int pct = static_cast<int>(std::llround(row.slider.getValue() * 100.0));
            row.readout.setText(juce::String(100 - pct) + "% A / " + juce::String(pct) + "% B",
                                juce::dontSendNotification);
        };
        addAndMakeVisible(row.slider);

        row.readout.setFont(juce::Font(juce::FontOptions(12.5f)));
        row.readout.setJustificationType(juce::Justification::centredRight);
        row.readout.setColour(juce::Label::textColourId, Theme::textMuted);
        addAndMakeVisible(row.readout);

        row.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor_.apvts(), paramId, row.slider);
        row.slider.onValueChange();
    };

    initAxisRow(harmony_row_, "Harmony", ParamIDs::harmonyBlend);
    initAxisRow(rhythm_row_, "Rhythm / Groove", ParamIDs::rhythmBlend);
    initAxisRow(voicing_row_, "Voicing / Space", ParamIDs::voicingBlend);
    initAxisRow(melody_row_, "Melody Phrasing", ParamIDs::melodyBlend);

    // Right: Key / Mode / Regenerate
    const juce::StringArray roots = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    for (int i = 0; i < roots.size(); ++i) key_root_combo_.addItem(roots[i], i + 1);
    addAndMakeVisible(key_root_combo_);
    key_root_att_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor_.apvts(), ParamIDs::keyRoot, key_root_combo_);

    const juce::StringArray modes = {
        "Minor", "Major", "Dorian", "Phrygian", "Mixolydian", "Harmonic Minor", "Lydian", "Locrian"
    };
    for (int i = 0; i < modes.size(); ++i) key_mode_combo_.addItem(modes[i], i + 1);
    addAndMakeVisible(key_mode_combo_);
    key_mode_att_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor_.apvts(), ParamIDs::keyMode, key_mode_combo_);

    addAndMakeVisible(regen_btn_);
    regen_btn_.setColour(juce::TextButton::buttonColourId, Theme::slotACyan.withAlpha(0.24f));
    regen_btn_.onClick = [this]() {
        processor_.regenerateProgression(true, true);
    };

    addAndMakeVisible(lock_seed_btn_);
    lock_seed_btn_.setClickingTogglesState(true);
    lock_seed_btn_.onClick = [this]() {
        processor_.setSeedLock(lock_seed_btn_.getToggleState());
        refreshFromProcessor();
    };

    addAndMakeVisible(key_info_lbl_);
    key_info_lbl_.setFont(juce::Font(juce::FontOptions(13.0f)));
    key_info_lbl_.setColour(juce::Label::textColourId, Theme::slotACyan);

    addAndMakeVisible(seed_lbl_);
    seed_lbl_.setFont(juce::Font(juce::FontOptions(12.5f)));
    seed_lbl_.setColour(juce::Label::textColourId, Theme::textMuted);

    addAndMakeVisible(sync_github_btn_);
    sync_github_btn_.setColour(juce::TextButton::buttonColourId, Theme::probEmerald.withAlpha(0.22f));
    sync_github_btn_.onClick = [this]() {
        processor_.syncRemotePresetsAsync();
        refreshFromProcessor();
    };

    addAndMakeVisible(catalog_url_btn_);
    catalog_url_btn_.onClick = [this]() { handleEditCatalogUrl(); };

    populateGenreFilter();
    populatePresetCombos();
    refreshFromProcessor();
}

void TopDetectDnaPanel::populateGenreFilter() {
    const auto genres = processor_.getAvailableGenres();
    genre_filter_combo_.clear(juce::dontSendNotification);
    genre_filter_combo_.addItem("All Genres", 1);

    int matchedId = 1;
    for (size_t i = 0; i < genres.size(); ++i) {
        const int itemId = static_cast<int>(i) + 2;
        genre_filter_combo_.addItem(genres[i], itemId);
        if (genres[i] == active_genre_filter_) {
            matchedId = itemId;
        }
    }
    if (matchedId == 1) {
        active_genre_filter_ = "All Genres";
    }
    genre_filter_combo_.setSelectedId(matchedId, juce::dontSendNotification);
}

void TopDetectDnaPanel::populatePresetCombos() {
    const auto lib = processor_.getPresetLibrary();
    const auto snap = processor_.getSnapshot();
    last_library_sig_ = buildLibrarySignature(lib);

    slot_a_combo_.clear(juce::dontSendNotification);
    slot_b_combo_.clear(juce::dontSendNotification);

    const bool showAll = (active_genre_filter_.empty() || active_genre_filter_ == "All Genres");
    std::string lastHeading;

    for (size_t i = 0; i < lib.size(); ++i) {
        const std::string g = lib[i].genre.empty() ? std::string("Uncategorized") : lib[i].genre;
        if (!showAll && g != active_genre_filter_) {
            continue;
        }
        if (showAll && g != lastHeading) {
            slot_a_combo_.addSectionHeading(g);
            slot_b_combo_.addSectionHeading(g);
            lastHeading = g;
        }
        const int itemId = static_cast<int>(i) + 1; // Preserve 1-based index into processor_.getPresetLibrary()
        const juce::String label = formatPresetComboLabel(lib[i]);
        slot_a_combo_.addItem(label, itemId);
        slot_b_combo_.addItem(label, itemId);
    }

    const int selA = processor_.findPresetIndexById(snap.slot_a.id) + 1;
    const int selB = processor_.findPresetIndexById(snap.slot_b.id) + 1;
    if (selA > 0 && slot_a_combo_.indexOfItemId(selA) >= 0) {
        slot_a_combo_.setSelectedId(selA, juce::dontSendNotification);
    } else {
        slot_a_combo_.setText(formatPresetComboLabel(snap.slot_a),
                              juce::dontSendNotification);
    }
    if (selB > 0 && slot_b_combo_.indexOfItemId(selB) >= 0) {
        slot_b_combo_.setSelectedId(selB, juce::dontSendNotification);
    } else {
        slot_b_combo_.setText(formatPresetComboLabel(snap.slot_b),
                              juce::dontSendNotification);
    }
}

void TopDetectDnaPanel::refreshFromProcessor() {
    const auto snap = processor_.getSnapshot();
    const auto lib = processor_.getPresetLibrary();
    const std::string curSig = buildLibrarySignature(lib);
    if (curSig != last_library_sig_) {
        populateGenreFilter();
        populatePresetCombos();
    } else {
        const int selA = processor_.findPresetIndexById(snap.slot_a.id) + 1;
        const int selB = processor_.findPresetIndexById(snap.slot_b.id) + 1;
        if (selA > 0 && slot_a_combo_.indexOfItemId(selA) >= 0) {
            if (slot_a_combo_.getSelectedId() != selA) {
                slot_a_combo_.setSelectedId(selA, juce::dontSendNotification);
            }
        } else {
            slot_a_combo_.setText(formatPresetComboLabel(snap.slot_a),
                                  juce::dontSendNotification);
        }
        if (selB > 0 && slot_b_combo_.indexOfItemId(selB) >= 0) {
            if (slot_b_combo_.getSelectedId() != selB) {
                slot_b_combo_.setSelectedId(selB, juce::dontSendNotification);
            }
        } else {
            slot_b_combo_.setText(formatPresetComboLabel(snap.slot_b),
                                  juce::dontSendNotification);
        }
    }

    target_a_btn_.setToggleState(target_slot_ == TranscriptionService::TargetSlot::SlotA, juce::dontSendNotification);
    target_b_btn_.setToggleState(target_slot_ == TranscriptionService::TargetSlot::SlotB, juce::dontSendNotification);

    capturing_ = processor_.isCapturingAudio();
    capture_peak_ = processor_.capturePeakLevel();
    capture_seconds_ = processor_.capturedSeconds();
    capture_btn_.setButtonText(capturing_ ? ("Stop (" + juce::String(capture_seconds_, 1) + "s)") : "Capture DAW");
    capture_btn_.setColour(juce::TextButton::buttonColourId,
                           capturing_ ? Theme::recCrimson.withAlpha(0.75f) : Theme::bgCard);

    const auto& mm = processor_.modelManager();
    const bool modelOk = mm.isModelAvailable();
    const auto dlState = mm.getDownloadState();
    if (dlState == ModelManager::DownloadState::Downloading) {
        model_badge_lbl_.setText("Downloading GGUF: " + juce::String(static_cast<int>(mm.getDownloadProgress() * 100.0f)) + "%",
                                 juce::dontSendNotification);
        model_badge_lbl_.setColour(juce::Label::textColourId, Theme::slotACyan);
        download_model_btn_.setButtonText("Cancel");
    } else if (dlState == ModelManager::DownloadState::Verifying) {
        model_badge_lbl_.setText("Verifying SHA-256...", juce::dontSendNotification);
        model_badge_lbl_.setColour(juce::Label::textColourId, Theme::slotACyan);
    } else if (modelOk) {
        model_badge_lbl_.setText("MuScriptor GGUF Ready", juce::dontSendNotification);
        model_badge_lbl_.setColour(juce::Label::textColourId, Theme::probEmerald);
        download_model_btn_.setButtonText("Model Ready");
    } else {
        model_badge_lbl_.setText("Spectral Fallback Active", juce::dontSendNotification);
        model_badge_lbl_.setColour(juce::Label::textColourId, Theme::lockAmber);
        download_model_btn_.setButtonText("Get MuScriptor");
    }

    const auto tx = processor_.getTranscriptionStatus();
    tx_progress_ = tx.running ? tx.progress : (dlState == ModelManager::DownloadState::Downloading ? mm.getDownloadProgress() : 0.0);
    if (!tx.message.empty()) {
        status_lbl_.setText(tx.message, juce::dontSendNotification);
    } else if (dlState == ModelManager::DownloadState::Failed) {
        status_lbl_.setText(mm.getLastError(), juce::dontSendNotification);
    } else {
        status_lbl_.setText("Drop .wav/.mp3/.mid or Capture DAW audio", juce::dontSendNotification);
    }

    const juce::String subNote = juce::MidiMessage::getMidiNoteName(snap.blended.voicing.sub_bass_clearance_midi, true, true, 3);
    const auto tonicSv = pitchClassName(snap.blended.harmony.tonic_pc);
    const auto modeSv = modeName(snap.blended.harmony.mode);
    key_info_lbl_.setText(
        juce::String(std::string(tonicSv)) + " " +
            juce::String(std::string(modeSv)) + "  |  " +
            juce::String(static_cast<int>(std::llround(snap.blended.default_bpm))) + " BPM  |  Floor >= " + subNote,
        juce::dontSendNotification);

    lock_seed_btn_.setToggleState(processor_.isSeedLocked(), juce::dontSendNotification);
    seed_lbl_.setText("Seed: " + juce::String(static_cast<juce::int64>(snap.seed & 0xFFFFFFFFULL)),
                      juce::dontSendNotification);

    const bool syncing = processor_.isCatalogSyncRunning();
    sync_github_btn_.setButtonText(syncing ? "Syncing..." : "Sync GitHub");
    sync_github_btn_.setEnabled(!syncing);

    repaint();
}

void TopDetectDnaPanel::paint(juce::Graphics& g) {
    auto drawCard = [&](juce::Rectangle<int> r, const juce::String& title, juce::Colour accent) {
        g.setColour(Theme::bgPanel);
        g.fillRoundedRectangle(r.toFloat(), 8.0f);
        g.setColour(Theme::border);
        g.drawRoundedRectangle(r.toFloat(), 8.0f, 1.0f);

        g.setColour(accent);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(title, r.getX() + 10, r.getY() + 6, r.getWidth() - 20, 18, juce::Justification::centredLeft);
    };

    drawCard(left_card_, "1. DETECT / AUDIO-TO-MIDI", Theme::lockAmber);
    drawCard(center_card_, "2. CROSS-GENRE STYLE DNA", Theme::slotACyan);
    drawCard(right_card_, "3. KEY / SCALE & SEED", Theme::probEmerald);

    // Drop zone visual box
    if (!drop_zone_rect_.isEmpty()) {
        const auto dz = drop_zone_rect_.toFloat();
        g.setColour(drag_hover_ ? Theme::slotACyan.withAlpha(0.18f) : Theme::bgMain.withAlpha(0.75f));
        g.fillRoundedRectangle(dz, 6.0f);
        g.setColour(drag_hover_ ? Theme::slotACyan : Theme::borderBright);
        const float dashes[] = {5.0f, 4.0f};
        juce::Path p;
        p.addRoundedRectangle(dz, 6.0f);
        juce::PathStrokeType stroke(1.2f);
        stroke.createDashedStroke(p, p, dashes, 2);
        g.strokePath(p, stroke);

        g.setColour(drag_hover_ ? Theme::textPrimary : Theme::textMuted);
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText("Drop Audio or MIDI File", drop_zone_rect_.withTrimmedRight(112), juce::Justification::centred);
    }

    // Live capture meter
    if (!meter_rect_.isEmpty()) {
        g.setColour(Theme::bgMain);
        g.fillRoundedRectangle(meter_rect_.toFloat(), 3.0f);
        const float lvl = std::clamp(capture_peak_, 0.0f, 1.0f);
        if (lvl > 0.005f) {
            g.setColour(capturing_ ? Theme::recCrimson : Theme::probEmerald);
            g.fillRoundedRectangle(meter_rect_.toFloat().withWidth(meter_rect_.getWidth() * lvl), 3.0f);
        }
    }

    // Transcription / download progress bar
    if (!progress_rect_.isEmpty() && tx_progress_ > 0.001) {
        g.setColour(Theme::bgMain);
        g.fillRoundedRectangle(progress_rect_.toFloat(), 2.0f);
        g.setColour(Theme::slotACyan);
        g.fillRoundedRectangle(
            progress_rect_.toFloat().withWidth(static_cast<float>(progress_rect_.getWidth() * tx_progress_)), 2.0f);
    }

    // Slot A / Slot B color badges inside Center card
    g.setColour(Theme::slotACyan);
    g.fillEllipse(static_cast<float>(center_card_.getX() + 10), static_cast<float>(center_card_.getY() + 34), 8.0f, 8.0f);
    g.setColour(Theme::slotBViolet);
    const int halfW = center_card_.getWidth() / 2;
    g.fillEllipse(static_cast<float>(center_card_.getX() + halfW + 4), static_cast<float>(center_card_.getY() + 34), 8.0f, 8.0f);
}

void TopDetectDnaPanel::resized() {
    auto area = getLocalBounds();
    const int gap = 8;
    const int w = area.getWidth();
    const int leftW = static_cast<int>(w * 0.30f);
    const int rightW = static_cast<int>(w * 0.23f);

    left_card_ = area.removeFromLeft(leftW);
    area.removeFromLeft(gap);
    right_card_ = area.removeFromRight(rightW);
    area.removeFromRight(gap);
    center_card_ = area;

    // --- Left Card layout ---
    {
        auto b = left_card_.reduced(10, 8);
        b.removeFromTop(20); // header
        drop_zone_rect_ = b.removeFromTop(34);
        browse_btn_.setBounds(drop_zone_rect_.withLeft(drop_zone_rect_.getRight() - 108).reduced(3));

        b.removeFromTop(6);
        auto capRow = b.removeFromTop(24);
        capture_btn_.setBounds(capRow.removeFromLeft(96));
        capRow.removeFromLeft(6);
        target_b_btn_.setBounds(capRow.removeFromRight(62));
        capRow.removeFromRight(4);
        target_a_btn_.setBounds(capRow.removeFromRight(62));
        capRow.removeFromRight(6);
        meter_rect_ = capRow.reduced(0, 6);

        b.removeFromTop(6);
        auto modRow = b.removeFromTop(24);
        load_gguf_btn_.setBounds(modRow.removeFromRight(74));
        modRow.removeFromRight(4);
        download_model_btn_.setBounds(modRow.removeFromRight(92));
        modRow.removeFromRight(6);
        model_badge_lbl_.setBounds(modRow);

        b.removeFromTop(4);
        progress_rect_ = b.removeFromTop(4);
        b.removeFromTop(2);
        status_lbl_.setBounds(b);
    }

    // --- Center Card layout ---
    {
        genre_filter_combo_.setBounds(center_card_.getRight() - 148, center_card_.getY() + 5, 138, 20);
        auto b = center_card_.reduced(10, 8);
        b.removeFromTop(20);
        auto slotRow = b.removeFromTop(26);
        save_blend_btn_.setBounds(slotRow.removeFromRight(72));
        slotRow.removeFromRight(6);

        const int slotSpace = slotRow.getWidth();
        auto leftHalf = slotRow.removeFromLeft(slotSpace / 2 - 4);
        slotRow.removeFromLeft(8);
        auto rightHalf = slotRow;

        leftHalf.removeFromLeft(12); // cyan dot
        load_a_json_btn_.setBounds(leftHalf.removeFromRight(42));
        leftHalf.removeFromRight(4);
        slot_a_combo_.setBounds(leftHalf);

        rightHalf.removeFromLeft(12); // violet dot
        load_b_json_btn_.setBounds(rightHalf.removeFromRight(42));
        rightHalf.removeFromRight(4);
        slot_b_combo_.setBounds(rightHalf);

        b.removeFromTop(8);
        auto layoutAxis = [&](AxisSliderRow& row, juce::Rectangle<int> r) {
            row.title.setBounds(r.removeFromLeft(108));
            row.readout.setBounds(r.removeFromRight(88));
            row.slider.setBounds(r);
        };
        const int rowH = std::max(22, (b.getHeight() - 12) / 4);
        layoutAxis(harmony_row_, b.removeFromTop(rowH));
        b.removeFromTop(3);
        layoutAxis(rhythm_row_, b.removeFromTop(rowH));
        b.removeFromTop(3);
        layoutAxis(voicing_row_, b.removeFromTop(rowH));
        b.removeFromTop(3);
        layoutAxis(melody_row_, b.removeFromTop(rowH));
    }

    // --- Right Card layout ---
    {
        auto b = right_card_.reduced(10, 8);
        b.removeFromTop(20);
        key_info_lbl_.setBounds(b.removeFromTop(20));
        b.removeFromTop(4);

        auto keyRow = b.removeFromTop(24);
        key_root_combo_.setBounds(keyRow.removeFromLeft(64));
        keyRow.removeFromLeft(6);
        key_mode_combo_.setBounds(keyRow);

        b.removeFromTop(6);
        auto regenRow = b.removeFromTop(26);
        regen_btn_.setBounds(regenRow.removeFromLeft(regenRow.getWidth() - 88));
        regenRow.removeFromLeft(6);
        lock_seed_btn_.setBounds(regenRow);

        b.removeFromTop(5);
        seed_lbl_.setBounds(b.removeFromTop(17));

        b.removeFromTop(5);
        auto syncRow = b.removeFromTop(24);
        catalog_url_btn_.setBounds(syncRow.removeFromRight(46));
        syncRow.removeFromRight(5);
        sync_github_btn_.setBounds(syncRow);
    }
}

bool TopDetectDnaPanel::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& f : files) {
        if (isSupportedDropExtension(f)) return true;
    }
    return false;
}

void TopDetectDnaPanel::fileDragEnter(const juce::StringArray&, int, int) {
    drag_hover_ = true;
    repaint();
}

void TopDetectDnaPanel::fileDragExit(const juce::StringArray&) {
    drag_hover_ = false;
    repaint();
}

void TopDetectDnaPanel::filesDropped(const juce::StringArray& files, int, int) {
    drag_hover_ = false;
    for (const auto& f : files) {
        if (isSupportedDropExtension(f)) {
            processor_.analyzeDroppedFile(juce::File(f), target_slot_);
            break;
        }
    }
    repaint();
}

void TopDetectDnaPanel::handleBrowseAudioOrMidi() {
    file_chooser_ = std::make_unique<juce::FileChooser>(
        "Select Audio or MIDI Reference",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.aac;*.mid;*.midi");
    file_chooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (file.existsAsFile()) {
                processor_.analyzeDroppedFile(file, target_slot_);
            }
        });
}

void TopDetectDnaPanel::handleLoadJsonProfile(bool isSlotB) {
    file_chooser_ = std::make_unique<juce::FileChooser>(
        isSlotB ? "Load Slot B StyleProfile JSON" : "Load Slot A StyleProfile JSON",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.json");
    file_chooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, isSlotB](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            auto loaded = loadFromFile(file.getFullPathName().toStdString());
            if (loaded) {
                processor_.setSlotProfile(isSlotB, *loaded, true);
                populateGenreFilter();
                populatePresetCombos();
                refreshFromProcessor();
            } else {
                status_lbl_.setText("JSON error: " + loaded.error(), juce::dontSendNotification);
            }
        });
}

void TopDetectDnaPanel::handleSaveBlendedProfile() {
    const auto userDir = StyleScalerAudioProcessor::userPresetsDirectory();
    userDir.createDirectory();
    file_chooser_ = std::make_unique<juce::FileChooser>(
        "Save Blended StyleProfile JSON",
        userDir.getChildFile("blended_style.json"),
        "*.json");
    file_chooser_->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [this, userDir](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (file == juce::File()) return;
            if (!file.hasFileExtension(".json")) file = file.withFileExtension(".json");
            auto snap = processor_.getSnapshot();
            snap.blended.id = file.getFileNameWithoutExtension().replaceCharacter(' ', '_').toLowerCase().toStdString();
            snap.blended.name = file.getFileNameWithoutExtension().toStdString();
            snap.blended.genre_source = "blended";
            if (file.getParentDirectory() == userDir) {
                (void)processor_.saveProfileToLocalDatabase(snap.blended);
                populateGenreFilter();
                populatePresetCombos();
            } else {
                (void)saveToFile(snap.blended, file.getFullPathName().toStdString());
            }
        });
}

void TopDetectDnaPanel::handleLoadCustomGguf() {
    file_chooser_ = std::make_unique<juce::FileChooser>(
        "Select MuScriptor GGUF Checkpoint",
        ModelManager::defaultModelsDirectory(),
        "*.gguf");
    file_chooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            std::string err;
            if (!processor_.modelManager().setCustomModelFile(file, err)) {
                status_lbl_.setText(err, juce::dontSendNotification);
            }
            refreshFromProcessor();
        });
}

void TopDetectDnaPanel::handleEditCatalogUrl() {
    auto* aw = new juce::AlertWindow(
        "Preset Catalog URL",
        "Enter a GitHub raw catalog.json URL or local catalog.json path:",
        juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor("url", processor_.getCatalogUrl());
    aw->addButton("Save & Sync", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Reset Default", 2);
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<TopDetectDnaPanel> safeThis(this);
    aw->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, aw](int result) {
            if (safeThis == nullptr) return;
            if (result == 1) {
                safeThis->processor_.setCatalogUrl(aw->getTextEditorContents("url").trim().toStdString());
                safeThis->processor_.syncRemotePresetsAsync();
                safeThis->refreshFromProcessor();
            } else if (result == 2) {
                safeThis->processor_.setCatalogUrl(PresetCatalogSync::kDefaultCatalogUrl);
                safeThis->refreshFromProcessor();
            }
        }),
        true);
}

} // namespace style_scaler::ui
