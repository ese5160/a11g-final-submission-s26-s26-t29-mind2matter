#ifndef M2M_APP_CONFIG_H
#define M2M_APP_CONFIG_H

#include <stdint.h>

#define M2M_DEVICE_ID                   "glasses01"
#define M2M_FW_VARIANT_V1               1U
#define M2M_FW_VARIANT_V2               2U
#define M2M_FW_VARIANT_V3               3U
#define M2M_FW_VARIANT_V4               4U
#define M2M_DEMO_FW_VARIANT             M2M_FW_VARIANT_V3

#if (M2M_DEMO_FW_VARIANT == M2M_FW_VARIANT_V1)
#define M2M_FIRMWARE_VERSION            "devboard-mvp-a10-otau-v1"
#define M2M_STARTUP_WELCOME_ENABLED     1U
#define M2M_STARTUP_WELCOME_TEXT        "Welcome to Mind2Matter V1."
#elif (M2M_DEMO_FW_VARIANT == M2M_FW_VARIANT_V2)
#define M2M_FIRMWARE_VERSION            "devboard-mvp-a10-otau-v2"
#define M2M_STARTUP_WELCOME_ENABLED     1U
#define M2M_STARTUP_WELCOME_TEXT        "Welcome to Mind2Matter V2."
#elif (M2M_DEMO_FW_VARIANT == M2M_FW_VARIANT_V3)
#define M2M_FIRMWARE_VERSION            "devboard-mvp-a10-otau-v3"
#define M2M_STARTUP_WELCOME_ENABLED     1U
#define M2M_STARTUP_WELCOME_TEXT        "Welcome to Mind2Matter V3."
#elif (M2M_DEMO_FW_VARIANT == M2M_FW_VARIANT_V4)
#define M2M_FIRMWARE_VERSION            "devboard-mvp-a10-otau-v4"
#define M2M_STARTUP_WELCOME_ENABLED     1U
#define M2M_STARTUP_WELCOME_TEXT        "Welcome to Mind2Matter V4. Firmware update complete. OTA demo ready."
#else
#error "Unsupported M2M_DEMO_FW_VARIANT"
#endif

#define M2M_MQTT_CLIENT_ID              "mind2matter-glasses01-devkit"
#define M2M_PUBLIC_IP                   "20.119.220.234"
#define M2M_MQTT_BROKER_IP              M2M_PUBLIC_IP
#define M2M_MQTT_BROKER_PORT            1883
#define M2M_MQTT_CLIENT_PORT            1
#define M2M_MQTT_KEEPALIVE_SECONDS      60
#define M2M_MQTT_CONNECT_TIMEOUT_MS     5000
#define M2M_MQTT_DISCONNECT_TIMEOUT_MS  1000U
#define M2M_MQTT_PUBLISH_TIMEOUT_MS     0U
#define M2M_MQTT_PUBLISH_RETRY_COUNT    4U
#define M2M_MQTT_PUBLISH_RETRY_DELAY_MS 150U

#define M2M_HTTP_FILE_HOST_IP          M2M_PUBLIC_IP
#define M2M_HTTP_FILE_HOST_NAME        M2M_PUBLIC_IP
#define M2M_HTTP_FILE_HOST_PORT        80
#define M2M_HTTP_FILE_HOST_TLS         0
#define M2M_HTTP_API_BASE_URL          "http://" M2M_HTTP_FILE_HOST_IP "/api"
#define M2M_HTTP_UPLOAD_API_URL        M2M_HTTP_API_BASE_URL "/upload"
#define M2M_DEFAULT_OTA_RESOURCE       "firmware/fp_fw_2708.rps"

#define M2M_TOPIC_DEVICE_STATUS        "mind2matter/device/glasses01/status"
#define M2M_TOPIC_DEVICE_FALL          "mind2matter/device/glasses01/event/fall"
#define M2M_TOPIC_DEVICE_REMINDER_ACK  "mind2matter/device/glasses01/reminder/ack"
#define M2M_TOPIC_DEVICE_DEBUG_BUTTON  "mind2matter/device/glasses01/debug/button"
#define M2M_TOPIC_DEVICE_VOICE_REQUEST "mind2matter/device/glasses01/voice/request"
#define M2M_TOPIC_DEVICE_OTA_STATUS    "mind2matter/device/glasses01/ota/status"

