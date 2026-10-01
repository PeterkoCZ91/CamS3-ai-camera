# CamS3 5MP Advanced Camera Firmware

[![Build](https://github.com/PeterkoCZ91/CamS3-ai-camera/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/PeterkoCZ91/CamS3-ai-camera/actions/workflows/build.yml)
[![Firmware version](https://img.shields.io/badge/firmware-v2.0.0-2ea44f)](CHANGELOG.md)
[![License: MIT](https://img.shields.io/github/license/PeterkoCZ91/CamS3-ai-camera)](LICENSE)

[![Hardware: M5Stack Unit CamS3 5MP](https://img.shields.io/badge/hardware-M5Stack%20Unit%20CamS3%205MP-orange)](https://docs.m5stack.com/en/unit/Unit-CAMS3%205MP)
[![MCU: ESP32-S3](https://img.shields.io/badge/MCU-ESP32--S3-E7352C?logo=espressif&logoColor=white)](https://www.espressif.com/en/products/socs/esp32-s3)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-project-f5822a?logo=platformio&logoColor=white)](https://platformio.org/)
[![Framework: Arduino](https://img.shields.io/badge/framework-Arduino-00878F?logo=arduino&logoColor=white)](https://github.com/espressif/arduino-esp32)

[![Clients: Web · MQTT · Telegram](https://img.shields.io/badge/clients-Web%20%C2%B7%20MQTT%20%C2%B7%20Telegram-4285F4)](#integrations)

Surveillance firmware for the **M5Stack Unit CamS3 5MP** (ESP32-S3, PY260 sensor):
motion detection on an adaptive background model, cascaded on-device person
detection (Edge Impulse FOMO) with tracking, and a lock-free frame ring buffer
that feeds two MJPEG streams and every detector task from a single capture.
Detection zones and an ROI mask, SD storage with rotation, a Telegram bot, MQTT
with Home Assistant auto-discovery, OTA, and a bilingual (EN/CS) web UI.

It is a young project. The pipeline, the web UI and the integrations work, but
several endpoints are deliberate stubs and one detector is switched off — both
listed honestly in [Known limitations](#known-limitations).

---

> ### ⚠️ Upgrading from an earlier build? Re-flash the filesystem.
>
> `partitions.csv` now carries a **64 kB coredump partition**, taken out of the
> filesystem partition. An older LittleFS image no longer matches the new,
> smaller partition and **will not mount** — the web UI disappears and
> `serveStatic()` serves nothing.
>
> ```bash
> pio run -t upload        # firmware
> pio run -t uploadfs      # ← REQUIRED, not optional
> ```
>
> NVS keeps its offset and size, so the **encrypted secrets (Wi-Fi, Telegram,
> MQTT credentials) survive** the upgrade. Only the filesystem has to be rebuilt.

---

## Hardware

| Item | Value |
|---|---|
| Board | M5Stack Unit CamS3 5MP |
| MCU | ESP32-S3-WROOM-1-N16R8 (built as `esp32-s3-devkitc-1`) |
| Flash | 16 MB, QIO |
| PSRAM | 8 MB, **OPI** (`board_build.arduino.memory_type = qio_opi`) |
| Sensor | PY260, 5 MP (OV5640 derivative) |
| Default capture size | `FRAMESIZE_UXGA` 1600×1200, JPEG quality 8 |
| Storage | microSD over SPI |
| Extras on board | white LED, PDM microphone |

Pin map — `include/board_config.h`:

| Function | GPIO |
|---|---|
| XCLK / PCLK / VSYNC / HREF | 11 / 12 / 42 / 18 |
| SCCB SDA / SCL, RESET | 17 / 41, 21 (no PWDN pin) |
| D0…D7 (`Y2`…`Y9`) | 6, 15, 16, 7, 5, 10, 4, 13 |
| LED, PDM mic DATA / CLK | 14, 48 / 47 |
| SD CLK / MOSI / MISO / CS | 39 / 38 / 40 / 9 |

XCLK runs at 20 MHz. The mic pins are defined but **no audio code exists yet** —
see [Known limitations](#known-limitations).

### Partition layout

```
nvs       0x009000   20 kB   encrypted secrets (survives an FS re-flash)
otadata   0x00E000    8 kB
app0      0x010000    3 MB   OTA slot A
app1      0x310000    3 MB   OTA slot B
spiffs    0x610000  9.88 MB  LittleFS — web UI, config, event log, stats
coredump  0xFF0000   64 kB   panic dumps
```

The Arduino core is built with `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`; without a
`coredump` partition every panic dump is silently discarded. The 64 kB comes out of
the filesystem — hence the warning at the top of this file.

### Build footprint

| Build | Flash (of 3 MB app slot) | DRAM (of 320 kB) |
|---|---|---|
| Default | **45.9 %** (≈1.44 MB) | **21.7 %** (≈71 kB) |
| `-DINCLUDE_FACE_DETECT` | 67.1 % (≈2.11 MB) | 24.7 % (≈81 kB) |
| `-DLITE_MODE` | 45.6 % | 21.4 % |
| without `-DINCLUDE_PERSON_DETECT` | 42.7 % | 19.8 % |

esp-dl's face models cost roughly **660 kB of flash** on their own.

## Quick start

```bash
git clone <this-repo> cams3-firmware
cd cams3-firmware
pio run                                   # build
pio run -t upload  --upload-port /dev/ttyACM0
pio run -t uploadfs --upload-port /dev/ttyACM0   # web UI → LittleFS
pio device monitor -b 115200
```

Detailed first-flash instructions, USB/CDC notes and recovery steps live in
[`docs/FIRST_FLASH.md`](docs/FIRST_FLASH.md). If a large write dies halfway with
"The chip stopped responding", `python3 tools/flash_chunked.py` writes the image in
64 kB pieces; `python3 tools/provision_wifi.py --ssid <name>` stores WiFi credentials
over USB when no phone is at hand to use the setup portal.

### The FOMO model

`src/person_detection.cpp` includes `<Person_detection_FOMO_inferencing.h>`
directly, so **without an Edge Impulse FOMO export in `lib/ei-person-fomo/` the
build fails on that include — on purpose.** A missing model should be a visible
error, not a detector that silently returns nothing. Either drop in a model
(see [`docs/fomo_setup.md`](docs/fomo_setup.md)) or comment out
`-DINCLUDE_PERSON_DETECT` in `platformio.ini`.

The model is **not** in this repository. An Edge Impulse Arduino export is ~24 MB and
1300+ generated files (the EI SDK with a vendored TensorFlow Lite Micro), so
`lib/ei-person-fomo/` is gitignored. Committing it would bloat every clone forever,
bury real changes under generated diffs, and publish one specific trained network as
if it were part of the firmware. Bring your own — trained on your own footage, or from
a public Edge Impulse project.

### First boot

1. With no stored Wi-Fi credentials the firmware starts an AP + captive portal:
   SSID **`CamS3-Setup`**, password **`cams3admin`**. Join it, open any URL, pick
   your network on the Wi-Fi page.
2. Once associated, reach the camera at `http://cams3.local` (mDNS, hostname
   default `cams3`) or `http://<camera-ip>`.
3. **Change the default admin password immediately** — it ships as `admin` /
   `admin`. Until you do, `/api/status` reports `"default_password": true` and the
   admin page shows a banner.

| URL | What it is |
|---|---|
| `http://<camera-ip>/` | web UI |
| `http://<camera-ip>:81/stream` | MJPEG, GUI role (max 3 clients) |
| `http://<camera-ip>:81/detection-stream` | MJPEG, analytics role (max 2 clients) |
| `http://<camera-ip>:81/snapshot` | single JPEG |
| `http://<camera-ip>/update` | ElegantOTA (Basic Auth) |
| `http://<camera-ip>/log-viewer` | live log, auto-refresh |

The full HTTP surface — every endpoint, parameter and JSON schema — is in
[`docs/API.md`](docs/API.md).

## Architecture

One capture task feeds a PSRAM ring buffer; every other consumer reads from it.
Capture owns core 1 together with the Arduino loop, everything else runs on core 0.

| Task | Core | Prio | Stack | Job |
|---|---|---|---|---|
| `capture` | 1 | 6 | 4 kB | `esp_camera_fb_get()` → ring buffer, FPS accounting, deferred sensor writes |
| `motion` | 0 | 3 | 6 kB | JPEG decode, background model, ROI, triggers the cascade |
| `persondet` | 0 | 2 | 8 kB | FOMO inference + tracker, waits on the cascade semaphore |
| `facedet` | 0 | 2 | 8 kB | only when `INCLUDE_FACE_DETECT` is on (off by default) |
| `timelapse` | 0 | 2 | 4 kB | interval capture to SD |
| `telegram` | 0 | 2 | 8 kB | send queue + `getUpdates` polling |
| `mqtt` | 0 | 2 | 6 kB | broker connection, discovery, telemetry |
| stream `httpd` | 0 | — | 8 kB | MJPEG server on port 81 |
| Arduino `loop` | 1 | — | — | Wi-Fi supervision, health watchdog, WebSocket status broadcast |

Every long-running task subscribes to the **task watchdog** (`esp_task_wdt_init(20, true)`,
panic on trigger → coredump + reset).

### The frame ring buffer

This is the core of the design. One capture serves every consumer with **zero
copies after the capture** — readers get a raw pointer into a PSRAM slot and
hold a reference count while they use it.

```
   capture task ──▶ [ ring buffer: 3 slots × 256 kB, PSRAM ] ──┬──▶ /stream  :81
                                                              ├──▶ /detection-stream
                                                              ├──▶ motion ─sem─▶ person
                                                              ├──▶ timelapse → SD
                                                              └──▶ /snapshot, Telegram

  ringBuffer[3]   slot = { uint8_t* data, len, width, height, timestamp_ms,
                           volatile int ref_count }

  ref_count protocol (all accesses via __atomic_*, SEQ_CST):
        -1  writer owns the slot     0  free     > 0  number of active readers

  WRITER (capture task)                READER (stream / detector)
  ─────────────────────                ──────────────────────────
  for i in 0..2:                       idx = load(latestIndex)
    idx = (writeIndex+i) % 3           if idx < 0: fail
    CAS ref_count 0 → -1               CAS ref_count n → n+1   (n >= 0)
    └─ success? take the slot            └─ retry up to 16× with taskYIELD()
                                         └─ ref_count < 0 → writer owns it, fail
  memcpy(slot.data, fb->buf, len)
  slot.len/width/height/ts = …         use slot.data …          ← contents stable:
  store(ref_count, 0)   ← release          the writer can only claim ref_count == 0
  store(latestIndex, idx) ← publish
                                       ringBufferRelease(handle)
  no free slot → ringDroppedFrames++     └─ CAS n → n-1, never below 0
```

Details that matter:

- **Release before publish.** `ref_count` returns to `0` *before* `latestIndex` is
  stored, so a reader that sees the new index never finds `-1`.
- **Slots are allocated once per boot and never freed**, not even across a camera
  reinit — readers hold raw pointers into them, so freeing under a streaming client
  was a use-after-free, and 3 × 256 kB of churn re-fragmented PSRAM every time. A
  reinit only invalidates `latestIndex` and the slot lengths.
- **Per-frame dimensions.** Each slot records the size the frame was *captured* at;
  decoders size their buffers from that, never from `appConfig.camera.frame_size`,
  which can change between capture and decode.
- **Dropped frames are counted.** All three slots held → the frame is discarded and
  `ringDroppedFrames` increments (logged every 50th). A stalled consumer used to
  look like low FPS.
- **JPEG decoding is serialized** behind `cameraDecodeLock()`: tjpgd keeps decode
  state in static globals and is not reentrant.

Adaptive frame rate: the capture task targets `active_fps` (default 15) while any
stream client is connected, `idle_fps` (default 1) otherwise.

## Detection pipeline

Motion is the cheap gate; FOMO only runs on frames motion already flagged.

```
   frame ──▶ MOTION (5 fps)                    ──▶ PERSON (~3 fps, gated)
             │                                      │
             │ jpg2rgb565 @ 1/8 scale               │ jpg2rgb565 @ 1/8
             │ box-filter → 80×60 gray              │ → gray → bilinear → model size
             │ 4×4 average → 20×15 grid             │ contrast stretch (1st/99th pct)
             │ EMA background + ROI mask            │ FOMO inference
             │ spatial (8-neigh) + temporal filter  │ greedy nearest-neighbour tracker
             ▼                                      ▼
        motion event ──── semaphore ───────▶  NONE / UNCERTAIN / CONFIDENT
             │                                      │
             │ arm 5 s fallback                     ├─ CONFIDENT → Telegram photo + MQTT
             └──────────────────────────────────────┴─ UNCERTAIN → MQTT only
                   quiet after 5 s? send the motion photo after all
```

### Motion detection (`src/motion_detect.cpp`)

Runs at 5 fps:

1. **Decode** at 1/8 scale, then **box-filter** the decoder's real output
   (200×150 at UXGA) down to 80×60 grayscale — area-averaging every source pixel,
   because nearest-neighbour amplified sensor noise on dark scenes — and **4×4
   average** that into the **20×15 = 300-block grid**.
2. **Background model**: per-block EMA, α = 0.5 for the first 15 training frames,
   then 0.92 by day / 0.98 at night. Blocks that *are* moving still drift at
   α = 0.998, so gradual light changes never become permanent positives.
3. **Adaptive threshold** = `threshold + agc_gain × agc_gain_factor`, ×0.70 while
   motion is already active (hysteresis), ×2.5 in night mode. A block counts only
   if the absolute difference clears the threshold **and** the relative
   difference exceeds 12 %.
4. **ROI mask** — one character per block, `'0'` masks it out. Masked blocks keep
   tracking the background (so re-enabling the ROI later does not fire a false
   positive) but never count as motion, and the changed-area percentage is
   computed **over the ROI area**, not the whole grid — otherwise masking half the
   frame would silently halve sensitivity in the other half.
5. **Global-illumination rejects**: a frame-wide brightness jump > 80 resets the
   model to the current frame, and more than `max_area_pct` (50 %) of blocks changed
   is a lighting change, not motion.
6. **Spatial filter** — 8-connectivity, a block survives with ≥ 1 active neighbour
   (≥ 2 at night); then the **area gate** (`min_area_pct`, ×2.5 at night).
7. **Temporal filter** — ≥ 2 consecutive motion frames **whose masks overlap**, so
   two unrelated single-frame noise bursts cannot pass as continuous motion.

One event = one episode of motion bounded by quiet periods; a new event fires on
the rising edge or after `cooldown_sec` during a long episode.

**Night mode** (`avg_brightness < brightness_min`) does not mute detection — it
raises the per-block threshold *and* the required area by 2.5×, because raising
only the threshold left noise-driven single-block clusters able to trigger.

| Setting | Default | Note |
|---|---|---|
| `enabled` | `false` | `training_frames` 15, `ema_alpha_day` / `_night` 0.92 / 0.98 |
| `threshold` | 20 | Telegram `/prah` accepts 5–80 |
| `cooldown_sec` | 10 | re-fire interval during a long episode |
| `min_area_pct` / `max_area_pct` | 5 / 50 | trigger area; above the upper bound → lighting change, reject |
| `agc_gain_factor` | 1.0 | threshold gain per AGC step |
| `brightness_min` | 10 | night-mode boundary |
| `spatial_filter` / `temporal_filter` / `night_suppress` | on | |

`/api/motion/debug` exposes the live internals: changed %, active and clustered
blocks, adaptive threshold, AGC gain, brightness, night flag, decode and
analysis milliseconds.

### Person detection (`src/person_detection.cpp`)

Waits on the cascade semaphore (2 s timeout), respects its own cooldown, then
decodes → grayscale → bilinear resize → **contrast stretch** → FOMO inference.
The resize target is taken from the linked model
(`EI_CLASSIFIER_INPUT_WIDTH`/`_HEIGHT`), falling back to 64 × 64 — the firmware
never assumes a size the model was not trained on.

The contrast stretch matters more than it sounds. FOMO is trained on well-exposed
images, and a night frame from this sensor occupies a narrow band (say 20…70) of
the range, so every feature the model looks for is compressed into a fraction of
the input scale. Stretching the 1st/99th-percentile band to 0…255 recovers the
expected contrast, and it only touches the model input — never the frame that gets
saved or sent. Guard rails: a band already wider than 200 has nothing to gain, and
one narrower than 20 is sensor noise that stretching would turn into fabricated
detail; both are left alone.

The **tensor arena is forced into PSRAM** by overriding `ei_malloc` / `ei_calloc` /
`ei_free`: Edge Impulse's default `MALLOC_CAP_DEFAULT` lands in internal DRAM,
which the async web server, TLS handshakes and camera buffers all compete for.

**Three-state verdict**, both thresholds runtime-tunable and never invertible (if
`confident_threshold < confidence_threshold` the lower value wins for both):

| Verdict | Condition | Action |
|---|---|---|
| `CONFIDENT` | score ≥ `confident_threshold` (0.75) | Telegram photo/text, event log, suppresses the motion fallback |
| `UNCERTAIN` | score ≥ `confidence_threshold` (0.60) | MQTT `<base>/person_uncertain` only — for an external verifier |
| `NONE` | below both | nothing |

### Tracking (`src/tracker.cpp`)

Greedy nearest-neighbour association on detection centroids, closest pair first,
gated by `match_dist` (squared Euclidean); up to 16 tracks and 16 detections per
frame. A new detection starts a track as `TENTATIVE`, which becomes `CONFIRMED`
after `confirm_hits` matches (default 3) and `LOST` when it goes unmatched;
`max_misses` (5) unmatched frames delete it, and a `TENTATIVE` track dies after
just 2. Each track carries a `notified` flag, so a person who stays in frame
produces **one** Telegram message, not one per inference.

### Motion → person fallback

With person detection enabled, an immediate motion photo is mostly noise (a car, a
branch) and a duplicate whenever a person *is* found — but `UNCERTAIN`, the common
case in poor light, used to mean nothing was sent at all and the event was lost.

So a motion event **arms a deferred notification** instead of sending one.
`MOTION_AI_FALLBACK_MS` (5 000 ms, `include/motion_detect.h`) later, the motion task
checks `getLastPersonNotifyTime()`: if the person path notified in the meantime it
stays quiet, otherwise it sends the motion photo after all. The fallback is serviced
unconditionally at the top of the motion loop, so none of the `continue` paths below
it can strand a pending notification.

## Web UI

Six pages in `data/www/` plus three shared assets (`common.js`, `i18n.js`,
`style.css`), served from LittleFS by `serveStatic("/", LittleFS, "/www/")` with
`Cache-Control: public, max-age=86400` and ETag/304 support. No framework, no
build step for the JS — plain ES5-ish scripts and CSS grid.

| Page | Contents |
|---|---|
| `index.html` | live MJPEG box with FPS/resolution badges, snapshot link, 16 status tiles (IP, uptime, heap, PSRAM, FPS, RSSI, chip temperature, day/dusk/night profile, SD, firmware, clients, four detector states), quick toggles for motion / timelapse / face / person / Telegram / LED, and shortcuts to the raw JSON endpoints |
| `settings.html` | 12 collapsible sections: basic, image, exposure & gain, sensor corrections, motion, face, person (FOMO), timelapse, Telegram, MQTT, security, performance |
| `zones.html` | detection-zone editor **and** ROI mask editor over a live preview |
| `gallery.html` | SD browser with breadcrumb navigation, previews, download, delete |
| `wifi.html` | connection card, network scan, manual join (also the captive-portal landing page) |
| `admin.html` | system info, camera counters + reinit, SD status, OTA link, reboot / factory reset behind a confirmation modal, endpoint reference |

**Bilingual (EN / CS).** `i18n.js` carries **271 keys per language** and rewrites
`data-i18n[-placeholder|-title|-aria-label|-alt]` targets in place, so there are
no duplicated pages and no firmware round-trip. The choice lives in
`localStorage['cams3_lang']`, `navigator.language` picks the initial value (`cs`
and `sk` → Czech, everything else English), and switching fires an `i18n:change`
event so pages re-render dynamic content. A missing key leaves the markup default
in place rather than showing a raw key. Firmware-generated text (Telegram
captions) is Czech, in `include/cz_text.h` — which also handles Czech's three
plural forms, `1 osoba` / `3 osoby` / `7 osob`.

### Zone and ROI editor

The overlay is a **CSS grid of `<div>` cells** over the preview, not a canvas —
which is what makes it keyboard-accessible: the grid is `tabindex="0"` and
Space/Enter toggles the focused cell. Grid dimensions are **not hardcoded in the
page**; they come from `GET /api/roi`, so `MOTION_GRID_W`/`_H` (20 × 15 = 300
blocks) stays the single source of truth. ROI mode paints by click or drag (red =
masked out) with select-all / clear / invert and an `active / 300` counter; zone
mode draws rectangles by dragging or by typing X/Y/W/H, with a name, colour and a
per-zone alert flag, capped at the firmware's 8 zones × 4 rectangles.

### SD gallery

Breadcrumb navigation over `GET /api/sd/list`, 24 entries per page, download and
CSRF-protected delete per row, full-screen modal preview. The device cannot
generate thumbnails, so **every preview is the full-resolution JPEG** — the page
says so out loud. Only one page of rows exists at a time and images are
lazy-loaded through an `IntersectionObserver`; previews can also be switched off.

### Build-time gzip

`tools/gzip_www.py` is a PlatformIO *pre* script that never touches `data/`: it
stages a copy under `$BUILD_DIR/fsdata`, gzips
`.html .htm .css .js .json .svg .ico .txt .xml` ≥ 256 B there, and repoints
`PROJECT_DATA_DIR` at the staging dir. Only the `.gz` ships — ESPAsyncWebServer
prefers `.gz` even when both exist, so a plain twin would cost ~4× the flash for
identical bytes. Output is deterministic (`mtime=0`), and it only runs for
`buildfs` / `uploadfs` / `uploadfsota`, so a plain `pio run` is unaffected.

## Integrations

### Telegram (`src/telegram.cpp`)

Set the bot token and chat ID on the settings page. Only **one** chat is
authorized — updates from any other `chat_id` are dropped and logged. Sends are
queued and non-blocking, and photos that fail can spill to SD to be retried.
Polling is a short poll (`getUpdates?timeout=0&limit=5`) driven by the send
queue's timeout, every `poll_interval_ms` (default 30 s).

| Command | Effect |
|---|---|
| `/foto`, `/photo` | latest frame as a photo |
| `/status`, `/stav` | IP, uptime, heap, PSRAM, RSSI, FPS, clients, detector and notify flags, threshold, cooldown, active hours |
| `/detekce` | toggle motion notifications |
| `/obliceje` | toggle face notifications |
| `/osoba` | toggle person (FOMO) notifications |
| `/prah N` | set motion threshold, 5–80 |
| `/hlidej` | watch everything: all notifications + photos on, hours 0–23, all detectors on |
| `/arm` | like `/hlidej`, and clears any active mute |
| `/disarm` | all notifications off, detectors keep running |
| `/ticho [N]` | mute for N minutes (1–1440, RAM-only); with no argument, mute permanently and persist |
| `/hodiny HH-HH` | quiet-hours window, 0–23, overnight wrap supported |
| `/cooldown N` | notification cooldown, 5–3600 s |
| `/ip` | IP, hostname, web and stream URLs |
| `/restart`, `/reboot` | reboot the camera |
| `/help`, `/napoveda`, `/start` | command list |

The eight most-used commands are registered with `setMyCommands`, so Telegram shows
a native slash menu; the rest work but are not advertised. There are no inline
keyboards. Command replies bypass the notification cooldown (otherwise `/foto`
would be silently dropped); event notifications respect both `cooldown_sec` and the
active-hours window.

### MQTT and Home Assistant (`src/mqtt_handler.cpp`)

Auto-discovery publishes **10 entities** under
`homeassistant/<component>/<hostname>/<suffix>/config`, retained, QoS 1:

- **3 × `binary_sensor`** — Motion (`device_class: motion`), Person
  (`occupancy`, plus a JSON attributes topic carrying the count), Face
  (`occupancy`).
- **7 × `sensor`**, all templated out of the one `<base>/status` payload — Uptime
  (s), FPS, Free Heap (kB), Min Heap (kB), Free PSRAM (MB), RSSI (dBm,
  `signal_strength`) and Clients. The last six are
  `entity_category: diagnostic`.

The device block carries manufacturer, model, `sw_version` and the MAC
connection, so all ten group under one HA device. There is deliberately **no
camera entity** — HA should pull the MJPEG stream from port 81 directly.

Topics (`<base>` = `topic_prefix`, default `cams3`):

| Topic | Payload |
|---|---|
| `<base>/availability` | `online` / `offline` — retained, also the LWT |
| `<base>/motion/state`, `<base>/face/state` | `ON` / `OFF`, retained |
| `<base>/person/state` + `/attributes` | state + `{"count":N}` |
| `<base>/person_uncertain` | `{"confidence":0.68,"tracks":1}` |
| `<base>/status` | telemetry JSON, every 30 s |
| `<base>/camera/status/heartbeat` | `{"uptime","free_heap"}`, every 5 s |
| `<base>/camera/status/profile` | `DAY` / `DUSK` / `NIGHT`, retained |
| `<base>/config/set/#` | inbound commands, QoS 1 |

The MQTT task polls the detector flags every 500 ms and publishes the three
`binary_sensor` states on change; only the `UNCERTAIN` verdict is published
directly by the detector, since it has no corresponding entity state.

Three inbound keys are accepted — `motion/enabled`, `motion/threshold` (5–80 only)
and `person/enabled`, each with a legacy flat alias; anything else is logged and
ignored. Publishes made while offline are queued in PSRAM and replayed on
reconnect (retry every 10 s). TLS uses `/ca.pem` from LittleFS if present,
otherwise falls back to `setInsecure()`.

### A12 analytics contract

The companion service (**A12**) does the heavy detection off-device: it consumes
`/detection-stream` continuously and runs its own models. The camera's role in that
pairing is a stable frame source plus cheap always-on gating — not a detector that
occasionally asks for help. A12 polls `/health` for liveness.

**A12 is not in this repository.** It is Python and it lives in the sibling repo:
[PeterkoCZ91/DFR1154-ai-camera](https://github.com/PeterkoCZ91/DFR1154-ai-camera)
→ [`a12_system/`](https://github.com/PeterkoCZ91/DFR1154-ai-camera/tree/main/a12_system).
Set its `camera_url` to this camera and see
[`docs/A12_INTEGRATION.md`](docs/A12_INTEGRATION.md) — including the two places where
the topic and field names do not line up by default.

`GET /a12/status` (alias `/api/a12/status`) is one flat JSON document offered for
automation and dashboards: identity, stream URLs, capture counters, `frame_age_ms`,
detection state including `person_decision` / `person_top_score` /
`person_inference_ms`, and integration flags. Field list, MQTT topic map and the
topic-naming caveats are in
[`docs/A12_INTEGRATION.md`](docs/A12_INTEGRATION.md).

The rule worth repeating here: **analytics consumers must use
`/detection-stream`, not `/stream`.** Detection clients are counted separately, a
lone detection client still pins the capture task to `active_fps`, and the MJPEG
parts carry `X-Timestamp` / `X-Frame-Age` so a consumer can tell a fresh frame
from a stale one.

## Compile-time toggles

All flags live in `platformio.ini` under `build_flags`. Comment a line out to
drop the feature — every guarded module compiles away cleanly.

| Flag | Default | Enables |
|---|---|---|
| `INCLUDE_SD_CARD` | on | SD mount, `sd_store` rotation/breaker, capture saving, gallery endpoints |
| `INCLUDE_MOTION_DETECT` | on | `motion_detect.cpp` + the motion task (prerequisite for person/face/zones) |
| `INCLUDE_PERSON_DETECT` | on | `person_detection.cpp` + tracker; **needs a FOMO model or the build fails** |
| `INCLUDE_FACE_DETECT` | **off** | esp-dl MSR01/MNP01 face task — deprecated upstream (headers removed in arduino-esp32 v3.1+), +660 kB flash, and a panic was observed with it enabled |
| `INCLUDE_ZONES` | on | `zone_manager.cpp`, `/api/zones`, `/api/roi`, ROI mask persistence |
| `INCLUDE_EVENT_LOG` | on | `event_log.cpp`, `/api/events` |
| `INCLUDE_TELEGRAM` | on | bot task, notifications, commands |
| `INCLUDE_MQTT` | on | MQTT client, HA discovery, telemetry |
| `INCLUDE_TIMELAPSE` | on | timelapse task |
| `INCLUDE_OTA` | on | ElegantOTA at `/update` |
| `INCLUDE_MDNS` | on | `<hostname>.local` |
| `INCLUDE_LED_CONTROL` | on | status LED on GPIO 14 |
| `INCLUDE_AVI_WRITER` | on | **currently a no-op** — nothing references it, `avi_writer.cpp` builds either way |
| `LITE_MODE` | off | skips the timelapse, person and face tasks (~12 kB SRAM) |

**Why face detection is off:** `INCLUDE_FACE_DETECT` depends on esp-dl's
`HumanFaceDetectMSR01`, which is deprecated in the Arduino core and **crashes at
runtime on this target**. `src/face_detect.cpp` is kept and still compiles, so
re-enabling it is one line — but a default build must not ship a task that
panics, and it costs ~660 kB of flash and ~3 % more DRAM for the privilege. The
path forward is a FOMO *face* model through the same Edge Impulse pipeline already
used for person detection.

## Operations and resilience

### Health watchdog

The Arduino `loop()` runs a check every 30 s (`src/main.cpp`):

- **Camera liveness** — no capture for 10 s counts a failure; three in a row
  trigger `cameraReinit()`. If reinit *refuses* (the capture task is wedged
  inside the driver, so tearing the driver down would corrupt DMA state) it is
  retried up to 3 times and then escalates to `ESP.restart()`.
- **Heap health** — free heap, minimum-ever heap, **largest contiguous block**,
  fragmentation (`1 − max_block/free`) and drift against the first measurement. Free
  heap alone hides the failure mode that actually bites this board: it stays "big
  enough" while the largest block shrinks below what a TLS handshake or a JPEG copy
  needs. Below 40 kB free an `EVT_LOW_MEMORY` event is persisted, max one per 10 min.
- **Detection counters** — motion events, changed %, brightness, night flag,
  decode/analysis ms, person events, track count, inference ms, last verdict.

### Wi-Fi supervision

Auto-reconnect plus a **silent-death watchdog**: `WiFi.status()` can stay
`WL_CONNECTED` after the link is actually dead, so if no sign of life (non-zero IP
*and* non-zero RSSI) is seen for 90 s the firmware forces a full
disconnect/reconnect cycle. Attempts are rate-limited to one per 30 s and logged as
`EVT_WIFI_RECONNECT`. Credentials are never written through `WiFi.persistent()`.

### Restart accounting and coredumps

`src/system_stats.cpp` keeps a magic+version-guarded record in `/sysstats.bin`:
total restarts, the reset reason by name, per-cause counters (power-on, brownout,
WDT, panic, software, other) and the longest uptime ever, written at most once per
10 minutes so the health loop does not wear the flash. The boot log and
`/api/status` also report whether a **coredump is present in flash** and how large —
otherwise nobody would think to look. The brownout detector is re-enabled at boot.

### SD card durability (`src/sd_store.cpp`)

Every firmware write goes through `sdStoreWriteJpeg()`, which **rotates** the target
directory first (delete oldest until usage is below `SD_MAX_USAGE_PERCENT` 90 %
minus a 5 % margin, capped at 20 deletions per call so a detection task is never
blocked for seconds) and trips a **circuit breaker** after 3 consecutive write
failures, disabling writes for the session and raising `EVT_SD_FAILURE` — the admin
UI can re-arm it. Without this, a full card made `SD.open()` return a false `File`
whose failure was swallowed, so the device kept reporting healthy while storing
nothing. Mounting retries at 20 MHz then 4 MHz, which recovers loose contacts.

### Encrypted secrets (`src/config_manager.cpp`)

The Wi-Fi password, Telegram token and chat ID, and MQTT credentials are **never**
written to the JSON config on LittleFS — they live in NVS as **AES-256-CTR** blobs,
keyed per device:

```
key = HMAC-SHA256( key = 16-byte random salt (NVS, generated once),
                   msg = 8-byte eFuse MAC )
```

Legacy plaintext values are detected and migrated on load. Secrets are read from NVS
*before* the JSON config is parsed, so an unmountable LittleFS no longer takes the
credentials down with it; `POST /api/secrets/clear` wipes them.

### OTA

ElegantOTA at `/update`, behind the same Basic Auth credentials. After a
successful flash the firmware logs the **SHA-256 of the newly written
partition**, and `/api/status` reports the SHA-256 of the *running* partition, so
an operator can verify that what is running matches their build output. That is a
partition hash, not the build-time ELF hash.

## Security and privacy

Everything the firmware sees stays on the device unless you configure Telegram
or MQTT. There is no cloud service, no telemetry and no phone-home.

**What is protected:**

| Mechanism | Scope |
|---|---|
| HTTP Basic Auth | all mutating endpoints, SD list/download/delete, `/credentials`, `/api/csrf`, ElegantOTA |
| CSRF token | 32-hex-char token from `esp_random()`, regenerated each boot, RAM only; `X-CSRF-Token` required on `/api/*` POST/DELETE. The UI's `apiFetch()` wrapper attaches it to every mutating call and re-fetches once on a 403, so a reboot mid-session does not need a page reload. |
| `Origin`-based CSRF | legacy non-`/api/` endpoints (`/settings`, `/record`, `/ir-control`, `/save_frame`) accept a token-less request **only** when neither `Origin` nor `Sec-Fetch-Site` is present, i.e. it clearly did not come from a browser page |
| SD path whitelist | card paths must start with `/captures`, `/timelapse`, `/recordings`, `/telegram_pending` or `/logs` and must not contain `..` |
| Secrets at rest | AES-256-CTR in NVS, key from eFuse MAC + per-device salt |
| Telegram authorization | exactly one `chat_id`; everything else dropped and logged |
| Firmware identity | SHA-256 of the running and newly-flashed OTA partitions |

**What is not — read this before exposing the device:**

- **The MJPEG streams on port 81 are unauthenticated.** `/stream`,
  `/detection-stream` and `/snapshot` run on a separate `esp_http_server` with no
  auth check and `Access-Control-Allow-Origin: *` — anyone who can reach port 81
  can watch the camera.
- **The WebSocket `/ws` is unauthenticated.** Browsers forward cached Basic Auth to
  a WS upgrade inconsistently (Safari omits it), which left the UI stuck at
  "Connecting…" after a successful login. The broadcast carries only non-secret
  status and the socket accepts no commands, but anyone on the LAN can read it.
- **`/api/status`, `/status`, `/telemetry`, `/health`, `/a12/status`,
  `/api/sensor` (read-only register view), `/api/snapshot`, `/api/events`, `/log`,
  `/stream-stats` and `/psram-stats` are public** — that includes a JPEG frame and the log ring buffer.
- **Telegram TLS uses `setInsecure()`** on all four HTTPS paths: no certificate
  validation, so the connection to `api.telegram.org` is not protected against an
  active man-in-the-middle. MQTT TLS verifies *if* you upload a `/ca.pem`;
  otherwise it falls back to `setInsecure()` too.
- **Basic Auth over plain HTTP** — base64, not encryption. Empty credentials
  disable auth entirely, and `Access-Control-Allow-Origin: *` is set on every
  response.
- **Defaults are weak by design**: `admin` / `admin`, and
  `CamS3-Setup` / `cams3admin` for the setup AP. The firmware warns rather than
  auto-generating, because a user without a UART cable would be locked out.
- **The salt lives next to the ciphertext.** NVS encryption defends against someone
  reading a flash dump on a *different* device; it does not defend against an
  attacker who can read this device's flash *and* its eFuse MAC.

**Recommendation:** treat this as a trusted-LAN device. Put it on an isolated
VLAN or behind an authenticating reverse proxy, and do not port-forward it.

## Known limitations

| Item | State |
|---|---|
| Face detection | Off by default. `HumanFaceDetectMSR01` comes from esp-dl's deprecated `dl_lib` API — arduino-esp32 removed those headers in v3.1+, so the pinned core is the last one that compiles it — and it costs ~660 kB of flash. A `LoadStoreError` panic was also observed on this hardware with it enabled; see [`docs/known_issues.md`](docs/known_issues.md) for the signature and what is and is not verified. Code kept and building; one line to re-enable. Migration target: a FOMO face model. |
| Capture resolution | The sensor is 5 MP, but the firmware runs UXGA 1600×1200 (`frame_size 13`). The PY260 driver in the Arduino core cannot sustain capture at QSXGA — it yields 0 fps and errors. Decode buffers are nevertheless sized for QSXGA/8 so no `frame_size` can overflow them. |
| Audio | The PDM microphone pins are in `board_config.h`, but there is no I²S code. `/audio-status` returns `available: false`. |
| AVI recording | `AviWriter` (RIFF/AVI 1.0 MJPEG muxer) is implemented and unit-clean, but nothing calls it. `POST /record` returns HTTP 501. `INCLUDE_AVI_WRITER` currently has no effect. |
| IR illuminator | `/ir-status` and `/ir-control` are compatibility stubs reporting `available: false`; the board has no IR LED. |
| `curl` and gzipped assets | The static handler does not check `Accept-Encoding`, so `curl http://<camera-ip>/style.css` returns raw gzip. Use `curl --compressed`. API endpoints are unaffected. |
| Stream client limits | 3 GUI + 2 detection clients; further connections get HTTP 503. |

More detail, with reproduction steps, in [`docs/known_issues.md`](docs/known_issues.md).

## Troubleshooting

**Web UI is blank / 404, or `LittleFS mount failed` in the boot log.** The
filesystem was not re-flashed after the partition table shrank: run
`pio run -t uploadfs`. `LittleFS.begin(true)` formats on the second attempt, which
loses the config JSON and the event log — the NVS secrets survive.

**Camera init fails / 0 fps.** Check `frame_size`: QXGA and QSXGA are unstable with
this sensor driver, so stay at UXGA (13) or below. The boot log prints the sensor
PID — `0x0000` means the SCCB bus is not answering, usually a reseated flex cable.

**Motion fires constantly.** Raise `threshold` and `min_area_pct`, keep both
filters on, and read `/api/motion/debug`: a high `changed_pct` with nothing moving
means the background model is still training (15 frames) or the scene has a global
light flicker. Mask a swaying branch or a road out with the ROI editor.

**Motion never fires at night.** Night mode multiplies both the threshold and the
required area by 2.5 — lower `brightness_min` so the scene is not classified as
night, or turn `night_suppress` off.

**Person detection finds nothing.** Lower `confidence_threshold`, then check
`raw_detections` in `/api/status`: non-zero while `person_count` is 0 means the
tracker is rejecting detections (tune `confirm_hits` / `match_dist`); zero means
the model itself is not firing.

**`ring buffer full, frame dropped`.** A consumer is holding a slot too long —
usually a stream client on a slow link, or a detector at an oversized
`frame_size`. Reduce `active_fps` or the frame size.

**Rising fragmentation in the health line.** Watch `max_block`, not `heap`: a TLS
handshake needs a large contiguous allocation, so when `max_block` drops below
roughly 40 kB, Telegram sends fail before anything else does.

**Telegram is silent.** Event notifications are gated by the active-hours window
(`/hodiny`), `/ticho` and the cooldown; `/foto` is gated by none of them, so if
`/foto` works and events do not, it is one of those three.

**SD card stopped recording.** Look for `Too many failures — SD writes disabled
for this session`: the breaker tripped. Check `sd_usage_percent`, swap or reformat
the card (FAT), re-arm from the admin page.

**A crash to diagnose.** `/api/status` reports `coredump_present` and
`coredump_size`; pull it with `esptool.py read_flash 0xFF0000 0x10000
coredump.bin` and decode with `espcoredump.py`.

## Documentation

| File | Contents |
|---|---|
| [`docs/API.md`](docs/API.md) | complete HTTP API reference — endpoints, parameters, JSON schemas, auth requirements |
| [`docs/FIRST_FLASH.md`](docs/FIRST_FLASH.md) | flashing a blank board, USB/CDC, recovery |
| [`docs/known_issues.md`](docs/known_issues.md) | known bugs and limitations with reproduction steps |
| [`docs/fomo_setup.md`](docs/fomo_setup.md) | training and dropping in an Edge Impulse FOMO model |
| [`docs/A12_INTEGRATION.md`](docs/A12_INTEGRATION.md) | pairing with the A12 companion detector: what it consumes, the MQTT topic map and where the topic names do not line up |
| [`SECURITY.md`](SECURITY.md) | threat model, what is protected, hardening checklist |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | build/verify steps and the concurrency rules worth knowing before touching tasks |
| [`CHANGELOG.md`](CHANGELOG.md) | version history |

## License

MIT — see [`LICENSE`](LICENSE).

## Acknowledgments

- **[arduino-esp32](https://github.com/espressif/arduino-esp32)** + esp32-camera,
  **[ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer)** (whose
  gzip-aware static handler the build-time compression relies on),
  **[ElegantOTA](https://github.com/ayushsharma82/ElegantOTA)**,
  **[ArduinoJson](https://github.com/bblanchon/ArduinoJson)** and
  **[arduino-mqtt](https://github.com/256dpi/arduino-mqtt)**.
- **[Edge Impulse](https://edgeimpulse.com)** — the FOMO architecture and the
  Arduino-library export pipeline that make on-device person detection practical
  on an ESP32-S3.
- **[PeterkoCZ91/DFR1154-ai-camera](https://github.com/PeterkoCZ91/DFR1154-ai-camera)**
  — the sibling firmware this project takes its feature set, endpoint
  compatibility layer and general shape from.
- **M5Stack** for the [Unit CamS3 5MP documentation](https://docs.m5stack.com/en/unit/Unit-CAMS3%205MP).
