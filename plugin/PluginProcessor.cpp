#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "StylePresetData.h"
#include "style_scaler/SmfIO.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace style_scaler {

namespace {

const juce::StringArray& keyRootChoices() {
    static const juce::StringArray kKeys = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    return kKeys;
}

const juce::StringArray& keyModeChoices() {
    static const juce::StringArray kModes = {
        "Minor", "Major", "Dorian", "Phrygian", "Mixolydian", "Harmonic Minor", "Lydian", "Locrian"
    };
    return kModes;
}

const juce::StringArray& tempoScaleChoices() {
    static const juce::StringArray kScales = {"0.5x", "1x", "2x"};
    return kScales;
}

const juce::StringArray& barsChoices() {
    static const juce::StringArray kBars = {"4", "8", "16"};
    return kBars;
}

} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout StyleScalerAudioProcessor::createParameterLayout() {
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ParamIDs::harmonyBlend, 1}, "Harmony Blend",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.005f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ParamIDs::rhythmBlend, 1}, "Rhythm Blend",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.005f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ParamIDs::voicingBlend, 1}, "Voicing Blend",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.005f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ParamIDs::melodyBlend, 1}, "Melody Blend",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.005f), 0.0f));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ParamIDs::tempoScale, 1}, "Tempo Scale", tempoScaleChoices(), 1));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{ParamIDs::grooveSync, 1}, "Groove Sync", true));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ParamIDs::voicingSpread, 1}, "Voicing Spread",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.005f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ParamIDs::melodyDensity, 1}, "Melody Density",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.005f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ParamIDs::humanize, 1}, "Humanize",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.005f), 0.15f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ParamIDs::previewGain, 1}, "Preview Gain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.005f), 0.75f));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{ParamIDs::enableChords, 1}, "Enable Chords", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{ParamIDs::enableMelody, 1}, "Enable Melody", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{ParamIDs::enableBass, 1}, "Enable Bass", true));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ParamIDs::keyRoot, 1}, "Key Root", keyRootChoices(), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ParamIDs::keyMode, 1}, "Key Mode", keyModeChoices(), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ParamIDs::progressionBars, 1}, "Progression Bars", barsChoices(), 1));

    return {params.begin(), params.end()};
}

StyleScalerAudioProcessor::StyleScalerAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "STYLE_SCALER_PARAMS", createParameterLayout()) {

    p_preview_gain_ = apvts_.getRawParameterValue(ParamIDs::previewGain);
    p_enable_chords_ = apvts_.getRawParameterValue(ParamIDs::enableChords);
    p_enable_melody_ = apvts_.getRawParameterValue(ParamIDs::enableMelody);
    p_enable_bass_ = apvts_.getRawParameterValue(ParamIDs::enableBass);

    loadBundledAndUserPresets();

    for (const char* pid : {
             ParamIDs::harmonyBlend, ParamIDs::rhythmBlend, ParamIDs::voicingBlend, ParamIDs::melodyBlend,
             ParamIDs::tempoScale, ParamIDs::grooveSync, ParamIDs::voicingSpread, ParamIDs::melodyDensity,
             ParamIDs::humanize, ParamIDs::enableChords, ParamIDs::enableMelody, ParamIDs::enableBass,
             ParamIDs::keyRoot, ParamIDs::keyMode, ParamIDs::progressionBars}) {
        apvts_.addParameterListener(pid, this);
    }

    // Initialize default Slot A = 90s_hiphop, Slot B = liquid_dnb
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        slot_a_ = makeDefaultStyleProfile();
        slot_b_ = makeDefaultStyleProfile();
        for (const auto& p : preset_library_) {
            if (p.id == "90s_hiphop") slot_a_ = p;
            if (p.id == "liquid_dnb") slot_b_ = p;
        }
        if (slot_a_.id == "default_minor" && !preset_library_.empty()) slot_a_ = preset_library_.front();
        if (slot_b_.id == "default_minor" && preset_library_.size() > 1) slot_b_ = preset_library_[1];
    }

    syncKeyParamsFromProfile(slot_a_);
    regenerateNowSync(false);
}

