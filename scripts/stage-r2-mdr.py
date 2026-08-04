#!/usr/bin/env python3
"""Stage playable Reference/MDR songs for Cloudflare R2 upload.

Copies MDR files that can be played without a missing PDX/TDX bank, plus any
locally present PDX they reference, into a staging directory and writes
catalog.json with public HTTPS URLs.

Excluded: MDR files that declare a PDX/TDX name that is not present beside them.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path


def ascii_slug(name: str) -> str:
    stem = Path(name).stem
    slug = re.sub(r"[^A-Za-z0-9._-]+", "_", stem)
    slug = re.sub(r"_+", "_", slug).strip("._-")
    return slug or "song"


def read_mdr_header(path: Path) -> tuple[str, str]:
    data = path.read_bytes()
    marker = data.find(b"\r\n\x1a")
    if marker < 0:
        raise ValueError(f"MDR title terminator missing: {path}")
    title = data[:marker].decode("cp932", errors="replace")
    pdx_start = marker + 3
    zero = data.find(0, pdx_start)
    if zero < 0:
        raise ValueError(f"MDR PDX terminator missing: {path}")
    pdx_name = data[pdx_start:zero].decode("cp932", errors="replace")
    # Collapse multi-line titles for catalog display.
    title = " / ".join(
        line.strip() for line in title.replace("\r", "\n").split("\n") if line.strip()
    )
    return title, pdx_name


def index_pdx(folder: Path) -> dict[str, Path]:
    return {
        path.name.lower(): path
        for path in folder.iterdir()
        if path.is_file() and path.suffix.lower() == ".pdx"
    }


def resolve_pdx(pdx_name: str, pdx_index: dict[str, Path]) -> Path | None:
    if not pdx_name:
        return None
    candidates = [pdx_name]
    if Path(pdx_name).suffix == "":
        candidates.append(pdx_name + ".pdx")
    for candidate in candidates:
        found = pdx_index.get(candidate.lower())
        if found is not None:
            return found
    stem = Path(pdx_name).stem.lower()
    for key, path in pdx_index.items():
        if Path(key).stem == stem:
            return path
    return None


def classify(source: Path) -> tuple[list[dict], list[dict]]:
    pdx_index = index_pdx(source)
    playable: list[dict] = []
    excluded: list[dict] = []
    for mdr in sorted(source.glob("*.[Mm][Dd][Rr]"), key=lambda p: p.name.lower()):
        title, pdx_name = read_mdr_header(mdr)
        if not pdx_name:
            playable.append(
                {
                    "path": mdr,
                    "title": title or mdr.stem,
                    "pdx_name": "",
                    "pdx_path": None,
                }
            )
            continue
        # Skip TDX-backed / talking extensions until a bank is available.
        if pdx_name.lower().endswith(".tdx"):
            excluded.append(
                {
                    "file": mdr.name,
                    "title": title,
                    "reason": f"requires TDX bank '{pdx_name}' (not staged)",
                }
            )
            continue
        pdx_path = resolve_pdx(pdx_name, pdx_index)
        if pdx_path is None:
            excluded.append(
                {
                    "file": mdr.name,
                    "title": title,
                    "reason": f"missing PDX '{pdx_name}'",
                }
            )
            continue
        playable.append(
            {
                "path": mdr,
                "title": title or mdr.stem,
                "pdx_name": pdx_name,
                "pdx_path": pdx_path,
            }
        )
    return playable, excluded


def unique_object_name(original: str, used: set[str]) -> str:
    candidate = original
    if candidate.lower() in used:
        stem = Path(original).stem
        suffix = Path(original).suffix
        index = 2
        while True:
            candidate = f"{stem}_{index}{suffix}"
            if candidate.lower() not in used:
                break
            index += 1
    used.add(candidate.lower())
    return candidate


def stage(
    source: Path,
    dest: Path,
    base_url: str,
    prefix: str,
) -> tuple[dict, list[dict]]:
    playable, excluded = classify(source)
    songs_dir = dest / "songs"
    songs_dir.mkdir(parents=True, exist_ok=True)

    base = base_url.rstrip("/")
    prefix = prefix.strip("/")
    url_root = f"{base}/{prefix}" if prefix else base

    used_names: set[str] = set()
    songs: list[dict] = []
    copied_pdx: dict[Path, str] = {}

    for item in playable:
        mdr_path: Path = item["path"]
        object_name = unique_object_name(mdr_path.name, used_names)
        shutil.copy2(mdr_path, songs_dir / object_name)
        song = {
            "id": ascii_slug(object_name),
            "title": item["title"],
            "mdr": f"{url_root}/{object_name}",
        }
        pdx_path: Path | None = item["pdx_path"]
        if pdx_path is not None:
            if pdx_path.resolve() not in copied_pdx:
                pdx_object = unique_object_name(pdx_path.name, used_names)
                shutil.copy2(pdx_path, songs_dir / pdx_object)
                copied_pdx[pdx_path.resolve()] = pdx_object
            song["pdx"] = f"{url_root}/{copied_pdx[pdx_path.resolve()]}"
        songs.append(song)

    catalog = {"version": 1, "songs": songs}
    (dest / "catalog.json").write_text(
        json.dumps(catalog, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    (dest / "excluded.json").write_text(
        json.dumps(excluded, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    summary = {
        "source": str(source),
        "dest": str(dest),
        "playable": len(playable),
        "excluded": len(excluded),
        "pdx_files": len(copied_pdx),
        "catalog": str(dest / "catalog.json"),
        "base_url": url_root,
    }
    (dest / "SUMMARY.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return catalog, excluded


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("Reference/MDR"),
        help="Local MDR folder (default: Reference/MDR)",
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=Path("r2-stage"),
        help="Staging directory (default: r2-stage)",
    )
    parser.add_argument(
        "--base-url",
        required=True,
        help="Public R2 base URL, e.g. https://pub-xxxxx.r2.dev",
    )
    parser.add_argument(
        "--prefix",
        default="mdr",
        help="Object key prefix under the bucket (default: mdr)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Only print counts; do not copy files",
    )
    args = parser.parse_args()

    if not args.source.is_dir():
        print(f"source folder not found: {args.source}", file=sys.stderr)
        return 1

    playable, excluded = classify(args.source)
    print(f"playable: {len(playable)}")
    print(f"excluded: {len(excluded)}")
    for item in excluded:
        print(f"  - {item['file']}: {item['reason']}")

    if args.dry_run:
        return 0

    if args.dest.exists():
        shutil.rmtree(args.dest)
    catalog, _ = stage(args.source, args.dest, args.base_url, args.prefix)
    print(f"wrote {len(catalog['songs'])} songs to {args.dest}/songs/")
    print(f"catalog: {args.dest / 'catalog.json'}")
    print(f"excluded list: {args.dest / 'excluded.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
