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

        notify(DownloadState::Downloading, 0.0f, "");
        target_file_.getParentDirectory().createDirectory();
        const juce::File tempFile = target_file_.getSiblingFile(target_file_.getFileName() + ".part");
        tempFile.deleteFile();

        const juce::URL url(manifest_.download_url);
        int statusCode = 0;
        auto stream = url.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(15000)
                .withNumRedirectsToFollow(5)
                .withStatusCode(&statusCode));

        if (!stream || (statusCode >= 400)) {
            notify(DownloadState::Failed, 0.0f,
                   "HTTP download failed (status " + std::to_string(statusCode) + ")");
            return;
        }

        {
            juce::FileOutputStream out(tempFile);
            if (!out.openedOk()) {
                notify(DownloadState::Failed, 0.0f, "Could not write temporary model file");
                return;
            }

            std::array<char, 65536> buf{};
            std::int64_t totalRead = 0;
            const std::int64_t contentLen = stream->getTotalLength() > 0
                                                ? stream->getTotalLength()
                                                : manifest_.expected_bytes;

            while (!threadShouldExit() && !owner_.cancel_requested_.load(std::memory_order_acquire)) {
                const int n = stream->read(buf.data(), static_cast<int>(buf.size()));
                if (n <= 0) break;
                if (!out.write(buf.data(), static_cast<size_t>(n))) {
                    notify(DownloadState::Failed, 0.0f, "Disk write error while downloading GGUF");
                    tempFile.deleteFile();
                    return;
                }
                totalRead += n;
                const float frac = contentLen > 0
                                       ? std::clamp(static_cast<float>(totalRead) / static_cast<float>(contentLen), 0.0f, 0.99f)
                                       : 0.5f;
                owner_.progress_.store(frac, std::memory_order_relaxed);
                if (callback_) {
                    auto cb = callback_;
                    juce::MessageManager::callAsync([cb, frac]() {
                        cb(DownloadState::Downloading, frac, "");
                    });
                }
            }
            out.flush();
        }

        if (threadShouldExit() || owner_.cancel_requested_.load(std::memory_order_acquire)) {
            tempFile.deleteFile();
            notify(DownloadState::Idle, 0.0f, "Download cancelled");
            return;
        }

        notify(DownloadState::Verifying, 0.99f, "");
        const juce::SHA256 sha(tempFile);
        const juce::String hex = sha.toHexString();
        if (!hex.equalsIgnoreCase(manifest_.sha256_hex)) {
            tempFile.deleteFile();
            notify(DownloadState::Failed, 0.0f,
                   "SHA-256 verification failed for downloaded checkpoint");
            return;
        }

        if (!tempFile.moveFileTo(target_file_)) {
            tempFile.deleteFile();
            notify(DownloadState::Failed, 0.0f, "Failed moving verified model into models folder");
            return;
        }

        notify(DownloadState::Ready, 1.0f, "");
    }

private:
    ModelManager& owner_;
    juce::File target_file_;
    ModelManifestInfo manifest_;
    std::function<void(DownloadState, float, std::string)> callback_;
};

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
