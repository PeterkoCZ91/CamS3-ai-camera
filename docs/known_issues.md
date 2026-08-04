# Known issues and deliberate limitations

An honest list of what this firmware does not do, does badly, or does only under
conditions worth knowing about in advance. Where something is a deliberate trade-off
rather than a bug, the reasoning is included so you can decide whether it is a
trade-off you also want to make.

Contents:

1. [Face detection is disabled in the default build](#1-face-detection-is-disabled-in-the-default-build)
2. [Resolutions above UXGA do not work](#2-resolutions-above-uxga-do-not-work)
3. [Streams and the WebSocket have no authentication](#3-streams-and-the-websocket-have-no-authentication)
4. [Telegram TLS does not validate the server certificate](#4-telegram-tls-does-not-validate-the-server-certificate)
5. [Gallery previews are full-size JPEGs](#5-gallery-previews-are-full-size-jpegs)
6. [AVI recording is not wired up](#6-avi-recording-is-not-wired-up)
7. [Stubbed endpoints for hardware this board does not have](#7-stubbed-endpoints-for-hardware-this-board-does-not-have)
8. [The person detection model is not in this repository](#8-the-person-detection-model-is-not-in-this-repository)
9. [Frames larger than 256 kB are dropped without a trace](#9-frames-larger-than-256-kb-are-dropped-without-a-trace)
10. [Notification text is Czech only](#10-notification-text-is-czech-only)
11. [Smaller deliberate constraints](#11-smaller-deliberate-constraints)
12. [Toolchain is pinned and cannot be upgraded yet](#12-toolchain-is-pinned-and-cannot-be-upgraded-yet)

---

## 1. Face detection is disabled in the default build

`-DINCLUDE_FACE_DETECT` is **commented out** in `platformio.ini`. A stock build has
no face detection task, and `/api/status` omits `face_detected`, `face_count`,
`face_event_count` and `face_inference_ms` (the `face_detect_*` settings keys are
still accepted and reported — they are simply inert).

### Why

`src/face_detect.cpp` uses `HumanFaceDetectMSR01` and `HumanFaceDetectMNP01` from
esp-dl's old `dl_lib` API. On this target that combination crashes:

```
Guru Meditation Error: Core 0 panic'ed (LoadStoreError)
  in tie728_s16_depthwise_conv2d_hwc1_relu_loop
  EXCVADDR = 0x431d13f0     <- I-cache region, i.e. a corrupted pointer
```

It reproduces on the first motion event that wakes the cascade. A 16-byte alignment
fix for the decode buffer is already in place (`heap_caps_aligned_alloc(16, …)`,
because the tie728 SIMD kernels require 16-byte-aligned input and plain `ps_malloc`
is only 4-byte aligned) — it was necessary but not sufficient.

The root cause is upstream, not in this code. The `dl_lib` API is deprecated:
arduino-esp32 **removed these headers entirely in v3.1+**
([arduino-esp32#10881](https://github.com/espressif/arduino-esp32/issues/10881)),
and Espressif moved to esp-dl v3.x, which takes ONNX models and is ESP-IDF-only.
The pinned `espressif32@6.12.0` (Arduino core 2.0.17) is the **last** version where
this module compiles at all, and the library is unmaintained there. The same family
of failures is reported in
[esp-who#69](https://github.com/espressif/esp-who/issues/69),
[esp-dl#237](https://github.com/espressif/esp-dl/issues/237) and
[arduino-esp32#9671](https://github.com/espressif/arduino-esp32/issues/9671) —
fragility and crashes in `dl_matrix3d_init_bias` after tens of seconds of running.

Fixing it inside this project would mean rewriting `dl_lib` or migrating to the new
esp-dl. So the code stays, builds cleanly, and stays off: **a default build must not
ship a task that panics.**

### Turning it on anyway

Uncomment one line in `platformio.ini`:

```ini
-DINCLUDE_FACE_DETECT
```

Expect the crash above. It also costs flash:

| Build | Application image | Share of the 3 MB app partition |
| --- | --- | --- |
| Default (no face detection) | ~1.44 MB | **45.9 %** |
| With `-DINCLUDE_FACE_DETECT` | ~2.11 MB | **67.1 %** |

Roughly 670 kB of esp-dl model weights and kernels, for a feature that does not
survive its first detection.

### The path forward

Train a **FOMO face model** in Edge Impulse — the same pipeline already used for
person detection (`docs/fomo_setup.md`), just a different dataset — and run it
through the existing `src/person_detection.cpp` machinery. That removes the `dl_lib`
dependency completely and unblocks a future move to arduino-esp32 3.x.

Do **not** upgrade the platform package to arduino-esp32 3.1+ before that migration:
the face module stops compiling even when the feature flag is off, because the
missing headers are still `#include`d inside the `#ifdef` branch.

---

## 2. Resolutions above UXGA do not work

The sensor is a 5 MP PY260 and `POST /api/settings` clamps `frame_size` to
`FRAMESIZE_QVGA` … `FRAMESIZE_QSXGA`, i.e. **5 … 21** — but only **5 … 13**
(QVGA … UXGA 1600×1200) is usable. The settings page deliberately offers nothing
above 13:

```
5  QVGA  320x240      10 XGA  1024x768
6  CIF   400x296      11 HD   1280x720
8  VGA   640x480      12 SXGA 1280x1024
9  SVGA  800x600      13 UXGA 1600x1200   <- default, FRAME_SIZE_DEFAULT
```

### Why

Above UXGA the PY260 driver in arduino-esp32 2.0.x cannot sustain capture — you get
0 fps and a stream of `esp_camera_fb_get()` errors, not a slow-but-working stream.
The note is in `include/config.h`:

> `FRAMESIZE_UXGA (1600x1200)` — QSXGA is sensor-max but the PY260 driver in
> arduino-esp32 2.0.17 can't sustain capture at that size (0 fps, errors).

The API clamp is intentionally looser than the UI so the limit lives in one place
(the driver) rather than being enforced twice. If you set `frame_size: 18` through
the API you will get a camera that produces nothing until you set it back — the
health watchdog will notice the capture stall and reinit, then reboot after three
failed recoveries.

Note that the decode buffers throughout the firmware **are** sized for QSXGA
(`IMG_SRC_MAX_W/H` in `include/image_utils.h`), so a large frame cannot overflow a
detector's buffer. The limitation is throughput, not memory safety.

---

## 3. Streams and the WebSocket have no authentication

* **Port 81 in full**: `/stream`, `/detection-stream`, `/snapshot`. No Basic Auth,
  no token, no configuration option to add one. Anyone who can reach TCP 81 can
  watch the video.
* **`/ws` on port 80**: broadcasts a complete `/api/status` document every 2 seconds
  to any client that connects.
* **A long list of read-only endpoints on port 80**, including `/api/status`,
  `/api/snapshot`, `/log`, `/api/events` and `/api/wifi/scan`.

`docs/API.md` §8 enumerates exactly what that leaks (live video, MAC, SSID list,
crash history, MQTT broker address and username, event log, serial log) and what it
does not (no tokens, no passwords).

### Why

For port 81 it is simply not implemented: the IDF `esp_http_server` handlers in
`src/stream_server.cpp` register no auth check. Adding Basic Auth there is
mechanical, but MJPEG consumers vary wildly in whether they can carry credentials,
and the firmware's own detection consumer would need them too.

For `/ws` it is a deliberate trade-off recorded in `setupWebSocket()`: browsers
inconsistently forward cached Basic Auth to a WebSocket upgrade handshake — Safari
omits it, Chrome and Firefox vary by version — so enforcing auth there left the
dashboard stuck at “Connecting…” even after a successful HTTP login. The socket
carries only non-secret status, and every mutating operation goes through an
auth-protected POST.

### Mitigation

Network-level only: keep the camera off untrusted segments, and terminate TLS plus
authentication in a reverse proxy if the stream has to leave the LAN. The device
itself speaks plain HTTP — there is no HTTPS listener.

Also note that **empty credentials disable authentication entirely**: if
`http_user` or `http_pass` is an empty string, `requireAuth()` returns true for
everything.

---

## 4. Telegram TLS does not validate the server certificate

`src/telegram.cpp` calls `client.setInsecure()` on every `WiFiClientSecure` it
creates — four call sites: `sendTextSync()`, `sendPhotoSync()`,
`checkTelegramUpdates()` and `registerBotCommands()`. The TLS session is encrypted
but the peer is unauthenticated: no CA bundle, no certificate pinning, no hostname
verification.

### Threat model

* **Protected against**: passive eavesdropping. Somebody sniffing your LAN or
  upstream link sees a TLS session to `api.telegram.org`, not your bot token or your
  images.
* **Not protected against**: an active man-in-the-middle who can redirect
  `api.telegram.org` (DNS spoofing, a hostile router, ARP poisoning, a compromised
  upstream). Such an attacker terminates the TLS session themselves and obtains the
  **bot token in the request URL** — which is full control of the bot: reading your
  camera's messages, and sending anything to your chat.

### Why there is no pinning

1. **Rotation.** Telegram's certificate chain changes. A pinned leaf or
   intermediate turns every renewal into a silent notification outage on every
   deployed device — the worst failure mode for a security camera, because it fails
   quiet.
2. **Cost.** Verifying against a CA bundle needs the root chain in flash and adds
   heap pressure to a handshake that already competes with camera buffers and the
   async server on a fragmented 300-ish kB DRAM budget. `getMaxAllocHeap()` is
   already the number that predicts handshake failures on this board (it is logged
   every 30 s for exactly that reason).
3. **The realistic threat here is not MitM.** This is a LAN device whose whole HTTP
   surface is unauthenticated video (§3). An attacker positioned to MitM its
   outbound TLS can already watch the stream directly on port 81. Certificate
   validation would be hardening the strongest link in the chain.

If your deployment has a different threat model, the fix is localized: replace
`setInsecure()` with `setCACert()` and a pinned root, and accept the rotation
maintenance.

**MQTT is different.** `src/mqtt_handler.cpp` does validate, *if* you give it
something to validate against: with `mqtt_tls_enabled` set, it loads `/ca.pem` from
LittleFS and calls `setCACert()`; only when that file is absent does it fall back to
`setInsecure()`. So for MQTT, certificate validation is opt-in and available. Upload
`data/ca.pem` before `pio run -t uploadfs` (the file is gitignored).

---

## 5. Gallery previews are full-size JPEGs

`gallery.html` shows previews of the images on the SD card by fetching each one
through `GET /api/sd/download` — the **complete** file. A grid of twelve UXGA
captures is 2–3 MB over the wire, served by a single-core-bound async web server
from an SPI-attached SD card.

### Why

The firmware cannot generate thumbnails. Producing one means a full JPEG decode plus
a downscale plus a re-encode per file — several hundred milliseconds and a large
PSRAM working set each, on a device whose capture task, three detection tasks and
two MJPEG streams are already competing for the JPEG decoder (which has to be
serialized behind `cameraDecodeLock()` because tjpgd is not reentrant). Doing it on
demand would stall the browser; doing it at capture time would double the write cost
of every detection event and need a cache directory to invalidate.

The UI says so out loud rather than pretending otherwise:

> The device cannot generate thumbnails, so every preview is the full JPEG.

### Mitigations in place

* **Pagination**: `PAGE_SIZE = 24`, and only the current page's rows exist in the
  DOM at any time.
* **Lazy loading**: an `IntersectionObserver` sets `img.src` only when a row scrolls
  into view, with `loading="lazy"` as the fallback for browsers without it.
* **A toggle**: “Show thumbnails” can be switched off (persisted in
  `localStorage`), leaving a placeholder and a click-to-open button, which is the
  right mode on a slow link.

---

## 6. AVI recording is not wired up

`src/avi_writer.cpp` compiles (`-DINCLUDE_AVI_WRITER` is enabled) and is a correct
RIFF/AVI 1.0 MJPEG muxer as far as it goes — but **nothing calls it**. There is no
recorder module. `POST /record` always answers:

```json
{"success":false,"message":"AVI recording is not enabled on CamS3 yet"}
```

with HTTP `501`. `/api/status` reports `"is_recording": false` as a hardcoded stub,
and the `recording_started` / `recording_stopped` event types exist in the event log
enum but are never emitted. The `/recordings` directory is created on the SD card at
boot and stays empty; it is in the SD path whitelist so a future recorder needs no
API changes.

### The header is also incomplete

`begin()` sets `dwFlags = 0x10` (`AVIF_HASINDEX`) in the `avih` header, but `end()`
never writes an `idx1` chunk — it only patches the RIFF size, `movi` size,
`dwTotalFrames` and `strh.dwLength`. Any file produced by this muxer today would
therefore *claim* to have an index and not have one.

Most players cope (VLC and ffmpeg rebuild the index by scanning `movi`), but
strict readers may reject the file or refuse to seek. Whoever wires up the recorder
must either accumulate `(offset, size)` per frame and emit a real `idx1`, or clear
the `AVIF_HASINDEX` flag. The second option is a two-character change and honest;
the first is what a real recorder should do.

---

## 7. Stubbed endpoints for hardware this board does not have

| Endpoint | Always returns |
| --- | --- |
| `GET /audio-status` | `{"enabled":false,"available":false,"reason":"audio_not_enabled"}` |
| `GET /ir-status` | `{"ir_led_state":false,"auto_mode":false,"available":false}` |
| `POST /ir-control` | `{"success":true,"ir_led_state":false,"available":false}` |

`POST /ir-control` accepts and discards any body. It requires Basic Auth but not a
CSRF token, which is safe precisely because it mutates nothing.

Related hardcoded stubs in `/api/status`: `ir_led` (always `false`),
`is_recording` (always `false`), `motion_telegram_video` (always `false`),
`telegram_queue_depth`/`telegram_sent`/`telegram_fail`/`telegram_drops` (always
`0`), `telegram_queue_ready`/`telegram_task_ready` (always `true`),
`telegram_uploading` (always `false`).

### Why they exist at all

The M5Stack Unit CamS3 has **no IR LED, no ambient light sensor and no microphone**.
These routes exist so a dashboard or integration written against a board that does
have them gets a well-formed “not available” instead of a 404 that it may treat as
“device offline”. The Telegram counters are the same story: parity with another
firmware's status document, not measurements. Nothing in this build increments them.

One field deserves calling out because its name lies: **`ambient_light_lux` is not
lux.** It is the motion detector's average frame brightness, 0–255. `camera_profile`
(`NIGHT` / `DUSK` / `DAY`) is derived from it with thresholds of 15 and 40 on that
same 0–255 scale.

---

## 8. The person detection model is not in this repository

`lib/ei-person-fomo/` is in `.gitignore`. A fresh clone with the default flags
**will not build**:

```
error: "Edge Impulse FOMO model not found in lib/ei-person-fomo/. Export your
model as an Arduino library (see docs/fomo_setup.md), or comment out
-DINCLUDE_PERSON_DETECT in platformio.ini to build without person detection."
```

### Why

An Edge Impulse Arduino export is ~24 MB and 1300+ generated files (the EI SDK plus
a vendored TensorFlow Lite Micro). Committing it would bloat every clone forever,
bury real changes under generated diffs, and publish one specific trained network as
if it were part of the firmware.

The hard `#error` is also deliberate. A silent no-op would be worse: you would flash
a camera that quietly never detects anyone and only find out weeks later.

Two ways forward: follow `docs/fomo_setup.md` to bring your own model, or comment out
`-DINCLUDE_PERSON_DETECT` in `platformio.ini`. Everything else around the model —
the motion cascade, JPEG decode, resize, contrast normalization, the tracker, the
three-state `NONE`/`UNCERTAIN`/`CONFIDENT` decision, the Telegram and MQTT paths —
is in this repository.

---

## 9. Frames larger than 256 kB are dropped without a trace

`MAX_FRAME_SIZE` in `src/camera_manager.cpp` is 256 kB per ring-buffer slot
(3 slots, PSRAM). The capture task publishes a frame only `if (fb->len <=
MAX_FRAME_SIZE)`. There is no `else`: an oversized frame is skipped, and **no
counter records it.**

`ringDroppedFrames` (`ring_dropped` in `/api/status`) counts a different case —
every slot held by a reader, which is logged — so an oversized-frame problem does
not show up there either.

### When it bites

UXGA (1600×1200) at `jpeg_quality` 4–6 on a detailed, well-lit scene. The symptom is
a stream that stutters or freezes and a `capture_fps` that stays healthy, because
capture *is* working — the frames just never reach the ring buffer, and every
consumer (both MJPEG streams, all three detectors, `/api/snapshot`, timelapse,
`/save_frame`) reads exclusively from the ring.

### Workaround

Raise `jpeg_quality` (numerically higher = more compression = smaller frames). The
default of 8 keeps UXGA frames comfortably under the limit. If you need both UXGA
and quality 4, raise `MAX_FRAME_SIZE` — the cost is 3 × the increase in PSRAM, of
which there are 8 MB.

---

## 10. Notification text is Czech only

Every string the firmware sends outward is Czech, hardcoded: Telegram captions,
command replies, the boot message, the `/ticho` (mute) confirmations.
`include/cz_text.h` exists specifically to get Czech plural agreement right
(`1 osoba` / `3 osoby` / `7 osob`), which is not a structure that generalizes to a
translation table.

The **web UI** is separate and does have i18n (`data/www/i18n.js`, English and
Czech). Only the outbound notifications are monolingual.

Changing this means either accepting English-only captions (delete `cz_text.h` and
the `czPlural()` calls) or building a real message catalogue in the firmware. There
is no language setting today.

---

## 11. Smaller deliberate constraints

Things that are limits rather than bugs, worth knowing before you file an issue.

**Telegram silent mode is lost on reboot.** `/ticho <minutes>` suppresses
notifications until a `millis()` deadline held in RAM only. This is intentional: a
crash or power cycle cancels the silence rather than accidentally muting a security
camera for hours. `/ticho` with no argument is a permanent mute and *is* persisted.

**Event log timestamps are uptime, not wall clock.** Entries carry `uptime_s`
(seconds since **that** boot), so values reset across reboots. Correlate with the
`boot` entries in the same list. Capacity is `EVENT_LOG_SIZE` = 100 entries in a RAM
ring mirrored to `/events.jsonl`, and `detail` is capped at 48 bytes.

**`/log` holds 100 lines of 256 characters.** A PSRAM ring, ~25 kB. Anything older
is gone, and only output written through `logCapture()` appears — a module calling
`Serial.printf()` directly is invisible to `/log` and `/log-viewer`.

**Core dumps cannot be retrieved over HTTP.** The partition table reserves 64 kB for
coredumps and `/api/status` reports `coredump_present` and `coredump_size`, but
getting the dump out requires `esp-coredump` over a serial connection. There is no
download endpoint.

**Several settings keys are not range-checked.** `telegram_cooldown`,
`telegram_active_start`, `telegram_active_end`, `motion_notify_start_hour`,
`motion_notify_end_hour`, `telegram_poll_interval`, `idle_fps` and `active_fps` are
stored verbatim from the JSON body with no `clampInt()`. Everything else in
`POST /api/settings` is clamped. An hour of `99` or an `active_fps` of `10000` will
be accepted and persisted.

**`http_pass` shorter than 4 characters is silently ignored**, not rejected: the
request still returns `{"success":true}` and the password is unchanged. Same for an
over-long `http_user` or `hostname`.

**`POST /api/zones` and `POST /api/roi` parse only the first body chunk.** They
ignore the `index`/`total` parameters that `POST /api/settings` uses for
reassembly. Their payloads fit in one TCP segment in practice (a zone object is a few
hundred bytes; an ROI mask is 300 characters), but a larger body fails to parse.

**Zones are capped at 8, with 4 rectangles each.** Rectangles beyond the fourth are
**silently dropped**, not rejected. Zone names are the primary key and are truncated
to 23 usable characters. `color` is a UI hint the firmware never reads.

**Streams never repeat a frame,** so stream frame rate is bounded by capture rate.
With the default `idle_fps = 1`, a newly connected viewer can wait up to a second
for its first frame before the rate climbs to `active_fps`.

**A factory reset does not clear secrets.** `POST /api/reset` clears
`/config.json`; WiFi credentials, the HTTP password, the Telegram token and chat id
and the MQTT credentials live in AES-256-CTR-encrypted NVS, which is a separate
store. Use `POST /api/secrets/clear?target=telegram|mqtt` for those, and note that
there is no endpoint at all to clear the stored WiFi password.

**The default admin password is not auto-randomized.** It stays
`DEFAULT_HTTP_PASS` until you change it; the firmware logs a warning at boot and
`/api/status` reports `default_password: true`, which drives a banner in the UI. A
generated password was rejected deliberately — a user without a UART connection
would be locked out of their own camera.

---

## 12. Toolchain is pinned and cannot be upgraded yet

`platformio.ini` pins `platform = espressif32@6.12.0`, i.e. arduino-esp32 **2.0.17**.
This is not incidental:

* it is the last core version in which `src/face_detect.cpp` compiles at all (§1),
  and the headers are `#include`d from inside the `#ifdef` branch, so the file breaks
  the build on 3.1+ even with the feature flag off;
* the PY260 driver behaviour documented in §2 is specific to the 2.0.x
  esp32-camera component.

Migrating to arduino-esp32 3.x therefore has a prerequisite: replace the `dl_lib`
face detector with a FOMO face model (§1), which deletes the only blocking
dependency. Until then, treat the pin as load-bearing.

`lib_ldf_mode = deep+` is likewise required, not cosmetic — it is what lets the
`__has_include()` shim in `src/person_detection.cpp` find the Edge Impulse library.