StyleScalerAudioProcessor::~StyleScalerAudioProcessor() {
    cancelPendingUpdate();
    transcription_service_.cancel();
    regen_pool_.removeAllJobs(true, 4000);
    for (const char* pid : {
             ParamIDs::harmonyBlend, ParamIDs::rhythmBlend, ParamIDs::voicingBlend, ParamIDs::melodyBlend,
             ParamIDs::tempoScale, ParamIDs::grooveSync, ParamIDs::voicingSpread, ParamIDs::melodyDensity,
             ParamIDs::humanize, ParamIDs::enableChords, ParamIDs::enableMelody, ParamIDs::enableBass,
             ParamIDs::keyRoot, ParamIDs::keyMode, ParamIDs::progressionBars}) {
        apvts_.removeParameterListener(pid, this);
    }
}

void StyleScalerAudioProcessor::loadBundledAndUserPresets() {
    std::lock_guard<std::mutex> lk(state_mutex_);
    preset_library_.clear();

    for (int i = 0; i < StylePresetData::namedResourceListSize; ++i) {
        int size = 0;
        const char* data = StylePresetData::getNamedResource(StylePresetData::namedResourceList[i], size);
        if (data != nullptr && size > 0) {
            auto parsed = loadFromJsonString(std::string_view(data, static_cast<size_t>(size)));
            if (parsed) {
                preset_library_.push_back(std::move(*parsed));
            }
        }
    }

    const juce::File userDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                   .getChildFile("StyleScaler")
                                   .getChildFile("presets");
    if (userDir.isDirectory()) {
        for (const auto& f : userDir.findChildFiles(juce::File::findFiles, false, "*.json")) {
            auto parsed = loadFromFile(f.getFullPathName().toStdString());
            if (parsed) {
                auto it = std::find_if(preset_library_.begin(), preset_library_.end(),
                                       [&](const StyleProfile& ex) { return ex.id == parsed->id; });
                if (it != preset_library_.end()) *it = std::move(*parsed);
                else preset_library_.push_back(std::move(*parsed));
            }
        }
    }

    if (preset_library_.empty()) {
        preset_library_.push_back(makeDefaultStyleProfile());
    }
}

void StyleScalerAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/) {
    sequencer_.prepare(sampleRate);
    preview_synth_.prepare(sampleRate);
    capture_ring_.prepare(sampleRate);
}

void StyleScalerAudioProcessor::releaseResources() {
    preview_synth_.allNotesOff();
    sequencer_.reset();
}

bool StyleScalerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto outSet = layouts.getMainOutputChannelSet();
    if (outSet != juce::AudioChannelSet::stereo()) {
        return false;
    }
    const auto inSet = layouts.getMainInputChannelSet();
    return inSet.isDisabled() || inSet == juce::AudioChannelSet::stereo();
}

void StyleScalerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& midiMessages) {
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0) return;

    // 1. Capture incoming audio BEFORE clearing output buffer so preview synth does not feed back
    if (capture_ring_.isCapturing()) {
        const auto inBus = getBusBuffer(buffer, true, 0);
        if (inBus.getNumChannels() > 0) {
            capture_ring_.pushBlock(inBus.getArrayOfReadPointers(), inBus.getNumChannels(), numSamples);
        }
    }

    // 2. Clear audio output buffer for preview synth
    buffer.clear();

    const bool enChords = p_enable_chords_ != nullptr && p_enable_chords_->load(std::memory_order_relaxed) >= 0.5f;
    const bool enMelody = p_enable_melody_ != nullptr && p_enable_melody_->load(std::memory_order_relaxed) >= 0.5f;
    const bool enBass = p_enable_bass_ != nullptr && p_enable_bass_->load(std::memory_order_relaxed) >= 0.5f;
    const float gain = p_preview_gain_ != nullptr ? p_preview_gain_->load(std::memory_order_relaxed) : 0.75f;

    // 3. Emit sample-accurate Ch 1/2/3 MIDI + pad/keyboard auditions
    sequencer_.processBlock(numSamples, getPlayHead(), pattern_slot_, midiMessages, enChords, enMelody, enBass);

    // 4. Render built-in zero-allocation preview synth
    preview_synth_.renderBlock(buffer, midiMessages, gain, enChords, enMelody, enBass);
}

