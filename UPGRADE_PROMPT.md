# StyleScaler — Server Upgrade & Execution Prompt

> **How to use this prompt on the development server:**
> Stop any old running job, run `git pull origin main`, inspect the current working tree for any local changes or generated presets you want to keep, and execute **Tasks 1 through 7** in order. Run all builds and tests locally and verify a green GitHub Actions **`Windows VST3 & CTest CI`** build after pushing.

---

## Mission & Context

Upgrade **StyleScaler** (`whatisadigitalfeedbackcompressor/style-scaler`) so that:
1. **Windows CI builds cleanly** and produces downloadable `StyleScaler.vst3`, `StyleScaler.exe`, `style_cli.exe`, and `style_deconstruct.exe` artifacts.
2. **Real commercial/user songs (`.mp3`, `.wav`, `.flac`, `.ogg`, `.aif`, `.mid`)** deconstruct accurately through `style_deconstruct.exe` and the in-plugin drop zone—with **`.mp3` decoding enabled**, **non-empty Melody extraction**, **beat-phase ($\phi_0$) downbeat alignment**, **HPSS drum-transient rejection**, **smart drop/chorus windowing**, **chord smoothing / top-$K$ pruning**, **a live terminal progress bar with ETA**, and **incremental `--merge-existing` corpus accumulation**.
3. **Song Sections (`Intro / Chorus / Outro`)** are automatically extracted from full songs, preserved across incremental merges and cross-genre blends, and selectable in the Scaler-style UI.
4. **Musical Generation & Starter Presets** produce non-deterministic branching chord progressions, true 1-to-1 voice leading without low-end mud, drift-free 4-bar melodic phrases, and genre-authentic bass articulation.
5. **Scaler Performance UX** supports live **MIDI Bind** (`C2..D#3`), circular 30s DAW audio capture, DAW-synced `.mid` export BPM, and non-blocking DAW state recall.

---

## Task 1 — Fix Windows GitHub Actions CI (`Configure CMake` Step 5) & Enable `.mp3` Decoding

**Files:** `CMakeLists.txt`, `.github/workflows/ci.yml`

### 1A. Fix `muscriptor.cpp` MSVC `/arch:AVX2` Configure Failure
- **Root Cause:** `.github/workflows/ci.yml` configures with `-G "Visual Studio 17 2022" -A x64` (MSVC `cl.exe`) without `MUSCRIPTOR_NATIVE` or `MUSCRIPTOR_CPU_VARIANTS`. In `DamRsn/muscriptor.cpp/cpp/CMakeLists.txt`, that branch enables `GGML_F16C=ON`. Under MSVC `cl.exe`, `ggml` translates `GGML_F16C=ON` to `/arch:AVX2`, which trips `muscriptor.cpp`'s configure-time baseline check (`if(MUSCRIPTOR_CPU_FLAGS MATCHES "...|/arch:AVX2|...") message(FATAL_ERROR "ggml-cpu compiles with /arch:AVX2...")`).
- **Required Change:**
  1. In `CMakeLists.txt` (right before `FetchContent_Declare(muscriptor_cpp ...)` around line 217), add:
     ```cmake
     if(MSVC AND NOT MUSCRIPTOR_CPU_VARIANTS)
         set(MUSCRIPTOR_NATIVE ON CACHE BOOL "" FORCE)
     endif()
     ```
  2. In `.github/workflows/ci.yml`, pass `-DMUSCRIPTOR_NATIVE=ON` to the `cmake -S . -B build` configure command (or verify the `CMakeLists.txt` cache force cleanly bypasses the baseline check on `windows-latest` MSVC runners while keeping `ggml-cpu` optimized).

