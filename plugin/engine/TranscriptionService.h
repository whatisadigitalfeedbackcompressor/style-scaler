#pragma once

#include "engine/ModelManager.h"
#include "style_scaler/AudioMetadata.h"
#include "style_scaler/MidiStyleAnalyzer.h"
#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <JuceHeader.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if STYLE_SCALER_HAS_MUSCRIPTOR
#include <muscriptor/muscriptor.hpp>
#endif

namespace style_scaler {

class SpectralFallbackTranscriber {
public:
    static constexpr double kSampleRate = 16000.0;
    static std::vector<TimedNoteSec> transcribe(const std::vector<float>& mono16k,
                                                const std::function<bool(float)>& onProgress = {});
};

class TranscriptionService {
public:
    enum class TargetSlot {
        SlotA = 0,
        SlotB = 1
    };

    struct Result {
        bool success = false;
        TargetSlot target_slot = TargetSlot::SlotA;
        std::string backend_name;
        std::string error;
        StyleProfile profile;
        SongMetadata metadata;
        size_t num_notes = 0;
    };

    struct DeconstructedSong {
        bool success = false;
        std::string backend_name;
        std::string error;
        SongInput song_input;
        SongMetadata metadata;
        std::vector<TimedNoteSec> timed_notes;
    };

    struct DeconstructOptions {
        double max_seconds = 45.0; // <= 0 reads full track up to 600 s
        int n_threads = 0;         // 0 = auto half cores
        double bpm_hint = 0.0;     // <= 0 estimates BPM automatically
        bool force_spectral = false;
    };

    explicit TranscriptionService(ModelManager& modelManager);
    ~TranscriptionService();

    TranscriptionService(const TranscriptionService&) = delete;
    TranscriptionService& operator=(const TranscriptionService&) = delete;

    void transcribeFileAsync(const juce::File& file,
                             TargetSlot targetSlot,
                             std::function<void(float, std::string)> onProgress,
                             std::function<void(Result)> onComplete);

    void transcribeAudioAsync(std::vector<float> monoSamples,
                              double sourceSampleRate,
                              std::string sourceLabel,
                              TargetSlot targetSlot,
                              std::function<void(float, std::string)> onProgress,
                              std::function<void(Result)> onComplete);

    DeconstructedSong deconstructFileBlocking(const juce::File& file,
                                              const DeconstructOptions& opts,
                                              const std::function<bool(float, std::string)>& onProgress = {});

    DeconstructedSong deconstructAudio16kBlocking(const std::vector<float>& mono16k,
                                                  const DeconstructOptions& opts,
                                                  const std::function<bool(float, std::string)>& onProgress = {});

    void cancel();
    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    static std::vector<float> resampleTo16kMono(const std::vector<float>& input, double sourceSampleRate);
    static std::vector<float> decodeAudioFileTo16kMono(const juce::File& file,
                                                       std::string& outError,
                                                       double maxSeconds = 45.0);

private:
    class WorkerThread;

    ModelManager& model_manager_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_requested_{false};
    std::unique_ptr<WorkerThread> worker_;

#if STYLE_SCALER_HAS_MUSCRIPTOR
    std::unique_ptr<msl::Transcriber> cached_transcriber_;
    std::filesystem::path cached_model_path_;
#endif
};

} // namespace style_scaler
