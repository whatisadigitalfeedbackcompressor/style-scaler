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
    parser.add_argument("--mode", choices=["corpus", "per-folder", "per-song"], default="per-folder")
    parser.add_argument("--out-dir", default=str(ROOT / "presets" / "styles"))
    parser.add_argument("--catalog", default=str(ROOT / "presets" / "catalog.json"))
    parser.add_argument("--id", default=None)
    parser.add_argument("--name", default=None)
    parser.add_argument("--genre", default=None)
    parser.add_argument("--model", default=None, help="Path to MuScriptor .gguf")
    parser.add_argument("--download-model", action="store_true")
    parser.add_argument("--spectral", action="store_true")
    parser.add_argument("--save-midi-dir", default=None)
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
        print("Running:", " ".join(cmd))
        subprocess.run(cmd, check=True)
    else:
        print("style_deconstruct.exe not found; falling back to Python analyze_styles.py (.mid/.wav only)")
        files = collect_inputs(args.input)
        if not files:
            raise SystemExit("No .mid/.midi/.wav files found for Python fallback")
        pid = args.id or Path(args.input[0]).stem.lower().replace(" ", "_")
        pname = args.name or pid.replace("_", " ").title()
        pgenre = args.genre or "Deconstructed"
        prof = analyze_corpus(files, pid, pname, pgenre)
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