### 1B. Enable Native `.mp3` Decoding in JUCE 8
- **Root Cause:** JUCE 8 disables `MP3AudioFormat` inside `AudioFormatManager::registerBasicFormats()` unless `JUCE_USE_MP3AUDIOFORMAT=1` is defined. Currently `style_deconstruct.exe` and `StyleScaler.vst3` advertise `.mp3` support but reject `.mp3` files with `"Unsupported or unreadable audio format"`.
- **Required Change:**
  - In `CMakeLists.txt`, define `JUCE_USE_MP3AUDIOFORMAT=1` on `StyleScaler`, `style_deconstruct`, and `style_plugin_tests` (and propagate to `juce::juce_audio_formats` if needed so JUCE compiles `juce_MP3AudioFormat` into the audio formats module).
  - Verify `.mp3` files actually open via `juce::AudioFormatManager::registerBasicFormats()`.

---

## Task 2 — Live Progress Bar & Incremental Song Merging in `style_deconstruct.exe`

**Files:** `cli/deconstruct_main.cpp`, `core/include/style_scaler/StyleProfile.h`, `core/src/StyleProfile.cpp`, `core/include/style_scaler/MidiStyleAnalyzer.h`, `core/src/MidiStyleAnalyzer.cpp`, `tools/offline_analyzer/deconstruct_and_publish.py`

### 2A. Real-Time Terminal Progress Bar & Batch ETA
- **Problem:** `cli/deconstruct_main.cpp` currently calls `txService.deconstructFileBlocking(juce::File(f.string()), dopts)` without an `onProgress` callback, leaving the terminal frozen with no feedback during long neural transcriptions.
- **Required Change:**
  - In both the `BatchMode::AutoGenre` loop and the `JobGroup` loop in `cli/deconstruct_main.cpp`:
    - Record batch start time (`std::chrono::steady_clock::now()`), current track index `[i/N]`, and per-track start time.
    - Pass an `onProgress(float trackProg, std::string stage)` callback to `txService.deconstructFileBlocking(...)` that updates an in-place `\r` line (throttled to ~15–20 Hz or $\ge 1\%$ progress delta to avoid console overhead):
      ```text
      [3/24 | Total 11%] track_name.mp3 [██████████░░░░░░░░░░]  52% | Transcribing with MuScriptor... | Elapsed 00:38 | ETA 04:52
      ```
    - Ensure ASCII/UTF-8 safe rendering on Windows consoles (pad trailing spaces to overwrite shorter stage names cleanly).
    - Compute rolling batch ETA from elapsed seconds divided by fractional tracks completed (`(completedTracks + trackProg) / totalTracks`).
    - When a track finishes (or skips), clear the `\r` line and print a clean summary line with detected backend, note count, BPM, key/mode, genre + source, sections extracted, and per-track duration:
      ```text
      ✓ [3/24] track_name.mp3 | MuScriptor (CPU) | 1420 notes | 174.0 BPM | F Minor | [Drum & Bass via id3v2] | Sections: Intro/Chorus/Outro (14.2s)
      ```

### 2B. Incremental Corpus Merging (`--merge-existing` / `track_count`)
- **Problem:** Currently `style_deconstruct.exe` overwrites `presets/styles/<genre>.json` on each run. Users deleting processed audio files after pushing presets cannot add new songs to existing genre profiles without losing prior songs.
- **Required Change:**
  1. Add `int track_count = 1;` to `StyleProfile` in `StyleProfile.h` and serialize/deserialize `"track_count"` in `StyleProfile.cpp` (defaulting to `1` when missing in older JSONs, keeping `schema_version = 1`).
  2. Set `out.track_count` accurately in `MidiStyleAnalyzer::analyzeCorpus` (equal to the number of valid analyzed tracks).
  3. Add `static StyleProfile mergeProfiles(const StyleProfile& existing, const StyleProfile& incoming);` to `MidiStyleAnalyzer`:
     - Perform a **`track_count`-weighted merge** (`wA = existing.track_count`, `wB = incoming.track_count`) across:
       - `harmony` (weighted vocabulary weights, cadence weights, row transition counts re-normalized, weighted `chromaticism`)
       - `rhythm` (weighted 16-step `chord_onset_grid`, `melody_onset_grid`, `bass_onset_grid` normalized to max 1.0, plus weighted scalar groove fields)
       - `voicing` (weighted register bounds, spread, extension/drop2/inversion/voice-leading stats, clearance, min/max note bounds)
       - `melody` (weighted 25-bin `interval_histogram`, chord/tension ratios, density, repetition, arch, legato, register)
       - `tags` (deduplicated union) and `bpm_min` / `bpm_max` / `default_bpm`
       - `sections` (`"intro"`, `"chorus"`, `"outro"` `SectionDNA` merged with the same `track_count` weights)
     - Set `out.track_count = existing.track_count + incoming.track_count;` and call `normalizeProfileDistributions(out)`.
  4. Add `--merge-existing` (default **enabled** in `auto-genre` and `per-folder` modes) and `--no-merge` (force overwrite) flags to `cli/deconstruct_main.cpp` and `tools/offline_analyzer/deconstruct_and_publish.py`. When merging is enabled and `destJson` already exists on disk, load the existing `StyleProfile`, merge the newly analyzed batch profile into it, then write `destJson` and rebuild `catalog.json`.

