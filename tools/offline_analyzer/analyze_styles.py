#!/usr/bin/env python3
"""
Batch MIDI & WAV analyzer that extracts factorized StyleProfile v1 JSON files
for StyleScaler (harmony, rhythm, voicing, melody).
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
import json
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple
import wave

import mido
import numpy as np


MAJOR_SCALE_REF = [0, 2, 4, 5, 7, 9, 11]

MODE_TEMPLATES: Dict[str, Tuple[List[int], List[float]]] = {
    "minor": (
        [0, 2, 3, 5, 7, 8, 10],
        [6.33, 2.20, 3.52, 5.38, 2.20, 3.53, 2.20, 4.75, 4.00, 2.20, 3.50, 2.30],
    ),
    "major": (
        [0, 2, 4, 5, 7, 9, 11],
        [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88],
    ),
    "dorian": (
        [0, 2, 3, 5, 7, 9, 10],
        [6.20, 2.10, 3.50, 5.20, 2.10, 3.55, 2.20, 4.70, 2.25, 4.05, 3.45, 2.10],
    ),
    "phrygian": (
        [0, 1, 3, 5, 7, 8, 10],
        [6.25, 4.10, 2.10, 5.15, 2.15, 3.55, 2.15, 4.75, 3.90, 2.10, 3.45, 2.10],
    ),
    "mixolydian": (
        [0, 2, 4, 5, 7, 9, 10],
        [6.20, 2.15, 3.45, 2.20, 4.45, 3.85, 2.20, 5.05, 2.20, 3.55, 4.00, 2.15],
    ),
    "harmonic_minor": (
        [0, 2, 3, 5, 7, 8, 11],
        [6.25, 2.15, 3.45, 5.25, 2.15, 3.55, 2.15, 4.85, 3.90, 2.15, 2.20, 4.10],
    ),
    "locrian": (
        [0, 1, 3, 5, 6, 8, 10],
        [6.10, 4.05, 2.10, 4.95, 2.15, 3.55, 4.20, 2.15, 3.85, 2.10, 3.45, 2.10],
    ),
}

CHORD_TEMPLATES: List[Tuple[str, List[int]]] = [
    ("maj", [0, 4, 7]),
    ("min", [0, 3, 7]),
    ("maj7", [0, 4, 7, 11]),
    ("min7", [0, 3, 7, 10]),
    ("dom7", [0, 4, 7, 10]),
    ("maj9", [0, 4, 7, 11, 14]),
    ("min9", [0, 3, 7, 10, 14]),
    ("dom9", [0, 4, 7, 10, 14]),
    ("min11", [0, 3, 7, 10, 14, 17]),
    ("add9", [0, 4, 7, 14]),
    ("minadd9", [0, 3, 7, 14]),
    ("sus2", [0, 2, 7]),
    ("sus4", [0, 5, 7]),
    ("hdim7", [0, 3, 6, 10]),
    ("dim7", [0, 3, 6, 9]),
    ("dim", [0, 3, 6]),
]

MINOR_LIKE_QUALITIES = {
    "min", "dim", "min7", "hdim7", "dim7", "minmaj7", "min9", "min11", "minadd9", "min6"
}

QUALITY_SUFFIX = {
    "maj": "",
    "min": "",
    "dim": "dim",
    "aug": "aug",
    "sus2": "sus2",
    "sus4": "sus4",
    "maj7": "maj7",
    "min7": "7",
    "dom7": "7",
    "hdim7": "m7b5",
    "dim7": "dim7",
    "minmaj7": "mMaj7",
    "maj9": "maj9",
    "min9": "9",
    "dom9": "9",
    "min11": "11",
    "add9": "add9",
    "minadd9": "add9",
    "min6": "6",
    "maj6": "6",
    "power5": "5",
}


@dataclass
class NoteEvent:
    onset_beats: float
    duration_beats: float
    pitch: int
    velocity: int
    channel: int
    is_drum: bool = False
    program: int = 0


def format_roman(degree: int, accidental: int, quality: str) -> str:
    upper = ["I", "II", "III", "IV", "V", "VI", "VII"]
    lower = ["i", "ii", "iii", "iv", "v", "vi", "vii"]
    prefix = ("b" * (-accidental)) if accidental < 0 else ("#" * accidental)
    num = lower[degree - 1] if quality in MINOR_LIKE_QUALITIES else upper[degree - 1]
    return f"{prefix}{num}{QUALITY_SUFFIX[quality]}"


def read_midi_events(path: Path) -> Tuple[List[NoteEvent], float, int, int]:
    mid = mido.MidiFile(str(path))
    tpb = max(1, mid.ticks_per_beat)
    tempo = 500000  # 120 bpm
    ts_num, ts_den = 4, 4
    found_tempo = False
    found_ts = False

    events: List[NoteEvent] = []
    for track in mid.tracks:
        tick = 0
        programs = [0] * 16
        active: Dict[Tuple[int, int], List[Tuple[int, int, int]]] = defaultdict(list)
        for msg in track:
            tick += msg.time
            if msg.is_meta:
                if msg.type == "set_tempo" and not found_tempo:
                    tempo = msg.tempo
                    found_tempo = True
                elif msg.type == "time_signature" and not found_ts:
                    ts_num = msg.numerator
                    ts_den = msg.denominator
                    found_ts = True
                continue
            if msg.type == "program_change":
                programs[msg.channel] = msg.program
            elif msg.type == "note_on" and msg.velocity > 0:
                active[(msg.channel, msg.note)].append((tick, msg.velocity, programs[msg.channel]))
            elif msg.type == "note_off" or (msg.type == "note_on" and msg.velocity == 0):
                stack = active.get((msg.channel, msg.note))
                if stack:
                    on_tick, vel, prog = stack.pop(0)
                    dur = max(1.0 / tpb, (tick - on_tick) / tpb)
                    events.append(
                        NoteEvent(
                            onset_beats=on_tick / tpb,
                            duration_beats=dur,
                            pitch=msg.note,
                            velocity=vel,
                            channel=msg.channel + 1,
                            is_drum=(msg.channel == 9),
                            program=prog,
                        )
                    )

    bpm = 60_000_000.0 / max(1, tempo)
    events.sort(key=lambda e: (e.onset_beats, e.channel, e.pitch))
    return events, bpm, ts_num, ts_den


def read_wav_as_notes(path: Path, bpm_hint: float = 120.0) -> Tuple[List[NoteEvent], float, int, int]:
    """Lightweight STFT/chromagram fallback for WAV reference folders without heavy ML deps."""
    with wave.open(str(path), "rb") as wf:
        sr = wf.getframerate()
        n_ch = wf.getnchannels()
        sw = wf.getsampwidth()
        raw = wf.readframes(wf.getnframes())

    if sw == 2:
        samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    elif sw == 4:
        samples = np.frombuffer(raw, dtype=np.int32).astype(np.float32) / 2147483648.0
    else:
        samples = np.frombuffer(raw, dtype=np.uint8).astype(np.float32) / 128.0 - 1.0

    if n_ch > 1:
        samples = samples.reshape(-1, n_ch).mean(axis=1)

    win = 4096
    hop = int(sr * 0.05)  # 50 ms frames
    if len(samples) < win:
        return [], bpm_hint, 4, 4

    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    hann = np.hanning(win).astype(np.float32)
    midi_bins = {}
    for m in range(28, 92):
        f = 440.0 * (2.0 ** ((m - 69) / 12.0))
        lo = f * (2.0 ** (-0.4 / 12.0))
        hi = f * (2.0 ** (0.4 / 12.0))
        idx = np.where((freqs >= lo) & (freqs <= hi))[0]
        if len(idx) > 0:
            midi_bins[m] = idx

    events: List[NoteEvent] = []
    beats_per_frame = (hop / sr) * (bpm_hint / 60.0)
    active_pitches: Dict[int, float] = {}
    frame_idx = 0
    for pos in range(0, len(samples) - win, hop):
        spec = np.abs(np.fft.rfft(samples[pos : pos + win] * hann))
        energies = {m: float(np.max(spec[idx])) for m, idx in midi_bins.items()}
        peak = max(energies.values(), default=0.0)
        t_beat = frame_idx * beats_per_frame
        if peak > 1e-3:
            thresh = peak * 0.35
            top = sorted(
                [(e, m) for m, e in energies.items() if e >= thresh], reverse=True
            )[:6]
            now_on = {m for _, m in top}
        else:
            now_on = set()

        for m in list(active_pitches.keys()):
            if m not in now_on:
                start_b = active_pitches.pop(m)
                events.append(
                    NoteEvent(
                        onset_beats=start_b,
                        duration_beats=max(0.125, t_beat - start_b),
                        pitch=m,
                        velocity=90,
                        channel=3 if m < 48 else (2 if m >= 72 else 1),
                    )
                )
        for m in now_on:
            if m not in active_pitches:
                active_pitches[m] = t_beat
        frame_idx += 1

    return events, bpm_hint, 4, 4


def detect_key_mode(events: Sequence[NoteEvent]) -> Tuple[int, str]:
    chroma = np.zeros(12, dtype=np.float64)
    for ev in events:
        if ev.is_drum:
            continue
        w = max(0.05, ev.duration_beats) * (1.4 if ev.channel == 3 else 1.0)
        chroma[ev.pitch % 12] += w
    if chroma.sum() <= 1e-9:
        return 0, "minor"

    best_pc = 0
    best_mode = "minor"
    best_score = -1e9
    for pc in range(12):
        rotated = np.roll(chroma, -pc)
        for m_name, (_, tmpl) in MODE_TEMPLATES.items():
            t_arr = np.asarray(tmpl, dtype=np.float64)
            corr = float(np.corrcoef(rotated, t_arr)[0, 1])
            if m_name in ("minor", "major"):
                corr += 0.005
            if corr > best_score:
                best_score = corr
                best_pc = pc
                best_mode = m_name
    return best_pc, best_mode


def match_chord_to_roman(
    window_chroma: np.ndarray, bass_pc: Optional[int], tonic_pc: int
) -> Tuple[str, int, int, str]:
    total_mass = float(window_chroma.sum())
    if total_mass <= 1e-9:
        return format_roman(1, 0, "min"), 1, 0, "min"

    best_root = tonic_pc
    best_qual = "min"
    best_score = -1e9

    for root in range(12):
        for q_name, ivs in CHORD_TEMPLATES:
            pcs = {(root + iv) % 12 for iv in ivs}
            hit = sum(window_chroma[pc] for pc in pcs)
            miss = sum(window_chroma[pc] for pc in range(12) if pc not in pcs)
            covered = sum(1 for pc in pcs if window_chroma[pc] > total_mass * 0.08)
            score = (hit - 0.75 * miss) / total_mass
            if bass_pc is not None and bass_pc == root:
                score += 0.25
            score += 0.06 * covered - 0.045 * len(ivs)
            if score > best_score:
                best_score = score
                best_root = root
                best_qual = q_name

    rel = (best_root - tonic_pc) % 12
    best_deg = 1
    best_acc = 0
    min_abs = 99
    for d in range(7):
        diff = rel - MAJOR_SCALE_REF[d]
        if diff > 6:
            diff -= 12
        if diff < -6:
            diff += 12
        if abs(diff) < min_abs or (abs(diff) == min_abs and diff < 0):
            min_abs = abs(diff)
            best_deg = d + 1
            best_acc = diff

    sym = format_roman(best_deg, best_acc, best_qual)
    return sym, best_deg, best_acc, best_qual


def split_roles(pitched: Sequence[NoteEvent]) -> Tuple[List[NoteEvent], List[NoteEvent], List[NoteEvent]]:
    ch1_count = sum(1 for e in pitched if e.channel == 1)
    ch2_count = sum(1 for e in pitched if e.channel == 2)
    ch3_count = sum(1 for e in pitched if e.channel == 3)
    has_explicit_melody = ch2_count >= 4 and ch2_count >= 0.05 * (ch1_count + ch2_count)

    chords: List[NoteEvent] = []
    melody: List[NoteEvent] = []
    bass: List[NoteEvent] = []
    if has_explicit_melody:
        for e in pitched:
            if e.channel == 2:
                melody.append(e)
            elif e.channel == 3:
                bass.append(e)
            else:
                chords.append(e)
    else:
        remaining: List[NoteEvent] = []
        for e in pitched:
            if e.channel == 3 or (32 <= e.program <= 39):
                bass.append(e)
            elif e.channel == 2:
                melody.append(e)
            else:
                remaining.append(e)

        i = 0
        n = len(remaining)
        while i < n:
            j = i + 1
            while j < n and (remaining[j].onset_beats - remaining[i].onset_beats) <= 0.15:
                j += 1
            cluster = sorted(remaining[i:j], key=lambda x: x.pitch)
            if len(cluster) >= 3:
                start = 0
                if ch3_count == 0 and cluster[0].pitch < 48 and (cluster[1].pitch - cluster[0].pitch) >= 5:
                    bass.append(cluster[0])
                    start = 1
                chords.extend(cluster[start:])
                if cluster[-1].pitch >= 60 and ((cluster[-1].pitch - cluster[-2].pitch) >= 5 or cluster[-1].pitch >= 68):
                    melody.append(cluster[-1])
            elif len(cluster) == 2:
                if ch3_count == 0 and cluster[0].pitch < 50:
                    bass.append(cluster[0])
                    (melody if cluster[1].pitch >= 60 else chords).append(cluster[1])
                else:
                    chords.append(cluster[0])
                    if cluster[1].pitch >= 60:
                        melody.append(cluster[1])
                        if cluster[1].pitch - cluster[0].pitch <= 12:
                            chords.append(cluster[1])
                    else:
                        chords.append(cluster[1])
            else:
                e = cluster[0]
                if ch3_count == 0 and e.pitch < 48:
                    bass.append(e)
                elif e.pitch >= 60 or e.duration_beats <= 1.25:
                    melody.append(e)
                    if e.duration_beats >= 0.5 and e.pitch < 76:
                        chords.append(e)
                else:
                    chords.append(e)
            i = j

    if len(melody) < 2 and len(pitched) >= 4:
        bucket_top: Dict[int, NoteEvent] = {}
        for e in pitched:
            if e.channel == 3 or e.pitch < 52:
                continue
            b = int(np.floor(e.onset_beats * 2.0))
            if b not in bucket_top or e.pitch > bucket_top[b].pitch:
                bucket_top[b] = e
        melody = [bucket_top[k] for k in sorted(bucket_top)]

    if not chords:
        chords = [e for e in pitched if e.pitch >= 45] or list(pitched)
    return chords, melody, bass


def analyze_corpus(
    input_paths: Sequence[Path],
    profile_id: str,
    name: str,
    genre: str,
    description: str = "",
    bpm_min: Optional[float] = None,
    bpm_max: Optional[float] = None,
    default_bpm: Optional[float] = None,
    sub_bass_clearance_override: Optional[int] = None,
    forced_mode: Optional[str] = None,
) -> dict:
    songs: List[Tuple[List[NoteEvent], float, int, int]] = []
    for p in input_paths:
        if p.suffix.lower() in (".mid", ".midi"):
            evs, bpm, tn, td = read_midi_events(p)
            if evs:
                songs.append((evs, bpm, tn, td))
        elif p.suffix.lower() == ".wav":
            evs, bpm, tn, td = read_wav_as_notes(p, default_bpm or 120.0)
            if evs:
                songs.append((evs, bpm, tn, td))

    if not songs:
        raise ValueError("No valid MIDI or WAV notes found in input paths")

    bpms = [s[1] for s in songs]
    calc_default_bpm = float(default_bpm if default_bpm is not None else np.median(bpms))
    calc_bpm_min = float(bpm_min if bpm_min is not None else max(40.0, min(bpms) - 4.0))
    calc_bpm_max = float(bpm_max if bpm_max is not None else min(240.0, max(bpms) + 4.0))

    # Aggregate across all songs using relative Roman chords per song
    vocab_meta: Dict[str, Tuple[int, int, str]] = {}
    vocab_weight: Counter[str] = Counter()
    cadence_weight: Counter[str] = Counter()
    trans_counts: Dict[str, Counter[str]] = defaultdict(Counter)

    chord_grid = np.zeros(16, dtype=np.float64)
    melody_grid = np.zeros(16, dtype=np.float64)
    bass_grid = np.zeros(16, dtype=np.float64)
    swing_vals: List[float] = []
    pacing_vals: List[float] = []
    sustain_vals: List[float] = []
    chrom_pairs: List[Tuple[float, float]] = []

    all_chord_pitches: List[int] = []
    spreads: List[float] = []
    voice_counts: List[int] = []
    lowest_chord_pitches: List[int] = []
    ext_chords = 0
    inv_chords = 0
    total_clusters = 0
    centroid_jumps: List[float] = []
    bass_pitches: List[int] = []

    interval_hist = np.zeros(25, dtype=np.float64)
    mel_pitches: List[int] = []
    mel_chord_hits = 0
    mel_tension_hits = 0
    mel_total = 0
    mel_legato_vals: List[float] = []
    mel_notes_per_bar: List[float] = []
    mel_rep_vals: List[float] = []
    mel_arch_steps: List[int] = []

    primary_tonic = 0
    primary_mode = forced_mode or "minor"

    for song_idx, (events, _bpm, ts_num, ts_den) in enumerate(songs):
        pitched = [e for e in events if not e.is_drum and e.channel != 10]
        if not pitched:
            continue
        pitched.sort(key=lambda e: (e.onset_beats, e.pitch))
        chords, melody, bass = split_roles(pitched)

        tonic_pc, detected_mode = detect_key_mode(pitched)
        mode = forced_mode or detected_mode
        if song_idx == 0:
            primary_tonic = tonic_pc
            primary_mode = mode

        scale_ivs, _ = MODE_TEMPLATES.get(mode, MODE_TEMPLATES["minor"])
        in_scale = {(tonic_pc + iv) % 12 for iv in scale_ivs}
        total_dur = sum(e.duration_beats for e in pitched)
        chrom_dur = sum(e.duration_beats for e in pitched if (e.pitch % 12) not in in_scale)
        chrom_pairs.append((chrom_dur, total_dur))

        beats_per_bar = ts_num * (4.0 / ts_den)
        max_beat = max(e.onset_beats + e.duration_beats for e in pitched)
        total_bars = max(1, int(np.ceil(max_beat / beats_per_bar)))
        win_beats = max(1.0, beats_per_bar * 0.5)
        num_wins = max(1, int(np.ceil((total_bars * beats_per_bar) / win_beats)))

        segments: List[Tuple[str, int, int, str, float, float, bool]] = []
        for w in range(num_wins):
            ws = w * win_beats
            we = ws + win_beats
            chroma = np.zeros(12, dtype=np.float64)
            for e in chords:
                ov = min(e.onset_beats + e.duration_beats, we) - max(e.onset_beats, ws)
                if ov > 0:
                    chroma[e.pitch % 12] += ov
            bass_pc: Optional[int] = None
            low_b = 128
            for e in bass:
                ov = min(e.onset_beats + e.duration_beats, we) - max(e.onset_beats, ws)
                if ov > 0 and e.pitch < low_b:
                    low_b = e.pitch
                    bass_pc = e.pitch % 12
            if bass_pc is not None:
                chroma[bass_pc] += 0.65 * win_beats
            if chroma.sum() <= 1e-6:
                continue

            sym, deg, acc, qual = match_chord_to_roman(chroma, bass_pc, tonic_pc)
            bar_idx = int(ws / beats_per_bar)
            phrase_end = ((bar_idx + 1) % 4 == 0) and (we >= (bar_idx + 1) * beats_per_bar - 0.1)
            if segments and segments[-1][0] == sym:
                prev = segments[-1]
                segments[-1] = (prev[0], prev[1], prev[2], prev[3], prev[4], we, prev[6] or phrase_end)
            else:
                segments.append((sym, deg, acc, qual, ws, we, phrase_end))

        for s_idx, (sym, deg, acc, qual, sb, eb, ph_end) in enumerate(segments):
            vocab_meta[sym] = (deg, acc, qual)
            vocab_weight[sym] += max(0.5, eb - sb)
            if ph_end or s_idx + 1 == len(segments):
                cadence_weight[sym] += 1.0
            if s_idx + 1 < len(segments):
                trans_counts[sym][segments[s_idx + 1][0]] += 1.0
        if len(segments) >= 2:
            trans_counts[segments[-1][0]][segments[0][0]] += 0.75

        pacing_vals.append(max(1, len(segments)) / float(total_bars))

        # Rhythm grids
        def add_grid(evs: Sequence[NoteEvent], grid: np.ndarray, dedup: bool) -> None:
            last_on = -10.0
            for e in evs:
                if dedup and (e.onset_beats - last_on) <= 0.12:
                    continue
                last_on = e.onset_beats
                bar_pos = e.onset_beats % beats_per_bar
                step = int(round((bar_pos / beats_per_bar) * 16.0)) % 16
                grid[step] += 1.0

        add_grid(chords, chord_grid, True)
        add_grid(melody, melody_grid, False)
        add_grid(bass, bass_grid, False)

        for e in pitched:
            bf = e.onset_beats % 1.0
            if 0.42 <= bf <= 0.72:
                swing_vals.append(float(np.clip((bf - 0.5) / 0.167, 0.0, 1.0)))

        if chords:
            med_dur = float(np.median([e.duration_beats for e in chords]))
            expected_slice = beats_per_bar / max(0.5, pacing_vals[-1])
            sustain_vals.append(float(np.clip(med_dur / expected_slice, 0.2, 1.15)))

        # Voicing stats
        i = 0
        prev_cent: Optional[float] = None
        while i < len(chords):
            j = i + 1
            while j < len(chords) and (chords[j].onset_beats - chords[i].onset_beats) <= 0.15:
                j += 1
            if j - i >= 2:
                ps = sorted(e.pitch for e in chords[i:j])
                all_chord_pitches.extend(ps)
                spreads.append(float(ps[-1] - ps[0]))
                voice_counts.append(len(ps))
                lowest_chord_pitches.append(ps[0])
                cent = float(np.mean(ps))
                if prev_cent is not None:
                    centroid_jumps.append(abs(cent - prev_cent))
                prev_cent = cent
                if len({p % 12 for p in ps}) >= 4:
                    ext_chords += 1
                t_b = chords[i].onset_beats
                for sym, deg, acc, qual, sb, eb, _ in segments:
                    if sb - 0.1 <= t_b <= eb + 0.1:
                        root_pc = (tonic_pc + MAJOR_SCALE_REF[deg - 1] + acc) % 12
                        if (ps[0] % 12) != root_pc:
                            inv_chords += 1
                        break
                total_clusters += 1
            i = j

        bass_pitches.extend(e.pitch for e in bass)

        # Melody stats
        if len(melody) >= 2:
            mel_notes_per_bar.append(len(melody) / float(total_bars))
            ivs: List[int] = []
            for k, mn in enumerate(melody):
                mel_pitches.append(mn.pitch)
                mel_total += 1
                if k > 0:
                    diff = int(np.clip(mn.pitch - melody[k - 1].pitch, -12, 12))
                    interval_hist[diff + 12] += 1.0
                    ivs.append(diff)
                    mel_arch_steps.append(diff)
                    ioi = max(0.0625, mn.onset_beats - melody[k - 1].onset_beats)
                    mel_legato_vals.append(float(np.clip(melody[k - 1].duration_beats / ioi, 0.0, 1.1)))
                pc = mn.pitch % 12
                is_ct = False
                for sym, deg, acc, qual, sb, eb, _ in segments:
                    if sb - 0.1 <= mn.onset_beats <= eb + 0.1:
                        root_pc = (tonic_pc + MAJOR_SCALE_REF[deg - 1] + acc) % 12
                        q_ivs = next((q[1] for q in CHORD_TEMPLATES if q[0] == qual), [0, 3, 7])
                        if pc in {(root_pc + x) % 12 for x in q_ivs}:
                            is_ct = True
                        break
                if is_ct:
                    mel_chord_hits += 1
                elif pc in in_scale:
                    mel_tension_hits += 1

            if len(ivs) >= 4:
                bg = Counter(zip(ivs[:-1], ivs[1:]))
                rep = sum(c for c in bg.values() if c >= 2)
                mel_rep_vals.append(float(np.clip(rep / float(len(ivs)), 0.1, 0.95)))

    # Normalize distributions cleanly
    def norm_grid(g: np.ndarray, fallback: List[float]) -> List[float]:
        mx = float(g.max())
        if mx <= 1e-9:
            return fallback
        return [round(float(x / mx), 4) for x in g]

    cg = norm_grid(chord_grid, [1.0, 0.0, 0.0, 0.25, 0.0, 0.0, 0.35, 0.0, 0.5, 0.0, 0.0, 0.2, 0.0, 0.0, 0.3, 0.0])
    mg = norm_grid(melody_grid, [0.7, 0.1, 0.4, 0.2, 0.6, 0.1, 0.45, 0.2, 0.65, 0.15, 0.4, 0.25, 0.5, 0.1, 0.35, 0.15])
    bg = norm_grid(bass_grid, [1.0, 0.0, 0.0, 0.35, 0.0, 0.0, 0.45, 0.0, 0.25, 0.0, 0.55, 0.0, 0.0, 0.2, 0.3, 0.0])

    total_gw = sum(cg[s] + 0.5 * bg[s] for s in range(16))
    off_gw = sum(cg[s] + 0.5 * bg[s] for s in range(16) if s % 4 != 0)
    syncopation = float(np.clip(off_gw / total_gw, 0.0, 1.0)) if total_gw > 0 else 0.35
    anticipation = float(np.clip((cg[14] + cg[15]) / max(1e-6, sum(cg)) * 2.5, 0.0, 1.0))
    swing = float(np.mean(swing_vals)) if len(swing_vals) >= 3 else 0.0
    harmonic_pacing = float(np.clip(np.mean(pacing_vals) if pacing_vals else 1.0, 0.25, 4.0))
    sustain_ratio = float(np.clip(np.mean(sustain_vals) if sustain_vals else 0.85, 0.2, 1.15))

    # Vocabulary & Markov matrix (top-K=12 pruning + healthy Dirichlet smoothing)
    vocab_list = []
    sorted_syms = sorted(vocab_weight.keys(), key=lambda s: vocab_weight[s], reverse=True)[:12]
    tot_vw = sum(vocab_weight[s] for s in sorted_syms) or 1.0
    tot_cw = sum(cadence_weight[s] for s in sorted_syms) or 1.0
    for sym in sorted_syms:
        deg, acc, qual = vocab_meta[sym]
        vocab_list.append({
            "symbol": sym,
            "degree": int(deg),
            "accidental": int(acc),
            "quality": qual,
            "bass_degree_offset": 0,
            "weight": round(float(vocab_weight[sym] / tot_vw), 6),
            "cadence_weight": round(float(cadence_weight[sym] / tot_cw), 6),
        })

    transitions: Dict[str, Dict[str, float]] = {}
    for fsym in sorted_syms:
        row_obs = sum(trans_counts[fsym][item["symbol"]] for item in vocab_list)
        row: Dict[str, float] = {}
        for item in vocab_list:
            tsym = item["symbol"]
            raw_p = (trans_counts[fsym][tsym] / row_obs) if row_obs > 0 else item["weight"]
            c = 0.42 * raw_p + 0.58 * item["weight"]
            if tsym == fsym and len(sorted_syms) > 1:
                c *= 0.25
            row[tsym] = c
        rsum = sum(row.values()) or 1.0
        transitions[fsym] = {k: round(float(v / rsum), 6) for k, v in row.items()}

    # Voicing
    reg_low = int(np.clip(np.floor(np.percentile(all_chord_pitches, 8)), 36, 72)) if all_chord_pitches else 48
    reg_high = int(np.clip(np.ceil(np.percentile(all_chord_pitches, 92)), reg_low + 8, 96)) if all_chord_pitches else 76
    center_pitch = int(round(np.median(all_chord_pitches))) if all_chord_pitches else 62
    spread_semitones = float(np.clip(np.mean(spreads), 5.0, 30.0)) if spreads else 14.0
    min_notes = int(np.clip(min(voice_counts), 2, 5)) if voice_counts else 3
    max_notes = int(np.clip(max(voice_counts), min_notes, 6)) if voice_counts else 5
    ext_richness = float(np.clip(ext_chords / max(1, total_clusters), 0.0, 1.0))
    inv_prob = float(np.clip(inv_chords / max(1, total_clusters), 0.0, 1.0))
    drop2_prob = float(np.clip((spread_semitones - 11.0) / 14.0, 0.0, 1.0))
    vl_tight = float(np.clip(1.0 - (np.mean(centroid_jumps) / 14.0), 0.1, 0.98)) if centroid_jumps else 0.8
    clearance = (
        sub_bass_clearance_override
        if sub_bass_clearance_override is not None
        else (
            int(np.clip(np.floor(np.percentile(lowest_chord_pitches, 10)), 36, min(60, reg_high - 6)))
            if lowest_chord_pitches
            else 48
        )
    )
    bass_low = int(np.clip(np.percentile(bass_pitches, 5), 21, 50)) if bass_pitches else 28
    bass_high = int(np.clip(np.percentile(bass_pitches, 95), bass_low + 5, 58)) if bass_pitches else 45

    # Melody
    if interval_hist.sum() > 0:
        interval_hist = interval_hist / interval_hist.sum()
    else:
        interval_hist = np.array([
            0.005, 0.005, 0.005, 0.010, 0.015, 0.025, 0.015, 0.035, 0.055, 0.080, 0.140, 0.080,
            0.080,
            0.080, 0.140, 0.080, 0.055, 0.035, 0.015, 0.025, 0.015, 0.010, 0.005, 0.005, 0.005,
        ])
        interval_hist /= interval_hist.sum()

    mel_low = int(np.clip(np.percentile(mel_pitches, 5), 48, 84)) if mel_pitches else 64
    mel_high = int(np.clip(np.percentile(mel_pitches, 95), mel_low + 7, 108)) if mel_pitches else 86
    ct_ratio = float(np.clip(mel_chord_hits / max(1, mel_total), 0.15, 0.95)) if mel_total else 0.72
    tt_ratio = float(np.clip(mel_tension_hits / max(1, mel_total), 0.02, 0.65)) if mel_total else 0.18
    motif_density = float(np.clip(np.mean(mel_notes_per_bar) / 8.0, 0.1, 1.0)) if mel_notes_per_bar else 0.45
    motif_rep = float(np.mean(mel_rep_vals)) if mel_rep_vals else 0.6
    arch_contour = float(np.clip(0.5 + (np.mean(mel_arch_steps) / 10.0), 0.1, 0.9)) if mel_arch_steps else 0.5
    legato_ratio = float(np.clip(np.mean(mel_legato_vals), 0.15, 1.0)) if mel_legato_vals else 0.7

    chrom_total = sum(p[1] for p in chrom_pairs) or 1.0
    chromaticism = float(np.clip(sum(p[0] for p in chrom_pairs) / chrom_total, 0.0, 1.0))

    base_harmony = {
        "tonic_pc": int(primary_tonic),
        "mode": primary_mode,
        "vocabulary": vocab_list,
        "transitions": transitions,
        "chromaticism": round(chromaticism, 4),
    }
    base_rhythm = {
        "subdivision": 16,
        "chord_onset_grid": cg,
        "melody_onset_grid": mg,
        "bass_onset_grid": bg,
        "syncopation": round(syncopation, 4),
        "anticipation": round(anticipation, 4),
        "swing": round(swing, 4),
        "harmonic_pacing": round(harmonic_pacing, 4),
        "sustain_ratio": round(sustain_ratio, 4),
    }
    base_voicing = {
        "register_low": reg_low,
        "register_high": reg_high,
        "center_pitch": center_pitch,
        "spread_semitones": round(spread_semitones, 3),
        "min_notes": min_notes,
        "max_notes": max_notes,
        "extension_richness": round(ext_richness, 4),
        "drop2_prob": round(drop2_prob, 4),
        "inversion_prob": round(inv_prob, 4),
        "voice_leading_tightness": round(vl_tight, 4),
        "sub_bass_clearance_midi": int(clearance),
        "bass_register_low": bass_low,
        "bass_register_high": bass_high,
    }
    base_melody = {
        "interval_histogram": [round(float(x), 6) for x in interval_hist],
        "chord_tone_ratio": round(ct_ratio, 4),
        "tension_tone_ratio": round(tt_ratio, 4),
        "motif_density": round(motif_density, 4),
        "motif_repetition": round(motif_rep, 4),
        "arch_contour": round(arch_contour, 4),
        "register_low": mel_low,
        "register_high": mel_high,
        "legato_ratio": round(legato_ratio, 4),
    }

    import copy
    intro_r = copy.deepcopy(base_rhythm)
    intro_r["harmonic_pacing"] = round(float(np.clip(harmonic_pacing * 0.55, 0.25, 4.0)), 4)
    intro_r["sustain_ratio"] = round(float(np.clip(sustain_ratio * 1.15, 0.2, 1.5)), 4)
    intro_r["syncopation"] = round(float(np.clip(syncopation * 0.65, 0.0, 1.0)), 4)
    intro_r["bass_onset_grid"] = [round(v * (1.0 if idx % 8 == 0 else 0.35), 4) for idx, v in enumerate(bg)]
    intro_r["melody_onset_grid"] = [round(v * 0.55, 4) for v in mg]
    intro_v = copy.deepcopy(base_voicing)
    intro_v["register_low"] = min(reg_high - 6, reg_low + 4)
    intro_v["spread_semitones"] = round(float(np.clip(spread_semitones * 0.85, 5.0, 30.0)), 3)
    intro_m = copy.deepcopy(base_melody)
    intro_m["motif_density"] = round(float(np.clip(motif_density * 0.60, 0.1, 1.0)), 4)

    outro_r = copy.deepcopy(base_rhythm)
    outro_r["sustain_ratio"] = round(float(np.clip(sustain_ratio * 1.20, 0.2, 1.5)), 4)
    outro_r["syncopation"] = round(float(np.clip(syncopation * 0.75, 0.0, 1.0)), 4)
    outro_m = copy.deepcopy(base_melody)
    outro_m["motif_density"] = round(float(np.clip(motif_density * 0.70, 0.1, 1.0)), 4)

    return {
        "schema_version": 1,
        "id": profile_id,
        "name": name,
        "genre": genre,
        "description": description,
        "bpm_min": round(calc_bpm_min, 2),
        "bpm_max": round(calc_bpm_max, 2),
        "default_bpm": round(calc_default_bpm, 2),
        "time_signature": {"numerator": 4, "denominator": 4},
        "source": "offline_analyzer",
        "track_count": len(songs),
        "harmony": base_harmony,
        "rhythm": base_rhythm,
        "voicing": base_voicing,
        "melody": base_melody,
        "sections": {
            "intro": {
                "harmony": copy.deepcopy(base_harmony),
                "rhythm": intro_r,
                "voicing": intro_v,
                "melody": intro_m,
            },
            "chorus": {
                "harmony": copy.deepcopy(base_harmony),
                "rhythm": copy.deepcopy(base_rhythm),
                "voicing": copy.deepcopy(base_voicing),
                "melody": copy.deepcopy(base_melody),
            },
            "outro": {
                "harmony": copy.deepcopy(base_harmony),
                "rhythm": outro_r,
                "voicing": copy.deepcopy(base_voicing),
                "melody": outro_m,
            },
        },
    }


def collect_inputs(paths: Sequence[str]) -> List[Path]:
    found: List[Path] = []
    for raw in paths:
        p = Path(raw)
        if p.is_dir():
            for ext in ("*.mid", "*.midi", "*.wav"):
                found.extend(sorted(p.rglob(ext)))
        elif p.exists():
            found.append(p)
    return found


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze MIDI/WAV references into a StyleProfile v1 JSON.")
    parser.add_argument("--input", nargs="+", required=True, help="Input MIDI/WAV files or folders")
    parser.add_argument("--out", required=True, help="Output StyleProfile JSON path")
    parser.add_argument("--id", required=True, help="Profile id")
    parser.add_argument("--name", required=True, help="Display name")
    parser.add_argument("--genre", required=True, help="Genre label")
    parser.add_argument("--description", default="", help="Optional description")
    parser.add_argument("--bpm-min", type=float, default=None)
    parser.add_argument("--bpm-max", type=float, default=None)
    parser.add_argument("--default-bpm", type=float, default=None)
    parser.add_argument("--sub-bass-clearance", type=int, default=None)
    parser.add_argument("--mode", default=None)
    args = parser.parse_args()

    files = collect_inputs(args.input)
    if not files:
        raise SystemExit(f"No .mid/.midi/.wav files found under {args.input}")

    profile = analyze_corpus(
        input_paths=files,
        profile_id=args.id,
        name=args.name,
        genre=args.genre,
        description=args.description,
        bpm_min=args.bpm_min,
        bpm_max=args.bpm_max,
        default_bpm=args.default_bpm,
        sub_bass_clearance_override=args.sub_bass_clearance,
        forced_mode=args.mode,
    )

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(profile, indent=2) + "\n")
    print(f"Wrote {out_path} ({len(files)} reference files analyzed, key_pc={profile['harmony']['tonic_pc']}, mode={profile['harmony']['mode']})")


if __name__ == "__main__":
    main()
