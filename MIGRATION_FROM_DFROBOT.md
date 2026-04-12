# CamS3 Migration Checklist — Feature Parity with DFRobot v3.12

Reference firmware: `~/Plocha/Verze: 3.2.5 (OV3660 Opt + Snapshot Fix)/src/ESP32_Firmware_PDM_Core3/`
Target firmware: `~/Plocha/M5Stack/CamS3-Firmware/`

## Hardware Differences

| | DFRobot FireBeetle2 | M5Stack CamS3 |
|---|---|---|
| MCU | ESP32-S3-WROOM-1-N16R8 | ESP32-S3-WROOM-1-N16R8 |
| Sensor | OV3660 (3MP) | PY260 / OV5640 (5MP) |
| PSRAM | 8MB OPI | 8MB OPI |
| Flash | 16MB | 16MB |
| Lux sensor | LTR-308 (I2C) | None |
| Microphone | PDM (GPIO 38/39) | None on board (pins available) |
| IR LED | GPIO-controlled | None |
| SD Card | MicroSD slot | None (external via GPIO) |
| Web framework | ESP-IDF httpd (3 ports: 80/81/82) | AsyncWebServer (single port) |
| JSON lib | ArduinoJson v6 | ArduinoJson v7 |

## What CamS3 Already Has

- [x] Motion detection (EMA background model, block grid, spatial+temporal filters)
- [x] Face detection (MSR01/MNP01 cascade — Espressif esp-dl)
- [x] Telegram notifications (text + photo, queue-based async)
- [x] MJPEG streaming (/stream endpoint)
- [x] Timelapse (periodic JPEG snapshots)
- [x] Config persistence (LittleFS JSON)
- [x] OTA updates (ElegantOTA)
- [x] mDNS
- [x] MQTT (publish-only)
- [x] LED control
- [x] Active hours scheduling
- [x] WiFi manager with reconnect
- [x] Health watchdog

## What's Missing (vs DFRobot v3.12)

### Priority 1 — Core Detection Upgrade

| Feature | Files to create/modify | Effort | Notes |
|---|---|---|---|
| **FOMO Person Detection** | `person_detection.h/cpp` (new), `lib/ei-person-detection-fomo/` (new) | 3-5 days | Replace or complement face detection with Edge Impulse FOMO 64x64 grayscale. Requires EI Studio export (.zip Arduino library). Train on OV5640 images for better accuracy. Pipeline: JPEG → RGB565 → grayscale → bilinear resize → FOMO inference. ESP-NN=0 (crashes on ESP32-S3 with PSRAM). |
| **ByteTrack Tracker** | `tracker.h/cpp` (new, ~300 lines total) | 2-3 days | Greedy nearest-neighbor matching on FOMO centroids. Persistent track IDs across frames. States: tentative→confirmed→lost→deleted. Prevents duplicate Telegram notifications per person. |
| **4-Layer Detection Cascade** | Modify `motion_detect.cpp`, `main.cpp` | 2 days | Motion task signals person detection task via FreeRTOS semaphore (not sequential in same task). Temporal filter: require 2 consecutive FOMO detections before confirming. |
| **Temporal Filter** | Inside `person_detection.cpp` | included above | `PD_TEMPORAL_FRAMES=2` consecutive detections to confirm. Eliminates ~90% false positives. |

### Priority 2 — Stability & Memory

| Feature | Files to modify | Effort | Notes |
|---|---|---|---|
| **LITE_MODE_NO_RUNTIME_TASKS** | `platformio.ini` (build flag), `main.cpp` | 1 day | Skip boot-time starts of unused tasks (audio, RTSP, recording, timelapse). Frees ~42 KB SRAM for SSL handshakes. Critical if Telegram fails with "esp-sha: Failed to allocate buf memory". |
| **Telegram Heap Drift Fix** | `telegram.cpp` | 0.5 day | `checkTelegramUpdates()`: replace String concat with `char[256]+snprintf`, `DynamicJsonDocument` with `StaticJsonDocument<1536>`, zero-copy text extraction, `atol()` chat_id compare. Polling interval 5s → 15s (configurable `TELEGRAM_POLL_INTERVAL_MS`). Without fix: ~10 KB/min heap leak. |
| **CAS-based Ring Buffer** | `camera_manager.cpp` | 1-2 days | Atomic compare-exchange for frame buffer sharing. Enables zero-copy multi-consumer (motion + FOMO read same frame simultaneously). Current semaphore approach serializes readers. |

### Priority 3 — Sensor & Camera Profiles

| Feature | Files to create/modify | Effort | Notes |
|---|---|---|---|
| **LTR-308 Ambient Light Sensor** | `ir_handler.cpp` (new) | 1-2 days | CamS3 has no LTR-308 on board. Options: (A) add external I2C LTR-308 breakout, (B) estimate brightness from frame histogram (software-only, less accurate). |
| **Camera Profiles (DAY/DUSK/NIGHT)** | `camera_manager.cpp` | 1 day | Automatic brightness/contrast/AEC/AGC switching based on lux. OV5640 has different register map than OV3660 — profiles need re-tuning. Hysteresis thresholds prevent rapid switching. |
| **IR LED Auto-Control** | `ir_handler.cpp` | 1 day | CamS3 has no IR LED. Skip unless external IR illuminator is added. |

### Priority 4 — Recording & Media

