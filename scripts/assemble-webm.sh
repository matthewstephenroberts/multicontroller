#!/usr/bin/env bash
# assemble-webm.sh — turns the frames captured by generate-demo.mjs into
# docs/assets/demo-walkthrough.webm using ffmpeg's concat demuxer, where each
# source PNG is held on screen for its own duration (from manifest.json)
# rather than a fixed frame rate.
#
# Usage: bash scripts/assemble-webm.sh [manifest_dir] [output_webm]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FRAMES_DIR="${1:-$ROOT/scripts/_demo_frames}"
OUTPUT="${2:-$ROOT/docs/assets/demo-walkthrough.webm}"
MANIFEST="$FRAMES_DIR/manifest.json"

if [ ! -f "$MANIFEST" ]; then
  echo "❌ no manifest at $MANIFEST — run: node scripts/generate-demo.mjs" >&2
  exit 1
fi

if ! command -v ffmpeg &>/dev/null; then
  echo "❌ ffmpeg not found — install with: brew install ffmpeg" >&2
  exit 1
fi
if ! command -v jq &>/dev/null; then
  echo "❌ jq not found — install with: brew install jq" >&2
  exit 1
fi

CONCAT_FILE="$FRAMES_DIR/concat.txt"
: > "$CONCAT_FILE"

COUNT=$(jq '.frames | length' "$MANIFEST")
echo "📝 building concat list for $COUNT frames…"

jq -r '.frames[] | "\(.file)\t\(.durationMs)"' "$MANIFEST" | while IFS=$'\t' read -r file duration_ms; do
  duration_s=$(awk "BEGIN { printf \"%.3f\", $duration_ms / 1000 }")
  printf "file '%s'\nduration %s\n" "$file" "$duration_s" >> "$CONCAT_FILE"
done

# ffmpeg's concat demuxer quirk: the last "duration" line is honoured only if the
# final file is also repeated once more without a duration directive.
LAST_FILE=$(jq -r '.frames[-1].file' "$MANIFEST")
printf "file '%s'\n" "$LAST_FILE" >> "$CONCAT_FILE"

echo "🎞  encoding webm (this can take a minute)…"
mkdir -p "$(dirname "$OUTPUT")"
ffmpeg -y -f concat -safe 0 -i "$CONCAT_FILE" \
  -vf "fps=12,scale=900:-2:flags=lanczos,format=yuv420p" \
  -c:v libvpx-vp9 -b:v 0 -crf 32 -row-mt 1 \
  "$OUTPUT" -loglevel error -stats

SIZE=$(du -h "$OUTPUT" | cut -f1)
echo "✅ wrote $OUTPUT ($SIZE)"
