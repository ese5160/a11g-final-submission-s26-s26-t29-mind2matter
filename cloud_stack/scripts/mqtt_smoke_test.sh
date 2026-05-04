#!/usr/bin/env bash
set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REQUEST_ID="req_mqtt_smoke_$(date +%s)"
ARG1="${1:-}"
BASE_URL="${PUBLIC_BASE_URL:-http://20.119.220.234}"
WORK_DIR="$(mktemp -d)"
WAV_FILE="$WORK_DIR/test-tone.wav"
UPLOAD_JSON="$WORK_DIR/upload.json"

cleanup() {
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

if [[ -n "$ARG1" ]]; then
  if [[ "$ARG1" == http://*"/uploads/"* || "$ARG1" == https://*"/uploads/"* ]]; then
    UPLOAD_URL="$ARG1"
  else
    BASE_URL="$ARG1"
    UPLOAD_URL=""
  fi
else
  UPLOAD_URL=""
fi

cd "$STACK_DIR"

if [[ -z "$UPLOAD_URL" ]]; then
  echo "[0/6] Generate and upload test WAV"
  python3 "$STACK_DIR/scripts/generate_test_wav.py" "$WAV_FILE" 1.2 >/dev/null
  curl -fsS -X POST "$BASE_URL/api/upload" \
    -F "kind=devkit" \
    -F "request_id=$REQUEST_ID" \
    -F "file=@$WAV_FILE;type=audio/wav" | tee "$UPLOAD_JSON" >/dev/null

  UPLOAD_URL="$(python3 - "$UPLOAD_JSON" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    print(json.load(fh)["url"])
PY
)"
fi

echo "[1/6] Publish device status"
docker compose exec -T mosquitto mosquitto_pub -h 127.0.0.1 -t mind2matter/device/glasses01/status -m \
'{"wifi":true,"mqtt":true,"battery_pct":92,"voice_busy":false,"fall_pending":false,"debug_led":false,"ota_in_progress":false,"ota_status":"idle","fw_version":"cloud-smoke-test","uptime_s":12}'

echo "[2/6] Publish debug button press"
docker compose exec -T mosquitto mosquitto_pub -h 127.0.0.1 -t mind2matter/device/glasses01/debug/button -m \
'{"pressed":true,"device_id":"glasses01","fw_version":"cloud-smoke-test"}'

echo "[3/6] Publish fall event"
docker compose exec -T mosquitto mosquitto_pub -h 127.0.0.1 -t mind2matter/device/glasses01/event/fall -m \
'{"device_id":"glasses01","phase":"candidate","confirmed":false,"ax_mg":0,"ay_mg":0,"az_mg":3200,"fw_version":"cloud-smoke-test","uptime_ms":1000}'

echo "[4/6] Listen once for cloud reply/notice topics in background"
docker compose exec -T mosquitto sh -lc \
"timeout 10 mosquitto_sub -h 127.0.0.1 -v -t 'mind2matter/cloud/glasses01/voice/reply' -t 'mind2matter/cloud/glasses01/system/notice' -C 1" &
SUB_PID=$!
sleep 1

echo "[5/6] Publish voice request to trigger Node-RED -> AI worker -> MQTT reply"
docker compose exec -T mosquitto mosquitto_pub -h 127.0.0.1 -t mind2matter/device/glasses01/voice/request -m \
"{\"request_id\":\"$REQUEST_ID\",\"device_id\":\"glasses01\",\"mode\":\"qa\",\"audio_url\":\"$UPLOAD_URL\",\"image_url\":null,\"dev_board_stub\":false,\"fw_version\":\"cloud-smoke-test\",\"uptime_ms\":1000}"

wait "$SUB_PID" || true

echo
echo "[6/6] Uploaded audio URL"
echo "  $UPLOAD_URL"
echo
echo "Dashboard:"
echo "  http://20.119.220.234:1880/dashboard"
echo "Check that Status / Button / Fall / AI Activity updated."
