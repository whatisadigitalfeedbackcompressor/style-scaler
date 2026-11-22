#include "engine/TranscriptionService.h"
#include "style_scaler/SmfIO.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <thread>

namespace style_scaler {

namespace {
constexpr int kFftOrder = 12;
constexpr int kFftSize = 1 << kFftOrder; // 4096
constexpr int kHopSamples = 800;         // 50 ms at 16 kHz
constexpr double kTwoPi = 6.28318530717958647692;
} // namespace

std::vector<TimedNoteSec> SpectralFallbackTranscriber::transcribe(
    const std::vector<float>& mono16k,
    const std::function<bool(float)>& onProgress) {

    std::vector<TimedNoteSec> out;
    if (mono16k.size() < static_cast<size_t>(kFftSize)) {
        return out;
    }

    juce::dsp::FFT fft(kFftOrder);
    std::vector<float> window(kFftSize, 0.0f);
    for (int i = 0; i < kFftSize; ++i) {
        window[static_cast<size_t>(i)] =
            0.5f * (1.0f - std::cos(static_cast<float>(kTwoPi * i / (kFftSize - 1))));
    }

    // Precompute FFT bin ranges for MIDI pitches 28..96
    struct MidiBinRange {
        int pitch = 60;
        int binLo = 0;
        int binHi = 0;
    };
    std::vector<MidiBinRange> midiBins;
    for (int m = 28; m <= 96; ++m) {
        const double hz = 440.0 * std::pow(2.0, (static_cast<double>(m) - 69.0) / 12.0);
        const double loHz = hz * std::pow(2.0, -0.45 / 12.0);
        const double hiHz = hz * std::pow(2.0, 0.45 / 12.0);
        const int bLo = std::max(1, static_cast<int>(std::floor(loHz * kFftSize / kSampleRate)));
        const int bHi = std::min(kFftSize / 2 - 1, static_cast<int>(std::ceil(hiHz * kFftSize / kSampleRate)));
        if (bLo <= bHi) {
            midiBins.push_back(MidiBinRange{m, bLo, bHi});
        }
    }

    std::vector<float> fftData(kFftSize * 2, 0.0f);
    const size_t totalFrames = (mono16k.size() - kFftSize) / kHopSamples + 1;
    std::array<double, 128> activeOnsetSec{};
    activeOnsetSec.fill(-1.0);
    std::array<float, 128> activePeakMag{};
    activePeakMag.fill(0.0f);

    const double frameDurSec = static_cast<double>(kHopSamples) / kSampleRate;

    for (size_t frame = 0; frame < totalFrames; ++frame) {
        if (onProgress && (frame % 16 == 0)) {
            if (!onProgress(static_cast<float>(frame) / static_cast<float>(std::max<size_t>(1, totalFrames)))) {
                return out;
            }
        }

        const size_t pos = frame * kHopSamples;
        std::fill(fftData.begin(), fftData.end(), 0.0f);
        for (int i = 0; i < kFftSize; ++i) {
            fftData[static_cast<size_t>(i)] = mono16k[pos + static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
        }
        fft.performFrequencyOnlyForwardTransform(fftData.data());

        std::array<float, 128> pitchMag{};
        float frameMax = 0.0f;
        for (const auto& mb : midiBins) {
            float peak = 0.0f;
            for (int b = mb.binLo; b <= mb.binHi; ++b) {
                peak = std::max(peak, fftData[static_cast<size_t>(b)]);
            }
            // Mild octave harmonic suppression so fundamentals dominate over 2nd harmonics
            if (mb.pitch >= 40) {
                peak = std::max(0.0f, peak - 0.30f * pitchMag[static_cast<size_t>(mb.pitch - 12)]);
            }
            pitchMag[static_cast<size_t>(mb.pitch)] = peak;
            frameMax = std::max(frameMax, peak);
        }

        std::array<bool, 128> nowActive{};
        if (frameMax > 8.0f) {
            const float thresh = frameMax * 0.32f;
            std::vector<std::pair<float, int>> cands;
            for (const auto& mb : midiBins) {
                const float mag = pitchMag[static_cast<size_t>(mb.pitch)];
                if (mag >= thresh) {
                    cands.emplace_back(mag, mb.pitch);
                }
            }
            std::sort(cands.begin(), cands.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            const size_t keep = std::min<size_t>(6, cands.size());
            for (size_t k = 0; k < keep; ++k) {
                nowActive[static_cast<size_t>(cands[k].second)] = true;
            }
        }

        const double tSec = static_cast<double>(frame) * frameDurSec;
        for (int p = 28; p <= 96; ++p) {
            const size_t idx = static_cast<size_t>(p);
            if (nowActive[idx]) {
                if (activeOnsetSec[idx] < 0.0) {
                    activeOnsetSec[idx] = tSec;
                    activePeakMag[idx] = pitchMag[idx];
                } else {
                    activePeakMag[idx] = std::max(activePeakMag[idx], pitchMag[idx]);
                }
            } else if (activeOnsetSec[idx] >= 0.0) {
                const double dur = tSec - activeOnsetSec[idx];
                if (dur >= 0.08) {
                    TimedNoteSec n;
                    n.onset_sec = activeOnsetSec[idx];
                    n.offset_sec = tSec;
                    n.pitch = p;
                    n.velocity = std::clamp(static_cast<int>(std::llround(60.0 + std::min(1.0f, activePeakMag[idx] / 250.0f) * 55.0)), 45, 118);
                    n.program = (p < 48) ? 33 : 0;
                    n.is_drum = false;
                    out.push_back(n);
                }
                activeOnsetSec[idx] = -1.0;
                activePeakMag[idx] = 0.0f;
            }
        }
    }

    const double endSec = static_cast<double>(totalFrames) * frameDurSec;
    for (int p = 28; p <= 96; ++p) {
        const size_t idx = static_cast<size_t>(p);
        if (activeOnsetSec[idx] >= 0.0 && (endSec - activeOnsetSec[idx]) >= 0.08) {
            TimedNoteSec n;
            n.onset_sec = activeOnsetSec[idx];
            n.offset_sec = endSec;
            n.pitch = p;
            n.velocity = 92;
            n.program = (p < 48) ? 33 : 0;
            out.push_back(n);
        }
    }

    std::sort(out.begin(), out.end(), [](const TimedNoteSec& a, const TimedNoteSec& b) {
        if (a.onset_sec != b.onset_sec) return a.onset_sec < b.onset_sec;
        return a.pitch < b.pitch;
    });
    return out;
}

class TranscriptionService::WorkerThread : public juce::Thread {
public:
    WorkerThread(TranscriptionService& owner,
                 juce::File file,
                 std::vector<float> audioSamples,
                 double sampleRate,
                 std::string label,
                 TargetSlot targetSlot,
                 std::function<void(float, std::string)> onProgress,
                 std::function<void(Result)> onComplete)
        : juce::Thread("StyleScalerTranscribe"),
          owner_(owner),
          file_(std::move(file)),
          audio_samples_(std::move(audioSamples)),
          sample_rate_(sampleRate),
          label_(std::move(label)),
          target_slot_(targetSlot),
          on_progress_(std::move(onProgress)),
          on_complete_(std::move(onComplete)) {}

    void run() override {
        auto reportProgress = [&](float p, const std::string& status) {
            if (on_progress_) {
                auto cb = on_progress_;
                juce::MessageManager::callAsync([cb, p, status]() { cb(p, status); });
            }
        };

        auto finish = [&](Result r) {
            owner_.running_.store(false, std::memory_order_release);
            if (on_complete_) {
                auto cb = on_complete_;
                juce::MessageManager::callAsync([cb, r = std::move(r)]() mutable { cb(std::move(r)); });
            }
        };

        Result res;
        res.target_slot = target_slot_;

        // 1. Direct MIDI file drop
        if (file_.existsAsFile() &&
            (file_.hasFileExtension(".mid") || file_.hasFileExtension(".midi"))) {
            reportProgress(0.2f, "Reading MIDI file...");
            auto smf = SmfReader::readFile(file_.getFullPathName().toStdString());
            if (!smf) {
                res.error = smf.error();
                finish(std::move(res));
                return;
            }
            reportProgress(0.7f, "Extracting 4-axis StyleProfile...");
            AnalysisOptions opts;
            opts.id = file_.getFileNameWithoutExtension().replaceCharacter(' ', '_').toLowerCase().toStdString();
            opts.name = file_.getFileNameWithoutExtension().toStdString();
            opts.genre = "Analyzed MIDI";
            opts.source = "in_plugin_midi";
            opts.time_sig_num = smf->time_sig_num;
            opts.time_sig_den = smf->time_sig_den;
            res.profile = MidiStyleAnalyzer::analyze(smf->notes, smf->bpm, opts);
            res.num_notes = smf->notes.size();
            res.backend_name = "MIDI Direct";
            res.success = true;
            reportProgress(1.0f, "MIDI analysis complete");
            finish(std::move(res));
            return;
        }

        // 2. Audio file decode or live capture resample to 16 kHz mono
        std::vector<float> mono16k;
        std::string sourceTitle = label_.empty() ? "Captured Audio" : label_;

        if (file_.existsAsFile()) {
            sourceTitle = file_.getFileNameWithoutExtension().toStdString();
            reportProgress(0.08f, "Decoding & resampling audio...");
            std::string decodeErr;
            mono16k = decodeAudioFileTo16kMono(file_, decodeErr);
            if (mono16k.empty()) {
                res.error = decodeErr.empty() ? "Failed to decode audio file" : decodeErr;
                finish(std::move(res));
                return;
            }
        } else if (!audio_samples_.empty()) {
            reportProgress(0.08f, "Resampling captured DAW audio...");
            mono16k = resampleTo16kMono(audio_samples_, sample_rate_);
        } else {
            res.error = "No audio or MIDI input provided";
            finish(std::move(res));
            return;
        }

        if (owner_.cancel_requested_.load(std::memory_order_acquire) || threadShouldExit()) {
            res.error = "Cancelled";
            finish(std::move(res));
            return;
        }

        std::vector<TimedNoteSec> timedNotes;
        std::string backendUsed;

#if STYLE_SCALER_HAS_MUSCRIPTOR
        if (owner_.model_manager_.isModelAvailable()) {
            const auto ggufPath = std::filesystem::path(owner_.model_manager_.getActiveModelFile().getFullPathName().toStdString());
            if (!owner_.cached_transcriber_ || owner_.cached_model_path_ != ggufPath) {
                reportProgress(0.12f, "Loading MuScriptor GGUF...");
                msl::LoadOptions lopts;
                lopts.should_cancel = [&]() {
                    return owner_.cancel_requested_.load(std::memory_order_acquire) || threadShouldExit();
                };
                lopts.on_progress = [&](float p) {
                    reportProgress(0.12f + 0.18f * p, "Loading MuScriptor weights...");
                };
                auto loaded = msl::Transcriber::load(ggufPath, lopts);
                if (loaded) {
                    owner_.cached_transcriber_ = std::make_unique<msl::Transcriber>(std::move(*loaded));
                    owner_.cached_model_path_ = ggufPath;
                } else {
                    owner_.cached_transcriber_.reset();
                }
            }

            if (owner_.cached_transcriber_) {
                msl::TranscribeOptions topts;
                const unsigned hw = std::thread::hardware_concurrency();
                topts.n_threads = std::max(1, static_cast<int>(hw > 0 ? hw / 2 : 2));
                topts.should_cancel = [&]() {
                    return owner_.cancel_requested_.load(std::memory_order_acquire) || threadShouldExit();
                };

                reportProgress(0.30f, "Transcribing with NeuralNote MuScriptor...");
                auto mslNotes = owner_.cached_transcriber_->transcribe(
                    mono16k, topts, [&](const msl::TranscriptionUpdate& upd) {
                        reportProgress(0.30f + 0.60f * upd.progress, "Transcribing with MuScriptor...");
                        return !owner_.cancel_requested_.load(std::memory_order_acquire) && !threadShouldExit();
                    });

                if (mslNotes) {
                    timedNotes.reserve(mslNotes->size());
                    for (const auto& n : *mslNotes) {
                        TimedNoteSec tn;
                        tn.onset_sec = n.onset;
                        tn.offset_sec = std::max(n.onset + 0.02, n.offset);
                        tn.pitch = n.pitch;
                        tn.velocity = 96;
                        tn.program = n.program;
                        tn.is_drum = n.is_drum;
                        timedNotes.push_back(tn);
                    }
                    backendUsed = "MuScriptor (" + owner_.cached_transcriber_->device().backend + ")";
                } else if (mslNotes.error() == msl::Error::Cancelled) {
                    res.error = "Cancelled";
                    finish(std::move(res));
                    return;
                }
            }
        }
#endif

        if (backendUsed.empty()) {
            reportProgress(0.35f, "Transcribing audio (Spectral Fallback)...");
            timedNotes = SpectralFallbackTranscriber::transcribe(
                mono16k, [&](float p) {
                    reportProgress(0.35f + 0.55f * p, "Transcribing audio (Spectral Fallback)...");
                    return !owner_.cancel_requested_.load(std::memory_order_acquire) && !threadShouldExit();
                });
            backendUsed = "Spectral Fallback";
        }

        if (owner_.cancel_requested_.load(std::memory_order_acquire) || threadShouldExit()) {
            res.error = "Cancelled";
            finish(std::move(res));
            return;
        }

        if (timedNotes.empty()) {
            res.error = "No pitched notes detected in audio";
            finish(std::move(res));
            return;
        }

        reportProgress(0.92f, "Building 4-axis StyleProfile...");
        AnalysisOptions aopts;
        aopts.id = juce::String(sourceTitle).replaceCharacter(' ', '_').toLowerCase().toStdString();
        aopts.name = sourceTitle;
        aopts.genre = "Transcribed Audio";
        aopts.source = backendUsed;

        res.profile = MidiStyleAnalyzer::analyzeTimed(timedNotes, aopts);
        res.num_notes = timedNotes.size();
        res.backend_name = backendUsed;
        res.success = true;
        reportProgress(1.0f, "Style analysis ready (" + std::to_string(res.num_notes) + " notes)");
        finish(std::move(res));
    }

private:
    TranscriptionService& owner_;
    juce::File file_;
    std::vector<float> audio_samples_;
    double sample_rate_ = 44100.0;
    std::string label_;
    TargetSlot target_slot_ = TargetSlot::SlotA;
    std::function<void(float, std::string)> on_progress_;
    std::function<void(Result)> on_complete_;
};

TranscriptionService::TranscriptionService(ModelManager& modelManager)
    : model_manager_(modelManager) {}

TranscriptionService::~TranscriptionService() {
    cancel();
}

void TranscriptionService::cancel() {
    cancel_requested_.store(true, std::memory_order_release);
    if (worker_) {
        worker_->signalThreadShouldExit();
        worker_->stopThread(6000);
        worker_.reset();
    }
    running_.store(false, std::memory_order_release);
}

void TranscriptionService::transcribeFileAsync(const juce::File& file,
                                               TargetSlot targetSlot,
                                               std::function<void(float, std::string)> onProgress,
                                               std::function<void(Result)> onComplete) {
    cancel();
    cancel_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_ = std::make_unique<WorkerThread>(
        *this, file, std::vector<float>{}, 44100.0, file.getFileNameWithoutExtension().toStdString(),
        targetSlot, std::move(onProgress), std::move(onComplete));
    worker_->startThread(juce::Thread::Priority::normal);
}

void TranscriptionService::transcribeAudioAsync(std::vector<float> monoSamples,
                                                double sourceSampleRate,
                                                std::string sourceLabel,
                                                TargetSlot targetSlot,
                                                std::function<void(float, std::string)> onProgress,
                                                std::function<void(Result)> onComplete) {
    cancel();
    cancel_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_ = std::make_unique<WorkerThread>(
        *this, juce::File(), std::move(monoSamples), sourceSampleRate, std::move(sourceLabel),
        targetSlot, std::move(onProgress), std::move(onComplete));
    worker_->startThread(juce::Thread::Priority::normal);
}

std::vector<float> TranscriptionService::resampleTo16kMono(const std::vector<float>& input,
                                                            double sourceSampleRate) {
    if (input.empty() || sourceSampleRate <= 0.0) return {};
    if (std::abs(sourceSampleRate - SpectralFallbackTranscriber::kSampleRate) < 1.0) {
        return input;
    }

    const double ratio = sourceSampleRate / SpectralFallbackTranscriber::kSampleRate;
    const int outCount = std::max(1, static_cast<int>(std::floor(static_cast<double>(input.size()) / ratio)));
    std::vector<float> output(static_cast<size_t>(outCount), 0.0f);

    juce::LagrangeInterpolator interp;
    interp.process(ratio, input.data(), output.data(), outCount);
    return output;
}

std::vector<float> TranscriptionService::decodeAudioFileTo16kMono(const juce::File& file,
                                                                  std::string& outError) {
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (!reader) {
        outError = "Unsupported or unreadable audio format: " + file.getFileName().toStdString();
        return {};
    }

    const int maxSamples = static_cast<int>(std::min<juce::int64>(
        reader->lengthInSamples,
        static_cast<juce::int64>(std::llround(reader->sampleRate * 45.0))));
    if (maxSamples <= 0) {
        outError = "Audio file is empty";
        return {};
    }

    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), maxSamples);
    if (!reader->read(&buf, 0, maxSamples, 0, true, true)) {
        outError = "Failed reading samples from audio file";
        return {};
    }

    std::vector<float> mono(static_cast<size_t>(maxSamples), 0.0f);
    const int nCh = buf.getNumChannels();
    const float invCh = 1.0f / static_cast<float>(std::max(1, nCh));
    for (int ch = 0; ch < nCh; ++ch) {
        const float* src = buf.getReadPointer(ch);
        for (int i = 0; i < maxSamples; ++i) {
            mono[static_cast<size_t>(i)] += src[i] * invCh;
        }
    }

    return resampleTo16kMono(mono, reader->sampleRate);
}

} // namespace style_scaler
