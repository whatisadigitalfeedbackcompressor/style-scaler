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
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest().lower()


def build_catalog(styles_dir: Path, out_catalog: Path, catalog_name: str) -> Dict[str, Any]:
    styles_dir = styles_dir.resolve()
    out_catalog = out_catalog.resolve()
    cat_parent = out_catalog.parent

    entries: List[Dict[str, Any]] = []
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    for json_path in sorted(styles_dir.glob("*.json")):
        if json_path.resolve() == out_catalog:
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

        entries.append({
            "id": str(data["id"]),
            "name": str(data["name"]),
            "genre": str(data["genre"]),
            "description": str(data.get("description", "")),
            "file": rel_file,
            "sha256": sha256_file(json_path),
            "bpm_min": float(data["bpm_min"]),
            "bpm_max": float(data["bpm_max"]),
            "default_bpm": float(data["default_bpm"]),
            "mode": str(data["harmony"].get("mode", "minor")),
            "tonic_pc": int(data["harmony"].get("tonic_pc", 0)),
            "updated_at": stamp,
        })

    catalog = {
        "schema_version": 1,
        "catalog_name": catalog_name,
        "updated_at": stamp,
        "presets": entries,
    }
    out_catalog.parent.mkdir(parents=True, exist_ok=True)
    out_catalog.write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
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
