#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
  echo "Usage: $0 /path/to/image.(jpg|jpeg|png|webp) [question] [base_url]"
  exit 1
fi

IMAGE_FILE="$1"
QUESTION="${2:-Describe what is visible in this image briefly.}"
BASE_URL="${3:-${PUBLIC_BASE_URL:-http://20.119.220.234}}"
WORK_DIR="$(mktemp -d)"
UPLOAD_JSON="$WORK_DIR/upload.json"
VISION_JSON="$WORK_DIR/vision.json"
REQUEST_ID="req_vision_$(date +%s)"

cleanup() {
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

if [ ! -f "$IMAGE_FILE" ]; then
  echo "Image file not found: $IMAGE_FILE"
  exit 1
fi

echo "[1/4] Health check: $BASE_URL/api/health"
curl -fsS "$BASE_URL/api/health"
echo

echo "[2/4] Upload image"
curl -fsS -X POST "$BASE_URL/api/upload" \
  -F "kind=vision" \
  -F "request_id=$REQUEST_ID" \
  -F "file=@$IMAGE_FILE" | tee "$UPLOAD_JSON"
echo

IMAGE_URL="$(python3 - "$UPLOAD_JSON" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    print(json.load(fh)["url"])
PY
)"

echo "[3/4] Submit vision request through AI worker"
curl -fsS -X POST "$BASE_URL/api/voice-request" \
  -H "Content-Type: application/json" \
  -d "{
    \"request_id\": \"$REQUEST_ID\",
    \"device_id\": \"glasses01\",
    \"mode\": \"vision\",
    \"audio_url\": null,
    \"image_url\": \"$IMAGE_URL\",
    \"text_prompt\": \"$QUESTION\",
    \"dev_board_stub\": false,
    \"fw_version\": \"cloud-vision-smoke-test\",
    \"uptime_ms\": 1000
  }" | tee "$VISION_JSON"
echo

echo "[4/4] Summary"
python3 - "$VISION_JSON" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    vision = json.load(fh)
print(f"request_id={vision.get('request_id')}")
print(f"text={vision.get('text')}")
print(f"audio_url={vision.get('audio_url')}")
print(f"transcript={vision.get('transcript')}")
print(f"mock_mode={vision.get('mock_mode')}")
PY
