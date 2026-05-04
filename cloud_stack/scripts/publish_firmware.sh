#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "Usage: $0 /path/to/firmware_file [published_name]"
  exit 1
fi

SRC_FILE="$1"
DEST_DIR="$(cd "$(dirname "$0")/../public/firmware" && pwd)"
DEST_NAME="${2:-$(basename "$SRC_FILE")}"

if [ ! -f "$SRC_FILE" ]; then
  echo "Firmware file not found: $SRC_FILE"
  exit 1
fi

cp "$SRC_FILE" "$DEST_DIR/$DEST_NAME"
echo "Published: $DEST_DIR/$DEST_NAME"