---

## Task 3 — Real-Song Ingestion Fixes in `TranscriptionService` & `MidiStyleAnalyzer`

**Files:** `plugin/engine/TranscriptionService.cpp`, `core/src/MidiStyleAnalyzer.cpp`, `tools/offline_analyzer/analyze_styles.py`

### 3A. CRITICAL: Fix Empty `melodyNotes` on Every Transcribed Audio Song
- **Root Cause:**
  - `TranscriptionService.cpp` (`timedNotesToEvents`) and `MidiStyleAnalyzer.cpp` (`analyzeTimed`) assign `channel = 3` to bass notes (`program 32..39`) and `channel = 1` to everything else—never `channel = 2`.
  - Then `MidiStyleAnalyzer::analyze()` checks `hasExplicitMultiChannel = chans.size() >= 2`. Because `{1, 3}` has size `2`, `hasExplicitMultiChannel` evaluates to **`true`**, which executes `if (n.channel == 2) melodyNotes.push_back(n)` and leaves **`melodyNotes` 100% empty** on every transcribed audio track while polluting `chordNotes` with lead/vocal notes!
- **Required Fix:**
  1. In `timedNotesToEvents()` (`TranscriptionService.cpp`) and `analyzeTimed()` (`MidiStyleAnalyzer.cpp`), map GM solo/lead/reed/pipe/vocal/pluck synth programs (`(program >= 56 && program <= 87) || (program >= 104 && program <= 111)`) to `channel = 2`, bass (`32..39`) to `channel = 3`, and chordal/key/guitar/ensemble programs (`0..31`, `40..55`, `88..103`) to `channel = 1`.
  2. In `MidiStyleAnalyzer::analyze()`, do **not** trust `chans.size() >= 2` blindly to skip stream separation:
     - Only treat the input as having an explicit separated melody track when `channel == 2` actually contains meaningful notes (e.g., `melodyChannelCount >= 4` and at least `5%` of pitched non-bass notes).
     - Whenever `channel == 2` is empty/sparse (typical for single-program MIDI or transcribed audio), keep any identified bass (`channel == 3` or `< 48`) separated and run a **skyline top-voice + monophonic contour separator** over the remaining notes:
       - Group simultaneous attacks/sustaining windows; identify monophonic or upper-register (`pitch >= 60`) skyline notes that move melodically (not part of simultaneous $\ge 3$-note block chord stacks unless clearly standing out above a wide interval gap $\ge 5$ semitones), placing the top melodic line into `melodyNotes` and harmonic support into `chordNotes`.

