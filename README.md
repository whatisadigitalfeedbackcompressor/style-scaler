# StyleScaler

**StyleScaler** is a JUCE 8 / C++23 VST3 + Standalone plugin styled like Scaler 2/3 that combines **NeuralNote's audio-to-MIDI transcription (`DamRsn/muscriptor.cpp`)** with a **4-axis factorized cross-genre chord, voicing, bass, and melody generator**.

Drop an audio track or MIDI file (or capture live DAW audio), extract a factorized `StyleProfile`, and cross-breed genres independently across four musical dimensions — for example, **90s Boom-Bap Hip-Hop harmony & voicing styled with Liquid DnB rhythm and sub-bass clearance**.

---

## Core Features

1. **Factorized `StyleProfile` (`schema_version: 1`)**
   - **`harmony`**: Roman-numeral chord vocabulary, row-normalized Markov transition matrix, key/mode, chromaticism.
   - **`rhythm`**: 16th-note bar-relative onset grids for Chords, Melody, and Bass, plus syncopation, beat-4-and anticipation, swing, harmonic pacing, and sustain ratio.
   - **`voicing`**: Pitch register, spread, voice count, extension richness (triad/7th/9th/11th), drop-2 & inversion probabilities, voice-leading tightness, and strict **`sub_bass_clearance_midi`** floor.
   - **`melody`**: Signed semitone interval histogram (`-12..+12`), chord-tone & tension-tone targeting, motif density & repetition, arch contour, and register.
2. **Cross-Genre Style DNA (`Slot A` Foundation ↔ `Slot B` Style)**
   - Four independent per-axis blend sliders (`Harmony`, `Rhythm / Groove`, `Voicing / Space`, `Melody Phrasing`) plus **`0.5× / 1× / 2×`** tempo scaling.
3. **Scaler-Style 3-Tier UI & Real-Time Engine**
   - **Top Tier**: Audio/MIDI drag-and-drop & live DAW audio capture via `AudioCaptureRingBuffer` + `muscriptor.cpp` (with automatic spectral fallback when no `.gguf` is downloaded yet), Dual-Slot Style DNA sliders, Key/Scale & Seed controls.
   - **Middle Tier**: 16 playable chord pads with real-time probability highlights for likely next chords and drag-to-timeline assignment.
   - **Bottom Tier**: Lockable 4/8/16-bar progression timeline (per-slot `Lock`, `Sub`, `Voi`), Perform strip, interactive 6-octave keyboard (`C1–C7`) showing live channel notes & sub-bass floor line, built-in zero-allocation preview synth, and drag-and-drop MIDI export (`Chords`, `Melody`, `Bass`, `Full`).
   - **DAW Multi-Channel MIDI Routing**:
     - **Channel 1**: Chords (strictly above `sub_bass_clearance_midi`)
     - **Channel 2**: Melody
     - **Channel 3**: Bass (cleanly below `sub_bass_clearance_midi`)

---

## Building from Source (Windows / MSVC 2022 / CMake 3.24+)

### Standard Standalone Build
CMake fetches `nlohmann/json`, `Catch2`, `JUCE 8.0.15`, and `DamRsn/muscriptor.cpp` (`ggml`) automatically:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
cmake --build build --config Release --target style_core style_cli style_deconstruct style_tests style_plugin_tests StyleScaler_VST3 StyleScaler_Standalone --parallel
ctest --test-dir build -C Release --output-on-failure
```

To reuse an existing local JUCE 8 checkout:
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_DIR=C:/path/to/JUCE
```

Built plugin & tool artifacts appear in:
- `build/StyleScaler_artefacts/Release/VST3/StyleScaler.vst3`
- `build/StyleScaler_artefacts/Release/Standalone/StyleScaler.exe`
- `build/Release/style_cli.exe`
- `build/style_deconstruct_artefacts/Release/style_deconstruct.exe`

---

## Offline Song Deconstruction → GitHub Catalog → In-Plugin Sync

You can analyze entire folders of `.mp3`, `.wav`, `.flac`, `.ogg`, `.aif`, or `.mid` reference tracks offline (using NeuralNote `muscriptor.cpp` or fast spectral fallback), publish them into `presets/catalog.json`, and pull updates inside the plugin with one click:

1. **Deconstruct songs offline & rebuild `presets/catalog.json`:**
```powershell
# First-time setup: optionally download MuScriptor small GGUF (~209 MB) once and deconstruct a folder of genre subfolders
.\dist\style_deconstruct.exe `
  --input C:\Music\ReferenceFolders `
  --mode per-folder `
  --out-dir presets/styles `
  --catalog presets/catalog.json `
  --download-model
```
Or use the Python orchestrator (which runs `style_deconstruct.exe`, updates `presets/catalog.json` with SHA-256 digests, and optionally commits & pushes to GitHub with `--push`):
```powershell
.\tools\offline_analyzer\.venv\Scripts\python tools/offline_analyzer/deconstruct_and_publish.py `
  --input C:\Music\ReferenceFolders `
  --mode per-folder `
  --push
```
*(Pass `--spectral` to skip downloading GGUF weights and run fast polyphonic spectral analysis instead.)*

