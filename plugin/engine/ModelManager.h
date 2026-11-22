#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace style_scaler {

struct ModelManifestInfo {
    const char* file_name = "muscriptor-small-f16.gguf";
    const char* download_url =
        "https://huggingface.co/DamRsn/muscriptor-gguf/resolve/"
        "d7045f94e8b19427f4ff9542975035e66596e51c/v1/muscriptor-small-f16.gguf";
    std::int64_t expected_bytes = 209425152;
    const char* sha256_hex = "925f55af65a20ebc4f8b45ceaf095a12b72493d436cb112623cd0041a1af23d4";
};

class ModelManager {
public:
    ModelManager();
    ~ModelManager();

    static ModelManifestInfo defaultManifest() noexcept;
    static juce::File defaultModelsDirectory();

    bool isModelAvailable() const;
    juce::File getActiveModelFile() const;
    bool setCustomModelFile(const juce::File& file, std::string& outError);

    enum class DownloadState {
        Idle,
        Downloading,
        Verifying,
        Ready,
        Failed
    };

    void startDownloadAsync(std::function<void(DownloadState, float, std::string)> onUpdate = {});
    void cancelDownload();

    DownloadState getDownloadState() const noexcept { return state_.load(std::memory_order_acquire); }
    float getDownloadProgress() const noexcept { return progress_.load(std::memory_order_relaxed); }
    std::string getLastError() const;

    static bool isValidGgufHeader(const juce::File& file);

private:
    class DownloadThread;

    mutable std::mutex mutex_;
    juce::File custom_model_file_;
    std::string last_error_;
    std::atomic<DownloadState> state_{DownloadState::Idle};
    std::atomic<float> progress_{0.0f};
    std::atomic<bool> cancel_requested_{false};
    std::unique_ptr<DownloadThread> download_thread_;
};

} // namespace style_scaler
