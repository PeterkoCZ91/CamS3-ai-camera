# Changelog

All notable changes to this firmware. Versions follow semantic versioning, where
"breaking" means *an upgrade needs manual steps*, not an API change.

## [Unreleased]

## [2.3.0] — 2026-11-20

Upgrade over the air needs the **filesystem image** too (the settings page gained two
controls), which resets `config.json` to defaults: note your settings first.

### Added
- **QXGA (2048x1536)** is selectable. It holds ~11 fps at JPEG quality 12 on core 3.3.9 with an OV3660; the 2.0.x driver could not sustain anything above UXGA. The ring slots grew from 256 kB to 512 kB (3 x 512 kB PSRAM) so a large frame is no longer dropped as oversize.
- **Optional login for the stream and snapshot ports** (`stream_auth`, Settings > Security). Off by default so Home Assistant / A12 keep working; when on, HTTP Basic with the admin credentials is required.

### Changed
- Six task stacks (motion, person, telegram, mqtt, timelapse, focus) live in PSRAM and every flash write/read from them goes through one small worker task. Free internal heap rose from ~70 kB to ~100 kB, largest block from ~31 kB to ~55 kB.

### Verified
- 24-hour continuous soak test completed: 288/288 samples reachable (100%), 0 panics, 0 WDT resets, 0 restarts, average ~98.8 kB internal free heap, zero memory leak, average 10.69 FPS (live uptime >40 h).

## [2.2.0] — 2026-11-18

Moves the firmware from arduino-esp32 2.0.17 / ESP-IDF 4.4 to arduino-esp32 3.3.9 /
ESP-IDF 5.5 (pioarduino platform 55.03.39).

