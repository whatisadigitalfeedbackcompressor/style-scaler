#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace style_scaler {

class PresetCatalogSync {
public:
    static constexpr const char* kDefaultCatalogUrl =
        "https://raw.githubusercontent.com/whatisadigitalfeedbackcompressor/style-scaler/main/presets/catalog.json";

    struct SyncResult {
        bool success = false;
        int downloaded = 0;
        int skipped = 0;
        int failed = 0;
        int total_in_catalog = 0;
        std::string message;
    };

    PresetCatalogSync();
    PresetCatalogSync(juce::File cacheDir, juce::File configFile);
    ~PresetCatalogSync();

    PresetCatalogSync(const PresetCatalogSync&) = delete;
    PresetCatalogSync& operator=(const PresetCatalogSync&) = delete;

    static juce::File defaultCacheDirectory();
    static juce::File defaultConfigFile();

    std::string getCatalogUrl() const;
    void setCatalogUrl(const std::string& url);

    void syncAsync(std::function<void(float, std::string)> onProgress,
                   std::function<void(SyncResult)> onComplete);
    void cancel();
    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    static SyncResult syncBlocking(const std::string& catalogUrlOrPath,
                                   const juce::File& cacheDir,
                                   const std::function<bool(float, const std::string&)>& onProgress = {},
                                   const std::function<bool()>& shouldCancel = {});

private:
    class SyncThread;

    void loadConfig();
    void saveConfigLocked() const;

    juce::File cache_dir_;
    juce::File config_file_;
    mutable std::mutex mutex_;
    std::string catalog_url_{kDefaultCatalogUrl};

    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_requested_{false};
    std::unique_ptr<SyncThread> thread_;
};

} // namespace style_scaler
