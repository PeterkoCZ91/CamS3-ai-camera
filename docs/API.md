# HTTP API reference

Complete reference for the HTTP surface of the CamS3 firmware, generated from the
route registrations in the source rather than from memory:

* `src/web_server.cpp` — `setupApiRoutes()`, `setupStaticFiles()`, `webServerInit()`
* `src/stream_server.cpp` — `streamServerInit()`, `registerStreamRoute()`

Every registered handler is documented here, and nothing is documented that is not
registered. If you add a route, add it to this file in the same commit.

Replace `<camera-ip>` below with the address the device reports on the serial
console at boot (`Boot complete! Access at http://…`). With mDNS enabled
(`-DINCLUDE_MDNS`, on by default) `cams3.local` works too — the hostname comes from
`appConfig.wifi.hostname`, default `cams3`.

---

## 1. Two servers, two ports

| Port | Server | Purpose | Source |
| --- | --- | --- | --- |
| 80 (`HTTP_PORT`) | `ESPAsyncWebServer` | JSON API, web UI, WebSocket, OTA | `src/web_server.cpp` |
| 81 (`STREAM_PORT`) | `esp_http_server` (IDF) | MJPEG streams and a raw snapshot | `src/stream_server.cpp` |

The split is deliberate: an MJPEG handler blocks its socket for as long as the
client is connected, so it lives on a separate server with its own task and client
budget instead of starving the async server that serves the UI.

Both ports send `Access-Control-Allow-Origin: *`. Port 80 additionally sets
`Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS` and
`Access-Control-Allow-Headers: Content-Type, Authorization` as default headers on
every response.

Handler count: **51 on port 80** (44 API/compat routes + 3 captive-portal redirects
+ 3 OTA routes + 1 WebSocket, plus a static-file mount and a 404 fallback) and
**7 on port 81** — 58 method+path handlers in total.

---

## 2. Authentication

HTTP **Basic Auth**, checked by `requireAuth()`:

* credentials are `appConfig.auth.http_user` / `http_pass`, defaults
  `DEFAULT_HTTP_USER` / `DEFAULT_HTTP_PASS` from `include/config.h`;
* **if either is an empty string, auth is disabled entirely** and every endpoint is
  open. This keeps local development simple; do not ship it that way;
* on failure the handler replies `401` with a `WWW-Authenticate` challenge.

Not every endpoint is protected. See the tables below, and section 8 for the
complete list of what is reachable without credentials.

## 3. CSRF protection

`requireCsrf()` enforces a per-boot token:

* the token is 32 hex characters, generated from `esp_random()` at
  `webServerInit()`, held in RAM only, and **rotates on every reboot**;
* fetch it with `GET /api/csrf` (that endpoint itself requires Basic Auth);
* send it back as the `X-CSRF-Token` request header;
* wrong token → `403 {"error":"CSRF token invalid"}`; missing token →
  `403 {"error":"CSRF token missing"}`;
* the whole mechanism can be switched off by setting
  `appConfig.auth.csrf_required = false` (no HTTP endpoint exposes this — it is a
  compile/config-level escape hatch).

### The `/api/` vs legacy split

Mutating requests under `/api/**` **always** require the token.

For paths that do not start with `/api/` the check is conditional: a request that
carries neither an `Origin` nor a `Sec-Fetch-Site` header is let through without a
token.

The reasoning: these legacy paths (`POST /settings`, `POST /save_frame`) predate
`/api/csrf` and are used by scripted integrations — `curl`, `requests`, home
automation glue — that have no way to fetch a token first. But exempting them
wholesale was a real hole: a browser that has cached Basic Auth for the device will
happily submit a cross-site form POST to `/settings`. Browsers always attach
`Origin` (and modern ones `Sec-Fetch-Site`) to a cross-origin POST; command-line
clients send neither. So the exemption is granted only to requests that provably did
not originate from a page in somebody's browser.

Consequence for integrators: a script needs no token, but must also not send an
`Origin` header. A browser page must send the token.

`POST /ir-control` and `POST /record` are stubs that change no state, but they go
through `requireCsrf()` as well — the policy is uniform across every mutating handler
so that wiring one of them up later cannot silently leave a gap.

## 4. Request body limits

`POST` handlers that take a JSON body accumulate it across TCP chunks in
`accumulateJsonBody()`:

* `MAX_JSON_BODY` = **16384 bytes**. `Content-Length` above that (or exactly zero) →
  `413 {"success":false,"message":"Body too large"}`. The cap exists because the
  allocation size comes straight from a client-supplied `Content-Length`;
* allocation failure → `500 {"success":false,"message":"OOM"}`;
* a client contradicting its own `Content-Length` →
  `400 {"success":false,"message":"Bad body length"}`;
* unparseable JSON → `400 {"success":false,"message":"Invalid JSON"}`.

Chunk accumulation is used by `POST /api/settings`, `POST /settings` and
`POST /api/wifi`. `POST /api/zones` and `POST /api/roi` parse the **first chunk
only** — fine for their payload sizes (a zone object is a few hundred bytes, an ROI
mask is 300 characters plus JSON overhead), but a body larger than one TCP segment
would fail to parse there.

---

## 5. Port 80 — API and UI

Legend: **Auth** = Basic Auth required. **CSRF** = `X-CSRF-Token` required
(“browser only” = the conditional legacy rule from section 3.2).

### 5.1 Status and telemetry

| Method | Path | Auth | CSRF | Response |
| --- | --- | --- | --- | --- |
| GET | `/api/status` | no | – | Full status document, ~175 keys (section 6) |
| GET | `/status` | no | – | Alias of `/api/status` |
| GET | `/settings` | no | – | Alias of `/api/status` (reads current settings) |
| GET | `/telemetry` | no | – | Alias of `/api/status` |
| GET | `/health` | no | – | Compact liveness document |
| GET | `/a12/status` | no | – | Integration status document |
| GET | `/api/a12/status` | no | – | Same handler as `/a12/status` |
| GET | `/stream-stats` | no | – | Capture/stream counters |
| GET | `/psram-stats` | no | – | `{total, free, used, usage_pct}` |
| GET | `/api/motion/debug` | no | – | Motion detector internals |
| GET | `/motion-debug` | no | – | Alias of `/api/motion/debug` |
| GET | `/credentials` | **yes** | – | Which secrets are set |

`GET /health` — designed for an uptime monitor; everything in it is cheap to compute:

```json
{
  "ok": true, "uptime_sec": 4211, "free_heap": 132840, "free_psram": 7154176,
  "wifi_connected": true, "wifi_rssi": -58, "capture_fps": 14.9,
  "capture_errors": 0, "frame_age_ms": 62, "stream_clients": 1,
  "detection_clients": 0, "reset_reason": "SOFTWARE", "total_restarts": 12,
  "issues": ""
}
```