### 3B. Beat-Phase ($\phi_0$) & Downbeat Alignment + BPM Octave Disambiguation
- **Root Cause:** `analyzeTimed` and `timedNotesToEvents` compute `onset_beats = onset_sec * (bpm / 60.0)` assuming `t = 0.000s` is Bar 1 Beat 1. Encoder padding, silence, or pickup notes shift the entire 16-step rhythm grid and misalign chord windows.
- **Required Fix:**
  1. Add a beat-phase estimator in `MidiStyleAnalyzer`: given detected `bpm` ($\text{spb} = 60 / \text{bpm}$), search $\phi_0 \in [0, 4 \cdot \text{spb})$ in steps of $\text{spb} / 16$ (64 candidate phases across a 4/4 bar) to maximize:
     - Quarter-note beat alignment across all unique note onsets, **plus**
     - Strong bar-1 downbeat bonus where bass notes (`pitch < 50` or `channel == 3`) and multi-note chord clusters land on $\text{phase} \equiv 0 \pmod{4 \cdot \text{spb}}$.
  2. Subtract $\phi_0$ (or shift forward by `4*spb - phi_0` mod `4*spb` if needed to avoid clipping early pickup notes, then keep `onset_beats >= 0`) so Bar 1 Beat 1 consistently aligns with `step16 == 0`.
  3. **BPM Octave Disambiguation:** When `bpm` falls in the ambiguous octave zone (`70..98` vs `140..196`):
     - If `options.genre` or `options.tags` indicate Drum & Bass / Jungle / Neurofunk / Liquid DnB and `bpm < 115`, double `bpm` (`bpm *= 2.0`).
     - If `options.genre` or `options.tags` indicate 90s Hip-Hop / Boom-Bap / Lo-Fi Hip-Hop and `bpm > 142`, halve `bpm` (`bpm *= 0.5`).

### 3C. Smart Harmonic Window Finder & Spectral HPSS Drum Rejection
- **File:** `plugin/engine/TranscriptionService.cpp`
- **Required Fix:**
  1. In `decodeAudioFileTo16kMono(file, outError, maxSeconds)`: when `maxSeconds > 0` and track duration exceeds `maxSeconds * 1.25` (e.g., 45s cap inside the plugin UI on a 4-minute song):
     - Scan the full audio stream in lightweight 2-second blocks computing RMS energy and spectral flatness / zero-crossing ratio (distinguishing pitched/harmonic mid-band sections from silent or pure drum/FX intros).
     - Pick the starting offset of the contiguous `maxSeconds` window with the highest tonal energy score, and read/resample that segment instead of `0..maxSeconds`.
  2. In `SpectralFallbackTranscriber::transcribe`:
     - Maintain a 5-frame rolling magnitude buffer and compute a **5-frame temporal median** per bin (suppressing short broadband percussive hits like kicks/snares/hi-hats via harmonic-percussive separation while preserving sustained tonal notes).
     - Suppress 2nd (`+12`), 3rd (`+19`), and 4th (`+24`) harmonic bleed from lower active pitches before selecting top chord/melody peaks.

### 3D. Chord Smoothing & Top-$K$ Vocabulary Pruning on Full Songs
- **Files:** `core/src/MidiStyleAnalyzer.cpp`, `tools/offline_analyzer/analyze_styles.py`
- **Required Fix:**
  1. **Window Smoothing:** After initial 2-beat (`windowBeats`) chord classification, run a smoothing pass that collapses isolated single-window blips (`endBeat - startBeat <= windowBeats`) into the stronger neighboring chord when flanked by identical chords or when the blip shares $\ge 2$ pitch classes with an adjacent longer chord.
  2. **Top-$K$ Pruning:** Keep at most the top $K = 12$ chords by total duration weight (covering the core harmonic vocabulary), remap/prune rare noise transitions, and row-normalize `transitions` so full 4-minute tracks yield clean 6–12 chord vocabularies instead of 40+ micro-chords.

---

## Task 4 — `Intro / Chorus / Outro` Song Section Extraction, Blending & UI Selector

**Files:** `core/include/style_scaler/Types.h`, `core/include/style_scaler/StyleProfile.h`, `core/src/StyleProfile.cpp`, `core/include/style_scaler/MidiStyleAnalyzer.h`, `core/src/MidiStyleAnalyzer.cpp`, `core/include/style_scaler/StyleBlender.h`, `core/src/StyleBlender.cpp`, `core/include/style_scaler/GenerationEngine.h`, `core/src/GenerationEngine.cpp`, `plugin/PluginProcessor.h`, `plugin/PluginProcessor.cpp`, `plugin/ui/TimelineComponent.h`, `plugin/ui/TimelineComponent.cpp`, `cli/main.cpp`