void StyleScalerAudioProcessor::parameterChanged(const juce::String& parameterID, float) {
    if (suppress_param_regen_) return;
    const bool rebuildProg = (parameterID == ParamIDs::harmonyBlend ||
                              parameterID == ParamIDs::rhythmBlend ||
                              parameterID == ParamIDs::tempoScale ||
                              parameterID == ParamIDs::keyRoot ||
                              parameterID == ParamIDs::keyMode ||
                              parameterID == ParamIDs::progressionBars);
    scheduleBackgroundRegen(rebuildProg, true);
}

void StyleScalerAudioProcessor::handleAsyncUpdate() {
    const uint64_t ticket = regen_ticket_.load(std::memory_order_acquire);
    const bool rebuildProg = pending_rebuild_prog_.exchange(false, std::memory_order_acq_rel);
    const bool keepLocks = pending_keep_locks_.load(std::memory_order_acquire);

    regen_pool_.addJob([this, rebuildProg, keepLocks, ticket]() {
        performRebuildSync(rebuildProg, keepLocks, ticket);
    });
}

void StyleScalerAudioProcessor::scheduleBackgroundRegen(bool rebuildProgression, bool keepLocks) {
    regen_ticket_.fetch_add(1, std::memory_order_release);
    if (rebuildProgression) {
        pending_rebuild_prog_.store(true, std::memory_order_release);
    }
    pending_keep_locks_.store(keepLocks, std::memory_order_release);
    triggerAsyncUpdate();
}

BlendParams StyleScalerAudioProcessor::readBlendParamsLocked() const {
    BlendParams bp;
    bp.harmony = apvts_.getRawParameterValue(ParamIDs::harmonyBlend)->load();
    bp.rhythm = apvts_.getRawParameterValue(ParamIDs::rhythmBlend)->load();
    bp.voicing = apvts_.getRawParameterValue(ParamIDs::voicingBlend)->load();
    bp.melody = apvts_.getRawParameterValue(ParamIDs::melodyBlend)->load();

    const int scIdx = static_cast<int>(std::llround(apvts_.getRawParameterValue(ParamIDs::tempoScale)->load()));
    bp.tempo_scale = (scIdx == 0) ? TempoScale::Half : (scIdx == 2 ? TempoScale::Double : TempoScale::Normal);
    bp.key_override_pc = static_cast<int>(std::llround(apvts_.getRawParameterValue(ParamIDs::keyRoot)->load()));
    bp.mode_override = static_cast<Mode>(std::clamp(
        static_cast<int>(std::llround(apvts_.getRawParameterValue(ParamIDs::keyMode)->load())), 0, 7));
    return bp;
}

PerformParams StyleScalerAudioProcessor::readPerformParamsLocked() const {
    PerformParams pp;
    pp.groove_sync = apvts_.getRawParameterValue(ParamIDs::grooveSync)->load() >= 0.5f;
    const int scIdx = static_cast<int>(std::llround(apvts_.getRawParameterValue(ParamIDs::tempoScale)->load()));
    pp.tempo_scale = (scIdx == 0) ? TempoScale::Half : (scIdx == 2 ? TempoScale::Double : TempoScale::Normal);
    pp.voicing_spread = apvts_.getRawParameterValue(ParamIDs::voicingSpread)->load();
    pp.melody_density = apvts_.getRawParameterValue(ParamIDs::melodyDensity)->load();
    pp.humanize = apvts_.getRawParameterValue(ParamIDs::humanize)->load();
    pp.enable_chords = apvts_.getRawParameterValue(ParamIDs::enableChords)->load() >= 0.5f;
    pp.enable_melody = apvts_.getRawParameterValue(ParamIDs::enableMelody)->load() >= 0.5f;
    pp.enable_bass = apvts_.getRawParameterValue(ParamIDs::enableBass)->load() >= 0.5f;
    return pp;
}