`issues` is always an empty string — reserved, not yet populated.

`GET /a12/status` — a flattened subset for an external integration (see
[`A12_INTEGRATION.md`](A12_INTEGRATION.md)). `ok` is `true` only when WiFi is connected, at
least one frame has been captured, and `frame_age_ms < 10000`. It carries
`stream_url`, `detection_stream_url`, `snapshot_url`, `stream_port`, the capture
counters, camera geometry, and motion/person/MQTT/Telegram/SD flags. Fields absent
from the build (e.g. person detection) are emitted as `false` / `0` / `"NONE"`
rather than omitted, so a consumer never has to test for key presence.

`GET /stream-stats`:

```json
{
  "stream_clients": 1, "detection_clients": 0, "total_stream_clients": 1,
  "capture_fps": 14.9, "capture_count": 62810, "capture_errors": 0,
  "last_capture_ms": 4210987, "frame_age_ms": 62,
  "active_fps": 15, "idle_fps": 1,
  "stream_url": "http://<camera-ip>:81/stream",
  "detection_stream_url": "http://<camera-ip>:81/detection-stream"
}
```

`GET /api/motion/debug` — requires `-DINCLUDE_MOTION_DETECT` (default on):

```json
{
  "changed_pct": 0.7, "avg_brightness": 96.4, "sensor_gain": 6,
  "adaptive_threshold": 21.3, "active_blocks": 2, "clustered_blocks": 0,
  "consecutive_frames": 0, "night_mode": false, "training": false,
  "decode_ms": 41, "analysis_ms": 3,
  "motion_detected": false, "event_count": 37
}
```

`GET /credentials` — behind auth, tells the UI what is stored without shipping the
secrets: `{wifi_ssid, wifi_pass_set, telegram_token_set, telegram_chat_id,
http_user, default_password}`. Note the Telegram **chat id is returned in clear
text** (only the bot token is masked).

### 5.2 Settings

| Method | Path | Auth | CSRF | Body |
| --- | --- | --- | --- | --- |
| POST | `/api/settings` | **yes** | **yes** | JSON object, see below |
| POST | `/settings` | **yes** | browser only | Same handler, legacy path |
| GET | `/settings-page` | no | – | `302` redirect to `/settings.html` |

One handler (`handleApiSettings`) serves both POST paths. Every key is optional;
only keys present in the body are touched. Unknown keys are ignored silently.
Numeric values are clamped with `clampInt()` / `clampFloat()` — an out-of-range
value is **not** an error, it is pulled to the nearest bound. Type matters: keys
listed as `int` require an integral JSON number (`10.5` is ignored), while keys
listed as `float` also accept a JSON integer.

Success responses:

```json
{"success":true,"message":"Settings applied"}
```

```json
{"success":true,"message":"Settings applied, camera restarting","camera_restart":true}
```

The second form is returned whenever `frame_size` actually changed value. A new
resolution needs the driver's DMA buffers rebuilt, so the handler schedules a full
`cameraReinit()` (`cameraRequestReinit()`), performed by the main loop rather than
inline — expect a **1–3 second gap in the stream** and treat `camera_restart: true`
as “reconnect your stream”. All other camera keys go through
`cameraRequestSettingsApply()`, a much cheaper SCCB write from the capture task.

After a successful apply the handler pushes a fresh status document to all
WebSocket clients.

#### Camera keys

| Key | Type | Range (clamped) | Notes |
| --- | --- | --- | --- |
| `frame_size` | int | `5` … `21` | `FRAMESIZE_QVGA` … `FRAMESIZE_QSXGA`. **Triggers camera restart.** Only `5`–`13` are usable in practice — see `docs/known_issues.md` |
| `jpeg_quality` | int | `4` … `63` | Lower = better quality, larger frames |
| `brightness` | int | `-2` … `2` | |
| `contrast` | int | `-2` … `2` | |
| `saturation` | int | `-2` … `2` | |
| `sharpness` | int | `-3` … `3` | |
| `denoise` | int | `0` … `8` | |
| `ae_level` | int | `-2` … `2` | |
| `aec_value` | int | `0` … `1200` | Manual exposure, used when `aec` is off |
| `agc_gain` | int | `0` … `30` | Manual gain, used when `agc` is off |
| `gainceiling` | int | `0` … `6` | |
| `wb_mode` | int | `0` … `4` | 0 = auto, 1–4 = presets |
| `vflip` | bool | – | Default `true` (the unit is usually mounted upside-down) |
| `hmirror` | bool | – | |
| `aec` | bool | – | `set_exposure_ctrl` |
| `aec2` | bool | – | `set_aec2`, DSP refinement — a separate knob from `aec` |
| `agc` | bool | – | |
| `awb` | bool | – | |
| `bpc` | bool | – | Black pixel correction |
| `wpc` | bool | – | White pixel correction |
| `raw_gma` | bool | – | |
| `lenc` | bool | – | Lens correction |

Any of these sets `needCameraApply`, which also persists the config.

#### Motion keys

| Key | Type | Range | Notes |
| --- | --- | --- | --- |
| `motion_enabled` | bool | – | |
| `motion_detection_enabled` | bool | – | Legacy alias of `motion_enabled` |
| `motion_threshold` | int | `5` … `80` | Per-block luma delta |
| `motion_cooldown` | int | `0` … `3600` | Seconds |
| `motion_telegram_cooldown` | int | `0` … `3600` | Legacy alias of `motion_cooldown` |
| `motion_min_area` | int | `1` … `100` | Percent of the ROI that must change |
| `motion_max_area` | int | `10` … `100` | Upper reject: above this share, the frame is treated as a global lighting change. `100` disables the reject |
| `motion_temporal_filter` | bool | – | |
| `motion_spatial_filter` | bool | – | |
| `motion_night_suppress` | bool | – | |
| `motion_save_sd` | bool | – | |
| `motion_ema_alpha_day` | float | `0.50` … `0.999` | Background model adaptation, day |
| `motion_ema_alpha_night` | float | `0.50` … `0.999` | Background model adaptation, night |
| `motion_training_frames` | int | `1` … `120` | Frames before the model is considered trained |
| `motion_agc_gain_factor` | float | `0.0` … `4.0` | How much sensor gain raises the threshold |
| `motion_brightness_min` | int | `0` … `128` | Below this average brightness, night rules apply |

The last five are expert knobs — persisted since day one, but previously only
reachable by hand-editing `/config.json`.

#### Face detection keys

Accepted regardless of whether `-DINCLUDE_FACE_DETECT` is compiled in; without it
they are stored and reported but nothing consumes them.