### 4A. Schema & Types (`Types.h`, `StyleProfile.h/.cpp`)
- Add `enum class SongSection { Intro = 0, Chorus = 1, Outro = 2 };` plus `songSectionName(SongSection)` (`"intro"`, `"chorus"`, `"outro"`) and `parseSongSection(std::string_view)`.
- Add `SectionDNA` to `StyleProfile.h`:
  ```cpp
  struct SectionDNA {
      HarmonyAxis harmony;
      RhythmAxis rhythm;
      VoicingAxis voicing;
      MelodyAxis melody;
      bool populated = false;

      bool operator==(const SectionDNA&) const = default;
  };
  ```
- Add `std::map<std::string, SectionDNA> sections;` (keys `"intro"`, `"chorus"`, `"outro"`) to `StyleProfile`.
- Serialize/deserialize `"sections"` in `StyleProfile.cpp` (optional on read so existing v1 JSONs load seamlessly; keep `schema_version = 1`), and normalize section distributions in `normalizeProfileDistributions()`.

### 4B. Automatic Song Section Segmentation (`MidiStyleAnalyzer`)
- In `MidiStyleAnalyzer::analyze()` (and Python `analyze_styles.py`):
  - Compute per-bar energy $E(b)$ for $b \in [0, \text{totalBars})$ from chord note count, bass activity, melody density, and average velocity.
  - **When `totalBars >= 12`** (real full songs / multi-section inputs):
    - **Intro bars:** Opening bars $[0, \text{introEnd})$ where $\text{introEnd} = \max(4, \lfloor 0.20 \cdot \text{totalBars} \rfloor)$ (clamped early if energy jumps above $0.75 \cdot E_{\max}$ after bar 4) → extract into `profile.sections["intro"]`.
    - **Outro bars:** Closing bars $[\text{outroStart}, \text{totalBars})$ where $\text{outroStart} = \text{totalBars} - \max(4, \lfloor 0.20 \cdot \text{totalBars} \rfloor)$ → extract into `profile.sections["outro"]`.
    - **Chorus / Drop bars:** Middle bars in $[\text{introEnd}, \text{outroStart})$ with $E(b) \ge \text{median}(E_{\text{mid}})$ (falling back to all middle bars if sparse) → extract into `profile.sections["chorus"]`.
    - Also ensure the **top-level main `StyleProfile` axes** are primarily weighted toward the tonal body (chorus/main body) rather than diluted by silent/drum-only intros!
  - **Fallback Section Derivation (`totalBars < 12` or missing `"sections"`):**
    - Provide a helper `deriveSectionDNA(const StyleProfile& prof, SongSection section)` that deterministically shapes:
      - **Intro:** Halved `harmonic_pacing` ($\times 0.55$), $+15\%$ `sustain_ratio`, sparser `bass_onset_grid` (downbeats emphasized) & `melody_onset_grid` ($\times 0.55$), narrower/higher atmospheric `voicing`, and boosted tonic/pedal chord weight.
      - **Chorus:** Full peak profile DNA (`harmony`, `rhythm`, `voicing`, `melody`).
      - **Outro:** Boosted tonic/cadential `cadence_weight`, $+20\%$ `sustain_ratio`, slightly reduced syncopation and density.

### 4C. Cross-Genre Section Blending & Generation (`StyleBlender` & `GenerationEngine`)
- Add `SongSection section = SongSection::Chorus;` to both `BlendParams` and `PerformParams`.
- In `StyleBlender::blend(slotA, slotB, params)`:
  - Resolve `SectionDNA secA = resolveSection(slotA, params.section)` and `SectionDNA secB = resolveSection(slotB, params.section)`.
  - Blend `secA` and `secB` across all 4 axes (`harmony`, `rhythm`, `voicing`, `melody`), while keeping `out.sections` populated appropriately if downstream callers inspect it.
