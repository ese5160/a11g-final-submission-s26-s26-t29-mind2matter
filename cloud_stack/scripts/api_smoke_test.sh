#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${1:-${PUBLIC_BASE_URL:-http://20.119.220.234}}"
WORK_DIR="$(mktemp -d)"
WAV_FILE="$WORK_DIR/test-tone.wav"
HEALTH_JSON="$WORK_DIR/health.json"
UPLOAD_JSON="$WORK_DIR/upload.json"
VOICE_JSON="$WORK_DIR/voice.json"
REQUEST_ID="req_smoke_$(date +%s)"

cleanup() {
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

python3 "$(dirname "$0")/generate_test_wav.py" "$WAV_FILE" 1.2 >/dev/null

echo "[1/4] Health check: $BASE_URL/api/health"
curl -fsS "$BASE_URL/api/health" | tee "$HEALTH_JSON"
echo

echo "[2/4] Upload test WAV"
curl -fsS -X POST "$BASE_URL/api/upload" \
  -F "kind=devkit" \
  -F "request_id=$REQUEST_ID" \
  -F "file=@$WAV_FILE;type=audio/wav" | tee "$UPLOAD_JSON"
echo

UPLOAD_URL="$(python3 - "$UPLOAD_JSON" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    print(json.load(fh)["url"])
PY
)"

echo "[3/4] Submit voice request through AI worker"
curl -fsS -X POST "$BASE_URL/api/voice-request" \
  -H "Content-Type: application/json" \
  -d "{
    \"request_id\": \"$REQUEST_ID\",
    \"device_id\": \"glasses01\",
    \"mode\": \"qa\",
    \"audio_url\": \"$UPLOAD_URL\",
    \"image_url\": null,
    \"text_prompt\": \"Please transcribe and answer briefly.\",
    \"dev_board_stub\": false,
    \"fw_version\": \"cloud-smoke-test\",
    \"uptime_ms\": 1000
  }" | tee "$VOICE_JSON"
echo

echo "[4/4] Summary"
python3 - "$HEALTH_JSON" "$VOICE_JSON" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    health = json.load(fh)
with open(sys.argv[2], "r", encoding="utf-8") as fh:
    voice = json.load(fh)
print(f"mock_mode={health.get('mock_mode')}")
print(f"request_id={voice.get('request_id')}")
print(f"text={voice.get('text')}")
print(f"audio_url={voice.get('audio_url')}")
print(f"transcript={voice.get('transcript')}")
PY