2. **Sync inside the StyleScaler VST3 / Standalone plugin:**
   - Click **Sync GitHub** in the top-right Key/Seed panel (or click **URL** to point at your own GitHub raw `catalog.json` URL or local shared folder path).
   - StyleScaler fetches `catalog.json`, SHA-256-verifies any new or updated presets into `%APPDATA%/StyleScaler/presets/`, and refreshes **Slot A** and **Slot B** dropdowns immediately without restarting your DAW.

---

## CLI Offline Exporter (`style_cli`)

Generate multi-track Type-1 `.mid` files directly from the command line to audition cross-genre blends in any DAW:

```powershell
# 90s Boom-Bap Hip-Hop harmony styled with Liquid DnB rhythm (2x double-time @ 174 BPM)
.\build\Release\style_cli.exe `
  --presets-dir presets/styles `
  --slot-a 90s_hiphop `
  --slot-b liquid_dnb `
  --harmony 0.15 --rhythm 0.85 --voicing 0.65 --melody 0.70 `
  --scale 2x --bpm 174 --bars 8 `
  --out build/hiphop_styled_like_dnb.mid
```

Analyze any existing `.mid` file into a `StyleProfile` JSON:
```powershell
.\build\Release\style_cli.exe --analyze reference.mid --out-profile my_style.json --id my_style --name "My Reference" --genre Custom
```

---

## Python Offline Batch Analyzer (`tools/offline_analyzer/`)

Extract `StyleProfile` JSON presets from folders of `.mid` or `.wav` reference tracks:

```powershell
python -m venv tools/offline_analyzer/.venv
.\tools\offline_analyzer\.venv\Scripts\pip install -r tools/offline_analyzer/requirements.txt

# Rebuild the 4 bundled starter presets (90s_hiphop, trap, liquid_dnb, neuro_dnb)
.\tools\offline_analyzer\.venv\Scripts\python tools/offline_analyzer/build_starter_presets.py

# Analyze your own folder of reference tracks
.\tools\offline_analyzer\.venv\Scripts\python tools/offline_analyzer/analyze_styles.py `
  --input path/to/midi_or_wav_folder `
  --out presets/styles/custom_style.json `
  --id custom_style --name "Custom Style" --genre Electronic
```

---

## NeuralNote `muscriptor.cpp` Model Weights & Licensing Notice

- **In-plugin usage**: Click **Get MuScriptor** in the top-left Detect panel to download `muscriptor-small-f16.gguf` (~209 MB, SHA-256 verified from `DamRsn/muscriptor-gguf`) into `%APPDATA%/StyleScaler/models/`, or click **Load GGUF** to point to an existing `.gguf` checkpoint (`small` / `medium` / `large`).
- **Fallback**: When no GGUF checkpoint is present, StyleScaler automatically uses its built-in polyphonic `SpectralFallbackTranscriber` so audio drag-and-drop and live DAW capture always work immediately.
- **Licensing**: Upstream MuScriptor GGUF model weights are distributed separately under **CC BY-NC 4.0** (non-commercial) and are **not** bundled in this repository.


## Automatic Genre Detection & Genre Database

StyleScaler extracts embedded track metadata natively across both the **VST3 / Standalone plugin** and the **offline song deconstructor (`style_deconstruct` / `deconstruct_and_publish.py`)**:

- **Supported container & tag formats (zero external dependencies):**
  - MP3: ID3v2.2 / v2.3 / v2.4 (`TCON`, `TIT2`, `TPE1`, `TALB`, `TBPM`, `TKEY`, `TXXX`, `COMM`) and ID3v1 footers (including numeric `(127)` genre codes).
  - FLAC & Ogg/Opus: `VORBIS_COMMENT` / `OpusTags` (`GENRE`, `STYLE`, `SUBGENRE`, `BPM`, `INITIALKEY`, `ARTIST`, `ALBUM`).
  - WAV: `RIFF LIST/INFO` (`IGNR`, `INAM`, `IART`, `IPRD`, `IBPM`/`ITMP`, `IKEY`) and embedded `id3 ` chunks.
  - AIFF: `NAME`, `AUTH`, `ANNO`, and `ID3 ` chunks.
  - MP4 / M4A: `ilst` atoms (`©gen`, `gnre`, `©nam`, `©ART`, `©alb`, `tmpo`, freeform keys).
  - MIDI: SMF meta text (`Genre:`, `Style:`), Tempo (`0x51`), and Key Signature (`0x59`).
- **Fallback chain & normalization:** Raw tags map through canonical genre aliases (e.g., `Boom Bap` -> `90s Hip-Hop`, `Neurofunk` -> `Neuro DnB`, `UKG` -> `UK Garage`). If a track has no genre tag, StyleScaler checks its enclosing folder/filename keywords (`path`) and finally infers a musical heuristic (`heuristic`) from tempo, syncopation, bass clearance, and chromaticism.
- **Auto-Genre Corpus Mode & Catalog Index:**
  - `style_deconstruct --mode auto-genre --input <songs_dir> --out-dir presets/styles --catalog presets/catalog.json` pools multiple songs automatically by detected genre into clean `<genre>.json` profiles.
  - `presets/catalog.json` (and the plugin's local `%APPDATA%/StyleScaler/presets/catalog.json` database) maintains a top-level `"genres"` index plus optional `tags`, `genre_source`, and `artist` fields.
  - Dropped songs analyzed inside the plugin are auto-tagged, persisted into the local preset database, and filterable via the **Genre Filter** dropdown and genre-grouped **Slot A / Slot B** selectors.