| Key | Type | Range |
| --- | --- | --- |
| `face_detect_enabled` | bool | – |
| `face_detect_two_stage` | bool | – |
| `face_detect_cooldown` | int | `0` … `3600` |
| `face_detect_save_sd` | bool | – |
| `face_score_threshold` | float | `0.05` … `0.99` |
| `face_nms_threshold` | float | `0.05` … `0.99` |

#### Person detection and tracker keys

| Key | Type | Range | Notes |
| --- | --- | --- | --- |
| `person_detect_enabled` | bool | – | |
| `person_detection_enabled` | bool | – | Legacy alias |
| `person_detect_confidence` | float | `0.05` … `0.99` | Detection floor |
| `person_confidence_threshold` | float | `0.05` … `0.99` | Legacy alias |
| `person_confident_threshold` | float | `0.05` … `1.0` | At or above → `CONFIDENT` (notify directly); between the two → `UNCERTAIN` (MQTT only, for external verification) |
| `person_detect_temporal` | int | `1` … `10` | Consecutive frames required |
| `person_recheck_interval` | int | `1` … `10` | Legacy alias of `person_detect_temporal` |
| `person_detect_cooldown` | int | `0` … `3600` | Seconds |
| `person_detection_cooldown` | int | `0` … `3600` | Legacy alias |
| `person_detect_save_sd` | bool | – | |
| `tracker_confirm_hits` | int | `1` … `10` | Hits before a track is confirmed |
| `tracker_max_misses` | int | `1` … `30` | Misses before a track is dropped |
| `tracker_match_dist` | int | `5` … `200` | Centroid match distance, pixels |

#### Timelapse keys

| Key | Type | Range |
| --- | --- | --- |
| `timelapse_enabled` | bool | – |
| `timelapse_interval` | int | `1` … `86400` seconds |
| `timelapse_save_sd` | bool | – |

#### MQTT keys

| Key | Type | Range | Notes |
| --- | --- | --- | --- |
| `mqtt_enabled` | bool | – | |
| `mqtt_server` | string | – | **Empty string = leave unchanged** (a blank field on the settings page must not silently disconnect the broker) |
| `mqtt_port` | int | `1` … `65535` | |
| `mqtt_topic_prefix` | string | no limit | |
| `mqtt_tls_enabled` | bool | – | Uses `/ca.pem` from LittleFS if present, otherwise `setInsecure()` |
| `mqtt_user` | string | – | **Empty = unchanged.** Stored encrypted in NVS |
| `mqtt_pass` | string | – | **Empty = unchanged.** Stored encrypted in NVS |

#### Telegram keys

| Key | Type | Range | Notes |
| --- | --- | --- | --- |
| `telegram_enabled` | bool | – | |
| `telegram_notify_on_motion` | bool | – | |
| `telegram_notify_on_face` | bool | – | |
| `telegram_notify_on_person` | bool | – | |
| `telegram_photo_on_motion` | bool | – | |
| `motion_telegram_photo` | bool | – | Legacy alias of `telegram_photo_on_motion` |
| `telegram_photo_on_face` | bool | – | |
| `telegram_photo_on_person` | bool | – | |
| `person_telegram_photo` | bool | – | Legacy alias of `telegram_photo_on_person` |
| `telegram_cooldown` | int | **not clamped** | Seconds |
| `telegram_active_start` | int | **not clamped** | Hour 0–23 by convention; overnight wrap supported |
| `telegram_active_end` | int | **not clamped** | Hour 0–23 by convention |
| `motion_notify_start_hour` | int | **not clamped** | Legacy alias of `telegram_active_start` |
| `motion_notify_end_hour` | int | **not clamped** | Legacy alias of `telegram_active_end` |
| `telegram_poll_interval` | int | **not clamped** | Milliseconds between `getUpdates` polls |
| `telegram_bot_token` | string | – | **Empty = unchanged.** Encrypted NVS |
| `telegram_chat_id` | string | – | **Empty = unchanged.** Encrypted NVS |

The “empty means unchanged” rule for secrets exists because the settings page posts
every field on every save; treating `""` as a value wiped the bot token whenever
anyone touched an unrelated slider. To actually erase them use
`POST /api/secrets/clear` (section 5.7). A stale code comment in
`src/web_server.cpp` mentions `/api/telegram/clear` — that endpoint does not exist.

#### Auth, network identity and misc keys

| Key | Type | Range | Notes |
| --- | --- | --- | --- |
| `http_user` | string | 1 … 32 chars | Empty or over-long is ignored |
| `http_pass` | string | 4 … 64 chars | **Empty = unchanged.** Anything shorter than 4 chars is ignored, not rejected. Written to encrypted NVS immediately |
| `hostname` | string | 1 … 32 chars | Applied on next boot (mDNS/DHCP name) |
| `led_enabled` | bool | – | Status LED |
| `idle_fps` | int | **not clamped** | Capture rate with no stream client |
| `active_fps` | int | **not clamped** | Capture rate while streaming |

There is no endpoint to change WiFi credentials through `/api/settings` — use
`POST /api/wifi`.

### 5.3 Camera

| Method | Path | Auth | CSRF | Response |
| --- | --- | --- | --- | --- |
| GET | `/api/snapshot` | no | – | `image/jpeg` |
| GET | `/frame` | no | – | Alias of `/api/snapshot` |
| GET | `/snapshot` | no | – | Alias of `/api/snapshot` |
| POST | `/api/camera/reinit` | **yes** | **yes** | `202 {"success":true,"message":"Camera reinit scheduled"}` |

`/api/snapshot` serves the newest frame from the shared ring buffer. The JPEG is
copied into PSRAM before the async send (a UXGA frame is 100–250 kB and taking that
out of DRAM starved the async server and TLS handshakes). Headers:
`Content-Disposition: inline; filename=snapshot.jpg`, `Cache-Control: no-cache`.
Errors: `503 {"error":"No frame available"}` when the ring is empty,
`503 {"error":"Memory allocation failed"}` when PSRAM cannot hold the copy.

`/api/camera/reinit` returns `202`, not `200`: stopping the capture task can take up
to 3 seconds, so the reinit is queued and executed by the main loop. If the camera
cannot be recovered after `MAX_FAILED_REINITS` (3) attempts the firmware reboots
itself.

### 5.4 WiFi

| Method | Path | Auth | CSRF | Body / params |
| --- | --- | --- | --- | --- |
| GET | `/api/wifi/scan` | no | – | – |
| POST | `/api/wifi` | **yes** | **yes** | `{"ssid": "...", "password": "..."}` |

`GET /api/wifi/scan` blocks for roughly 300 ms per channel and returns:

```json
[{"ssid":"MyNetwork","rssi":-52,"enc":true}]
```

