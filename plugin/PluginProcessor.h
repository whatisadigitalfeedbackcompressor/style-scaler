#pragma once

#include "engine/AudioCaptureRingBuffer.h"
#include "engine/LockFreePatternSlot.h"
#include "engine/MidiSequencer.h"
#include "engine/ModelManager.h"
#include "engine/PresetCatalogSync.h"
#include "engine/PreviewSynth.h"
#include "engine/TranscriptionService.h"

#include "style_scaler/GenerationEngine.h"
#include "style_scaler/StyleBlender.h"
#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace style_scaler {

namespace ParamIDs {
inline constexpr const char* harmonyBlend = "harmony_blend";
inline constexpr const char* rhythmBlend = "rhythm_blend";
inline constexpr const char* voicingBlend = "voicing_blend";
inline constexpr const char* melodyBlend = "melody_blend";
inline constexpr const char* tempoScale = "tempo_scale";
inline constexpr const char* grooveSync = "groove_sync";
inline constexpr const char* voicingSpread = "voicing_spread";
inline constexpr const char* melodyDensity = "melody_density";
inline constexpr const char* humanize = "humanize";
inline constexpr const char* previewGain = "preview_gain";
inline constexpr const char* enableChords = "enable_chords";
inline constexpr const char* enableMelody = "enable_melody";
inline constexpr const char* enableBass = "enable_bass";
inline constexpr const char* keyRoot = "key_root";
inline constexpr const char* keyMode = "key_mode";
inline constexpr const char* progressionBars = "progression_bars";
} // namespace ParamIDs