int StyleScalerAudioProcessor::readProgressionBarsLocked() const {
    const int idx = static_cast<int>(std::llround(apvts_.getRawParameterValue(ParamIDs::progressionBars)->load()));
    if (idx == 0) return 4;
    if (idx == 2) return 16;
    return 8;
}

void StyleScalerAudioProcessor::syncKeyParamsFromProfile(const StyleProfile& prof) {
    suppress_param_regen_ = true;
    if (auto* pRoot = apvts_.getParameter(ParamIDs::keyRoot)) {
        pRoot->setValueNotifyingHost(pRoot->convertTo0to1(static_cast<float>(std::clamp(prof.harmony.tonic_pc, 0, 11))));
    }
    if (auto* pMode = apvts_.getParameter(ParamIDs::keyMode)) {
        pMode->setValueNotifyingHost(pMode->convertTo0to1(static_cast<float>(std::clamp(static_cast<int>(prof.harmony.mode), 0, 7))));
    }
    suppress_param_regen_ = false;
}

void StyleScalerAudioProcessor::performRebuildSync(bool rebuildProgression, bool keepLocks, uint64_t ticket) {
    if (ticket != regen_ticket_.load(std::memory_order_acquire)) {
        return;
    }

    StyleProfile a, b;
    BlendParams bp;
    PerformParams pp;
    std::vector<ProgressionSlot> existing;
    int bars = 8;
    uint64_t seed = 42ULL;

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        a = slot_a_;
        b = slot_b_;
        bp = readBlendParamsLocked();
        pp = readPerformParamsLocked();
        bars = readProgressionBarsLocked();
        seed = seed_;
        if (keepLocks) existing = progression_;
    }

    StyleProfile blended = StyleBlender::blend(a, b, bp);
    std::vector<ProgressionSlot> nextProg = existing;
    if (rebuildProgression || nextProg.empty()) {
        nextProg = ProgressionGenerator::generateProgression(blended, bars, seed, existing, bp.tempo_scale);
    }

    GeneratedPattern nextPat = PatternGenerator::generatePattern(blended, nextProg, pp, seed);

    auto buf = std::make_unique<PatternBuffer>();
    buf->loop_beats = nextPat.loop_length_beats;
    buf->events.reserve(nextPat.all_events.size());
    for (const auto& ev : nextPat.all_events) {
        SequencerEvent se;
        se.beat_pos = ev.onset_beats;
        se.duration_beats = ev.duration_beats;
        se.pitch = static_cast<uint8_t>(std::clamp(ev.pitch, 0, 127));
        se.velocity = static_cast<uint8_t>(std::clamp(ev.velocity, 1, 127));
        se.channel = static_cast<uint8_t>(std::clamp(ev.channel, 1, 3));
        buf->events.push_back(se);
    }
    std::sort(buf->events.begin(), buf->events.end(), [](const SequencerEvent& x, const SequencerEvent& y) {
        if (x.beat_pos != y.beat_pos) return x.beat_pos < y.beat_pos;
        return x.channel < y.channel;
    });

    if (ticket != regen_ticket_.load(std::memory_order_acquire)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        blended_ = std::move(blended);
        progression_ = std::move(nextProg);
        pattern_ = std::move(nextPat);
    }

    sequencer_.setInternalBpm(blended.default_bpm);
    pattern_slot_.publish(std::move(buf));

    if (onStateChangedForUI) {
        juce::MessageManager::callAsync([cb = onStateChangedForUI]() {
            if (cb) cb();
        });
    }
}

