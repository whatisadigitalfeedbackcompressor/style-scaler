#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace style_scaler {

/**
 * Lock-free audio capture buffer for recording live DAW audio on the audio thread
 * without allocations or locks, then reading mono float samples on a background thread.
 */
class AudioCaptureRingBuffer {
public:
    explicit AudioCaptureRingBuffer(double maxSeconds = 30.0)
        : max_seconds_(maxSeconds) {}

    // Called from prepareToPlay (non-realtime)
    void prepare(double sampleRate) {
        sample_rate_ = std::max(8000.0, sampleRate);
        const size_t cap = static_cast<size_t>(std::llround(sample_rate_ * max_seconds_));
        if (buffer_.size() != cap) {
            buffer_.assign(cap, 0.0f);
        }
        reset();
    }

    void reset() {
        is_capturing_.store(false, std::memory_order_release);
        written_samples_.store(0, std::memory_order_release);
        peak_level_.store(0.0f, std::memory_order_release);
    }

    void startCapture() {
        written_samples_.store(0, std::memory_order_release);
        peak_level_.store(0.0f, std::memory_order_release);
        is_capturing_.store(true, std::memory_order_release);
    }

    void stopCapture() {
        is_capturing_.store(false, std::memory_order_release);
    }

    bool isCapturing() const noexcept {
        return is_capturing_.load(std::memory_order_acquire);
    }

    // Audio-thread only: lock-free, allocation-free true circular buffer
    void pushBlock(const float* const* channelData, int numChannels, int numSamples) noexcept {
        if (!is_capturing_.load(std::memory_order_acquire) || numChannels <= 0 || numSamples <= 0 || buffer_.empty()) {
            peak_level_.store(0.0f, std::memory_order_relaxed);
            return;
        }
        in_push_.store(true, std::memory_order_release);

        const size_t cap = buffer_.size();
        size_t pos = written_samples_.load(std::memory_order_relaxed);
        float blockPeak = 0.0f;

        const float invCh = 1.0f / static_cast<float>(numChannels);
        for (int i = 0; i < numSamples; ++i) {
            float mono = 0.0f;
            for (int c = 0; c < numChannels; ++c) {
                mono += channelData[c][i];
            }
            mono *= invCh;
            blockPeak = std::max(blockPeak, std::abs(mono));
            buffer_[pos % cap] = mono;
            ++pos;
        }

        written_samples_.store(pos, std::memory_order_release);
        peak_level_.store(blockPeak, std::memory_order_relaxed);

        in_push_.store(false, std::memory_order_release);
    }

    struct CapturedAudio {
        std::vector<float> mono_samples;
        double sample_rate = 44100.0;
    };

    // Message / background thread: stops capture and extracts linearized most recent samples
    CapturedAudio extractSnapshot() {
        stopCapture();
        while (in_push_.load(std::memory_order_acquire)) {
            // Brief spin until active audio callback push exits (microseconds)
        }
        const size_t cap = buffer_.size();
        const size_t totalWritten = written_samples_.load(std::memory_order_acquire);
        const size_t count = std::min(cap, totalWritten);
        CapturedAudio out;
        out.sample_rate = sample_rate_;
        out.mono_samples.resize(count);
        if (count > 0) {
            if (totalWritten <= cap) {
                std::copy_n(buffer_.begin(), count, out.mono_samples.begin());
            } else {
                const size_t head = totalWritten % cap;
                const size_t firstPart = cap - head;
                std::copy_n(buffer_.begin() + static_cast<std::ptrdiff_t>(head), firstPart, out.mono_samples.begin());
                std::copy_n(buffer_.begin(), head, out.mono_samples.begin() + static_cast<std::ptrdiff_t>(firstPart));
            }
        }
        return out;
    }

    double capturedSeconds() const noexcept {
        const size_t stored = std::min(buffer_.size(), written_samples_.load(std::memory_order_relaxed));
        return static_cast<double>(stored) / std::max(1.0, sample_rate_);
    }

    float peakLevel() const noexcept {
        return peak_level_.load(std::memory_order_relaxed);
    }

    double sampleRate() const noexcept { return sample_rate_; }

private:
    double max_seconds_ = 30.0;
    double sample_rate_ = 44100.0;
    std::vector<float> buffer_;
    std::atomic<bool> is_capturing_{false};
    std::atomic<bool> in_push_{false};
    std::atomic<size_t> written_samples_{0};
    std::atomic<float> peak_level_{0.0f};
};

} // namespace style_scaler
