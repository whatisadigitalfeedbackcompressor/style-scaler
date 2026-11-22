#include "engine/TranscriptionService.h"
#include "style_scaler/SmfIO.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace style_scaler {

namespace {
constexpr int kFftOrder = 12;
constexpr int kFftSize = 1 << kFftOrder; // 4096
constexpr int kHopSamples = 800;         // 50 ms at 16 kHz
constexpr double kTwoPi = 6.28318530717958647692;

std::vector<NoteEvent> timedNotesToEvents(const std::vector<TimedNoteSec>& notes, double bpm) {
    const double beatsPerSec = std::max(20.0, bpm) / 60.0;
    std::vector<NoteEvent> events;
    events.reserve(notes.size());
    for (const auto& n : notes) {
        NoteEvent ev;
        ev.onset_beats = std::max(0.0, n.onset_sec * beatsPerSec);
        ev.duration_beats = std::max(0.0625, (n.offset_sec - n.onset_sec) * beatsPerSec);
        ev.pitch = std::clamp(n.pitch, 0, 127);
        ev.velocity = std::clamp(n.velocity, 1, 127);
        ev.program = n.program;
        ev.is_drum = n.is_drum || n.program == 128;
        if (ev.is_drum) {
            ev.channel = 10;
        } else if (ev.program >= 32 && ev.program <= 39) {
            ev.channel = 3;
        } else if ((ev.program >= 56 && ev.program <= 87) || (ev.program >= 104 && ev.program <= 111)) {
            ev.channel = 2;
        } else {
            ev.channel = 1;
        }
        events.push_back(ev);
    }
    return events;
}
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

    // 5-frame rolling magnitude buffer for HPSS temporal median drum-transient rejection
    std::array<std::array<float, 128>, 5> magHistory{};
    size_t histCount = 0;

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

        auto& curRaw = magHistory[frame % 5];
        curRaw.fill(0.0f);
        for (const auto& mb : midiBins) {
            float peak = 0.0f;
            for (int b = mb.binLo; b <= mb.binHi; ++b) {
                peak = std::max(peak, fftData[static_cast<size_t>(b)]);
            }
            curRaw[static_cast<size_t>(mb.pitch)] = peak;
        }
        histCount = std::min<size_t>(5, histCount + 1);

