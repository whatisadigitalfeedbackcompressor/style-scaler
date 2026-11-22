#!/usr/bin/env python3
"""
Synthesizes curated multi-song MIDI corpora for the 4 starter genres and runs
analyze_styles.py to build presets/styles/{90s_hiphop,trap,liquid_dnb,neuro_dnb}.json.
"""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
from typing import List, Sequence, Tuple

import mido

from analyze_styles import analyze_corpus, collect_inputs


ROOT = Path(__file__).resolve().parents[2]
PRESETS_DIR = ROOT / "presets" / "styles"
TPB = 480


def write_song_midi(
    path: Path,
    bpm: float,
    chord_events: Sequence[Tuple[float, float, Sequence[int], int]],
    melody_events: Sequence[Tuple[float, float, int, int]],
    bass_events: Sequence[Tuple[float, float, int, int]],
) -> None:
    mid = mido.MidiFile(ticks_per_beat=TPB, type=1)
    tempo_track = mido.MidiTrack()
    tempo_track.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(bpm), time=0))
    tempo_track.append(mido.MetaMessage("time_signature", numerator=4, denominator=4, time=0))
    tempo_track.append(mido.MetaMessage("end_of_track", time=0))
    mid.tracks.append(tempo_track)

    def build_track(name: str, ch: int, raw: Sequence[Tuple[float, float, int, int]]) -> mido.MidiTrack:
        trk = mido.MidiTrack()
        trk.append(mido.MetaMessage("track_name", name=name, time=0))
        msgs = []
        for onset_b, dur_b, pitch, vel in raw:
            on_t = max(0, int(round(onset_b * TPB)))
            off_t = max(on_t + 10, int(round((onset_b + dur_b) * TPB)))
            msgs.append((on_t, 1, "note_on", pitch, vel))
            msgs.append((off_t, 0, "note_off", pitch, 0))
        msgs.sort(key=lambda x: (x[0], x[1], x[3]))
        prev_t = 0
        for t, _ord, typ, p, v in msgs:
            trk.append(mido.Message(typ, channel=ch, note=int(p), velocity=int(v), time=t - prev_t))
            prev_t = t
        trk.append(mido.MetaMessage("end_of_track", time=0))
        return trk

    flat_chords = [
        (on, dur, p, vel) for on, dur, ps, vel in chord_events for p in ps
    ]
    mid.tracks.append(build_track("Chords", 0, flat_chords))
    mid.tracks.append(build_track("Melody", 1, melody_events))
    mid.tracks.append(build_track("Bass", 2, bass_events))
    path.parent.mkdir(parents=True, exist_ok=True)
    mid.save(str(path))