void StyleScalerAudioProcessor::regenerateNowSync(bool keepLocks) {
    const uint64_t t = regen_ticket_.fetch_add(1, std::memory_order_acq_rel) + 1;
    performRebuildSync(true, keepLocks, t);
}

StyleScalerAudioProcessor::EngineSnapshot StyleScalerAudioProcessor::getSnapshot() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    EngineSnapshot snap;
    snap.slot_a = slot_a_;
    snap.slot_b = slot_b_;
    snap.blended = blended_;
    snap.progression = progression_;
    snap.pattern = pattern_;
    snap.seed = seed_;
    snap.num_bars = readProgressionBarsLocked();
    const int scIdx = static_cast<int>(std::llround(apvts_.getRawParameterValue(ParamIDs::tempoScale)->load()));
    snap.tempo_scale = (scIdx == 0) ? TempoScale::Half : (scIdx == 2 ? TempoScale::Double : TempoScale::Normal);
    return snap;
}

std::vector<StyleProfile> StyleScalerAudioProcessor::getPresetLibrary() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return preset_library_;
}

int StyleScalerAudioProcessor::findPresetIndexById(const std::string& id) const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    for (size_t i = 0; i < preset_library_.size(); ++i) {
        if (preset_library_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

void StyleScalerAudioProcessor::selectSlotAPreset(int libraryIndex) {
    StyleProfile chosen;
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (libraryIndex < 0 || static_cast<size_t>(libraryIndex) >= preset_library_.size()) return;
        chosen = preset_library_[static_cast<size_t>(libraryIndex)];
        slot_a_ = chosen;
    }
    syncKeyParamsFromProfile(chosen);
    scheduleBackgroundRegen(true, true);
}

void StyleScalerAudioProcessor::selectSlotBPreset(int libraryIndex) {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (libraryIndex < 0 || static_cast<size_t>(libraryIndex) >= preset_library_.size()) return;
        slot_b_ = preset_library_[static_cast<size_t>(libraryIndex)];
    }
    scheduleBackgroundRegen(true, true);
}

void StyleScalerAudioProcessor::setSlotProfile(bool isSlotB, const StyleProfile& profile, bool addToLibrary) {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (isSlotB) slot_b_ = profile;
        else slot_a_ = profile;

        if (addToLibrary) {
            auto it = std::find_if(preset_library_.begin(), preset_library_.end(),
                                   [&](const StyleProfile& ex) { return ex.id == profile.id; });
            if (it != preset_library_.end()) *it = profile;
            else preset_library_.push_back(profile);
        }
    }
    if (!isSlotB) {
        syncKeyParamsFromProfile(profile);
    }
    scheduleBackgroundRegen(true, true);
}

void StyleScalerAudioProcessor::setSlotLocked(size_t slotIndex, bool locked) {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (slotIndex >= progression_.size()) return;
        progression_[slotIndex].locked = locked;
    }
    if (onStateChangedForUI) onStateChangedForUI();
}

void StyleScalerAudioProcessor::replaceSlotChord(size_t slotIndex,
                                                 const std::string& romanSymbol,
                                                 const RomanChord& chord) {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (slotIndex >= progression_.size()) return;
        progression_[slotIndex].roman_symbol = romanSymbol;
        progression_[slotIndex].chord = chord;
        progression_[slotIndex].locked = true;
    }
    scheduleBackgroundRegen(false, true);
}

void StyleScalerAudioProcessor::setSlotVoicing(size_t slotIndex,
                                               int octaveShift,
                                               int inversionOverride,
                                               double richnessOverride) {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (slotIndex >= progression_.size()) return;
        progression_[slotIndex].octave_shift = std::clamp(octaveShift, -2, 2);
        progression_[slotIndex].inversion_override = inversionOverride;
        progression_[slotIndex].richness_override = richnessOverride;
    }
    scheduleBackgroundRegen(false, true);
}