| Feature | Files to create | Effort | Notes |
|---|---|---|---|
| **AVI Recording** | `avi_writer.h/cpp` (new, ~300 lines) | 3-4 days | MJPEG AVI container with optional I2S audio track. Auto-start on motion, auto-stop after timeout. SD card file lifecycle (auto-delete when >90% full). CamS3 has no SD slot — needs external SD module or skip entirely. |
| **RTSP Server** | `lib/ESP32-RTSPServer/` (copy from DFRobot) | 1 day | Local library copy. Enables VLC/NVR access. Port 554 default. |
| **PDM Audio** | `audio_handler.h/cpp` | 2-3 days | CamS3 board has no mic. Skip unless external I2S mic added. |

### Priority 5 — Web & Integrations

| Feature | Files to modify/create | Effort | Notes |
|---|---|---|---|
| **Telegram Bot Commands** | `telegram.cpp` | 1-2 days | Add `handleTelegramCommand()` with `/foto`, `/video`, `/restart`, `/status`, `/help`, `/hlidej`, `/ticho`, `/detekce`, `/osoba`, `/ir`, `/cooldown`, `/prah`. Requires `getUpdates` polling loop. |
| **MQTT Home Assistant Discovery** | `mqtt_handler.cpp` | 1 day | Auto-discovery payloads for camera entity, motion binary sensor, person detection sensor. CamS3 currently publishes only state. |
| **Live Log Streaming** | `ws_log.h/cpp` (new) | 1 day | Ring buffer (100 lines x 200 chars = 20KB BSS). Captures Serial.printf output. Served via `/log` (text) and `/log-viewer` (HTML with auto-refresh). |
| **Gzipped Dashboard** | `web_server.cpp` | 1 day | Compress HTML/CSS/JS into PROGMEM blob. Saves LittleFS space, faster load. CamS3 currently serves from LittleFS `/www/`. |

### Priority 6 — ArduinoJson Migration

| Issue | Impact | Resolution |
|---|---|---|
| CamS3 uses ArduinoJson **v7** | API incompatible with DFRobot code (v6) | Either: (A) downgrade CamS3 to v6, (B) port DFRobot code to v7 syntax, or (C) keep v7 and adapt ported code. Option C recommended — v7 is newer and better. |

Key v6→v7 differences:
- `DynamicJsonDocument` removed in v7 (use `JsonDocument` directly)
- `StaticJsonDocument<N>` removed (use `JsonDocument` with stack allocator)
- `.as<String>()` → `.as<std::string>()` or `const char*`
- `doc.containsKey("x")` → `doc["x"].is<T>()`

## Web Framework Difference (CRITICAL)

CamS3 uses **AsyncWebServer** (event-loop, single port, async handlers).
DFRobot uses **ESP-IDF httpd** (thread-per-connection, 3 ports, sync handlers).

This is the biggest architectural difference. Options:
1. **Keep AsyncWebServer** in CamS3 (recommended) — port DFRobot endpoint logic into async handlers. Less work, keeps ElegantOTA compatibility.
2. **Switch to ESP-IDF httpd** — requires rewriting all CamS3 web handlers. More consistent with DFRobot but loses ElegantOTA.

Recommendation: **Option 1** — keep AsyncWebServer, adapt DFRobot endpoint logic.

## OV5640 vs OV3660 Sensor Notes

- Both supported by `espressif/esp32-camera` library — same `camera_config_t` API
- OV5640 max resolution: 2592x1920 (5MP) vs OV3660: 2048x1536 (3MP)
- ISP register addresses differ — do NOT copy OV3660 `set_reg()` calls (0x5000, 0x5001, 0x5580 etc.)
- OV5640 has its own ISP tuning in `ov5640.c` within esp-camera driver
- FOMO input is 64x64 — both sensors downscale fine via `JPG_SCALE_4X`
- OV3660 register 0x5580 (NR) causes image inversion — OV5640 does NOT have this issue

## Estimated Total Effort

| Priority | Scope | Days |
|---|---|---|
| P1 — Detection upgrade | FOMO + tracker + cascade | 7-10 |
| P2 — Stability | LITE_MODE + heap fix + ring buffer | 2-3 |
| P3 — Sensor profiles | Lux sensor + camera profiles | 2-3 |
| P4 — Recording | AVI + RTSP + audio | 4-6 (skip if no SD/mic) |
| P5 — Web & integrations | Bot commands + MQTT discovery + log | 3-4 |
| P6 — ArduinoJson | v6→v7 compat | 1 |
| **Total** | | **~20-27 days** |

If CamS3 has no SD card and no mic (hardware absent): skip P4 entirely → **~15-20 days**.

If only P1 + P2 (detection + stability): **~10-13 days** for core functionality.

## Quick Start (recommended first steps)

1. Train FOMO model in Edge Impulse on OV5640 images (capture ~200 frames via `/frame`, label in EI Studio)
2. Export Arduino library, add to `lib/ei-person-detection-fomo/`
3. Create `person_detection.h/cpp` (copy from DFRobot, change `PD_INPUT_WIDTH=64`)
4. Create `tracker.h/cpp` (copy from DFRobot verbatim — sensor-independent)
5. Add cascade: motion_detect signals person_detection via semaphore
6. Add `TELEGRAM_POLL_INTERVAL_MS` fix to `telegram.cpp`
7. Test end-to-end: motion → FOMO → tracker → Telegram photo