- In `PatternGenerator::generatePattern`:
  - **`SongSection::Intro`:** Gradually ramp chord/bass/melody velocity and trigger probability across the loop (buildup arc across bars `0..N-1`).
  - **`SongSection::Chorus`:** Full drop/hook energy across all bars.
  - **`SongSection::Outro`:** Encourage cadential resolution on the final bar onto tonic (`i`/`I`), lengthen final chord sustain, and taper bass/melody density in the last 2 bars.

### 4D. Plugin APVTS, UI Selector & CLI
- Add `ParamIDs::songSection = "song_section"` in `PluginProcessor.h/.cpp` (`AudioParameterChoice` with `{"Intro", "Chorus", "Outro"}`, default `1` = `"Chorus"`).
- Wire `songSection` into `readBlendParamsLocked()`, `readPerformParamsLocked()`, and `parameterChanged()` (triggering progression + pattern rebuild when switched).
- In `TimelineComponent.h/.cpp`, add a 3-button segmented toggle **`[ Intro ] [ Chorus ] [ Outro ]`** in the Timeline header bar (next to the `4 / 8 / 16` bar count buttons) synced to `ParamIDs::songSection`.
- Add `--section intro|chorus|outro` to `cli/main.cpp` for offline testing/exporting sections.

---

## Task 5 — Musical Generation Engine & Starter Presets Overhaul

**Files:** `core/src/GenerationEngine.cpp`, `tools/offline_analyzer/build_starter_presets.py`, `presets/styles/*.json`, `presets/catalog.json`

1. **Fix Over-Collapsed Starter Preset Markov Matrices:**
   - Expand `tools/offline_analyzer/build_starter_presets.py` from 2 rigid 4-chord cycles per genre to **6–8 varied progressions and rhythmic grooves per genre**, and add **2 new crossover presets** (`neo_soul.json` and `melodic_house.json`).
   - Adjust Dirichlet transition smoothing in `analyze_styles.py` / `build_starter_presets.py` so top transitions fall in a healthy musical range (`35%–55%` primary, `15%–25%` secondary choices) and rhythm onset grids have nuanced velocity/probability gradients instead of `0.0 / 1.0`.
   - Rebuild all `presets/styles/*.json` and `presets/catalog.json`.
2. **True Bijective 1-to-1 Voice Leading & Low-Interval Mud Guard (`VoicingEngine::voiceChord`):**
   - Replace the one-way minimum loop in `computeVoiceLeadingDistance(a, b)` with sorted 1-to-1 voice mapping:
     $$d(a, b) = \frac{1}{M} \sum_{k=0}^{M-1} \left| a_{\lfloor k \cdot |a| / M \rfloor} - b_{\lfloor k \cdot |b| / M \rfloor} \right|$$
   - Add a **guide-tone resolution bonus** rewarding 0–2 semitone voice motion on the 3rd and 7th, and a **low-interval clarity penalty** penalizing adjacent chord voices below MIDI `60` separated by `< 4` semitones.
3. **4-Bar Phrase Arch & Zero-Drift Melody (`PatternGenerator::generatePattern`):**
   - Remove the linear drift bug at `GenerationEngine.cpp:760` (`contourBias = round((arch_contour - 0.5) * 4.0)` added cumulatively to `prevPitch`), which pins melodies to `melLo` or `melHi`.
   - Replace with a position-aware 4-bar phrase arch target ($\sin(\pi \cdot \text{barPos})$ scaled by `arch_contour`), `A - A' - B - Cadence` motif phrasing, automatic phrase-end breathing space at bars 2 and 4, and cadential resolution onto the chord root/3rd/5th.
4. **Genre-Aware Bass Articulation (`PatternGenerator::generatePattern`):**
   - Scale bass note duration from `profile.rhythm.sustain_ratio` (so DnB Reese/sub and Trap 808s sustain cleanly across steps while Hip-Hop plays tight plucked notes) and allow scale/chromatic pickup approach notes on the last onset before a chord change.

---