`enc` is `false` only for genuinely open networks.

`POST /api/wifi` replies `200 {"success":true,"message":"Connecting..."}`
**before** connecting — the blocking connect is deferred to the main loop
(`wifiRequestConnect()`), because an async handler must never block. `ssid` is
mandatory (`400 {"success":false,"message":"SSID required"}`). Credentials are
written to encrypted NVS. Poll `/api/status` (`wifi_mode`, `ip`) to learn the
outcome; a failed connect restarts the setup AP and the captive portal.

### 5.5 SD card

Requires `-DINCLUDE_SD_CARD` (default on).

| Method | Path | Auth | CSRF | Params |
| --- | --- | --- | --- | --- |
| GET | `/api/sd/list` | **yes** | – | `path` (default `/`) |
| GET | `/sd-list` | **yes** | – | Alias of `/api/sd/list` |
| GET | `/api/sd/download` | **yes** | – | `file` (required) |
| DELETE | `/api/sd/delete` | **yes** | **yes** | `file` (required) |
| POST | `/save_frame` | **yes** | browser only | – |

**Path whitelist.** Paths accepted from HTTP are restricted to the directories the
firmware itself creates. `SD_ALLOWED_PREFIXES` in `src/web_server.cpp`:

```
/captures  /timelapse  /recordings  /telegram_pending  /logs
```

`sdPathAllowed()` requires all of:

* the path starts with `/`;
* it contains **no `..`** anywhere (rejected before any prefix matching, so
  `/captures/../config.json` is refused);
* it equals one of the prefixes exactly, or starts with `<prefix>/`.

Listing is slightly looser: `sdListPathAllowed()` also allows `/` so the UI can
discover the roots. Everything else — including download and delete — uses the
strict form. A rejected path yields `403 {"error":"Path not allowed"}`.

Without the whitelist a request like `?file=/config.json` could read or delete
anything the SD driver can reach.

`GET /api/sd/list` returns a flat array of the directory's direct children:

```json
[{"name":"motion_4211987.jpg","size":184320,"dir":false}]
```

`404 {"error":"Directory not found"}` if the path is not an existing directory.
Note that `name` comes from `File::name()` and is the bare entry name.

`GET /api/sd/download` streams the file as `application/octet-stream`.
`400 {"error":"file param required"}`, `403`, `404 {"error":"File not found"}`.

`DELETE /api/sd/delete` → `{"success":true}` or
`500 {"success":false,"message":"Delete failed"}`.

`POST /save_frame` writes the newest ring-buffer frame to `/captures` with a
`manual_` prefix, through the guarded `sdStoreWriteJpeg()` (rotation + circuit
breaker):

```json
{"status":"ok","file":"/captures/manual_4211987.jpg"}
```

Failure modes: `503 {"status":"error","message":"sd_not_mounted"}`,
`503 {"status":"error","message":"sd_write_disabled"}` (breaker tripped after 3
consecutive write failures), `503 {"status":"error","message":"no_frame"}`,
`500 {"status":"error","message":"write_failed"}`.

### 5.6 Zones and ROI

Requires `-DINCLUDE_ZONES` (default on). Both features address the same
**20 × 15 block grid** — `MOTION_GRID_W` × `MOTION_GRID_H` from
`include/motion_detect.h`, i.e. `MOTION_GRID_SIZE` = **300** blocks. Coordinates are
in grid blocks, not pixels: x ∈ 0…19, y ∈ 0…14, independent of the configured
`frame_size`.

| Method | Path | Auth | CSRF | Params / body |
| --- | --- | --- | --- | --- |
| GET | `/api/zones` | no | – | – |
| POST | `/api/zones` | **yes** | **yes** | Zone object |
| DELETE | `/api/zones` | **yes** | **yes** | `name` (required) |
| GET | `/api/roi` | no | – | – |
| POST | `/api/roi` | **yes** | **yes** | `{"mask": "..."}` |

#### Zones

Limits from `include/zone_manager.h`:

| Constant | Value | Meaning |
| --- | --- | --- |
| `ZONE_MAX_COUNT` | 8 | Maximum zones stored |
| `ZONE_MAX_RECTS` | 4 | Rectangles per zone |
| `ZONE_NAME_LEN` | 24 | Name buffer, **including** the NUL terminator (23 usable characters) |

`GET /api/zones` returns the array as stored in `/zones.json` on LittleFS:

```json
[
  {
    "name": "driveway",
    "color": "#ff8800",
    "alert": true,
    "rects": [{"x": 2, "y": 8, "w": 10, "h": 6}]
  }
]
```

`POST /api/zones` takes exactly one zone object with the same shape:

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `name` | string | `""` | **Required.** Truncated to `ZONE_NAME_LEN`. Doubles as the primary key: posting an existing name **replaces** that zone, a new name appends |
| `color` | string | `"#ffffff"` | `#rrggbb`, 8-byte buffer. UI hint only |
| `alert` | bool | `false` | When true, motion inside the zone fires an event / Telegram notification |
| `rects` | array | `[]` | Up to `ZONE_MAX_RECTS`; **extra rectangles are silently dropped**, not rejected |
| `rects[].x`, `.y` | int | `0` | Top-left block |
| `rects[].w`, `.h` | int | `1` | Size in blocks |

Responses: `{"success":true}`; `400 {"error":"Zone name required"}` when `name` is
empty; `400 {"error":"Invalid JSON"}`; `500 {"error":"Save failed"}` when the write
to LittleFS fails — which is also what you get once `ZONE_MAX_COUNT` is reached with
a new name (`addOrUpdateZone()` returns false).

Coordinates are **not** range-checked on input; `getActiveZones()` clips them
against the grid at match time, so an out-of-range rectangle simply never matches.

`DELETE /api/zones?name=driveway` → `{"success":true}`, or
`404 {"error":"Zone not found"}`, or `400 {"error":"name param required"}`.

#### ROI mask

`GET /api/roi`:

```json
{"mask":"", "grid_w":20, "grid_h":15, "active_blocks":300, "total_blocks":300}
```

`grid_w`/`grid_h`/`total_blocks` are reported so an ROI editor does not have to
hardcode the grid; `active_blocks` is what the detector is currently using.

`POST /api/roi` body: `{"mask": "<string>"}`.

* One character per grid block, **row-major** (block `(x, y)` is at index
  `y * grid_w + x`).
* `'0'` masks the block out; **any other character** keeps it (the UI writes `'1'`).
* Length must be exactly `grid_w * grid_h` = **300** characters, **or** the empty
  string, which clears the mask and makes the whole frame active. Anything else:
  `400 {"error":"mask must be 300 chars (20x15 grid) or empty"}`.
