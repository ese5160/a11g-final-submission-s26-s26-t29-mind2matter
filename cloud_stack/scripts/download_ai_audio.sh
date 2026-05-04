#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "Usage: $0 AUDIO_URL [output.wav]"
  exit 1
fi

AUDIO_URL="$1"
OUTPUT="${2:-$(basename "${AUDIO_URL%%\?*}")}"

curl -fsS "$AUDIO_URL" -o "$OUTPUT"
echo "Saved: $(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
