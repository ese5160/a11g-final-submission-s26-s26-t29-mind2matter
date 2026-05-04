# Mind2Matter MCU Firmware

This folder contains the current Mind2Matter firmware for the SiLabs BRD2708A / SiWx917 bring-up platform. The firmware combines peripheral access, RTOS task orchestration, MQTT connectivity, voice-request handling, reminder audio, and HTTP-based OTA updates in a single application image.

## Firmware Responsibilities

- Initialize Wi-Fi, MQTT, PSRAM, and board-level peripherals
- Sample the IMU and generate fall-related events
- Handle button-driven interaction and local prompts
- Upload request media and download reply audio over HTTP
- Receive cloud replies, reminders, notices, and OTA commands over MQTT
- Download `.rps` firmware images and reboot into the updated version

## Hardware Interface Summary

| Function | Interface | Current Pin Mapping | Notes |
|----------|-----------|---------------------|-------|
| IMU | I2C | Auto-probe: `GPIO71/GPIO70`, `GPIO46/GPIO47`, `GPIO15/GPIO47` | The active route is printed at boot. |
| User button | GPIO input | `HP29` | Used for push-to-talk and local interactions. |
| Debug LED | GPIO output | `HP10` | Used for visible debug / demo indication. |
| Debug VCOM | UART | `TX=ULP11`, `RX=ULP9` | Main serial log path. |
| Camera control path | UART + reset | `TX=HP7`, `RX=HP6`, `RESET=HP12` | Current firmware keeps camera integration behind the board abstraction layer. |
| Audio playback path | I2S0 | `SCLK=HP25`, `WS=HP26`, `DIN=HP27`, `DOUT=HP28` | External amplifier enable is assumed to be asserted in hardware. |

## RTOS Architecture

The main runtime is organized around a few focused tasks:

- `mqtt_task`: Wi-Fi + MQTT connectivity and cloud message transport
- `system_control_task`: routes button events and cloud-side control actions
- `imu_task`: reads IMU data and produces fall-related events
- `voice_ai_task`: media upload, voice request flow, and reply download
- `reminder_audio_task`: local reminder / prompt playback
- `ota_task`: OTA state handling and update execution
- `status_indicator_task`: local LED state indication for headless operation

The board-specific driver boundary remains in `m2m_dev_board.c`, which makes later migration to a custom PCBA much easier.

## Local LED and Tone Indicators

The device can be used away from USB serial logs. HP10 is the primary local status LED, and short local I2S tones mark important transitions. These indicators are generated locally and do not depend on cloud TTS. Startup welcome speech can still be used by demo firmware variants, but it is optional and cloud-dependent.

In this headless demo mode, the local status indicator owns HP10. Cloud debug LED commands are still accepted and reported in state, but the physical HP10 output may be overwritten by the current local status pattern.

### Reading the Device Without USB

Wait for the very short heartbeat every 3 seconds before starting the next interaction. That pattern means Wi-Fi is connected, MQTT topics are subscribed, and the next button activation should be safe. If the LED keeps showing Wi-Fi or MQTT connection patterns, the device is online at the lower layer but not fully ready for cloud interaction. If the LED freezes completely instead of continuing a pattern, assume the firmware is stuck or resetting.

### Continuous LED Patterns

| LED Pattern | System State | Meaning |
|-------------|--------------|---------|
| Fast blink, about 5 Hz | Boot / startup self-test | Firmware is alive and RTOS, board peripherals, and startup diagnostics are still initializing. A short legacy boot pulse may blend with this pattern. |
| One short blink per second | Wi-Fi connecting | Wi-Fi startup, scan, or join is in progress. |
| Two short blinks repeating | MQTT connecting / offline | Wi-Fi is up, but MQTT is not fully connected and subscribed. Cloud publishes may be dropped while this pattern is active. |
| Very short heartbeat every 3 seconds | Ready | Wi-Fi and MQTT are up, cloud topics are subscribed, and the next short press or long press is ready. |
| Fast blink, about 5 Hz after startup | Voice / vision recording | Microphone capture is active after a short-press QA request or long-press vision request. |
| Medium blink, 50% duty | Uploading | Audio or image upload is in progress. |
| Three short blinks repeating | Waiting for cloud reply | Request metadata was sent, and the firmware is waiting for the cloud response. |
| Two short blinks repeating quickly | Downloading | Reply audio or other HTTP content is being downloaded. |
| Mostly on with a brief dip | Playing audio | Local prompt playback or downloaded reply playback is active. |
| Four short blinks repeating | OTA active | OTA was requested or the `.rps` firmware image is being downloaded. Do not press the button or power-cycle during this pattern. |
| Solid on immediately before reset | OTA success handoff | Firmware download completed, and the board is about to reboot into the new image. |
| Slow 1 Hz blink | Error / local failure | A local failure was detected, such as failed Wi-Fi startup. If this freezes completely, the system is likely stuck or resetting. |

### Local Tone Cues