* The mask is persisted to `/roi_mask.txt` **and applied immediately** —
  `motionDetectSetRoiMask()` / `motionDetectClearRoiMask()`, no reboot needed.
* Blocks outside the ROI never contribute to motion, and the trigger percentage is
  computed over the ROI area rather than the whole grid, so masking out half the
  frame does not halve the effective sensitivity in the rest of it.

Zones and the ROI mask are independent: the ROI decides what the detector looks at,
zones decide which detections raise an alert.

### 5.7 Secrets

| Method | Path | Auth | CSRF | Params |
| --- | --- | --- | --- | --- |
| POST | `/api/secrets/clear` | **yes** | **yes** | `target` (required) |

`target` is a **query parameter**, not a JSON body field, and accepts exactly two
values:

| `target` | Clears |
| --- | --- |
| `telegram` | `telegram.bot_token` and `telegram.chat_id` |
| `mqtt` | `mqtt.user` and `mqtt.password` |

Anything else (including a missing parameter) →
`400 {"success":false,"message":"target must be telegram or mqtt"}`. On success the
NVS blob is rewritten and the reply is `{"success":true}`.

This endpoint exists because an empty field in the settings form means “leave
unchanged” (section 5.2), so there was no other way to erase a stored credential
short of a factory reset.

```bash
curl -u admin:<password> -X POST \
  -H "X-CSRF-Token: $TOKEN" \
  "http://<camera-ip>/api/secrets/clear?target=telegram"
```

### 5.8 Events and log

| Method | Path | Auth | CSRF | Response |
| --- | --- | --- | --- | --- |
| GET | `/api/events` | no | – | Event ring, newest first |
| GET | `/events` | no | – | Alias of `/api/events` |
| GET | `/log` | no | – | `text/plain` |
| GET | `/log-viewer` | no | – | `text/html` |
| POST | `/api/motion/reset` | **yes** | **yes** | Reset the background model |

`/api/events` requires `-DINCLUDE_EVENT_LOG` (default on). Up to `EVENT_LOG_SIZE`
= **100** entries, in-RAM ring mirrored to `/events.jsonl`, **newest first**:

```json
[{"uptime_s": 4211, "type": "person", "detail": "score=0.81 CONFIDENT"}]
```

`type` is one of `boot`, `motion`, `person`, `face`, `armed`, `disarmed`,
`recording_started`, `recording_stopped`, `telegram_failed`, `wifi_reconnect`,
`low_memory`, `sd_failure`, `unknown`. `detail` is free text capped at
`EVENT_DETAIL_LEN` = 48 bytes. `uptime_s` is seconds since **that** boot, so it
resets across reboots — correlate with the `boot` entries.

`/log` returns the last `LOG_RING_LINES` = **100** lines of
`LOG_LINE_MAX_LEN` = 256 characters each, as plain text. The ring lives in PSRAM
(~25 kB), not BSS. `503 Log buffer unavailable` when the PSRAM allocation for the
snapshot fails. Only output written through `logCapture()` appears here — a module
calling `Serial.printf()` directly is invisible to `/log`.

`/log-viewer` is a self-contained HTML page that polls `/log` every 5 seconds.

`POST /api/motion/reset` discards the learned background model and restarts training
(`motion_training_frames` frames). Requires `-DINCLUDE_MOTION_DETECT`.
Reply: `{"success":true,"message":"Background model reset"}`.

### 5.9 System actions

| Method | Path | Auth | CSRF | Response |
| --- | --- | --- | --- | --- |
| GET | `/api/csrf` | **yes** | – | `{"token":"<32 hex chars>"}` |
| POST | `/api/reboot` | **yes** | **yes** | `{"success":true,"message":"Rebooting..."}` |
| POST | `/api/reset` | **yes** | **yes** | `{"success":true,"message":"Factory reset, rebooting..."}` |
| POST | `/record` | **yes** | yes (browser-originated) | `501` |
| POST | `/ir-control` | **yes** | yes (browser-originated) | Stub, see below |
| GET | `/ir-status` | no | – | Stub |
| GET | `/audio-status` | no | – | Stub |

`POST /api/reboot` and `POST /api/reset` both answer first and reboot ~500 ms later
from a throwaway FreeRTOS task, so the client always sees the response.
`/api/reset` calls `resetConfig()` — it clears `/config.json`; encrypted NVS
secrets are a separate store and are **not** wiped by it.

`POST /record` always returns
`501 {"success":false,"message":"AVI recording is not enabled on CamS3 yet"}`. The
AVI muxer compiles but is not wired to a recorder — see `docs/known_issues.md`.

`POST /ir-control` accepts and ignores any body, returning
`{"success":true,"ir_led_state":false,"available":false}`.
`GET /ir-status` returns `{"ir_led_state":false,"auto_mode":false,"available":false}`
and `GET /audio-status` returns
`{"enabled":false,"available":false,"reason":"audio_not_enabled"}`. All three exist
only so a client written against a board that *has* an IR LED or a microphone gets a
well-formed “not available” instead of a 404. The CamS3 has neither.

### 5.10 OTA

Registered by `ElegantOTA.begin(&server)` when `-DINCLUDE_OTA` is set (default on).
Basic Auth is applied via `ElegantOTA.setAuth()` **only if both** `http_user` and
`http_pass` are non-empty.

| Method | Path | Auth | Purpose |
| --- | --- | --- | --- |
| GET | `/update` | yes\* | Upload UI (HTML) |
| GET | `/ota/start` | yes\* | Begin an update session |
| POST | `/ota/upload` | yes\* | Multipart firmware/filesystem image |

`GET /ota/start` query parameters:

| Param | Values | Meaning |
| --- | --- | --- |
| `mode` | `fr` (default) / `fs` | Firmware image vs. filesystem image |
| `hash` | MD5 hex | Optional integrity check; an invalid value → `400 MD5 parameter invalid` |

