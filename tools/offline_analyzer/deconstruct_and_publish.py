#!/usr/bin/env python3
"""
End-to-end offline workflow:
1. Deconstructs audio/MIDI songs into StyleProfile JSON presets (using native
   style_deconstruct.exe with NeuralNote muscriptor.cpp when built, else Python
   analyze_styles.py for MIDI/WAV).
2. Rebuilds presets/catalog.json with SHA-256 verification.
3. Optionally commits and pushes presets + catalog.json to GitHub (`--push`) so
   the StyleScaler plugin can sync them via "Sync GitHub".
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
from typing import List, Optional

from analyze_styles import analyze_corpus, collect_inputs
from audio_metadata import canonicalize_genre, read_audio_metadata, sanitize_genre_slug
from build_catalog import build_catalog
import json


ROOT = Path(__file__).resolve().parents[2]


def find_deconstruct_exe() -> Optional[Path]:
    candidates = [
        ROOT / "dist" / "style_deconstruct.exe",
        ROOT / "build" / "style_deconstruct_artefacts" / "Release" / "style_deconstruct.exe",
        ROOT / "build" / "Release" / "style_deconstruct.exe",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def detect_github_raw_url(catalog_rel: str) -> Optional[str]:
    try:
        res = subprocess.run(
            ["git", "-C", str(ROOT), "remote", "get-url", "origin"],
            capture_output=True,
            text=True,
            check=True,
        )
        remote = res.stdout.strip()
    except Exception:
        return None

    m = re.search(r"github\.com[:/]([^/]+)/([^/.]+)(?:\.git)?$", remote)
    if not m:
        return None
    owner, repo = m.group(1), m.group(2)
    try:
        br = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip() or "main"
    except Exception:
        br = "main"
    return f"https://raw.githubusercontent.com/{owner}/{repo}/{br}/{catalog_rel}"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Deconstruct songs offline -> update catalog.json -> optionally push to GitHub."
    )
    parser.add_argument("--input", nargs="+", required=True, help="Audio/MIDI files or folders")
    parser.add_argument("--mode", choices=["corpus", "per-folder", "per-song", "auto-genre"], default="per-folder")
    parser.add_argument("--out-dir", default=str(ROOT / "presets" / "styles"))
    parser.add_argument("--catalog", default=str(ROOT / "presets" / "catalog.json"))
    parser.add_argument("--id", default=None)
    parser.add_argument("--name", default=None)
    parser.add_argument("--genre", default=None)
    parser.add_argument("--model", default=None, help="Path to MuScriptor .gguf")
    parser.add_argument("--download-model", action="store_true")
    parser.add_argument("--spectral", action="store_true")
    parser.add_argument("--save-midi-dir", default=None)
    parser.add_argument("--cache-dir", default=None, help="Per-song transcription checkpoint directory")
    parser.add_argument("--merge-existing", dest="merge_existing", action="store_true", default=None)
    parser.add_argument("--no-merge", dest="merge_existing", action="store_false")
    parser.add_argument("--push", action="store_true", help="Commit & push updated presets/catalog to GitHub origin")
    parser.add_argument("--commit-msg", default="Add deconstructed style presets to catalog")
    args = parser.parse_args()

    out_dir = Path(args.out_dir).resolve()
    catalog_path = Path(args.catalog).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    exe = find_deconstruct_exe()
    if exe is not None:
        cmd: List[str] = [str(exe), "--mode", args.mode, "--out-dir", str(out_dir)]
        for inp in args.input:
            cmd.extend(["--input", str(Path(inp).resolve())])
        if args.id:
            cmd.extend(["--id", args.id])
        if args.name:
            cmd.extend(["--name", args.name])
        if args.genre:
            cmd.extend(["--genre", args.genre])
        if args.model:
            cmd.extend(["--model", str(Path(args.model).resolve())])
        if args.download_model:
            cmd.append("--download-model")
        if args.spectral:
            cmd.append("--spectral")
        if args.save_midi_dir:
            cmd.extend(["--save-midi-dir", str(Path(args.save_midi_dir).resolve())])
        if args.cache_dir:
            cmd.extend(["--cache-dir", str(Path(args.cache_dir).resolve())])
        if args.merge_existing is True:
            cmd.append("--merge-existing")
        elif args.merge_existing is False:
            cmd.append("--no-merge")
        print("Running:", " ".join(cmd))
        subprocess.run(cmd, check=True)
    else:
        print("style_deconstruct.exe not found; falling back to Python analyze_styles.py (.mid/.wav only)")
        files = collect_inputs(args.input)
        if not files:
            raise SystemExit("No .mid/.midi/.wav files found for Python fallback")
        if args.mode == "auto-genre":
            grouped: dict[str, dict] = {}
            for fp in files:
                meta = read_audio_metadata(Path(fp))
                genre_name = meta.canonical_genre or (canonicalize_genre(args.genre) if args.genre else "Deconstructed")
                src_name = meta.genre_source if meta.has_genre else "fallback"
                slug = sanitize_genre_slug(genre_name)
                bucket = grouped.setdefault(slug, {"genre": genre_name, "files": [], "tags": [], "sources": [], "artists": set()})
                bucket["files"].append(fp)
                bucket["sources"].append(src_name)
                if meta.artist:
                    bucket["artists"].add(meta.artist)
                for t in meta.tags + [genre_name.lower()]:
                    if t not in bucket["tags"]:
                        bucket["tags"].append(t)
            for slug, bucket in grouped.items():
                prof = analyze_corpus(bucket["files"], slug, bucket["genre"], bucket["genre"])
                prof["tags"] = bucket["tags"]
                prof["genre_source"] = bucket["sources"][0] if bucket["sources"] else "fallback"
                if len(bucket["artists"]) == 1:
                    prof["artist"] = next(iter(bucket["artists"]))
                (out_dir / f"{slug}.json").write_text(json.dumps(prof, indent=2) + "\n", encoding="utf-8")
        else:
            meta = read_audio_metadata(Path(files[0]))
            pid = args.id or sanitize_genre_slug(meta.title or Path(args.input[0]).stem)
            pname = args.name or meta.title or pid.replace("_", " ").title()
            pgenre = canonicalize_genre(args.genre) if args.genre else (meta.canonical_genre or "Deconstructed")
            prof = analyze_corpus(files, pid, pname, pgenre)
            if meta.tags:
                prof["tags"] = meta.tags
            if meta.has_genre:
                prof["genre_source"] = meta.genre_source
            if meta.artist:
                prof["artist"] = meta.artist
            (out_dir / f"{pid}.json").write_text(json.dumps(prof, indent=2) + "\n", encoding="utf-8")

    cat = build_catalog(out_dir, catalog_path, "StyleScaler Preset Catalog")
    print(f"Catalog updated: {catalog_path} ({len(cat['presets'])} presets)")

    if args.push:
        subprocess.run(["git", "-C", str(ROOT), "add", str(out_dir), str(catalog_path)], check=True)
        diff = subprocess.run(["git", "-C", str(ROOT), "diff", "--cached", "--quiet"])
        if diff.returncode != 0:
            subprocess.run(["git", "-C", str(ROOT), "commit", "-m", args.commit_msg], check=True)
            subprocess.run(["git", "-C", str(ROOT), "push"], check=True)
            print("Pushed presets and catalog.json to GitHub.")
        else:
            print("No changes in presets or catalog.json to commit.")

    try:
        rel_cat = catalog_path.relative_to(ROOT).as_posix()
    except ValueError:
        rel_cat = "presets/catalog.json"
    raw_url = detect_github_raw_url(rel_cat)
    if raw_url:
        print(f"Plugin Catalog URL: {raw_url}")


if __name__ == "__main__":
    main()
