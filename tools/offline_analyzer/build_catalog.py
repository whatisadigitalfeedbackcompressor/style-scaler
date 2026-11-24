#!/usr/bin/env python3
"""
Scans a directory of StyleProfile v1 JSON presets, validates them, computes SHA-256
digests, and writes presets/catalog.json for GitHub-hosted preset syncing.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from typing import Any, Dict, List


REQUIRED_TOP_KEYS = {
    "schema_version",
    "id",
    "name",
    "genre",
    "bpm_min",
    "bpm_max",
    "default_bpm",
    "harmony",
    "rhythm",
    "voicing",
    "melody",
}


def sha256_file(path: Path) -> str:
    raw = path.read_bytes()
    lf_bytes = raw.replace(b"\r\n", b"\n")
    if raw != lf_bytes:
        path.write_bytes(lf_bytes)
    return hashlib.sha256(lf_bytes).hexdigest().lower()


def build_catalog(styles_dir: Path, out_catalog: Path, catalog_name: str) -> Dict[str, Any]:
    styles_dir = styles_dir.resolve()
    out_catalog = out_catalog.resolve()
    cat_parent = out_catalog.parent

    entries: List[Dict[str, Any]] = []
    genres_map: Dict[str, List[str]] = {}
    artists_map: Dict[str, List[str]] = {}
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    for json_path in sorted(styles_dir.glob("*.json")):
        if json_path.resolve() == out_catalog or json_path.name.lower() == "catalog.json":
            continue
        data = json.loads(json_path.read_text(encoding="utf-8"))
        missing = REQUIRED_TOP_KEYS - set(data.keys())
        if missing:
            raise ValueError(f"{json_path.name} missing required keys: {sorted(missing)}")
        if data["schema_version"] != 1:
            raise ValueError(f"{json_path.name} has unsupported schema_version={data['schema_version']}")

        try:
            rel_file = json_path.relative_to(cat_parent).as_posix()
        except ValueError:
            rel_file = json_path.name

        genre_name = str(data["genre"]) or "Uncategorized"
        preset_id = str(data["id"])
        entry: Dict[str, Any] = {
            "id": preset_id,
            "name": str(data["name"]),
            "genre": genre_name,
            "description": str(data.get("description", "")),
            "file": rel_file,
            "sha256": sha256_file(json_path),
            "bpm_min": float(data["bpm_min"]),
            "bpm_max": float(data["bpm_max"]),
            "default_bpm": float(data["default_bpm"]),
            "mode": str(data["harmony"].get("mode", "minor")),
            "tonic_pc": int(data["harmony"].get("tonic_pc", 0)),
            "updated_at": stamp,
        }
        if isinstance(data.get("tags"), list) and data["tags"]:
            entry["tags"] = [str(t) for t in data["tags"] if str(t)]
        if data.get("genre_source"):
            entry["genre_source"] = str(data["genre_source"])
        if data.get("artist"):
            artist_name = str(data["artist"])
            entry["artist"] = artist_name
            artists_map.setdefault(artist_name, []).append(preset_id)
        if data.get("album"):
            entry["album"] = str(data["album"])
        entries.append(entry)

        genres_map.setdefault(genre_name, []).append(preset_id)

    sorted_genres = {g: sorted(ids) for g, ids in sorted(genres_map.items(), key=lambda kv: kv[0].lower())}
    sorted_artists = {a: sorted(ids) for a, ids in sorted(artists_map.items(), key=lambda kv: kv[0].lower())}

    catalog = {
        "schema_version": 1,
        "catalog_name": catalog_name,
        "updated_at": stamp,
        "genres": sorted_genres,
        "artists": sorted_artists,
        "presets": entries,
    }
    out_catalog.parent.mkdir(parents=True, exist_ok=True)
    with out_catalog.open("w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(catalog, indent=2) + "\n")
    return catalog


def main() -> None:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="Build StyleScaler presets/catalog.json with SHA-256 digests.")
    parser.add_argument("--styles-dir", default=str(root / "presets" / "styles"))
    parser.add_argument("--out", default=str(root / "presets" / "catalog.json"))
    parser.add_argument("--name", default="StyleScaler Preset Catalog")
    args = parser.parse_args()

    cat = build_catalog(Path(args.styles_dir), Path(args.out), args.name)
    print(f"Built {args.out} with {len(cat['presets'])} presets.")


if __name__ == "__main__":
    main()