void StyleScalerAudioProcessor::appendOrReplaceSlotFromPad(const std::string& romanSymbol,
                                                           const RomanChord& chord,
                                                           int targetIndex) {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (progression_.empty()) return;
        size_t idx = 0;
        if (targetIndex >= 0 && static_cast<size_t>(targetIndex) < progression_.size()) {
            idx = static_cast<size_t>(targetIndex);
        } else {
            // Find first unlocked slot, else replace last
            idx = progression_.size() - 1;
            for (size_t i = 0; i < progression_.size(); ++i) {
                if (!progression_[i].locked) {
                    idx = i;
                    break;
                }
            }
        }
        progression_[idx].roman_symbol = romanSymbol;
        progression_[idx].chord = chord;
        progression_[idx].locked = true;
    }
    scheduleBackgroundRegen(false, true);
}

void StyleScalerAudioProcessor::clearAllLocks() {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        for (auto& s : progression_) s.locked = false;
    }
    if (onStateChangedForUI) onStateChangedForUI();
}

void StyleScalerAudioProcessor::regenerateProgression(bool keepLocks, bool newSeed) {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (newSeed && !seed_locked_) {
            std::random_device rd;
            seed_ = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(juce::Time::getMillisecondCounterHiRes());
            if (seed_ == 0) seed_ = 42ULL;
        }
        if (!keepLocks) {
            for (auto& s : progression_) s.locked = false;
        }
    }
    scheduleBackgroundRegen(true, keepLocks);
}

void StyleScalerAudioProcessor::regeneratePatternOnly() {
    scheduleBackgroundRegen(false, true);
}

uint64_t StyleScalerAudioProcessor::getSeed() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return seed_;
}

void StyleScalerAudioProcessor::setSeedLock(bool locked) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    seed_locked_ = locked;
}

bool StyleScalerAudioProcessor::isSeedLocked() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return seed_locked_;
}

void StyleScalerAudioProcessor::setInternalPlaying(bool playing) {
    sequencer_.setInternalPlaying(playing);
}

void StyleScalerAudioProcessor::auditionChordPress(const RomanChord& chord,
                                                   int octaveShift,
                                                   int inversion,
                                                   double richness) {
    auditionChordRelease();

    StyleProfile prof;
    double spread = 0.5;
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        prof = blended_;
        spread = apvts_.getRawParameterValue(ParamIDs::voicingSpread)->load();
    }

    const auto notes = VoicingEngine::voiceChord(
        chord, prof.harmony.tonic_pc, prof.harmony.mode, prof.voicing,
        {}, spread, octaveShift, inversion, richness);

    std::lock_guard<std::mutex> alk(audition_mutex_);
    held_audition_notes_ = notes;
    for (int p : notes) {
        sequencer_.enqueueAudition(AuditionEvent{1, static_cast<uint8_t>(std::clamp(p, 0, 127)), 96, true});
    }
}

void StyleScalerAudioProcessor::auditionChordRelease() {
    std::lock_guard<std::mutex> alk(audition_mutex_);
    for (int p : held_audition_notes_) {
        sequencer_.enqueueAudition(AuditionEvent{1, static_cast<uint8_t>(std::clamp(p, 0, 127)), 0, false});
    }
    held_audition_notes_.clear();
}

void StyleScalerAudioProcessor::auditionSingleNote(int channel1Based, int pitch, int velocity, bool noteOn) {
    sequencer_.enqueueAudition(AuditionEvent{
        static_cast<uint8_t>(std::clamp(channel1Based, 1, 3)),
        static_cast<uint8_t>(std::clamp(pitch, 0, 127)),
        static_cast<uint8_t>(std::clamp(velocity, 0, 127)),
        noteOn});
}