        std::array<float, 128> pitchMag{};
        float frameMax = 0.0f;
        for (const auto& mb : midiBins) {
            const size_t pIdx = static_cast<size_t>(mb.pitch);
            float medianMag = curRaw[pIdx];
            if (histCount >= 3) {
                std::array<float, 5> tmp{};
                for (size_t h = 0; h < histCount; ++h) tmp[h] = magHistory[h][pIdx];
                std::sort(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(histCount));
                medianMag = tmp[histCount / 2];
            }
            float suppressed = medianMag;
            if (mb.pitch >= 40) suppressed -= 0.35f * pitchMag[pIdx - 12]; // 2nd harmonic
            if (mb.pitch >= 47) suppressed -= 0.22f * pitchMag[pIdx - 19]; // 3rd harmonic
            if (mb.pitch >= 52) suppressed -= 0.15f * pitchMag[pIdx - 24]; // 4th harmonic
            suppressed = std::max(0.0f, suppressed);
            pitchMag[pIdx] = suppressed;
            frameMax = std::max(frameMax, suppressed);
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

TranscriptionService::DeconstructedSong TranscriptionService::deconstructAudio16kBlocking(
    const std::vector<float>& mono16k,
    const DeconstructOptions& opts,
    const std::function<bool(float, std::string)>& onProgress) {

    DeconstructedSong out;
    if (mono16k.empty()) {
        out.error = "Empty audio buffer";
        return out;
    }

    auto shouldCancel = [&]() -> bool {
        return cancel_requested_.load(std::memory_order_acquire);
    };

    std::vector<TimedNoteSec> timedNotes;
    std::string backendUsed;

#if STYLE_SCALER_HAS_MUSCRIPTOR
    if (!opts.force_spectral && model_manager_.isModelAvailable()) {
        const auto ggufPath = std::filesystem::path(model_manager_.getActiveModelFile().getFullPathName().toStdString());
        if (!cached_transcriber_ || cached_model_path_ != ggufPath) {
            if (onProgress && !onProgress(0.12f, "Loading MuScriptor GGUF...")) {
                out.error = "Cancelled";
                return out;
            }
            msl::LoadOptions lopts;
            lopts.should_cancel = shouldCancel;
            lopts.on_progress = [&](float p) {
                if (onProgress) onProgress(0.12f + 0.18f * p, "Loading MuScriptor weights...");
            };
            auto loaded = msl::Transcriber::load(ggufPath, lopts);
            if (loaded) {
                cached_transcriber_ = std::make_unique<msl::Transcriber>(std::move(*loaded));
                cached_model_path_ = ggufPath;
            } else {
                cached_transcriber_.reset();
            }
        }

        if (cached_transcriber_) {
            msl::TranscribeOptions topts;
            const unsigned hw = std::thread::hardware_concurrency();
            topts.n_threads = (opts.n_threads > 0)
                                  ? opts.n_threads
                                  : std::max(1, static_cast<int>(hw > 0 ? hw / 2 : 2));
            topts.should_cancel = shouldCancel;

            if (onProgress && !onProgress(0.30f, "Transcribing with NeuralNote MuScriptor...")) {
                out.error = "Cancelled";
                return out;
            }

            auto mslNotes = cached_transcriber_->transcribe(
                mono16k, topts, [&](const msl::TranscriptionUpdate& upd) {
                    const bool keepGoing = !shouldCancel();
                    if (onProgress && !onProgress(0.30f + 0.60f * upd.progress, "Transcribing with MuScriptor...")) {
                        return false;
                    }
                    return keepGoing;
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
                backendUsed = "MuScriptor (" + cached_transcriber_->device().backend + ")";
            } else if (mslNotes.error() == msl::Error::Cancelled) {
                out.error = "Cancelled";
                return out;
            }
        }
    }
#else
    (void)shouldCancel;
#endif

    if (backendUsed.empty()) {
        if (onProgress && !onProgress(0.35f, "Transcribing audio (Spectral Fallback)...")) {
            out.error = "Cancelled";
            return out;
        }
        timedNotes = SpectralFallbackTranscriber::transcribe(
            mono16k, [&](float p) {
                if (cancel_requested_.load(std::memory_order_acquire)) return false;
                if (onProgress) return onProgress(0.35f + 0.55f * p, "Transcribing audio (Spectral Fallback)...");
                return true;
            });
        backendUsed = "Spectral Fallback";
    }

    if (cancel_requested_.load(std::memory_order_acquire)) {
        out.error = "Cancelled";
        return out;
    }

    if (timedNotes.empty()) {
        out.error = "No pitched notes detected in audio";
        return out;
    }

    const double bpm = (opts.bpm_hint > 20.0) ? opts.bpm_hint : MidiStyleAnalyzer::estimateBpm(timedNotes);
    out.timed_notes = std::move(timedNotes);
    out.song_input.notes = timedNotesToEvents(out.timed_notes, bpm);
    out.song_input.bpm = bpm;
    out.song_input.time_sig_num = 4;
    out.song_input.time_sig_den = 4;
    out.backend_name = backendUsed;
    out.success = true;
    return out;
}

TranscriptionService::DeconstructedSong TranscriptionService::deconstructFileBlocking(
    const juce::File& file,
    const DeconstructOptions& opts,
    const std::function<bool(float, std::string)>& onProgress) {

    DeconstructedSong out;
    if (!file.existsAsFile()) {
        out.error = "File not found: " + file.getFullPathName().toStdString();
        return out;
    }

    const SongMetadata meta = AudioMetadataReader::readFile(
        std::filesystem::path(file.getFullPathName().toStdString()));

    DeconstructOptions effectiveOpts = opts;
    if (effectiveOpts.bpm_hint <= 20.0 && meta.bpm > 20.0) {
        effectiveOpts.bpm_hint = meta.bpm;
    }

    if (file.hasFileExtension(".mid") || file.hasFileExtension(".midi")) {
        if (onProgress && !onProgress(0.25f, "Reading MIDI file...")) {
            out.error = "Cancelled";
            return out;
        }
        auto smf = SmfReader::readFile(file.getFullPathName().toStdString());
        if (!smf) {
            out.error = smf.error();
            return out;
        }
        out.song_input.notes = std::move(smf->notes);
        out.song_input.bpm = (opts.bpm_hint > 20.0) ? opts.bpm_hint : (smf->bpm > 20.0 ? smf->bpm : effectiveOpts.bpm_hint);
        if (out.song_input.bpm <= 20.0) out.song_input.bpm = 120.0;
        out.song_input.time_sig_num = smf->time_sig_num;
        out.song_input.time_sig_den = smf->time_sig_den;
        out.backend_name = "MIDI Direct";
        out.metadata = meta;
        out.success = true;
        return out;
    }

    if (onProgress && !onProgress(0.08f, "Decoding & resampling audio...")) {
        out.error = "Cancelled";
        return out;
    }
    std::string decodeErr;
    const auto mono16k = decodeAudioFileTo16kMono(file, decodeErr, effectiveOpts.max_seconds);
    if (mono16k.empty()) {
        out.error = decodeErr.empty() ? "Failed to decode audio file" : decodeErr;
        return out;
    }

    out = deconstructAudio16kBlocking(mono16k, effectiveOpts, onProgress);
    if (out.success) {
        out.metadata = meta;
    }
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
        auto reportProgress = [&](float p, const std::string& status) -> bool {
            if (owner_.cancel_requested_.load(std::memory_order_acquire) || threadShouldExit()) {
                return false;
            }
            if (on_progress_) {
                auto cb = on_progress_;
                juce::MessageManager::callAsync([cb, p, status]() { cb(p, status); });
            }
            return true;
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

        DeconstructOptions dopts;
        dopts.max_seconds = 45.0;

        DeconstructedSong ds;
        std::string sourceTitle = label_.empty() ? "Captured Audio" : label_;
        if (file_.existsAsFile()) {
            sourceTitle = file_.getFileNameWithoutExtension().toStdString();
            ds = owner_.deconstructFileBlocking(file_, dopts, reportProgress);
        } else if (!audio_samples_.empty()) {
            if (!reportProgress(0.08f, "Resampling captured DAW audio...")) {
                res.error = "Cancelled";
                finish(std::move(res));
                return;
            }
            const auto mono16k = resampleTo16kMono(audio_samples_, sample_rate_);
            ds = owner_.deconstructAudio16kBlocking(mono16k, dopts, reportProgress);
        } else {
            res.error = "No audio or MIDI input provided";
            finish(std::move(res));
            return;
        }

        if (!ds.success) {
            res.error = ds.error;
            finish(std::move(res));
            return;
        }

        reportProgress(0.92f, "Building 4-axis StyleProfile & detecting genre...");
        const std::string displayTitle = !ds.metadata.title.empty() ? ds.metadata.title : sourceTitle;
        const std::string rawSlugSource = !ds.metadata.artist.empty()
                                              ? (ds.metadata.artist + "_" + displayTitle)
                                              : displayTitle;

        AnalysisOptions aopts;
        aopts.id = AudioMetadataReader::sanitizeGenreSlug(rawSlugSource);
        aopts.name = displayTitle;
        aopts.genre = ds.metadata.hasGenre()
                          ? ds.metadata.canonical_genre
                          : ((ds.backend_name == "MIDI Direct") ? "Analyzed MIDI" : "Transcribed Audio");
        aopts.source = ds.backend_name;
        aopts.tags = ds.metadata.tags;
        aopts.genre_source = ds.metadata.hasGenre() ? ds.metadata.genre_source : "none";
        aopts.artist = ds.metadata.artist;
        aopts.album = ds.metadata.album;
        aopts.time_sig_num = ds.song_input.time_sig_num;
        aopts.time_sig_den = ds.song_input.time_sig_den;

        res.profile = MidiStyleAnalyzer::analyze(ds.song_input.notes, ds.song_input.bpm, aopts);

        if (!ds.metadata.hasGenre()) {
            const std::string inferred = AudioMetadataReader::inferGenreFromProfileHeuristic(res.profile);
            res.profile.genre = inferred;
            res.profile.genre_source = "heuristic";
            std::string lowTag;
            lowTag.reserve(inferred.size());
            for (unsigned char c : inferred) lowTag.push_back(static_cast<char>(std::tolower(c)));
            if (std::find(res.profile.tags.begin(), res.profile.tags.end(), lowTag) == res.profile.tags.end()) {
                res.profile.tags.push_back(lowTag);
            }
            ds.metadata.canonical_genre = inferred;
            ds.metadata.genre_source = "heuristic";
            ds.metadata.tags = res.profile.tags;
        }

        res.profile.description = "Analyzed " + res.profile.genre + " (" + res.profile.genre_source + ")";
        if (!res.profile.artist.empty()) {
            res.profile.description += " by " + res.profile.artist;
        }

        res.metadata = std::move(ds.metadata);
        res.num_notes = ds.song_input.notes.size();
        res.backend_name = ds.backend_name;
        res.success = true;
        reportProgress(1.0f, "Ready [" + res.profile.genre + " via " + res.profile.genre_source + "] (" +
                                 std::to_string(res.num_notes) + " notes)");
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
                                                                  std::string& outError,
                                                                  double maxSeconds) {
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (!reader) {
        outError = "Unsupported or unreadable audio format: " + file.getFileName().toStdString();
        return {};
    }

    const double effectiveMaxSec = (maxSeconds > 0.0) ? maxSeconds : 600.0;
    const juce::int64 totalFileSamples = reader->lengthInSamples;
    const int maxSamples = static_cast<int>(std::min<juce::int64>(
        totalFileSamples,
        static_cast<juce::int64>(std::llround(reader->sampleRate * effectiveMaxSec))));
    if (maxSamples <= 0) {
        outError = "Audio file is empty";
        return {};
    }

    // Smart harmonic window finder: when cropping a longer song to maxSeconds:
    // - For long tracks with maxSeconds >= 30s, stitch 3 representative windows
    //   (Intro + Chorus/Peak + Outro) so section segmentation & synthesis see true
    //   Intro, Chorus, and Outro DNA in a fixed 3x shorter CPU budget.
    // - Otherwise, select the single contiguous maxSeconds window with highest tonal energy.
    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), maxSamples);
    std::vector<int> spliceJoins;

    const bool useMultiWindow =
        (maxSeconds >= 30.0) &&
        (totalFileSamples >= static_cast<juce::int64>(std::llround(reader->sampleRate * std::max(75.0, maxSeconds * 1.35))));

    const int blockSamples = std::max(1024, static_cast<int>(std::llround(reader->sampleRate * 2.0)));
    auto computeBlockStats = [&](double limitSec,
                                 std::vector<double>& outRms,
                                 std::vector<double>& outScores) {
        const juce::int64 scanLimit = std::min<juce::int64>(
            totalFileSamples,
            static_cast<juce::int64>(std::llround(reader->sampleRate * limitSec)));
        const int nBlocks = static_cast<int>(scanLimit / blockSamples);
        outRms.assign(static_cast<size_t>(std::max(0, nBlocks)), 0.0);
        outScores.assign(static_cast<size_t>(std::max(0, nBlocks)), 0.0);
        if (nBlocks <= 0) return;

        juce::AudioBuffer<float> scanBuf(static_cast<int>(reader->numChannels), blockSamples);
        const int nChScan = std::max(1, static_cast<int>(reader->numChannels));
        for (int b = 0; b < nBlocks; ++b) {
            const juce::int64 bStart = static_cast<juce::int64>(b) * blockSamples;
            if (!reader->read(&scanBuf, 0, blockSamples, bStart, true, true)) continue;
            double sqSum = 0.0;
            int zeroCrossings = 0;
            float prevSample = 0.0f;
            for (int i = 0; i < blockSamples; ++i) {
                float m = 0.0f;
                for (int ch = 0; ch < nChScan; ++ch) m += scanBuf.getSample(ch, i);
                m /= static_cast<float>(nChScan);
                sqSum += static_cast<double>(m) * static_cast<double>(m);
                if (i > 0 && ((m >= 0.0f && prevSample < 0.0f) || (m < 0.0f && prevSample >= 0.0f))) {
                    ++zeroCrossings;
                }
                prevSample = m;
            }
            const double rms = std::sqrt(sqSum / static_cast<double>(blockSamples));
            const double zcr = static_cast<double>(zeroCrossings) / static_cast<double>(blockSamples);
            outRms[static_cast<size_t>(b)] = rms;
            if (rms > 0.01) {
                const double tonalWeight = (zcr > 0.22) ? 0.35 : 1.0;
                outScores[static_cast<size_t>(b)] = rms * tonalWeight;
            }
        }
    };

    auto findPeakBlock = [](const std::vector<double>& scores,
                            int startBlk,
                            int endBlk,
                            int windowBlocks,
                            double& outBestSum) -> int {
        double curSum = 0.0;
        for (int b = startBlk; b < startBlk + windowBlocks; ++b) {
            curSum += scores[static_cast<size_t>(b)];
        }
        double bestSum = curSum;
        int bestStart = startBlk;
        for (int b = startBlk + windowBlocks; b < endBlk; ++b) {
            curSum += scores[static_cast<size_t>(b)] - scores[static_cast<size_t>(b - windowBlocks)];
            if (curSum > bestSum) {
                bestSum = curSum;
                bestStart = b - windowBlocks + 1;
            }
        }
        outBestSum = bestSum;
        return bestStart;
    };

    if (useMultiWindow) {
        const int win1 = maxSamples / 3;
        const int win2 = maxSamples / 3;
        const int win3 = maxSamples - win1 - win2;

        std::vector<double> blockRms, blockScores;
        computeBlockStats(420.0, blockRms, blockScores);
        const int nBlocks = static_cast<int>(blockScores.size());

        juce::int64 introStart = 0;
        juce::int64 chorusStart = std::min<juce::int64>(win1, totalFileSamples - win2 - win3);
        juce::int64 outroStart = std::max<juce::int64>(introStart + win1 + win2, totalFileSamples - win3);

        if (nBlocks >= 6) {
            // 1. Intro window: first non-silent block in opening quarter
            int introStartBlock = 0;
            const int introScanEnd = std::max(1, nBlocks / 4);
            for (int b = 0; b < introScanEnd; ++b) {
                if (blockRms[static_cast<size_t>(b)] > 0.008) {
                    introStartBlock = b;
                    break;
                }
            }
            introStart = std::clamp<juce::int64>(
                static_cast<juce::int64>(introStartBlock) * blockSamples,
                0,
                totalFileSamples - maxSamples);

            // 3. Outro window: last non-silent block in final third
            int outroEndBlock = nBlocks - 1;
            const int outroScanStart = std::min(nBlocks - 1, (nBlocks * 2) / 3);
            for (int b = nBlocks - 1; b >= outroScanStart; --b) {
                if (blockRms[static_cast<size_t>(b)] > 0.008) {
                    outroEndBlock = b;
                    break;
                }
            }
            outroStart = std::clamp<juce::int64>(
                static_cast<juce::int64>(outroEndBlock + 1) * blockSamples - win3,
                introStart + win1 + win2,
                totalFileSamples - win3);

            // 2. Chorus / Peak window: highest tonal energy inside [introStart + win1, outroStart - win2]
            const juce::int64 midMin = introStart + win1;
            const juce::int64 midMax = outroStart - win2;
            const int win2Blocks = std::max(1, win2 / blockSamples);
            const int midStartBlk = std::clamp(
                static_cast<int>((midMin + blockSamples - 1) / blockSamples),
                0,
                std::max(0, nBlocks - win2Blocks));
            const int midEndBlk = std::clamp(
                static_cast<int>((midMax + win2) / blockSamples),
                midStartBlk,
                nBlocks);

            if (midEndBlk - midStartBlk >= win2Blocks) {
                double bestSum = 0.0;
                const int bestChorusBlock = findPeakBlock(blockScores, midStartBlk, midEndBlk, win2Blocks, bestSum);
                chorusStart = std::clamp<juce::int64>(
                    static_cast<juce::int64>(bestChorusBlock) * blockSamples,
                    midMin,
                    midMax);
            } else {
                chorusStart = midMin;
            }
        }

        if (!reader->read(&buf, 0, win1, introStart, true, true) ||
            !reader->read(&buf, win1, win2, chorusStart, true, true) ||
            !reader->read(&buf, win1 + win2, win3, outroStart, true, true)) {
            outError = "Failed reading samples from audio file";
            return {};
        }
        spliceJoins = {win1, win1 + win2};
    } else {
        juce::int64 startSample = 0;
        if (maxSeconds > 0.0 &&
            totalFileSamples > static_cast<juce::int64>(std::llround(reader->sampleRate * maxSeconds * 1.25))) {
            std::vector<double> blockRms, blockScores;
            computeBlockStats(300.0, blockRms, blockScores);
            const int blocksPerWindow = std::max(1, maxSamples / blockSamples);
            const int nBlocks = static_cast<int>(blockScores.size());

            if (nBlocks > blocksPerWindow) {
                double bestSum = 0.0;
                const int bestStartBlock = findPeakBlock(blockScores, 0, nBlocks, blocksPerWindow, bestSum);
                if (bestSum > 0.0) {
                    startSample = std::min<juce::int64>(
                        static_cast<juce::int64>(bestStartBlock) * blockSamples,
                        totalFileSamples - maxSamples);
                }
            }
        }

        if (!reader->read(&buf, 0, maxSamples, startSample, true, true)) {
            outError = "Failed reading samples from audio file";
            return {};
        }
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

    // Smooth 64-sample linear taper at multi-window splice boundaries to prevent clicks
    constexpr int kFadeSamples = 64;
    for (int join : spliceJoins) {
        for (int i = 0; i < kFadeSamples; ++i) {
            const float ramp = static_cast<float>(i) / static_cast<float>(kFadeSamples);
            if (join - 1 - i >= 0) {
                mono[static_cast<size_t>(join - 1 - i)] *= ramp;
            }
            if (join + i < maxSamples) {
                mono[static_cast<size_t>(join + i)] *= ramp;
            }
        }
    }

    return resampleTo16kMono(mono, reader->sampleRate);
}

} // namespace style_scaler
