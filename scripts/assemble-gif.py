#!/usr/bin/env python3
"""assemble-gif.py — turns the frames captured by generate-demo.mjs into
docs/assets/demo-walkthrough.gif.

Reads scripts/_demo_frames/manifest.json (a list of {file, durationMs} entries
in playback order) and writes a single animated GIF with each frame held for
its own duration, using Pillow's palette optimisation to keep file size sane.

Usage: python3 scripts/assemble-gif.py [manifest_dir] [output_gif]
"""
import json
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FRAMES_DIR = ROOT / "scripts" / "_demo_frames"
DEFAULT_OUTPUT = ROOT / "docs" / "assets" / "demo-walkthrough.gif"

# GIF frame durations are quantised to 10ms and most viewers floor tiny values —
# keep everything comfortably playable.
MIN_DURATION_MS = 120


def main() -> None:
    frames_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_FRAMES_DIR
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUTPUT

    manifest_path = frames_dir / "manifest.json"
    if not manifest_path.exists():
        raise SystemExit(f"no manifest at {manifest_path} — run generate-demo.mjs first")

    manifest = json.loads(manifest_path.read_text())
    entries = manifest["frames"]
    if not entries:
        raise SystemExit("manifest has no frames")

    print(f"loading {len(entries)} frames from {frames_dir}…")
    images = []
    durations = []
    for entry in entries:
        img = Image.open(frames_dir / entry["file"]).convert("RGB")
        images.append(img)
        durations.append(max(entry.get("durationMs", 900), MIN_DURATION_MS))

    # Shrink a touch — full-resolution screenshots make for a large GIF; 720px wide
    # is plenty for docs/README embedding while keeping text legible.
    target_w = 900
    scaled = []
    for img in images:
        if img.width != target_w:
            ratio = target_w / img.width
            img = img.resize((target_w, int(img.height * ratio)), Image.LANCZOS)
        scaled.append(img)

    output.parent.mkdir(parents=True, exist_ok=True)
    first, rest = scaled[0], scaled[1:]
    first.save(
        output,
        format="GIF",
        save_all=True,
        append_images=rest,
        duration=durations,
        loop=0,
        optimize=True,
    )
    size_kb = output.stat().st_size / 1024
    print(f"✅ wrote {output} ({size_kb:.0f} KB, {len(scaled)} frames)")


if __name__ == "__main__":
    main()