void StyleScalerAudioProcessor::analyzeDroppedFile(const juce::File& file,
                                                     TranscriptionService::TargetSlot targetSlot) {
    {
        std::lock_guard<std::mutex> slk(status_mutex_);
        tx_status_ = TranscriptionStatus{true, 0.02f, "Starting analysis...", ""};
    }
    transcription_service_.transcribeFileAsync(
        file, targetSlot,
        [this](float prog, std::string msg) {
            std::lock_guard<std::mutex> slk(status_mutex_);
            tx_status_.running = true;
            tx_status_.progress = prog;
            tx_status_.message = std::move(msg);
        },
        [this](TranscriptionService::Result res) {
            {
                std::lock_guard<std::mutex> slk(status_mutex_);
                tx_status_.running = false;
                tx_status_.progress = res.success ? 1.0f : 0.0f;
                tx_status_.backend_name = res.backend_name;
                tx_status_.message = res.success
                                         ? ("Loaded into " + std::string(res.target_slot == TranscriptionService::TargetSlot::SlotB ? "Slot B" : "Slot A") +
                                            " (" + res.backend_name + ", " + std::to_string(res.num_notes) + " notes)")
                                         : ("Error: " + res.error);
            }
            if (res.success) {
                setSlotProfile(res.target_slot == TranscriptionService::TargetSlot::SlotB, res.profile, true);
            }
        });
}

void StyleScalerAudioProcessor::startLiveCapture() {
    capture_ring_.startCapture();
}

void StyleScalerAudioProcessor::stopAndAnalyzeCapture(TranscriptionService::TargetSlot targetSlot) {
    auto captured = capture_ring_.extractSnapshot();
    if (captured.mono_samples.size() < static_cast<size_t>(captured.sample_rate * 0.25)) {
        std::lock_guard<std::mutex> slk(status_mutex_);
        tx_status_ = TranscriptionStatus{false, 0.0f, "Capture too short (< 0.25 s) — check DAW audio routing", ""};
        return;
    }

    {
        std::lock_guard<std::mutex> slk(status_mutex_);
        tx_status_ = TranscriptionStatus{true, 0.05f, "Transcribing captured DAW audio...", ""};
    }

    transcription_service_.transcribeAudioAsync(
        std::move(captured.mono_samples), captured.sample_rate, "Live DAW Capture", targetSlot,
        [this](float prog, std::string msg) {
            std::lock_guard<std::mutex> slk(status_mutex_);
            tx_status_.running = true;
            tx_status_.progress = prog;
            tx_status_.message = std::move(msg);
        },
        [this](TranscriptionService::Result res) {
            {
                std::lock_guard<std::mutex> slk(status_mutex_);
                tx_status_.running = false;
                tx_status_.progress = res.success ? 1.0f : 0.0f;
                tx_status_.backend_name = res.backend_name;
                tx_status_.message = res.success
                                         ? ("Captured -> " + std::string(res.target_slot == TranscriptionService::TargetSlot::SlotB ? "Slot B" : "Slot A") +
                                            " (" + res.backend_name + ", " + std::to_string(res.num_notes) + " notes)")
                                         : ("Error: " + res.error);
            }
            if (res.success) {
                setSlotProfile(res.target_slot == TranscriptionService::TargetSlot::SlotB, res.profile, true);
            }
        });
}

StyleScalerAudioProcessor::TranscriptionStatus StyleScalerAudioProcessor::getTranscriptionStatus() const {
    std::lock_guard<std::mutex> slk(status_mutex_);
    return tx_status_;
}

bool StyleScalerAudioProcessor::exportMidiToFile(const juce::File& destFile,
                                                 int channelFilter,
                                                 std::string& outError) const {
    const auto snap = getSnapshot();
    const double bpm = snap.blended.default_bpm;
    auto res = SmfWriter::writeFilteredType1(
        destFile.getFullPathName().toStdString(),
        bpm,
        snap.blended.time_sig_num,
        snap.blended.time_sig_den,
        snap.pattern.all_events,
        channelFilter);
    if (!res) {
        outError = res.error();
        return false;
    }
    return true;
}