### Fixed
- **Units with the MEGA_CCM sensor (PID 0x039E) did not start** ("Camera probe failed 0x105", issue #1). Only the camera driver in core 3.x can probe it. MEGA_CCM gets its own settings path (different value ranges, no raw register access) and frame sizes round down to the ones it accepts (QVGA, VGA, HD, UXGA, FHD).
- The camera driver's internal task now has a 4 kB stack (2 kB before), the size that overflowed in the 2026-11 panics.

### Added
- **Automatic rollback of a bad update.** A new image is confirmed only after 60 s with Wi-Fi up and the camera delivering frames; a reboot before that (crash, watchdog, power cycle) makes the bootloader start the previous image again. Before, the core confirmed every image before `setup()`, so a crash-looping update stayed installed.

### Changed
- The task watchdog is reconfigured to 20 s (ESP-IDF 5 starts it at boot with 5 s; a plain init failed and silently left 5 s in force).
- `frame_size` keeps the 2.0.x numbering in `config.json`, the API, the UI and A12; the shifted ESP-IDF 5 enum is converted at the driver.
- `app_elf_sha256` in `/api/coredump/summary` is 8 hex digits (16 on 2.0.x).
- Free internal heap is ~30 kB lower (the prebuilt core keeps Wi-Fi/lwIP buffers in internal RAM). Measured largest free block ~30 kB; TLS handshakes allocate in smaller pieces, but see `docs/ROADMAP.md`.

### Removed
- Face detection (`-DINCLUDE_FACE_DETECT`, off by default) cannot be built on core 3.x and stops at an `#error`; see `docs/known_issues.md` §1.

### Upgrading
- Over the air, firmware only: the partition table, `config.json`, LittleFS and the encrypted secrets in NVS are unchanged and carry over. Tested on a unit flashed with 2.1.0, including a forced rollback.
- Building from source needs the pioarduino platform; PlatformIO downloads it from the URL in `platformio.ini`.

## [2.1.0] — 2026-11-18

### Note before downgrading

- Stored secrets are rewritten on first boot in a new format with an integrity tag
  (`ENC2`). Older firmware cannot read them: after a downgrade WiFi, the HTTP password,
  MQTT and Telegram credentials have to be entered again.

### Fixed
- **Black live view while A12 is connected.** The stream server runs every handler on one thread, so A12 holding `:81/detection-stream` left `:81/stream` waiting forever. Viewers now get their own server on **port 82**; `stream_url` in `/api/status`, `/a12/status` and `/stream-stats` points there (and back at port 81 if port 82 could not start). `:81/stream` is unchanged for existing setups. Home Assistant MJPEG cameras should be moved to `:82/stream`.
- `scheduleReboot()` honours its delay argument.
- **Wi-Fi scan froze the web server.** `GET /api/wifi/scan` ran a ~4 s synchronous scan inside the async handler, so the UI, `/health` and every other port-80 request stalled meanwhile. The main loop now runs the scan in the background; the endpoint answers `202 {"scanning":true}` until the result is ready and the Wi-Fi page polls it. The camera is still unreachable for ~3.5-4 s while the radio scans; that part is the radio and cannot be avoided. Scripts calling `/api/wifi/scan` (or `/wifi-scan`) must poll for the `200`.

### Fixed (code review 2026-11)
- **Motion events swallowed** when movement restarted inside `cooldown_sec` after the previous event (dead `cooldownExpired` branch); person detection now keeps inferring for 8 s after a motion trigger so the tracker can reach `confirm_hits` inside one event.
- **Telegram offline queue** no longer deletes queued photos while the network is simply down; every TLS/HTTP client has handshake/connect timeouts and feeds the task watchdog.
- **Boot loops from stored values**: out-of-range `poll_interval`, zone rectangles, tracker and timelapse parameters are clamped on load and on `POST /api/settings` (`sanitizeConfig()`); zone loops are bounded.
- **Use-after-free** in stream/snapshot `X-Framerate`/`X-Timestamp` headers; stream handler and SD writers no longer hold ring slots across slow I/O.
- Camera reinit now triggers on 10 *consecutive* capture errors; a camera that fails at boot is retried and finally rebooted; capture/focus/timelapse stacks raised to 6 KB with periodic high-water-mark logging.
- Atomic writes for zones, ROI mask, lens profiles, event log and stats; mutexes around zones and the event log; event details are JSON-escaped.
- CORS `*` removed; `/log`, `/api/events`, `/api/wifi/scan` require auth; empty auth credentials fail closed.
- UI: coredump card, "clear saved credentials", 401 handling, translated titles and toasts.

### Added
- **Core dump over HTTP**: `GET /api/coredump/summary` (task, PC, backtrace, ELF hash), `GET /api/coredump` (raw) and `POST /api/coredump/erase`, so a camera that panics in place can be diagnosed without USB.
- Opt-in `auto_profile` setting: a NIGHT exposure overlay driven by mean frame brightness (no light sensor on this board), applied from the capture task with hysteresis and a 60 s dwell.

- **`GET /api/focus`** (alias `/focus`): on-demand focus score (Laplacian variance of the centre crop) for focusing a manual lens by number, with a running `peak` and `?reset=1`. Costs nothing until requested, then samples every ~500 ms for 10 s after the last call.
- **Lens profiles**: `GET`/`POST /api/lens-profiles` save, apply and delete named snapshots (up to 4, stored in `/lens_profiles.json`) of `lens_fov_deg` + `lenc` + `sharpness`, so swapping a lens is one action. The `active` name is RAM-only.
- **`lens_ref_fov_deg`** setting (0-220, `0` = off): FOV the pixel thresholds were tuned for. When set together with `lens_fov_deg`, the tracker scales its match distance by `tan(ref/2)/tan(fov/2)`, clamped to 0.5-2 (both angles capped at 170). Only `match_dist` is scaled.

- **DFR1154 API alignment**: alias routes `/motion-status`, `/zones` (GET, POST, delete via
  `{"delete":"<name>"}`), `/roi-mask` (GET now also returns `w`, `h`), `/reg-debug`,
  `/wifi-scan`, `/sd-download`. `/status` and `/settings` gained `sensor`, `lens_fov_deg`
  (new persisted setting, 0-220), `uptime_days`, `uptime_hours`, `mqtt_state`,
  `mqtt_connects`, `mqtt_failures`, `mqtt_link_uptime_s`; `/health` gained `heap_health`,
  `wifi_quality`, `lens_fov_deg`, `sensor`. Not implemented: `/sd-clear`, `/sd-log`,
  `/wifi-save`, RTSP, audio. A12 users: set `AUDIO_ENABLED=false` and keep
  `camera_profiles.apply_from_a12=false`.
- **`GET /api/sensor`** — live sensor state sampled by the capture task: chip ID and the
  raw values of the exposure, gain, AEC target and metering-weight registers. Until now
  `/api/status` only echoed the configuration, so nobody could tell what auto exposure
  actually did. On hardware the sensor reports PID `0x5640` and the OV5640 register map
  holds (the exposure registers equal `aec_value`). `?reg=0xNNNN` reads one register.
- **`POST /reboot`** as an alias of `/api/reboot` for the A12 companion, which calls that
  path; `/health` now reports `device_name`, `ring_dropped` and `ring_oversize`.
- **`tools/flash_chunked.py`** writes the firmware in 64 kB pieces with retries, for boards
  whose single large write dies with "The chip stopped responding" (seen on a CamS3).
- **`tools/provision_wifi.py`** puts WiFi credentials into NVS over USB, so a board in
  AP mode can join the network without a phone.

### Fixed
- Recurring panics every 20-90 minutes (`Stack canary watchpoint triggered (cam_task)`): the camera driver's `cam_task` has a fixed 2 kB stack inside the prebuilt esp32-camera library, and logging a backed-up frame queue (`FBQ-RCV`/`FBQ-SND`) through the esp-insights hook and `vprintf` overflowed it. The `cam_hal` log tag is now silenced before `esp_camera_init()`; dropped frames are still counted.
- OTA updates now reboot into the new image: `ElegantOTA.loop()` was never called (a comment said otherwise), so a successful upload only took effect after the next power cycle or crash.

- **Permanent boot loop once the event log passed 100 lines.** Rotating
  `/events.jsonl` copied the whole ring (5.6 kB) onto the stack of whichever task
  logged the event; the motion task (6 kB) overflowed, and so did `setup()` on
  every following boot because the file was never rewritten. The snapshot now
  lives on the heap. A unit hit this in the field and crashed ~51,000 times
  before it was caught.
- `/status` and `/health` `sensor` is now derived from the sensor PID the driver
  read instead of a hard-coded "PY260" (a unit reporting 0x3660 is an OV3660).
- **The brownout detector was switched off.** `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0x10000)`
  clears the enable bits on the ESP32-S3, the opposite of its comment. A supply sag now
  gives a clean `BROWNOUT` reset instead of a lock-up or a power-on reset that hides the cause.
- **AP fallback was permanent.** After a power cut the camera often boots before the
  router; it fell back to `CamS3-Setup` and never tried the stored network again. It now
  retries every 45 s and leaves the portal on success; new credentials are stored only
  after they connect.
- **Saving secrets removed all keys first.** A power cut in between lost the WiFi
  credentials. Each key is now written on its own, only when it changed, with errors logged.
- **`config.json` could be replaced by a truncated file** on a full filesystem. The write
  is checked against the expected size, the previous file is kept as `config.json.bak`,
  and loading falls back to it.
- **Camera recovery:** a reinit that followed a failed one never restarted capture, and a
  reinit that "succeeded" without frames repeated forever; the watchdog counted frames that
  were dropped as delivered. All three now escalate to a reboot.
- **A stream client connecting after a failed camera init asserted** on a NULL mutex.
- **Motion analysed the same frame several times** at idle FPS, bypassing the two-frame
  confirmation, and `cooldown_sec` was effectively capped at 5 s.
- **`aec2` was accepted by `POST /api/settings` but never applied**; `frame_size` above
  UXGA (13) is no longer accepted, since it could leave the board rebooting at boot.
  The response now lists keys it ignored.
- **A stream client that disappeared while no frame was due was never released** on port 81.
  Port 81 still serves one stream client at a time; the docs now say so.
- **Telegram `/restart` rebooted the board again on every boot** until Telegram dropped the
  unacknowledged update; stale commands sent while offline also ran at boot. `/ticho` twice
  muted notifications permanently. A permanently rejected photo blocked the SD queue.
- **MQTT:** the person-detection task published from a second task (the client is not
  thread-safe); motion state is republished after reconnect and every second while ON;
  topic prefix and hostname are sanitised; truncated Home Assistant discovery payloads are
  no longer published.
- Docs: download mode on the button-less variant, flashing when large writes fail, and
  three new known issues (large writes, `/api/status` echoes config, no illumination output).

## [2.0.0]

Major because upgrading is not a plain OTA: the partition table changed, so the
filesystem image has to be re-uploaded (see below).

### Breaking

- **Partition table gained a `coredump` partition** (64 kB at `0xFF0000`), taken from
  the filesystem partition. The Arduino core is built with
  `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, so without this partition every panic was
  silently discarded. After flashing, run `pio run -t uploadfs` — the old LittleFS image
  no longer matches the shrunk partition and will not mount, losing `config.json`,
  zones and the event log. Encrypted secrets in NVS survive (same offset and size).
- **Face detection is off by default**, for two independent reasons: `HumanFaceDetectMSR01`
  comes from esp-dl's deprecated `dl_lib` API (arduino-esp32 removed the headers in v3.1+,
  so the pinned core is the last one that compiles it), and it costs ~660 kB of flash — the
  default build dropped from 67.1 % to 45.9 % flash and from 24.7 % to 21.7 % RAM. A
  `LoadStoreError` panic was also observed on this hardware with it enabled; see
  `docs/known_issues.md` for what is verified and what is a single unreproduced report.
  The code is kept and still builds; re-enable by uncommenting `-DINCLUDE_FACE_DETECT`.
- **The Edge Impulse FOMO model is no longer in the repository** (`lib/ei-person-fomo/`
  is gitignored). With `-DINCLUDE_PERSON_DETECT` on and no model present the build now
  fails with an explanatory `#error`. See `docs/fomo_setup.md`.

### Fixed — memory safety and stability

- **Heap corruption in person and face detection.** Both derived the decoded JPEG size
  from a hand-maintained `frame_size` table that never learned about QXGA (17) and
  QSXGA (22), fell through to a too-small default, and let `jpg2rgb565` write up to
  72 kB past the end of their PSRAM buffers. `frame_size` was settable over HTTP with
  no validation. The ring buffer now carries each frame's real dimensions, decoders
  derive the size from *that frame* and bounds-check before decoding, and buffers are
  sized for the sensor's maximum.
- **Use-after-free on camera reinit.** Teardown waited a flat 200 ms — less than one
  UXGA `esp_camera_fb_get()` — then freed ring buffer slots that streaming clients and
  detectors still held pointers into. Teardown now waits for the capture task to
  actually exit (up to 3 s), ring slots are never freed, and `cameraReinit()` reports
  failure instead of proceeding. The health watchdog escalates to a reboot after three
  failed recoveries.
- **SCCB/I²C race.** `applyConfigToCamera()` ran from the async web handler, writing
  sensor registers while the capture task was mid-frame, and called `set_framesize()`
  under a running driver. Sensor writes are now deferred to the capture task
  (`cameraRequestSettingsApply()`) and a resolution change goes through a scheduled
  reinit.
- **Off-by-one write** past the end of the WebSocket receive buffer.
- **Unbounded allocation** from a client-supplied `Content-Length` in POST handlers,
  now capped at 16 kB.
- **`/api/camera/reinit` blocked the async server task**; it now returns 202 and the
  main loop performs the reinit.

### Fixed — data loss and silent failures

- **SD card filled up silently.** `SD_MAX_USAGE_PERCENT` was defined and never read;
  motion, face and manual captures wrote into `/captures` with no rotation. Once the
  card was full `SD.open()` failed and the failure was swallowed — the device reported
  healthy while storing nothing. New `sd_store` module rotates the target directory and
  trips a session breaker after three consecutive failures, raising `EVT_SD_FAILURE` and
  exposing `sd_write_disabled` / `sd_usage_pct` in the status.
- **Saving settings wiped the Telegram credentials.** The settings page posted every
  field on every save, including an empty bot token, and the backend treated `""` as a
  value. Empty now means "leave unchanged" for all secrets and for `mqtt_server`; use
  `POST /api/secrets/clear?target=telegram|mqtt` to erase deliberately.
- **CSRF was disabled on any device that had saved a config**: the loader defaulted
  `csrf_required` to `false` while the struct default was `true`.
- **SD file endpoints had no authentication**, no CSRF and no path restriction — anyone
  on the network could list, download and delete card contents. Now behind Basic Auth
  with a directory whitelist and `..` rejection.
- **Config defaults disagreed with the struct**, so a saved config flipped `vflip` off
  (upside-down image), `contrast` and `sharpness` on the next boot.
- **"AEC" never controlled auto exposure**: it called `set_aec2()` (the DSP stage) and
  never `set_exposure_ctrl()`, so the manual exposure group had no effect. `aec2` is now
  a separate setting.
- **Home Assistant discovery could fail silently** — the MQTT client buffer (512 B) was
  smaller than a discovery payload plus its topic. Raised to 1024 B.
- **Capture frame rate was roughly half the configured value**: the limiter measured
  elapsed time from a timestamp it had just set, so every frame paid the full target
  delay on top of its capture time.

### Fixed — features that existed but did nothing

- **`/log` and `/log-viewer` were always empty.** Nothing ever called `logCapture()`;
  all 163 log sites used `Serial.printf()` directly. Logging now flows through the ring
  buffer, which also moved from BSS to PSRAM — freeing ~20 kB of internal DRAM.
- **The ROI mask was stored and ignored.** `/api/roi` persisted a mask that motion
  detection never read. It is now applied at boot and at runtime, and the trigger
  percentage is normalized over the ROI area instead of the whole grid, so masking part
  of the frame no longer raises the effective threshold for the rest.
- **The event log only recorded motion and boot.** Added person, face, Wi-Fi reconnect,
  low memory and SD failure events.
- **The temporal filter computed a spatial overlap and threw it away**, so two unrelated
  single-frame noise bursts passed as consecutive motion.
- **Night suppression only raised the pixel threshold**, letting noise-driven
  single-block clusters through; it now scales the required area too.

### Added

- **Restart accounting**: counters per reset cause (power-on, brownout, watchdog, panic,
  software), the current reset reason, longest uptime and core dump presence, persisted
  in LittleFS and exposed in `/api/status` and `/health`.
- **Lock-free ring buffer.** The writer claimed slots with a CAS instead of holding a
  mutex across a 256 kB copy, which used to serialize all five consumers (two MJPEG
  streams plus three detectors) behind every frame. Dropped frames are counted and
  logged (`ring_dropped`) instead of vanishing.
- **Motion → person fallback.** With person detection enabled, a motion notification is
  deferred for 5 s and sent only if the person path stayed quiet. Previously an
  `UNCERTAIN` verdict produced no notification at all and the event was lost.
- **FOMO input normalization**: the model input is contrast-stretched to its 1st/99th
  percentile, which is what low-light frames need to score above `UNCERTAIN`.
- **Zone and ROI editor** (`zones.html`): draw the mask and detection zones over the
  live image, with keyboard support.
- **Configuration reachable from the API and UI** that previously required hand-editing
  `config.json`: motion minimum area, EMA alphas, training frames, brightness floor,
  AGC factor, face thresholds, tracker parameters, MQTT username/password/TLS, hostname
  and the person "confident" threshold. All numeric settings are range-clamped.
- **Gzipped web assets**: a build-time pre-script compresses the UI into the filesystem
  image (198 kB → 46 kB) while leaving the sources uncompressed.
- **`Cache-Control` on static assets**, so navigating the UI no longer revalidates every
  file on every click.
- Health log now reports heap fragmentation and drift against a baseline, plus detection
  pipeline counters.
- `POST /api/secrets/clear`, `sd_usage_pct`, `ring_dropped`, restart fields and
  `telegram_token_set` in the status payload.

### Changed

- Gallery paginates (24 per page) and lazy-loads previews instead of downloading every
  full-resolution JPEG in a directory.
- `showToast`, `T` and `esc` consolidated into `common.js`; navigation markup unified.
- Accessibility: labels bound to their inputs, `:focus-visible` rings instead of
  `outline: none`, dialogs with `role="dialog"`, Escape and focus trapping, and
  `prefers-reduced-motion` support.
- Czech localization rewritten with correct diacritics, consistent terminology and
  typography; `aria-label` and `alt` are now translated. Two English strings were
  factually wrong and were corrected: the motion sensitivity label had its direction
  inverted, and "Night suppression" implied it suppressed detection rather than noise.
- Telegram messages use proper Czech diacritics and count-agreeing plurals.
- `/prah` in the Telegram bot accepts the same 5–80 range as the HTTP API (help text
  and validation previously disagreed).
- Shared image helpers extracted to `include/image_utils.h`; the RGB565→gray conversion
  and decoded-size logic were duplicated in three files.
- Event log appends instead of rewriting the whole file on every event.

### Removed

- `MIGRATION_FROM_DFROBOT.md` — stale and factually wrong (claimed this board has no
  microphone and no SD card; it has both).

## [1.0.0]

Initial CamS3 firmware: motion detection, FOMO person detection with tracking, MJPEG
streaming, Telegram, MQTT with Home Assistant discovery, timelapse, zones, event log,
OTA, captive-portal Wi-Fi setup and a bilingual web UI.