CSRF is not applied to these routes (they are the library's own handlers).

On success the firmware computes the **SHA-256 of the newly flashed partition** and
writes it to the log (`logNewFirmwareSha()`), so you can compare it against your
build output. `/api/status` reports the SHA-256 of the *running* partition as
`fw_sha256` — this is a hash of the flashed `.bin`, deliberately different from the
build-time ELF hash in `esp_app_desc_t::app_elf_sha256`.

### 5.11 Static files, captive portal and 404

`server.serveStatic("/", LittleFS, "/www/")` with default file `index.html` and
`Cache-Control: public, max-age=86400`. The handler still answers conditional
requests with `ETag` / `304`; a day of caching is fine because the assets only
change when the filesystem image is reflashed. **No authentication** — the UI
markup is public, and every privileged action it performs goes through an
auth-protected endpoint.

Pages shipped in `data/www/`: `index.html` (live view), `settings.html`,
`admin.html`, `gallery.html`, `wifi.html`, `zones.html`, plus `style.css`,
`common.js`, `i18n.js`.

Captive-portal probe endpoints, all `302` to `http://<camera-ip>/`:

| Method | Path | Client |
| --- | --- | --- |
| GET | `/generate_204` | Android |
| GET | `/hotspot-detect.html` | iOS / macOS |
| GET | `/fwlink` | Windows |

The 404 fallback redirects to the index while the captive portal is active
(`isCaptivePortalActive()`), and otherwise returns `404 Not Found` as plain text.

### 5.12 WebSocket

| Path | Auth | Protocol |
| --- | --- | --- |
| `/ws` | **no** | Text frames, one full `/api/status` JSON document per message |

Behaviour:

* on connect the server immediately sends one status document;
* thereafter `webSocketBroadcastStatus()` broadcasts at most **once per 2 seconds**,
  and only while at least one client is connected;
* an extra broadcast is emitted right after a successful `POST /api/settings`;
* inbound text frames are logged and otherwise ignored — there are no client
  commands. Everything mutating goes through the REST endpoints.

`/ws` is deliberately unauthenticated. It carries only non-secret status, and
browsers inconsistently forward cached Basic Auth to the WebSocket upgrade
handshake (Safari omits it; Chrome/Firefox vary by version), so enforcing auth left
the page stuck at “Connecting…” even after a successful HTTP login.

### 5.13 Legacy compatibility aliases

These paths exist so tooling written against the DFR1154 reference project and the
A12 integration keeps working. They are thin aliases — same handler, same response.

| Legacy path | Method | Canonical equivalent |
| --- | --- | --- |
| `/status` | GET | `/api/status` |
| `/settings` | GET | `/api/status` |
| `/settings` | POST | `/api/settings` (CSRF: browser only) |
| `/telemetry` | GET | `/api/status` |
| `/frame` | GET | `/api/snapshot` |
| `/snapshot` | GET | `/api/snapshot` |
| `/motion-debug` | GET | `/api/motion/debug` |
| `/events` | GET | `/api/events` |
| `/sd-list` | GET | `/api/sd/list` |
| `/a12/status` | GET | `/api/a12/status` (both registered) |
| `/settings-page` | GET | `302` → `/settings.html` |
| `/save_frame` | POST | No `/api/` equivalent (CSRF: browser only) |
| `/health` | GET | No `/api/` equivalent |
| `/stream-stats` | GET | No `/api/` equivalent |
| `/psram-stats` | GET | No `/api/` equivalent |
| `/credentials` | GET | No `/api/` equivalent |
| `/record` | POST | No `/api/` equivalent (`501`) |
| `/ir-status`, `/ir-control`, `/audio-status` | GET / POST / GET | Stubs, no equivalent |
| `/log`, `/log-viewer` | GET | No `/api/` equivalent |

Prefer the `/api/**` form for new integrations: it gets unconditional CSRF
enforcement, and the aliases are the part of the surface most likely to be dropped.

---

## 6. `/api/status` key reference

`buildStatusJson()` emits roughly **175 keys** in a default build (a few more with
`-DINCLUDE_FACE_DETECT`, fewer if feature flags are turned off). The same document
is served by `/status`, `/settings` (GET), `/telemetry` and every WebSocket
broadcast.

Two things to know before consuming it:

* **Legacy aliases** (marked *alias*) carry the same value as a canonical key under
  a different name, for older clients. Never write code that reads only the alias.
* **Stubs** (marked *stub*) are hardcoded constants for feature parity with other
  boards. They will never change value on this hardware.

### 6.1 Identity and firmware

| Key | Type | Notes |
| --- | --- | --- |
| `version` | string | `FIRMWARE_VERSION` build flag |
| `device` | string | `DEVICE_NAME` build flag |
| `fw_sha256` | string | SHA-256 of the running OTA partition (hex) |
| `fw_version` | string | `esp_app_desc_t::version` |
| `fw_built` | string | Compile date + time from the app descriptor |
| `sdk` | string | ESP-IDF version |
| `cpu_freq` | int | MHz |
| `flash_size` | int | Bytes |

### 6.2 Uptime and restart accounting

| Key | Type | Notes |
| --- | --- | --- |
| `uptime` | string | Human readable, `[Nd ]HH:MM:SS` |
| `uptime_sec` | int | Seconds |
| `uptime_seconds` | int | *alias* of `uptime_sec` |
| `total_restarts` | int | Persisted across reboots in `/sysstats.bin` |
| `reset_reason` | string | e.g. `POWERON`, `PANIC`, `TASK_WDT`, `BROWNOUT`, `SOFTWARE` |
| `reset_reason_code` | int | Raw `esp_reset_reason()` |
| `poweron_restarts` | int | Cable pulled / PSU glitch |
| `brownout_restarts` | int | Undervoltage |
| `wdt_restarts` | int | Task / int / RTC watchdog |
| `panic_restarts` | int | Crash with core dump |
| `sw_restarts` | int | Commanded: OTA, `/api/reboot` |
| `other_restarts` | int | |
| `longest_uptime_s` | int | Record, coarse resolution (saved at most every 10 min) |
| `coredump_present` | bool | A panic dump is sitting in the coredump partition |
| `coredump_size` | int | Bytes, `0` when absent |

These counters answer “is this the first attempt or the thirty-first?” without a
serial console attached.

### 6.3 Memory and thermals

| Key | Type | Notes |
| --- | --- | --- |
| `free_heap` | int | Bytes |
| `max_alloc_heap` | int | *alias*-ish addition; largest contiguous DRAM block — the number that actually predicts TLS/JPEG failures |
| `free_psram`, `total_psram` | int | Bytes |
| `psram_usage_pct` | float | |
| `flash_used` | int | Sketch size, bytes |
| `flash_usage_pct` | float | Sketch size / **flash chip size** — not / app partition size, so it reads lower than PlatformIO's figure |
| `chip_temp_c` | float | `temperatureRead()`, internal sensor, ±several °C |

### 6.4 Network

| Key | Type | Notes |
| --- | --- | --- |
| `ip` | string | STA address, or the AP address in setup mode |
| `rssi` | int | dBm; `0` in AP mode |
| `wifi_rssi` | int | *alias* of `rssi` |
| `mac` | string | |
| `wifi_mode` | string | `"STA"` or `"AP"` |
| `wifi_channel` | int | |
| `hostname` | string | |

### 6.5 Capture and streams

| Key | Type | Notes |
| --- | --- | --- |
| `capture_fps` | float | |
| `stream_fps` | float | *alias* of `capture_fps` |
| `capture_count` | int | Frames since boot |
| `capture_errors` | int | `esp_camera_fb_get()` failures |
| `ring_dropped` | int | Frames dropped because every ring slot was held by a reader |
| `last_capture_ms` | int | `millis()` of the newest frame |
| `frame_age_ms` | int | Age of the newest frame; `0xFFFFFFFF` (4294967295) when nothing has ever been captured |
| `stream_clients` | int | Clients on `/stream` |
| `clients` | int | *alias* of `stream_clients` |
| `detection_clients` | int | Clients on `/detection-stream` |
| `total_stream_clients` | int | Sum of the two |
| `stream_url`, `detection_stream_url` | string | Absolute, port 81 |
| `snapshot_url` | string | Absolute, port 80 `/frame` |
| `sensor_pid` | int | Sensor product id; **omitted** if the sensor handle is null |
| `active_fps`, `idle_fps` | int | Configured capture rates |

### 6.6 Camera settings

Current values of every key from section 5.2's camera table, under the same names:
`frame_size`, `jpeg_quality`, `brightness`, `contrast`, `saturation`, `sharpness`,
`denoise`, `ae_level`, `aec_value`, `agc_gain`, `gainceiling`, `wb_mode`, `vflip`,
`hmirror`, `aec`, `aec2`, `agc`, `awb`, `bpc`, `wpc`, `raw_gma`, `lenc`. These
report the stored config, which is what was last pushed to the sensor.

### 6.7 Motion

| Key | Type | Notes |
| --- | --- | --- |
| `motion_enabled` | bool | |
| `motion_detection_enabled` | bool | *alias* of `motion_enabled` |
| `motion_threshold`, `motion_cooldown`, `motion_min_area`, `motion_max_area` | int | Settings echo |
| `motion_telegram_cooldown` | int | *alias* of `motion_cooldown` |
| `motion_ema_alpha_day`, `motion_ema_alpha_night`, `motion_agc_gain_factor` | float | Settings echo |
| `motion_training_frames`, `motion_brightness_min` | int | Settings echo |
| `motion_temporal_filter`, `motion_spatial_filter`, `motion_night_suppress`, `motion_save_sd` | bool | Settings echo |
| `motion_detected` | bool | Live. Only with `-DINCLUDE_MOTION_DETECT` |
| `motion_event_count` | int | Live. Only with `-DINCLUDE_MOTION_DETECT` |
| `motion_telegram_photo` | bool | *alias* of `telegram_photo_on_motion` |
| `motion_notify_start_hour`, `motion_notify_end_hour` | int | *alias* of `telegram_active_start` / `telegram_active_end` |
| `motion_telegram_video` | bool | **stub**, always `false` — no video notifications exist |

### 6.8 Face detection

| Key | Type | Notes |
| --- | --- | --- |
| `face_detect_enabled`, `face_detect_two_stage`, `face_detect_save_sd` | bool | Settings echo, present in every build |
| `face_detect_cooldown` | int | Settings echo |
| `face_score_threshold`, `face_nms_threshold` | float | Settings echo |
| `face_detected` | bool | **Only with `-DINCLUDE_FACE_DETECT`** |
| `face_event_count`, `face_count`, `face_inference_ms` | int | **Only with `-DINCLUDE_FACE_DETECT`** |

Face detection is **off in the default build**, so the last two rows are absent from
a stock `/api/status`. See `docs/known_issues.md`.

### 6.9 Person detection and tracker

| Key | Type | Notes |
| --- | --- | --- |
| `person_detect_enabled` | bool | |
| `person_detection_enabled` | bool | *alias* |
| `person_detect_confidence` | float | |
| `person_confidence_threshold` | float | *alias* |
| `person_confident_threshold` | float | CONFIDENT / UNCERTAIN split point |
| `person_detect_temporal` | int | |
| `person_recheck_interval` | int | *alias* of `person_detect_temporal` |
| `person_detect_cooldown` | int | |
| `person_detection_cooldown` | int | *alias* |
| `person_detect_save_sd` | bool | |
| `tracker_confirm_hits`, `tracker_max_misses`, `tracker_match_dist` | int | Settings echo |
| `person_detected` | bool | Live |
| `person_event_count`, `person_count`, `person_track_count`, `person_inference_ms` | int | Live |
| `person_top_score` | float | Live |
| `person_decision` | string | `NONE` / `UNCERTAIN` / `CONFIDENT` |
| `person_last_detected` | bool | *alias* of the live detection flag |
| `person_last_confidence` | float | *alias* of `person_top_score` |
| `person_last_detections` | int | Raw detections before the tracker |
| `person_last_inference_ms` | int | *alias* of `person_inference_ms` |
| `person_last_decision` | string | *alias* of `person_decision` |
| `person_telegram_photo` | bool | *alias* of `telegram_photo_on_person` |

Without `-DINCLUDE_PERSON_DETECT` the live keys are still emitted, as
`false` / `0` / `"NONE"`.

### 6.10 Timelapse, MQTT, Telegram

| Key | Type | Notes |
| --- | --- | --- |
| `timelapse_enabled`, `timelapse_save_sd` | bool | |
| `timelapse_interval` | int | Seconds |
| `mqtt_enabled`, `mqtt_tls_enabled` | bool | |
| `mqtt_server`, `mqtt_topic_prefix` | string | |
| `mqtt_port` | int | |
| `mqtt_user` | string | Username is not treated as a secret |
| `mqtt_pass_set` | bool | Password presence only |
| `mqtt_connected` | bool | Live, only with `-DINCLUDE_MQTT` |
| `telegram_enabled` | bool | |
| `telegram_connected` | bool | With `-DINCLUDE_TELEGRAM`, this only means credentials exist — not that `api.telegram.org` is reachable |
| `telegram_notify_on_motion` / `_face` / `_person` | bool | |
| `telegram_photo_on_motion` / `_face` / `_person` | bool | |
| `telegram_cooldown` | int | Seconds |
| `telegram_active_start`, `telegram_active_end` | int | Hours |
| `telegram_poll_interval` | int | Milliseconds |
| `telegram_token_set`, `telegram_chat_id_set` | bool | Presence only — the values are never shipped here |
| `telegram_queue_depth` | int | **stub**, always `0` |
| `telegram_queue_ready`, `telegram_task_ready` | bool | **stub**, always `true` |
| `telegram_uploading` | bool | **stub**, always `false` |
| `telegram_sent`, `telegram_fail`, `telegram_drops` | int | **stub**, always `0` — no counters are wired to the Telegram task |

### 6.11 SD card

| Key | Type | Notes |
| --- | --- | --- |
| `sd_mounted` | bool | `false` without `-DINCLUDE_SD_CARD` |
| `sd_total_mb`, `sd_used_mb` | int | **Omitted** when no card is present |
| `sd_usage_pct` | int | **Omitted** when no card is present |
| `sd_write_disabled` | bool | Circuit breaker tripped — the card stopped accepting writes |
| `sd_write_failures` | int | Consecutive write failures |

`sd_write_disabled` is surfaced so a dead card looks like a card fault rather than
“detection just stopped saving anything”.

### 6.12 Environment and remaining stubs

| Key | Type | Notes |
| --- | --- | --- |
| `led_enabled` | bool | Status LED |
| `default_password` | bool | `true` while `http_pass` is still `DEFAULT_HTTP_PASS`; drives the warning banner in the UI |
| `ambient_light_lux` | float | **Misnamed**: this is the motion detector's average frame brightness, 0–255, not lux. `0` without `-DINCLUDE_MOTION_DETECT` |
| `camera_profile` | string | Derived from the above: `NIGHT` (< 15), `DUSK` (< 40), else `DAY` |
| `ir_led` | bool | **stub**, always `false` — no IR LED on this board |
| `is_recording` | bool | **stub**, always `false` — no recorder is wired up |

---

## 7. Port 81 — streams

No authentication, no CSRF, no config knob to add either. See section 8.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/stream` | MJPEG, UI/viewer role, max **3** clients (`MAX_STREAM_CLIENTS`) |
| HEAD | `/stream` | Headers only, for probing |
| OPTIONS | `/stream` | CORS preflight |
| GET | `/detection-stream` | MJPEG, detection role, max **2** clients (`MAX_DETECTION_STREAM_CLIENTS`) |
| HEAD | `/detection-stream` | Headers only |
| OPTIONS | `/detection-stream` | CORS preflight |
| GET | `/snapshot` | Single JPEG |

Both MJPEG routes share one handler, differing only in the `StreamRouteConfig` they
receive (path, role name, detection flag, client cap). The two caps are separate on
purpose: a browser tab left open on the dashboard must not lock out the machine-vision
consumer, and vice versa.

### Stream response

`Content-Type: multipart/x-mixed-replace; boundary=123456789000000000000987654321`

Response headers:

| Header | Value |
| --- | --- |
| `Access-Control-Allow-Origin` | `*` |
| `X-Framerate` | `appConfig.active_fps` |
| `X-Stream-Role` | `gui` or `detection` |
| `Cache-Control` | `no-cache, no-store, must-revalidate` |

Per-part headers:

| Header | Meaning |
| --- | --- |
| `Content-Type` | `image/jpeg` |
| `Content-Length` | JPEG size in bytes |
| `X-Timestamp` | `millis()` when the frame was captured |
| `X-Frame-Age` | Milliseconds between capture and send — the honest measure of stream latency |

Behaviour worth knowing:

* frames come from the shared capture ring buffer, and the handler tracks the last
  timestamp it sent, so **the same frame is never sent twice**. The stream rate is
  therefore bounded by the capture rate, not the other way round;
* a socket send timeout of **3 seconds** (`SO_SNDTIMEO`) is set so a stalled client
  cannot hold the server task forever;
* over the client cap: `503 Service Unavailable` with body `Too many stream clients`;
* `OPTIONS` replies with `Access-Control-Allow-Methods: GET, OPTIONS, HEAD` and
  `Access-Control-Allow-Headers: Content-Type`.

### `GET /snapshot` (port 81)

Serves the newest ring-buffer frame directly, with `Content-Disposition: inline;
filename=snapshot.jpg`, `Cache-Control: no-cache` and `X-Timestamp` when known. If
the ring is empty it falls back to a **direct synchronous `captureFrame()`** — which
is why it can succeed where port 80's `/api/snapshot` returns `503`. A failed capture
gives `500 Capture failed`.

Note the name collision: `/snapshot` exists on **both** ports with different
implementations. Port 80 copies into PSRAM and streams asynchronously; port 81 sends
from the ring buffer or captures on demand.

---

## 8. What is not authenticated

Stated plainly, because it determines where this device may be deployed:

1. **Everything on port 81.** `/stream`, `/detection-stream` and `/snapshot` are
   fully open. Anyone who can reach the port sees the video. There is no
   configuration option to change this — the IDF server registers the handlers with
   no auth check at all.
2. **The WebSocket `/ws`** on port 80 — see section 5.12 for why, and note it
   broadcasts a complete `/api/status` document every 2 seconds.
3. **`GET /api/status`** and all of its aliases (`/status`, `/settings`,
   `/telemetry`), plus `/health`, `/a12/status`, `/api/a12/status`,
   `/stream-stats`, `/psram-stats`, `/api/motion/debug`, `/motion-debug`,
   `/api/events`, `/events`, `/log`, `/log-viewer`, `/api/zones` (GET),
   `/api/roi` (GET), `/api/wifi/scan`, `/api/snapshot`, `/frame`, `/snapshot`, and
   the static UI.

What that exposes: live and still images, the WiFi SSID list around the device, its
MAC address, IP, hostname, uptime and crash history, complete detection state and
tuning, the MQTT broker address and username, zone geometry, the event log, and the
serial log. It does **not** expose the bot token, chat id, MQTT password, WiFi
password or HTTP password — those are reported as `*_set` booleans only, and
`/credentials` (which does reveal the chat id and SSID) requires auth.

Mitigation is network-level: keep the device off untrusted segments, and put a
reverse proxy in front of it if the streams must cross one.

---

## 9. Quick reference: authenticated `curl` flow

```bash
CAM=http://<camera-ip>
AUTH='-u admin:<your-password>'

# 1. Read state (no auth needed)
curl -s $CAM/api/status | jq '.uptime, .capture_fps, .default_password'

# 2. Fetch a CSRF token (auth required) and keep the value
TOKEN=$(curl -s $AUTH $CAM/api/csrf | jq -r .token)

# 3. Mutate: /api/** always needs the token
curl -s $AUTH -X POST $CAM/api/settings \
     -H 'Content-Type: application/json' \
     -H "X-CSRF-Token: $TOKEN" \
     -d '{"jpeg_quality":10,"motion_threshold":25}'

# 4. Legacy path from a script: no Origin header, so no token needed
curl -s $AUTH -X POST $CAM/settings \
     -H 'Content-Type: application/json' \
     -d '{"jpeg_quality":10}'

# 5. Grab a still, and watch the stream
curl -s -o frame.jpg $CAM/api/snapshot
curl -s -N http://<camera-ip>:81/stream --output - | head -c 0   # or open in a browser
```

The token survives until the next reboot. A `403` with
`{"error":"CSRF token invalid"}` after a device restart means: fetch it again.
