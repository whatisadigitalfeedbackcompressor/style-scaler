#include "engine/PresetCatalogSync.h"
#include "style_scaler/StyleProfile.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>

namespace style_scaler {

namespace {

bool isHttpUrl(const std::string& s) {
    return s.rfind("https://", 0) == 0 || s.rfind("http://", 0) == 0;
}

bool isFileUri(const std::string& s) {
    return s.rfind("file://", 0) == 0;
}

std::string sanitizeFilenameStem(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "preset" : out;
}

bool readResourceText(const std::string& urlOrPath, std::string& outText, std::string& outError) {
    if (isHttpUrl(urlOrPath)) {
        const juce::URL url(urlOrPath);
        int statusCode = 0;
        auto stream = url.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(15000)
                .withNumRedirectsToFollow(5)
                .withStatusCode(&statusCode));
        if (!stream || statusCode >= 400) {
            outError = "HTTP error (" + std::to_string(statusCode) + ") fetching " + urlOrPath;
            return false;
        }
        outText = stream->readEntireStreamAsString().toStdString();
        if (outText.empty()) {
            outError = "Empty response from " + urlOrPath;
            return false;
        }
        return true;
    }

    juce::File f = isFileUri(urlOrPath) ? juce::URL(urlOrPath).getLocalFile() : juce::File(urlOrPath);
    if (!f.existsAsFile()) {
        outError = "File not found: " + f.getFullPathName().toStdString();
        return false;
    }
    outText = f.loadFileAsString().toStdString();
    return true;
}

std::string resolveChildLocation(const std::string& catalogUrlOrPath,
                                 const std::string& entryFile,
                                 const std::string& explicitUrl) {
    if (!explicitUrl.empty()) return explicitUrl;
    if (isHttpUrl(entryFile) || isFileUri(entryFile)) return entryFile;

    std::string rel = entryFile;
    while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) rel.erase(rel.begin());
    if (rel.rfind("./", 0) == 0) rel = rel.substr(2);

    if (isHttpUrl(catalogUrlOrPath)) {
        const auto pos = catalogUrlOrPath.find_last_of('/');
        if (pos != std::string::npos) {
            return catalogUrlOrPath.substr(0, pos + 1) + rel;
        }
        return catalogUrlOrPath + "/" + rel;
    }

    const juce::File catFile = isFileUri(catalogUrlOrPath)
                                   ? juce::URL(catalogUrlOrPath).getLocalFile()
                                   : juce::File(catalogUrlOrPath);
    return catFile.getParentDirectory().getChildFile(rel).getFullPathName().toStdString();
}

} // namespace

class PresetCatalogSync::SyncThread : public juce::Thread {
public:
    SyncThread(PresetCatalogSync& owner,
               std::string catalogUrl,
               juce::File cacheDir,
               std::function<void(float, std::string)> onProgress,
               std::function<void(SyncResult)> onComplete)
        : juce::Thread("StyleScalerCatalogSync"),
          owner_(owner),
          catalog_url_(std::move(catalogUrl)),
          cache_dir_(std::move(cacheDir)),
          on_progress_(std::move(onProgress)),
          on_complete_(std::move(onComplete)) {}

    void run() override {
        auto report = [&](float prog, const std::string& msg) -> bool {
            if (owner_.cancel_requested_.load(std::memory_order_acquire) || threadShouldExit()) {
                return false;
            }
            if (on_progress_) {
                auto cb = on_progress_;
                juce::MessageManager::callAsync([cb, prog, msg]() { cb(prog, msg); });
            }
            return true;
        };

        auto res = syncBlocking(
            catalog_url_,
            cache_dir_,
            report,
            [&]() { return owner_.cancel_requested_.load(std::memory_order_acquire) || threadShouldExit(); });

        owner_.running_.store(false, std::memory_order_release);
        if (on_complete_) {
            auto cb = on_complete_;
            juce::MessageManager::callAsync([cb, res = std::move(res)]() mutable { cb(std::move(res)); });
        }
    }

private:
    PresetCatalogSync& owner_;
    std::string catalog_url_;
    juce::File cache_dir_;
    std::function<void(float, std::string)> on_progress_;
    std::function<void(SyncResult)> on_complete_;
};

PresetCatalogSync::PresetCatalogSync()
    : PresetCatalogSync(defaultCacheDirectory(), defaultConfigFile()) {}

PresetCatalogSync::PresetCatalogSync(juce::File cacheDir, juce::File configFile)
    : cache_dir_(std::move(cacheDir)), config_file_(std::move(configFile)) {
    loadConfig();
}

PresetCatalogSync::~PresetCatalogSync() {
    cancel();
}

juce::File PresetCatalogSync::defaultCacheDirectory() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("StyleScaler")
        .getChildFile("presets");
}

juce::File PresetCatalogSync::defaultConfigFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("StyleScaler")
        .getChildFile("catalog_config.json");
}

void PresetCatalogSync::loadConfig() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!config_file_.existsAsFile()) return;
    try {
        const auto j = nlohmann::json::parse(config_file_.loadFileAsString().toStdString());
        if (j.contains("catalog_url") && j["catalog_url"].is_string()) {
            const auto u = j["catalog_url"].get<std::string>();
            if (!u.empty()) catalog_url_ = u;
        }
    } catch (...) {}
}