## Task 6 — Scaler-Style "MIDI Bind" & Real-Time DAW Polish

**Files:** `plugin/engine/MidiSequencer.h`, `plugin/engine/MidiSequencer.cpp`, `plugin/engine/AudioCaptureRingBuffer.h`, `plugin/PluginProcessor.cpp`

1. **Scaler MIDI Bind (Live Pad Triggering via Incoming MIDI):**
   - In `MidiSequencer::processBlock`, read incoming MIDI Note-On/Note-Off events on MIDI notes `36..51` (`C2..D#3`) before replacing `outMidi`, and trigger/release the corresponding pad/slot chord voicing in real time (zero heap allocation via preallocated slot/pad chord pitch arrays pushed through `LockFreePatternSlot` or `PatternBuffer`), updating active keyboard highlights.
2. **True Circular `AudioCaptureRingBuffer`:**
   - Update `AudioCaptureRingBuffer::pushBlock` to wrap modulo `buffer_.size()` once `written_samples_ >= cap` instead of stopping capture at 30s, and linearize the most recent `min(written, cap)` samples in `extractSnapshot()`.
3. **Host-Synced `.mid` Export BPM & Non-Blocking State Recall:**
   - In `PluginProcessor::exportMidiToFile`, write `isDawSync() && currentBpm() > 20.0 ? currentBpm() : snap.blended.default_bpm`.
   - In `PluginProcessor::setStateInformation`, call `scheduleBackgroundRegen(false, true)` instead of blocking the DAW audio/message thread with `performRebuildSync`.

---

## Task 7 — Verification Suite & GitHub CI Check

1. Regenerate starter presets and catalog:
   ```powershell
   python tools/offline_analyzer/build_starter_presets.py
   python tools/offline_analyzer/build_catalog.py
   ```
2. Add comprehensive tests in `tests/core_tests.cpp` and `tests/plugin_tests.cpp` verifying:
   - Audio-transcribed single-channel/multi-program `TimedNoteSec` input produces **non-empty `melody` events** and distinct `Intro`, `Chorus`, `Outro` `SectionDNA` profiles.
   - Beat-phase offset $\phi_0$ recovery on time-shifted songs (e.g., `+0.31s` silence pre-roll aligns Bar 1 Beat 1 back to grid step 0).
   - Incremental merging (`MidiStyleAnalyzer::mergeProfiles`) preserves weighted transitions and increments `track_count`.
   - Every bundled starter preset has non-degenerate Markov entropy (max single transition probability `< 0.70`).
   - Bijective voice leading avoids low-interval mud and 4-bar melody stays centered without pinning against `melLo`/`melHi`.
   - `AudioCaptureRingBuffer` circular wrap-around retains the newest 30s of samples cleanly.
   - `PresetCatalogSync` and `Intro/Chorus/Outro` parameter switching work end-to-end.
3. Configure, build all targets (with `JUCE_USE_MP3AUDIOFORMAT=1` and `STYLE_SCALER_ENABLE_MUSCRIPTOR=ON`), and pass `ctest`:
   ```powershell
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" -DSTYLE_SCALER_BUILD_PLUGIN=ON -DSTYLE_SCALER_ENABLE_MUSCRIPTOR=ON -DMUSCRIPTOR_NATIVE=ON
   cmake --build build --config Release --target style_core style_cli style_deconstruct style_tests style_plugin_tests StyleScaler_VST3 StyleScaler_Standalone --parallel
   ctest --test-dir build -C Release --output-on-failure
   ```
4. Test `style_deconstruct.exe` with the live progress bar and `--mode auto-genre --merge-existing` on sample `.mp3`/`.wav`/`.mid` inputs.
5. Delete `UPGRADE_PROMPT.md` once all tasks are complete (or mark completed), commit cleanly, push to `origin main`, and confirm the GitHub Actions **`Windows VST3 & CTest CI`** run passes **green (`success`)** with all artifacts uploaded (`StyleScaler.vst3`, `StyleScaler.exe`, `style_cli.exe`, `style_deconstruct.exe`).