#define M2M_TOPIC_CLOUD_DEBUG_LED_SET "mind2matter/cloud/glasses01/debug/led/set"
#define M2M_TOPIC_CLOUD_REMINDER_SET  "mind2matter/cloud/glasses01/reminder/set"
#define M2M_TOPIC_CLOUD_VOICE_REPLY   "mind2matter/cloud/glasses01/voice/reply"
#define M2M_TOPIC_CLOUD_SYSTEM_NOTICE "mind2matter/cloud/glasses01/system/notice"
#define M2M_TOPIC_CLOUD_OTA_UPDATE    "mind2matter/cloud/glasses01/ota/update"

#define M2M_CLOUD_TX_QUEUE_DEPTH     12
#define M2M_BUTTON_QUEUE_DEPTH       8
#define M2M_SENSOR_QUEUE_DEPTH       4
#define M2M_AUDIO_QUEUE_DEPTH        6
#define M2M_VOICE_QUEUE_DEPTH        3
#define M2M_OTA_QUEUE_DEPTH          2
#define M2M_MQTT_PAYLOAD_MAX         640
#define M2M_AUDIO_PROMPT_TEXT_MAX    160
#define M2M_REMINDER_CONFIG_TEXT_MAX 192
#define M2M_OTA_URL_MAX              192
#define M2M_OTA_RESOURCE_MAX         160
#define M2M_OTA_TARGET_VERSION_MAX   48
#define M2M_OTA_STATUS_TEXT_MAX      48

#define M2M_STATUS_PERIOD_MS             10000U
#define M2M_BUTTON_POLL_PERIOD_MS        20U
#define M2M_BUTTON_DEBOUNCE_MS           60U
#define M2M_BUTTON_LONG_PRESS_MS         1800U
#define M2M_WIFI_RUN_STARTUP_SCAN_DIAGNOSTIC 1U
#define M2M_FALL_CONFIRM_TIMEOUT_MS      30000U
#define M2M_IMU_SAMPLE_PERIOD_MS         20U
#define M2M_VOICE_CAPTURE_START_BUDGET_MS 300U
#define M2M_VOICE_CAPTURE_AUTO_STOP_MS    60000U
#define M2M_VOICE_REPLY_WAIT_MS        30000U
#define M2M_VOICE_REPLY_AUDIO_DOWNLOAD_TIMEOUT_MS 8000U
#define M2M_OTA_STATUS_FLUSH_MS          500U
#define M2M_OTA_RECONNECT_WAIT_MS        5000U
#define M2M_OTA_REBOOT_DELAY_MS          1500U
#define M2M_WIFI_STARTUP_SETTLE_MS       1500U
#define M2M_WIFI_INIT_WWDT_INTERRUPT_DELAY 16U
#define M2M_WIFI_INIT_WWDT_RESET_DELAY     17U
#define M2M_STARTUP_WELCOME_WAIT_MS      15000U

#define M2M_DEVBOARD_BATTERY_PERCENT         87U
#define M2M_DEVBOARD_UPLOAD_BASE_URL         "http://" M2M_HTTP_FILE_HOST_IP "/uploads/devkit"
#define M2M_HTTP_RESPONSE_JSON_MAX           1024U
#define M2M_HTTP_DOWNLOAD_MAX_BYTES          (5U * 1024U * 1024U)
#define M2M_DEVBOARD_BOOT_LED_PATTERN_COUNT  1U
#define M2M_DEVBOARD_BOOT_LED_PATTERN_ON_MS  120U
#define M2M_DEVBOARD_BOOT_LED_PATTERN_OFF_MS 120U

#define M2M_AUDIO_CAPTURE_SAMPLE_RATE_HZ     16000U
#define M2M_AUDIO_CAPTURE_BITS_PER_SAMPLE    16U
#define M2M_AUDIO_CAPTURE_CHANNEL_COUNT      1U
#define M2M_VOICE_RECORDING_MAX_PCM_SAMPLES  ((M2M_AUDIO_CAPTURE_SAMPLE_RATE_HZ * M2M_VOICE_CAPTURE_AUTO_STOP_MS) / 1000U)
#define M2M_VOICE_RECORDING_MAX_PCM_BYTES    (M2M_VOICE_RECORDING_MAX_PCM_SAMPLES * sizeof(int16_t))
#define M2M_VOICE_RECORDING_MAX_WAV_BYTES    (44U + M2M_VOICE_RECORDING_MAX_PCM_BYTES)

#endif // M2M_APP_CONFIG_H
