#!/usr/bin/env python3
"""
Synthesizes curated multi-song MIDI corpora for the 6 starter genres and runs
analyze_styles.py to build presets/styles/*.json while preserving any existing
real song-deconstructed presets unless --force is passed.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import tempfile
from typing import Sequence, Tuple

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


def _render_corpus_songs(
    out_dir: Path,
    prefix: str,
    songs: Sequence[Tuple[float, Sequence[Tuple[Sequence[int], int]], Sequence[float], Sequence[int], str]],
) -> None:
    for idx, (bpm, prog, mel_steps, mel_pool, groove) in enumerate(songs):
        chord_evs = []
        mel_evs = []
        bass_evs = []
        for bar in range(8):
            ch_notes, bass_root = prog[bar % len(prog)]
            b0 = bar * 4.0
            b_pitch = (bass_root % 12) + 36
            if b_pitch > 44:
                b_pitch -= 12

            if groove == "boombap":
                ant = -0.41 if (bar % 2 == 1) else 0.0
                chord_evs.append((max(0.0, b0 + ant), 1.35, ch_notes, 92))
                chord_evs.append((b0 + 1.59, 0.75, ch_notes, 78))
                chord_evs.append((b0 + 2.59, 0.85, ch_notes, 84))
                bass_evs.append((b0 + 0.0, 0.85, b_pitch, 108))
                bass_evs.append((b0 + 1.59, 0.45, b_pitch + 7, 94))
                bass_evs.append((b0 + 2.5, 0.75, b_pitch, 102))
                bass_evs.append((b0 + 3.59, 0.35, b_pitch + 3, 90))
            elif groove == "trap":
                chord_evs.append((b0, 3.6, ch_notes, 86))
                if bar % 2 == 1:
                    chord_evs.append((b0 + 3.0, 0.85, ch_notes, 76))
                bass_evs.append((b0 + 0.0, 1.35, b_pitch, 118))
                bass_evs.append((b0 + 1.5, 0.85, b_pitch, 108))
                bass_evs.append((b0 + 2.75, 0.80, b_pitch + 12, 100))
                bass_evs.append((b0 + 3.5, 0.45, b_pitch, 96))
            elif groove == "liquid":
                ant = -0.5 if (bar % 2 == 1) else 0.0
                chord_evs.append((max(0.0, b0 + ant), 1.4, ch_notes, 95))
                chord_evs.append((b0 + 1.5, 1.25, ch_notes, 82))
                chord_evs.append((b0 + 3.0, 0.75, ch_notes, 86))
                bass_evs.append((b0 + 0.0, 1.35, b_pitch, 112))
                bass_evs.append((b0 + 1.5, 0.95, b_pitch, 102))
                bass_evs.append((b0 + 2.5, 0.80, b_pitch + 7, 96))
                bass_evs.append((b0 + 3.5, 0.45, b_pitch, 100))
            elif groove == "neuro":
                for st in (0.0, 0.75, 1.5, 2.5, 3.5):
                    chord_evs.append((b0 + st, 0.35, ch_notes, 108 if st == 0.0 else 96))
                bass_evs.append((b0 + 0.0, 0.72, b_pitch, 120))
                bass_evs.append((b0 + 0.75, 0.72, b_pitch, 110))
                bass_evs.append((b0 + 1.5, 0.85, b_pitch + 1, 114))
                bass_evs.append((b0 + 2.5, 0.72, b_pitch, 112))
                bass_evs.append((b0 + 3.5, 0.42, b_pitch + 6, 108))
            elif groove == "neosoul":
                chord_evs.append((b0 + 0.0, 1.5, ch_notes, 88))
                chord_evs.append((b0 + 1.62, 0.9, ch_notes, 76))
                chord_evs.append((b0 + 2.62, 1.1, ch_notes, 84))
                bass_evs.append((b0 + 0.0, 1.1, b_pitch, 104))
                bass_evs.append((b0 + 1.62, 0.6, b_pitch + 7, 92))
                bass_evs.append((b0 + 2.62, 0.75, b_pitch + 5, 96))
                bass_evs.append((b0 + 3.62, 0.35, b_pitch + 10, 88))
            else:  # melodic_house
                for st in (0.0, 0.75, 1.5, 2.25, 3.0):
                    chord_evs.append((b0 + st, 0.62, ch_notes, 94 if st == 0.0 else 82))
                for st in (0.5, 1.5, 2.5, 3.5):
                    bass_evs.append((b0 + st, 0.42, b_pitch, 108))
                bass_evs.append((b0 + 0.0, 0.35, b_pitch, 92))

            for m_idx, st in enumerate(mel_steps):
                p = mel_pool[(bar * 3 + m_idx) % len(mel_pool)]
                mel_evs.append((b0 + (st % 4.0), 0.48, p, 88))

        write_song_midi(out_dir / f"{prefix}_{idx}.mid", bpm, chord_evs, mel_evs, bass_evs)


def build_hiphop_corpus(out_dir: Path) -> None:
    cm9 = ([48, 51, 55, 58, 62], 48)
    fm9 = ([48, 53, 56, 60, 67], 53)
    abmaj7 = ([48, 51, 55, 56, 63], 44)
    bb7 = ([50, 56, 58, 62, 65], 58)
    gm7 = ([50, 55, 58, 62], 55)
    ebmaj7 = ([51, 55, 58, 62], 51)
    dm7b5 = ([50, 53, 56, 60], 50)
    g7 = ([47, 53, 55, 62], 55)
    mel = [72, 75, 74, 72, 70, 72, 75, 79, 77, 75, 74, 72]
    songs = [
        (90.0, [cm9, fm9, cm9, bb7], [0.0, 1.59, 2.59, 3.59], mel, "boombap"),
        (92.0, [cm9, abmaj7, cm9, g7], [0.0, 1.0, 1.59, 3.0], mel, "boombap"),
        (94.0, [cm9, gm7, fm9, cm9], [0.59, 1.59, 2.59, 3.59], mel, "boombap"),
        (88.0, [cm9, dm7b5, g7, cm9], [0.0, 1.59, 2.75], mel, "boombap"),
        (95.0, [cm9, fm9, bb7, g7], [0.0, 1.25, 2.59, 3.59], mel, "boombap"),
        (91.0, [cm9, ebmaj7, dm7b5, cm9], [0.0, 1.59, 3.0, 3.59], mel, "boombap"),
    ]
    _render_corpus_songs(out_dir, "hiphop", songs)


def build_trap_corpus(out_dir: Path) -> None:
    cm = ([48, 51, 55], 36)
    db = ([49, 53, 56], 37)
    abmaj7 = ([44, 48, 51, 55], 32)
    bb = ([46, 50, 53], 34)
    fm = ([48, 53, 56], 41)
    gm = ([43, 46, 50], 31)
    mel = [72, 75, 79, 75, 73, 72, 75, 79]
    songs = [
        (140.0, [cm, cm, abmaj7, bb], [0.0, 1.5, 3.0], mel, "trap"),
        (146.0, [cm, db, cm, gm], [0.0, 0.75, 1.5, 3.0], mel, "trap"),
        (138.0, [cm, fm, db, cm], [0.0, 1.5, 2.75], mel, "trap"),
        (144.0, [cm, abmaj7, fm, db], [0.0, 1.5, 3.5], mel, "trap"),
        (142.0, [cm, gm, abmaj7, db], [0.0, 1.0, 2.5, 3.5], mel, "trap"),
        (136.0, [cm, bb, abmaj7, gm], [0.5, 1.5, 3.0], mel, "trap"),
    ]
    _render_corpus_songs(out_dir, "trap", songs)


def build_liquid_dnb_corpus(out_dir: Path) -> None:
    cm9 = ([48, 58, 62, 63, 67], 36)
    abmaj9 = ([48, 56, 63, 67, 70], 32)
    fm11 = ([53, 60, 63, 67, 70], 41)
    bbadd9 = ([50, 58, 62, 65, 72], 34)
    ebmaj9 = ([51, 58, 62, 67, 70], 39)
    gm7 = ([50, 55, 58, 62, 67], 43)
    mel = [72, 74, 75, 79, 77, 75, 74, 72, 79, 82, 79, 77]
    songs = [
        (174.0, [cm9, abmaj9, fm11, bbadd9], [0.0, 0.5, 1.5, 2.0, 2.5, 3.5], mel, "liquid"),
        (172.0, [cm9, ebmaj9, fm11, gm7], [0.0, 0.75, 1.5, 2.5, 3.0, 3.5], mel, "liquid"),
        (175.0, [abmaj9, bbadd9, cm9, gm7], [0.0, 1.0, 1.5, 2.5, 3.5], mel, "liquid"),
        (173.0, [fm11, bbadd9, ebmaj9, abmaj9], [0.5, 1.5, 2.0, 3.0, 3.5], mel, "liquid"),
        (174.0, [cm9, fm11, abmaj9, gm7], [0.0, 0.5, 1.5, 2.5, 3.25], mel, "liquid"),
        (172.0, [ebmaj9, abmaj9, fm11, bbadd9], [0.0, 1.0, 2.0, 2.5, 3.5], mel, "liquid"),
    ]
    _render_corpus_songs(out_dir, "liquid", songs)


def build_neuro_dnb_corpus(out_dir: Path) -> None:
    cm = ([52, 55, 60, 67], 36)
    db = ([53, 56, 61, 68], 37)
    ddim7 = ([54, 57, 63, 69], 38)
    abmaj7 = ([56, 60, 63, 67], 32)
    gm = ([55, 58, 62, 67], 43)
    fsdim = ([54, 60, 63, 69], 42)
    mel = [72, 84, 73, 79, 72, 85, 78, 72]
    songs = [
        (174.0, [cm, db, ddim7, abmaj7], [0.0, 0.75, 1.5, 2.75, 3.5], mel, "neuro"),
        (175.0, [cm, db, gm, fsdim], [0.0, 0.75, 1.5, 2.5, 3.5], mel, "neuro"),
        (173.0, [cm, abmaj7, db, gm], [0.0, 1.0, 1.75, 2.5, 3.5], mel, "neuro"),
        (174.0, [cm, ddim7, gm, db], [0.25, 0.75, 1.5, 2.75, 3.5], mel, "neuro"),
        (176.0, [db, cm, fsdim, abmaj7], [0.0, 0.75, 1.75, 2.5, 3.25], mel, "neuro"),
        (172.0, [cm, gm, db, ddim7], [0.0, 1.25, 2.0, 2.75, 3.5], mel, "neuro"),
    ]
    _render_corpus_songs(out_dir, "neuro", songs)


def build_neo_soul_corpus(out_dir: Path) -> None:
    cm9 = ([48, 51, 55, 58, 62], 48)
    fm11 = ([53, 56, 60, 63, 67], 53)
    bb13 = ([50, 56, 58, 62, 67], 46)
    ebmaj9 = ([51, 55, 58, 62, 65], 51)
    abmaj9 = ([48, 55, 56, 60, 63], 44)
    dm7b5 = ([50, 53, 56, 60, 65], 50)
    mel = [72, 75, 77, 79, 82, 79, 77, 75, 74, 72]
    songs = [
        (88.0, [cm9, fm11, bb13, ebmaj9], [0.0, 0.62, 1.62, 2.62, 3.62], mel, "neosoul"),
        (86.0, [fm11, bb13, ebmaj9, abmaj9], [0.0, 1.0, 1.62, 2.62, 3.25], mel, "neosoul"),
        (92.0, [cm9, dm7b5, abmaj9, bb13], [0.62, 1.62, 2.62, 3.62], mel, "neosoul"),
        (85.0, [ebmaj9, abmaj9, fm11, cm9], [0.0, 0.62, 2.0, 3.0, 3.62], mel, "neosoul"),
        (90.0, [cm9, abmaj9, fm11, bb13], [0.0, 1.25, 1.62, 2.62, 3.5], mel, "neosoul"),
        (94.0, [fm11, cm9, dm7b5, ebmaj9], [0.0, 0.75, 1.62, 2.62, 3.62], mel, "neosoul"),
    ]
    _render_corpus_songs(out_dir, "neosoul", songs)


def build_melodic_house_corpus(out_dir: Path) -> None:
    cm = ([48, 55, 60, 63, 67], 36)
    abmaj7 = ([48, 56, 60, 63, 67], 44)
    ebmaj7 = ([51, 55, 58, 62, 67], 39)
    bb = ([50, 53, 58, 62, 65], 46)
    fm7 = ([48, 53, 56, 60, 63], 41)
    gm7 = ([50, 55, 58, 62, 67], 43)
    mel = [72, 74, 75, 79, 75, 74, 72, 79, 84, 82, 79, 75]
    songs = [
        (124.0, [cm, abmaj7, ebmaj7, bb], [0.0, 0.75, 1.5, 2.25, 3.0], mel, "melodic_house"),
        (122.0, [abmaj7, bb, cm, gm7], [0.0, 0.5, 1.5, 2.5, 3.0, 3.5], mel, "melodic_house"),
        (125.0, [cm, fm7, abmaj7, bb], [0.0, 0.75, 1.5, 2.5, 3.25], mel, "melodic_house"),
        (123.0, [ebmaj7, bb, fm7, cm], [0.5, 1.5, 2.25, 3.0, 3.5], mel, "melodic_house"),
        (126.0, [cm, gm7, abmaj7, ebmaj7], [0.0, 0.75, 1.5, 2.25, 3.5], mel, "melodic_house"),
        (124.0, [fm7, abmaj7, cm, bb], [0.0, 1.0, 1.75, 2.5, 3.25], mel, "melodic_house"),
    ]
    _render_corpus_songs(out_dir, "melodic_house", songs)


def is_real_song_deconstruction(path: Path) -> bool:
    if not path.exists():
        return False
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        src = str(data.get("source", ""))
        return "muscriptor" in src.lower() or "spectral" in src.lower() or src.startswith("offline_deconstruct")
    except Exception:
        return False


def main() -> None:
    parser = argparse.ArgumentParser(description="Build StyleScaler starter presets.")
    parser.add_argument("--force", action="store_true", help="Overwrite even real song-deconstructed profiles")
    args = parser.parse_args()

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
            (
                "neo_soul",
                "Neo-Soul",
                "R&B / Neo-Soul",
                "Dilla-swung extended 9th/11th/13th harmony, lush Rhodes voicings, expressive pickup melodies",
                build_neo_soul_corpus,
                82.0,
                98.0,
                88.0,
                48,
                "dorian",
            ),
            (
                "melodic_house",
                "Melodic House",
                "House / Melodic Techno",
                "Emotional minor-key arpeggiated harmony, driving off-beat bass, soaring 4-bar phrases",
                build_melodic_house_corpus,
                120.0,
                128.0,
                124.0,
                46,
                "minor",
            ),
        ]

        for pid, name, genre, desc, builder, bmin, bmax, bdef, clearance, mode in specs:
            out_file = PRESETS_DIR / f"{pid}.json"
            if not args.force and is_real_song_deconstruction(out_file):
                print(f"Preserving real song-deconstructed profile: {out_file.name}")
                continue
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
            out_file.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
            print(
                f"Generated {out_file.name}: vocab={len(profile['harmony']['vocabulary'])}, "
                f"swing={profile['rhythm']['swing']:.2f}, sync={profile['rhythm']['syncopation']:.2f}, "
                f"clearance={profile['voicing']['sub_bass_clearance_midi']}"
            )


if __name__ == "__main__":
    main()