juce::File StyleScalerAudioProcessor::exportTempMidiForDrag(int channelFilter) const {
    const juce::File tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("StyleScaler");
    tmpDir.createDirectory();
    const char* suffix = (channelFilter == 1) ? "Chords"
                       : (channelFilter == 2) ? "Melody"
                       : (channelFilter == 3) ? "Bass"
                                              : "Full";
    const juce::File outFile = tmpDir.getChildFile(juce::String("StyleScaler_") + suffix + ".mid");
    std::string err;
    exportMidiToFile(outFile, channelFilter, err);
    return outFile;
}

void StyleScalerAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto apvtsState = apvts_.copyState();
    auto rootXml = apvtsState.createXml();
    if (!rootXml) return;

    std::lock_guard<std::mutex> lk(state_mutex_);
    rootXml->setAttribute("slot_a_json", juce::String(toJson(slot_a_).dump()));
    rootXml->setAttribute("slot_b_json", juce::String(toJson(slot_b_).dump()));
    rootXml->setAttribute("seed", juce::String(std::to_string(seed_)));
    rootXml->setAttribute("seed_locked", seed_locked_);

    nlohmann::json progArr = nlohmann::json::array();
    for (const auto& s : progression_) {
        progArr.push_back({
            {"bar_index", s.bar_index},
            {"start_beat", s.start_beat},
            {"duration_beats", s.duration_beats},
            {"roman_symbol", s.roman_symbol},
            {"locked", s.locked},
            {"octave_shift", s.octave_shift},
            {"inversion_override", s.inversion_override},
            {"richness_override", s.richness_override}
        });
    }
    rootXml->setAttribute("progression_json", juce::String(progArr.dump()));

    copyXmlToBinary(*rootXml, destData);
}

void StyleScalerAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    std::unique_ptr<juce::XmlElement> rootXml(getXmlFromBinary(data, sizeInBytes));
    if (!rootXml) return;

    suppress_param_regen_ = true;
    if (rootXml->hasTagName(apvts_.state.getType())) {
        apvts_.replaceState(juce::ValueTree::fromXml(*rootXml));
    }
    suppress_param_regen_ = false;

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        const auto aStr = rootXml->getStringAttribute("slot_a_json").toStdString();
        if (!aStr.empty()) {
            if (auto pa = loadFromJsonString(aStr)) slot_a_ = *pa;
        }
        const auto bStr = rootXml->getStringAttribute("slot_b_json").toStdString();
        if (!bStr.empty()) {
            if (auto pb = loadFromJsonString(bStr)) slot_b_ = *pb;
        }
        const auto seedStr = rootXml->getStringAttribute("seed").toStdString();
        if (!seedStr.empty()) {
            try {
                seed_ = std::stoull(seedStr);
            } catch (...) {}
        }
        seed_locked_ = rootXml->getBoolAttribute("seed_locked", false);

        const auto pStr = rootXml->getStringAttribute("progression_json").toStdString();
        if (!pStr.empty()) {
            try {
                auto pj = nlohmann::json::parse(pStr);
                if (pj.is_array() && !pj.empty()) {
                    progression_.clear();
                    for (const auto& item : pj) {
                        ProgressionSlot s;
                        s.bar_index = item.value("bar_index", 0);
                        s.start_beat = item.value("start_beat", 0.0);
                        s.duration_beats = item.value("duration_beats", 4.0);
                        s.roman_symbol = item.value("roman_symbol", "i");
                        if (auto rc = parseRomanChord(s.roman_symbol)) s.chord = *rc;
                        s.locked = item.value("locked", false);
                        s.octave_shift = item.value("octave_shift", 0);
                        s.inversion_override = item.value("inversion_override", -1);
                        s.richness_override = item.value("richness_override", -1.0);
                        progression_.push_back(std::move(s));
                    }
                }
            } catch (...) {}
        }
    }

    const uint64_t t = regen_ticket_.fetch_add(1, std::memory_order_acq_rel) + 1;
    performRebuildSync(false, true, t);
}

juce::AudioProcessorEditor* StyleScalerAudioProcessor::createEditor() {
    return new StyleScalerAudioProcessorEditor(*this);
}

} // namespace style_scaler

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new style_scaler::StyleScalerAudioProcessor();
}
