#include "engine/ModelManager.h"

#include <array>

namespace style_scaler {

class ModelManager::DownloadThread : public juce::Thread {
public:
    DownloadThread(ModelManager& owner,
                   juce::File targetFile,
                   ModelManifestInfo manifest,
                   std::function<void(DownloadState, float, std::string)> cb)
        : juce::Thread("StyleScalerModelDownload"),
          owner_(owner),
          target_file_(std::move(targetFile)),
          manifest_(manifest),
          callback_(std::move(cb)) {}

    void run() override {
        auto notify = [&](DownloadState s, float prog, const std::string& msg) {
            owner_.state_.store(s, std::memory_order_release);
            owner_.progress_.store(prog, std::memory_order_relaxed);
            if (!msg.empty()) {
                std::lock_guard<std::mutex> lk(owner_.mutex_);
                owner_.last_error_ = msg;
            }
            if (callback_) {
                auto cb = callback_;
                juce::MessageManager::callAsync([cb, s, prog, msg]() { cb(s, prog, msg); });
            }
        };

        std::string err;
        const bool ok = owner_.downloadDefaultModelBlocking(
            [&](DownloadState s, float prog) {
                notify(s, prog, "");
                return !threadShouldExit() && !owner_.cancel_requested_.load(std::memory_order_acquire);
            },
            err);

        if (ok) {
            notify(DownloadState::Ready, 1.0f, "");
        } else if (err == "Cancelled") {
            notify(DownloadState::Idle, 0.0f, "Download cancelled");
        } else {
            notify(DownloadState::Failed, 0.0f, err);
        }
    }

private:
    ModelManager& owner_;
    juce::File target_file_;
    ModelManifestInfo manifest_;
    std::function<void(DownloadState, float, std::string)> callback_;
};

bool ModelManager::downloadDefaultModelBlocking(const std::function<bool(DownloadState, float)>& onProgress,
                                                std::string& outError) {
    const auto manifest = defaultManifest();
    const auto targetFile = defaultModelsDirectory().getChildFile(manifest.file_name);

    if (onProgress && !onProgress(DownloadState::Downloading, 0.0f)) {
        outError = "Cancelled";
        return false;
    }

    targetFile.getParentDirectory().createDirectory();
    const juce::File tempFile = targetFile.getSiblingFile(targetFile.getFileName() + ".part");
    tempFile.deleteFile();

    const juce::URL url(manifest.download_url);
    int statusCode = 0;
    auto stream = url.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(15000)
            .withNumRedirectsToFollow(5)
            .withStatusCode(&statusCode));

    if (!stream || statusCode >= 400) {
        outError = "HTTP download failed (status " + std::to_string(statusCode) + ")";
        return false;
    }

    {
        juce::FileOutputStream out(tempFile);
        if (!out.openedOk()) {
            outError = "Could not write temporary model file";
            return false;
        }

        std::array<char, 65536> buf{};
        std::int64_t totalRead = 0;
        const std::int64_t contentLen = stream->getTotalLength() > 0
                                            ? stream->getTotalLength()
                                            : manifest.expected_bytes;

        while (true) {
            if (onProgress && !onProgress(DownloadState::Downloading,
                                          contentLen > 0
                                              ? std::clamp(static_cast<float>(totalRead) / static_cast<float>(contentLen), 0.0f, 0.99f)
                                              : 0.5f)) {
                tempFile.deleteFile();
                outError = "Cancelled";
                return false;
            }
            const int n = stream->read(buf.data(), static_cast<int>(buf.size()));
            if (n <= 0) break;
            if (!out.write(buf.data(), static_cast<size_t>(n))) {
                tempFile.deleteFile();
                outError = "Disk write error while downloading GGUF";
                return false;
            }
            totalRead += n;
        }
        out.flush();
    }

    if (onProgress && !onProgress(DownloadState::Verifying, 0.99f)) {
        tempFile.deleteFile();
        outError = "Cancelled";
        return false;
    }

    const juce::SHA256 sha(tempFile);
    if (!sha.toHexString().equalsIgnoreCase(manifest.sha256_hex)) {
        tempFile.deleteFile();
        outError = "SHA-256 verification failed for downloaded checkpoint";
        return false;
    }

    if (!tempFile.moveFileTo(targetFile)) {
        tempFile.deleteFile();
        outError = "Failed moving verified model into models folder";
        return false;
    }

    state_.store(DownloadState::Ready, std::memory_order_release);
    progress_.store(1.0f, std::memory_order_relaxed);
    return true;
}

ModelManager::ModelManager() {
    const auto def = defaultModelsDirectory().getChildFile(defaultManifest().file_name);
    if (isValidGgufHeader(def)) {
        state_.store(DownloadState::Ready, std::memory_order_relaxed);
        progress_.store(1.0f, std::memory_order_relaxed);
    }
}

ModelManager::~ModelManager() {
    cancelDownload();
}

ModelManifestInfo ModelManager::defaultManifest() noexcept {
    return ModelManifestInfo{};
}

juce::File ModelManager::defaultModelsDirectory() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("StyleScaler")
        .getChildFile("models");
}

bool ModelManager::isValidGgufHeader(const juce::File& file) {
    if (!file.existsAsFile() || file.getSize() < 1024) return false;
    juce::FileInputStream in(file);
    if (!in.openedOk()) return false;
    char magic[4]{};
    if (in.read(magic, 4) != 4) return false;
    return magic[0] == 'G' && magic[1] == 'G' && magic[2] == 'U' && magic[3] == 'F';
}

juce::File ModelManager::getActiveModelFile() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (custom_model_file_.existsAsFile()) {
        return custom_model_file_;
    }
    return defaultModelsDirectory().getChildFile(defaultManifest().file_name);
}

bool ModelManager::isModelAvailable() const {
    return isValidGgufHeader(getActiveModelFile());
}

bool ModelManager::setCustomModelFile(const juce::File& file, std::string& outError) {
    if (!isValidGgufHeader(file)) {
        outError = "Selected file is not a valid GGUF checkpoint";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        custom_model_file_ = file;
        last_error_.clear();
    }
    state_.store(DownloadState::Ready, std::memory_order_release);
    progress_.store(1.0f, std::memory_order_relaxed);
    return true;
}

void ModelManager::startDownloadAsync(std::function<void(DownloadState, float, std::string)> onUpdate) {
    cancelDownload();
    cancel_requested_.store(false, std::memory_order_release);
    const auto target = defaultModelsDirectory().getChildFile(defaultManifest().file_name);
    download_thread_ = std::make_unique<DownloadThread>(*this, target, defaultManifest(), std::move(onUpdate));
    download_thread_->startThread(juce::Thread::Priority::background);
}

void ModelManager::cancelDownload() {
    cancel_requested_.store(true, std::memory_order_release);
    if (download_thread_) {
        download_thread_->signalThreadShouldExit();
        download_thread_->stopThread(4000);
        download_thread_.reset();
    }
}

std::string ModelManager::getLastError() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return last_error_;
}

} // namespace style_scaler