class StyleScalerAudioProcessor : public juce::AudioProcessor,
                                  private juce::AudioProcessorValueTreeState::Listener,
                                  private juce::AsyncUpdater {
public:
    StyleScalerAudioProcessor();
    ~StyleScalerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

#ifdef JucePlugin_Name
    const juce::String getName() const override { return JucePlugin_Name; }
#else
    const juce::String getName() const override { return "StyleScaler"; }
#endif
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& apvts() noexcept { return apvts_; }
    const juce::AudioProcessorValueTreeState& apvts() const noexcept { return apvts_; }

    // --- Thread-safe UI / Engine Snapshot Accessors ---
    struct EngineSnapshot {
        StyleProfile slot_a;
        StyleProfile slot_b;
        StyleProfile blended;
        std::vector<ProgressionSlot> progression;
        GeneratedPattern pattern;
        uint64_t seed = 42;
        int num_bars = 8;
        TempoScale tempo_scale = TempoScale::Normal;
    };

    EngineSnapshot getSnapshot() const;
    std::vector<StyleProfile> getPresetLibrary() const;
    std::vector<std::string> getAvailableGenres() const;
    std::vector<StyleProfile> getPresetsForGenre(const std::string& genre) const;
    int findPresetIndexById(const std::string& id) const;

    void selectSlotAPreset(int libraryIndex);
    void selectSlotBPreset(int libraryIndex);
    void setSlotProfile(bool isSlotB, const StyleProfile& profile, bool addToLibrary = true);

    void setSlotLocked(size_t slotIndex, bool locked);
    void replaceSlotChord(size_t slotIndex, const std::string& romanSymbol, const RomanChord& chord);
    void setSlotVoicing(size_t slotIndex, int octaveShift, int inversionOverride, double richnessOverride);
    void appendOrReplaceSlotFromPad(const std::string& romanSymbol, const RomanChord& chord, int targetIndex = -1);
    void clearAllLocks();
    void regenerateProgression(bool keepLocks, bool newSeed);
    void regeneratePatternOnly();
    void regenerateNowSync(bool keepLocks = true);

    uint64_t getSeed() const;
    void setSeedLock(bool locked);
    bool isSeedLocked() const;

    // --- Transport & Audition ---
    void setInternalPlaying(bool playing);
    bool isTransportPlaying() const noexcept { return sequencer_.isTransportPlaying(); }
    void setDawSync(bool enabled) { sequencer_.setDawSync(enabled); }
    bool isDawSync() const noexcept { return sequencer_.isDawSync(); }
    double currentBeatInLoop() const noexcept { return sequencer_.currentBeatInLoop(); }
    double currentLoopBeats() const noexcept { return sequencer_.currentLoopBeats(); }
    double currentBpm() const noexcept { return sequencer_.currentBpm(); }
    bool isNoteActive(int channel1Based, int pitch) const noexcept { return sequencer_.isNoteActive(channel1Based, pitch); }

    void auditionChordPress(const RomanChord& chord, int octaveShift = 0, int inversion = -1, double richness = -1.0);
    void auditionChordRelease();
    void auditionSingleNote(int channel1Based, int pitch, int velocity, bool noteOn);

    // --- Audio / MIDI Drop & Live Capture ---
    ModelManager& modelManager() noexcept { return model_manager_; }
    const ModelManager& modelManager() const noexcept { return model_manager_; }
    TranscriptionService& transcriptionService() noexcept { return transcription_service_; }

    void analyzeDroppedFile(const juce::File& file, TranscriptionService::TargetSlot targetSlot);
    void startLiveCapture();
    void stopAndAnalyzeCapture(TranscriptionService::TargetSlot targetSlot);
    bool isCapturingAudio() const noexcept { return capture_ring_.isCapturing(); }
    double capturedSeconds() const noexcept { return capture_ring_.capturedSeconds(); }
    float capturePeakLevel() const noexcept { return capture_ring_.peakLevel(); }

    struct TranscriptionStatus {
        bool running = false;
        float progress = 0.0f;
        std::string message;
        std::string backend_name;
    };
    TranscriptionStatus getTranscriptionStatus() const;

    // --- GitHub / Remote Preset Catalog Sync & Local Database ---
    static juce::File userPresetsDirectory();
    bool saveProfileToLocalDatabase(const StyleProfile& profile, std::string* outError = nullptr);
    void reloadPresetLibrary();
    void syncRemotePresetsAsync();
    bool isCatalogSyncRunning() const noexcept { return catalog_sync_.isRunning(); }
    std::string getCatalogUrl() const { return catalog_sync_.getCatalogUrl(); }
    void setCatalogUrl(const std::string& url) { catalog_sync_.setCatalogUrl(url); }
    TranscriptionStatus getCatalogSyncStatus() const;

    // --- MIDI Export (Ch 1/2/3/Full) ---
    bool exportMidiToFile(const juce::File& destFile, int channelFilter, std::string& outError) const;
    juce::File exportTempMidiForDrag(int channelFilter) const;

    // --- Persisted UI Window Size ---
    int getEditorWidth() const noexcept { return editor_width_.load(std::memory_order_relaxed); }
    int getEditorHeight() const noexcept { return editor_height_.load(std::memory_order_relaxed); }
    void setEditorSize(int w, int h) noexcept {
        editor_width_.store(std::clamp(w, 460, 1920), std::memory_order_relaxed);
        editor_height_.store(std::clamp(h, 300, 1260), std::memory_order_relaxed);
    }

    std::function<void()> onStateChangedForUI;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;

    void loadBundledAndUserPresets();
    void scheduleBackgroundRegen(bool rebuildProgression, bool keepLocks);
    void performRebuildSync(bool rebuildProgression, bool keepLocks, uint64_t ticket);
    BlendParams readBlendParamsLocked() const;
    PerformParams readPerformParamsLocked() const;
    int readProgressionBarsLocked() const;
    void syncKeyParamsFromProfile(const StyleProfile& prof);

    juce::AudioProcessorValueTreeState apvts_;

    // Raw atomic parameter pointers for allocation-free audio thread reads
    std::atomic<float>* p_preview_gain_ = nullptr;
    std::atomic<float>* p_enable_chords_ = nullptr;
    std::atomic<float>* p_enable_melody_ = nullptr;
    std::atomic<float>* p_enable_bass_ = nullptr;

    mutable std::mutex state_mutex_;
    std::vector<StyleProfile> preset_library_;
    StyleProfile slot_a_;
    StyleProfile slot_b_;
    StyleProfile blended_;
    std::vector<ProgressionSlot> progression_;
    GeneratedPattern pattern_;
    uint64_t seed_ = 42ULL;
    bool seed_locked_ = false;
    bool suppress_param_regen_ = false;

    std::vector<int> held_audition_notes_;
    std::mutex audition_mutex_;

    std::atomic<uint64_t> regen_ticket_{0};
    std::atomic<bool> pending_rebuild_prog_{false};
    std::atomic<bool> pending_keep_locks_{true};
    juce::ThreadPool regen_pool_{1};

    LockFreePatternSlot pattern_slot_;
    MidiSequencer sequencer_;
    PreviewSynth preview_synth_;
    AudioCaptureRingBuffer capture_ring_{30.0};
    ModelManager model_manager_;
    TranscriptionService transcription_service_{model_manager_};
    PresetCatalogSync catalog_sync_;

    mutable std::mutex status_mutex_;
    TranscriptionStatus tx_status_;
    TranscriptionStatus sync_status_;

    std::atomic<int> editor_width_{580};
    std::atomic<int> editor_height_{380};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StyleScalerAudioProcessor)
};

} // namespace style_scaler