void PresetCatalogSync::saveConfigLocked() const {
    config_file_.getParentDirectory().createDirectory();
    nlohmann::json j = {{"catalog_url", catalog_url_}};
    config_file_.replaceWithText(j.dump(2) + "\n");
}

std::string PresetCatalogSync::getCatalogUrl() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return catalog_url_;
}

void PresetCatalogSync::setCatalogUrl(const std::string& url) {
    std::lock_guard<std::mutex> lk(mutex_);
    catalog_url_ = url.empty() ? std::string(kDefaultCatalogUrl) : url;
    saveConfigLocked();
}

void PresetCatalogSync::cancel() {
    cancel_requested_.store(true, std::memory_order_release);
    if (thread_) {
        thread_->signalThreadShouldExit();
        thread_->stopThread(4000);
        thread_.reset();
    }
    running_.store(false, std::memory_order_release);
}

void PresetCatalogSync::syncAsync(std::function<void(float, std::string)> onProgress,
                                  std::function<void(SyncResult)> onComplete) {
    cancel();
    cancel_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::make_unique<SyncThread>(
        *this, getCatalogUrl(), cache_dir_, std::move(onProgress), std::move(onComplete));
    thread_->startThread(juce::Thread::Priority::background);
}

PresetCatalogSync::SyncResult PresetCatalogSync::syncBlocking(
    const std::string& catalogUrlOrPath,
    const juce::File& cacheDir,
    const std::function<bool(float, const std::string&)>& onProgress,
    const std::function<bool()>& shouldCancel) {

    SyncResult res;
    if (onProgress && !onProgress(0.05f, "Fetching preset catalog...")) {
        res.message = "Cancelled";
        return res;
    }

    std::string catalogText;
    std::string err;
    if (!readResourceText(catalogUrlOrPath, catalogText, err)) {
        res.message = err;
        return res;
    }

    nlohmann::json catJson;
    try {
        catJson = nlohmann::json::parse(catalogText);
    } catch (const std::exception& e) {
        res.message = std::string("Invalid catalog JSON: ") + e.what();
        return res;
    }

    if (!catJson.is_object() || catJson.value("schema_version", 0) != 1 ||
        !catJson.contains("presets") || !catJson["presets"].is_array()) {
        res.message = "Unsupported catalog format (expected schema_version: 1 and presets array)";
        return res;
    }

    cacheDir.createDirectory();
    const auto& presetsArr = catJson["presets"];
    res.total_in_catalog = static_cast<int>(presetsArr.size());

    for (size_t i = 0; i < presetsArr.size(); ++i) {
        if (shouldCancel && shouldCancel()) {
            res.message = "Cancelled";
            return res;
        }

        const auto& item = presetsArr[i];
        const std::string id = item.value("id", "");
        const std::string relFile = item.value("file", "");
        const std::string explicitUrl = item.value("url", "");
        const std::string expectedSha = item.value("sha256", "");
        const std::string safeStem = sanitizeFilenameStem(id.empty() ? relFile : id);

        const float prog = 0.10f + 0.85f * (static_cast<float>(i) / static_cast<float>(std::max<size_t>(1, presetsArr.size())));
        if (onProgress && !onProgress(prog, "Checking preset: " + safeStem)) {
            res.message = "Cancelled";
            return res;
        }

        const juce::File targetFile = cacheDir.getChildFile(juce::String(safeStem) + ".json");
        if (targetFile.existsAsFile() && !expectedSha.empty()) {
            const juce::SHA256 existingSha(targetFile);
            if (existingSha.toHexString().equalsIgnoreCase(expectedSha.c_str()) &&
                loadFromFile(targetFile.getFullPathName().toStdString()).has_value()) {
                ++res.skipped;
                continue;
            }
        }

        const std::string childLoc = resolveChildLocation(catalogUrlOrPath, relFile, explicitUrl);
        std::string presetText;
        std::string dlErr;
        if (!readResourceText(childLoc, presetText, dlErr)) {
            ++res.failed;
            continue;
        }

        if (!expectedSha.empty()) {
            const juce::SHA256 gotSha(presetText.data(), presetText.size());
            if (!gotSha.toHexString().equalsIgnoreCase(expectedSha.c_str())) {
                ++res.failed;
                continue;
            }
        }

        if (!loadFromJsonString(presetText).has_value()) {
            ++res.failed;
            continue;
        }

        const juce::File partFile = targetFile.getSiblingFile(targetFile.getFileName() + ".part");
        partFile.deleteFile();
        if (!partFile.replaceWithData(presetText.data(), presetText.size()) || !partFile.moveFileTo(targetFile)) {
            partFile.deleteFile();
            ++res.failed;
            continue;
        }
        ++res.downloaded;
    }

    res.success = (res.failed == 0 || res.downloaded > 0 || res.skipped > 0);
    if (res.downloaded == 0 && res.failed == 0) {
        res.message = "Catalog up to date (" + std::to_string(res.skipped) + " cached)";
    } else if (res.failed == 0) {
        res.message = "Synced: +" + std::to_string(res.downloaded) + " new, " +
                      std::to_string(res.skipped) + " cached";
    } else {
        res.message = "Synced +" + std::to_string(res.downloaded) + ", " +
                      std::to_string(res.skipped) + " cached, " +
                      std::to_string(res.failed) + " failed";
    }

    if (onProgress) onProgress(1.0f, res.message);
    return res;
}

} // namespace style_scaler