def build_hiphop_corpus(out_dir: Path) -> None:
    # 90s Boom-Bap: Dorian/Minor min9, maj7, dom7, heavy swing (~0.59 beat), beat-4-and anticipations
    songs = [
        (
            90.0,
            # C Dorian: Cm9 -> Fm9 -> Bb13(dom7) -> Cm9
            [
                ([51, 55, 58, 62], 48),   # Cm9 (C excluded to bass, or root+3+7+9)
                ([48, 53, 56, 60, 67], 53), # Fm9 (F)
                ([50, 56, 58, 62, 65], 58), # Bb7/9 (Bb)
                ([48, 51, 55, 58, 62], 48), # Cm9
            ],
            [0.0, 1.59, 3.59, 4.0, 5.59, 6.75, 7.59],
            [72, 75, 74, 72, 70, 72, 75, 79, 77, 75, 74, 72],
        ),
        (
            94.0,
            # D Minor: Dm9 -> Bbmaj7 -> Em7b5 -> A7
            [
                ([50, 53, 57, 60, 64], 50), # Dm9
                ([46, 53, 57, 62, 65], 46), # Bbmaj7
                ([52, 55, 58, 62], 52),     # Em7b5
                ([49, 55, 57, 61, 64], 45), # A7
            ],
            [0.0, 1.59, 2.59, 4.0, 6.0, 7.59],
            [74, 77, 76, 74, 72, 74, 77, 81, 79, 77, 76, 73],
        ),
    ]

    for idx, (bpm, prog, mel_steps, mel_pool) in enumerate(songs):
        chord_evs = []
        mel_evs = []
        bass_evs = []
        for bar in range(8):
            ch_notes, bass_root = prog[bar % len(prog)]
            b0 = bar * 4.0
            # Anticipate bar 2 and bar 4 by 8th note (swung 3.59)
            ant = -0.41 if (bar % 2 == 1) else 0.0
            chord_evs.append((max(0.0, b0 + ant), 1.4, ch_notes, 92))
            chord_evs.append((b0 + 1.59, 0.75, ch_notes, 78))
            chord_evs.append((b0 + 2.59, 0.85, ch_notes, 84))
            # Bass boom-bap pattern
            b_pitch = (bass_root % 12) + 36
            if b_pitch > 44:
                b_pitch -= 12
            bass_evs.append((b0 + 0.0, 0.9, b_pitch, 108))
            bass_evs.append((b0 + 1.59, 0.45, b_pitch + 7, 94))
            bass_evs.append((b0 + 2.5, 0.8, b_pitch, 102))
            bass_evs.append((b0 + 3.59, 0.35, b_pitch + 3, 90))
            # Melodic jazz-pentatonic phrase
            for m_idx, st in enumerate(mel_steps):
                t = (bar // 2) * 8.0 + st if st >= 4.0 else b0 + st
                if st < 4.0 or (bar % 2 == 0):
                    p = mel_pool[(bar * 3 + m_idx) % len(mel_pool)]
                    mel_evs.append((b0 + (st % 4.0), 0.55, p, 88))
        write_song_midi(out_dir / f"hiphop_{idx}.mid", bpm, chord_evs, mel_evs, bass_evs)


def build_trap_corpus(out_dir: Path) -> None:
    # Modern Trap: 138-146 BPM, Phrygian / Aeolian dark i -> bVI -> bVII / i -> bII -> i
    songs = [
        (
            140.0,
            [
                ([48, 51, 55], 36),       # Cm (i)
                ([48, 51, 55], 36),
                ([44, 48, 51, 55], 32),   # Abmaj7 (bVI)
                ([46, 50, 53], 34),       # Bb (bVII)
            ],
            [72, 75, 79, 75, 73, 72, 75, 79],
        ),
        (
            146.0,
            [
                ([48, 51, 55], 36),       # Cm (i)
                ([49, 53, 56], 37),       # Db (bII Phrygian)
                ([48, 51, 55], 36),       # Cm (i)
                ([43, 46, 50], 31),       # Gm (v)
            ],
            [72, 73, 79, 75, 72, 73, 75, 72],
        ),
    ]

    for idx, (bpm, prog, mel_pool) in enumerate(songs):
        chord_evs = []
        mel_evs = []
        bass_evs = []
        for bar in range(8):
            ch_notes, bass_p = prog[bar % len(prog)]
            b0 = bar * 4.0
            # Sparse sustained dark pads
            chord_evs.append((b0, 3.6, ch_notes, 86))
            # Trap 808 hits (downbeat + dotted-8th + bar end)
            bass_evs.append((b0 + 0.0, 1.25, bass_p, 118))
            bass_evs.append((b0 + 1.5, 0.75, bass_p, 108))
            bass_evs.append((b0 + 2.75, 0.75, bass_p + 12, 100))
            bass_evs.append((b0 + 3.5, 0.4, bass_p, 96))
            # Sparse eerie bell top-line
            mel_evs.append((b0 + 0.0, 0.75, mel_pool[(bar * 2) % len(mel_pool)], 90))
            mel_evs.append((b0 + 1.5, 0.75, mel_pool[(bar * 2 + 1) % len(mel_pool)], 84))
            if bar % 2 == 1:
                mel_evs.append((b0 + 3.0, 0.5, mel_pool[(bar * 2 + 2) % len(mel_pool)], 80))
        write_song_midi(out_dir / f"trap_{idx}.mid", bpm, chord_evs, mel_evs, bass_evs)


def build_liquid_dnb_corpus(out_dir: Path) -> None:
    # Liquid DnB: 172-175 BPM, lush i9 -> bVImaj9 -> iv11 -> bVIIadd9, open voicings >= C3 (48)
    songs = [
        (
            174.0,
            [
                ([48, 58, 62, 63, 67], 36),   # Cm9 (open drop-2, >=48)
                ([48, 56, 63, 67, 70], 32),   # Abmaj9 (root Ab2 in bass, chord >=48)
                ([53, 60, 63, 67, 70], 41),   # Fm11 (iv11)
                ([50, 58, 62, 65, 72], 34),   # Bbadd9 (bVIIadd9)
            ],
            [72, 74, 75, 79, 77, 75, 74, 72, 79, 82, 79, 77],
        ),
        (
            172.0,
            [
                ([50, 60, 64, 65, 69], 38),   # Dm9
                ([50, 58, 65, 69, 72], 34),   # Bbmaj9
                ([48, 55, 62, 67, 72], 43),   # Gm11
                ([48, 57, 64, 69, 72], 45),   # Am7 / Cadd9 feel
            ],
            [74, 76, 77, 81, 79, 77, 76, 74, 81, 84, 81, 79],
        ),
    ]

    for idx, (bpm, prog, mel_pool) in enumerate(songs):
        chord_evs = []
        mel_evs = []
        bass_evs = []
        for bar in range(8):
            ch_notes, bass_p = prog[bar % len(prog)]
            b0 = bar * 4.0
            # Syncopated liquid stabs & pushes + lush sustain
            ant = -0.5 if (bar % 2 == 1) else 0.0
            chord_evs.append((max(0.0, b0 + ant), 1.4, ch_notes, 95))
            chord_evs.append((b0 + 1.5, 1.25, ch_notes, 82))
            chord_evs.append((b0 + 3.0, 0.75, ch_notes, 86))
            # Rolling liquid sub-bass (straight 16th syncopation)
            bass_evs.append((b0 + 0.0, 1.25, bass_p, 112))
            bass_evs.append((b0 + 1.5, 0.9, bass_p, 102))
            bass_evs.append((b0 + 2.5, 0.75, bass_p + 7, 96))
            bass_evs.append((b0 + 3.5, 0.45, bass_p, 100))
            # Lyrical stepwise melody
            for k, st in enumerate([0.0, 0.5, 1.5, 2.0, 2.5, 3.5]):
                p = mel_pool[(bar * 6 + k) % len(mel_pool)]
                mel_evs.append((b0 + st, 0.45 if k != 4 else 0.85, p, 92))
        write_song_midi(out_dir / f"liquid_{idx}.mid", bpm, chord_evs, mel_evs, bass_evs)


def build_neuro_dnb_corpus(out_dir: Path) -> None:
    # Neuro DnB: 174 BPM, Phrygian/Locrian chromatic stabs i -> bII -> dim7 -> bVI -> v, clearance >= E3 (52)
    songs = [
        (
            174.0,
            [
                ([52, 55, 60, 67], 36),   # Cm stab (>=52)
                ([53, 56, 61, 68], 37),   # Db (bII)
                ([54, 57, 63, 69], 38),   # Ddim7
                ([56, 60, 63, 67], 32),   # Abmaj7 (bVI)
            ],
            [72, 84, 73, 79, 72, 85, 78, 72],
        ),
        (
            175.0,
            [
                ([52, 55, 60], 36),       # Cm (i)
                ([53, 56, 61], 37),       # Db (bII)
                ([55, 58, 62], 43),       # Gm (v)
                ([54, 60, 63, 69], 38),   # F#dim7 / Ddim7 tension
            ],
            [72, 79, 73, 84, 75, 81, 73, 72],
        ),
    ]

    for idx, (bpm, prog, mel_pool) in enumerate(songs):
        chord_evs = []
        mel_evs = []
        bass_evs = []
        for bar in range(8):
            ch_notes, bass_p = prog[bar % len(prog)]
            b0 = bar * 4.0
            # Aggressive 16th-note neuro stabs (steps 0, 3, 6, 10, 14)
            for st in [0.0, 0.75, 1.5, 2.5, 3.5]:
                chord_evs.append((b0 + st, 0.35, ch_notes, 108 if st == 0.0 else 96))
            # Reese / neuro sub hits
            bass_evs.append((b0 + 0.0, 0.7, bass_p, 120))
            bass_evs.append((b0 + 0.75, 0.7, bass_p, 110))
            bass_evs.append((b0 + 1.5, 0.8, bass_p + 1, 114))
            bass_evs.append((b0 + 2.5, 0.7, bass_p, 112))
            bass_evs.append((b0 + 3.5, 0.4, bass_p + 6, 108))
            # Angular jagged lead stabs
            for k, st in enumerate([0.0, 0.75, 1.5, 2.75, 3.5]):
                mel_evs.append((b0 + st, 0.3, mel_pool[(bar * 5 + k) % len(mel_pool)], 102))
        write_song_midi(out_dir / f"neuro_{idx}.mid", bpm, chord_evs, mel_evs, bass_evs)


def main() -> None:
    PRESETS_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        specs = [
            (
                "90s_hiphop",
                "90s Boom-Bap Hip-Hop",
                "Hip-Hop",
                "Swung Rhodes/jazz minor 9th harmony, beat-4-and anticipations, warm mid voicings",
                build_hiphop_corpus,
                86.0,
                96.0,
                92.0,
                48,
                "dorian",
            ),
            (
                "trap",
                "Modern Dark Trap",
                "Trap",
                "Phrygian/Aeolian sparse dark pads, deep 808 sub patterns, eerie upper bells",
                build_trap_corpus,
                130.0,
                150.0,
                140.0,
                44,
                "phrygian",
            ),
            (
                "liquid_dnb",
                "Liquid Drum & Bass",
                "Drum & Bass",
                "Lush extended 9th/11th harmony, wide open voicings with C3 sub-bass clearance, lyrical melody",
                build_liquid_dnb_corpus,
                170.0,
                176.0,
                174.0,
                48,
                "minor",
            ),
            (
                "neuro_dnb",
                "Neurofunk DnB",
                "Drum & Bass",
                "Phrygian/chromatic 16th stabs, stark tense voicings with E3 sub-bass clearance, angular leads",
                build_neuro_dnb_corpus,
                172.0,
                176.0,
                174.0,
                52,
                "phrygian",
            ),
        ]

        for pid, name, genre, desc, builder, bmin, bmax, bdef, clearance, mode in specs:
            cdir = tmp_path / pid
            builder(cdir)
            files = collect_inputs([str(cdir)])
            profile = analyze_corpus(
                input_paths=files,
                profile_id=pid,
                name=name,
                genre=genre,
                description=desc,
                bpm_min=bmin,
                bpm_max=bmax,
                default_bpm=bdef,
                sub_bass_clearance_override=clearance,
                forced_mode=mode,
            )
            out_file = PRESETS_DIR / f"{pid}.json"
            out_file.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
            print(
                f"Generated {out_file.name}: vocab={len(profile['harmony']['vocabulary'])}, "
                f"swing={profile['rhythm']['swing']:.2f}, sync={profile['rhythm']['syncopation']:.2f}, "
                f"clearance={profile['voicing']['sub_bass_clearance_midi']}"
            )


if __name__ == "__main__":
    main()