| Tone Cue | Trigger | Meaning |
|----------|---------|---------|
| Startup speaker self-test tone | Startup diagnostic speaker test | Confirms that the I2S playback path and external speaker path are working. |
| Rising three-step chirp | MQTT subscriptions complete | System is fully ready for the next activation. |
| Voice-start tone | Short press accepted | QA voice capture has started. |
| Vision-start tone | Long press accepted | Vision request capture has started. |
| Record-stop/upload tone | Recording stopped by button or timeout | Capture is complete, and upload is starting. |
| Request-sent tone | Voice request metadata sent | The device is waiting for the cloud reply. |
| Two sharp high beeps | Voice request rejected while busy | The button press was understood, but the device is already recording, uploading, waiting, playing, or otherwise busy. |
| Descending low failure tone | Any local failure detail ending in `failed` | A local failure occurred, such as Wi-Fi failure, upload failure, invalid reply, download failure, playback failure, or OTA failure. |
| OTA status tone | OTA request accepted, OTA busy, or OTA status change | OTA control flow is active. Watch the LED for OTA progress. |
| Fall-confirm tone or spoken prompt | Possible fall detected | The device is asking for user confirmation. Pressing the button while fall confirmation is pending cancels the fall alert. |
| Reminder / notice tone | Cloud reminder or system notice | A cloud or system notification was received. Some notices may use cloud TTS when available. |
| Variant welcome speech | Firmware variant startup prompt after cloud readiness | Optional demo speech used to identify V1, V2, V3, or V4 after boot or OTA. V4 currently says: `Welcome to Mind2Matter V4. Firmware update complete. OTA demo ready.` |

### Demo Interaction Checklist

1. Power on the board. Expect fast boot blink and the startup speaker self-test tone.
2. Wait for network readiness. Wi-Fi shows one blink per second, MQTT shows two short blinks, and full ready shows the heartbeat every 3 seconds plus the rising ready chirp.
3. Short press for QA. Expect the voice-start tone, recording blink, upload blink, waiting-for-reply blink, download blink, and playback LED pattern.
4. Long press for vision. Expect the vision-start tone followed by the same recording, upload, waiting, download, and playback sequence.
5. Trigger OTA from the cloud dashboard or Node-RED. Expect the OTA status tone, four-blink OTA pattern, solid LED before reset, reboot, and the new firmware variant welcome prompt when cloud speech is available.
6. If a request is ignored, listen for the busy beeps. If a failure tone plays or the LED stays in Wi-Fi/MQTT/error patterns, check network, MQTT, HTTP upload/download, or OTA resource availability.

## Build

### CLI Build

```bash
cd fp_fw_2708/cmake_gcc
cmake --preset project
cmake --build --preset default_config
```

### Simplicity Studio Build

Open `fp_fw_2708.slpb` in Simplicity Studio and build the `fp_fw_2708` project normally.

### Output Artifacts

Artifacts are generated under `fp_fw_2708/cmake_gcc/build/base/`:

- `fp_fw_2708.rps` for OTA
- `fp_fw_2708.hex` for local flash
- `fp_fw_2708.out`
- `fp_fw_2708.bin`
- `fp_fw_2708.s37`

## Local Configuration

The firmware expects Wi-Fi credentials in:

- `fp_fw_2708/config/wifi_secrets.h`

This file is intentionally ignored by Git.

Example:

```c
#pragma once
#define DEFAULT_WIFI_CLIENT_PROFILE_SSID "YourNetwork"
#define DEFAULT_WIFI_CLIENT_CREDENTIAL   "YourPassword"
```

## Firmware Variants for Demo

The current demo firmware supports four variants in `m2m_app_config.h`:

- `M2M_FW_VARIANT_V1`
- `M2M_FW_VARIANT_V2`
- `M2M_FW_VARIANT_V3`
- `M2M_FW_VARIANT_V4`

Select the active build with:

```c
#define M2M_DEMO_FW_VARIANT M2M_FW_VARIANT_V1
```

The checked-in configuration currently selects `M2M_FW_VARIANT_V3`.

Current demo behavior:

- `v1` reports firmware version `devboard-mvp-a10-otau-v1`
- `v2` reports firmware version `devboard-mvp-a10-otau-v2`
- `v3` reports firmware version `devboard-mvp-a10-otau-v3`
- `v4` reports firmware version `devboard-mvp-a10-otau-v4` and uses an OTA-demo-specific startup TTS prompt
- each variant can play a version-specific startup welcome prompt for OTA demonstration clarity

## Cloud Contract

MQTT broker settings:

- Host: `20.119.220.234`
- Port: `1883`

HTTP settings:

- File host: `http://20.119.220.234/`
- API base: `http://20.119.220.234/api`
- OTA default resource: `firmware/fp_fw_2708.rps`

Device-published topics:

- `mind2matter/device/glasses01/status`
- `mind2matter/device/glasses01/event/fall`
- `mind2matter/device/glasses01/reminder/ack`
- `mind2matter/device/glasses01/debug/button`
- `mind2matter/device/glasses01/voice/request`
- `mind2matter/device/glasses01/ota/status`

Cloud-published topics consumed by the MCU:

- `mind2matter/cloud/glasses01/debug/led/set`
- `mind2matter/cloud/glasses01/reminder/set`
- `mind2matter/cloud/glasses01/voice/reply`
- `mind2matter/cloud/glasses01/system/notice`
- `mind2matter/cloud/glasses01/ota/update`

## OTA Workflow

1. Build and locally flash a baseline image.
2. Build the update image and collect `fp_fw_2708.rps`.
3. Publish the `.rps` file into `cloud_stack/public/firmware/`.
4. In the Node-RED dashboard, enter the relative resource path such as `firmware/fp_fw_2708_v2.rps`.
5. Trigger `Start OTA`.
6. The MCU downloads the image, reboots, and reports the new firmware version.

## Porting Boundary for the Custom PCBA

The cleanest customization boundary is still `m2m_dev_board.c`. Future custom-board work should primarily replace:

- `m2m_dev_board_read_imu()`
- `m2m_dev_board_start_audio_capture()`
- `m2m_dev_board_stop_audio_capture()`
- `m2m_dev_board_capture_image_stub()`
- `m2m_dev_board_play_prompt()`

This keeps the application queues, topic contract, and task layout stable while the hardware implementation evolves.

## Release vs Local Debug Notes

- This README is the submission-oriented release document for the MCU firmware.
- Local debug notes or vendor references can be kept as `*.local.md` files and are ignored by Git.
