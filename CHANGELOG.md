# Changelog

All notable changes to this firmware. Versions follow semantic versioning, where
"breaking" means *an upgrade needs manual steps*, not an API change.

## [Unreleased]

### Note before downgrading

- Stored secrets are rewritten on first boot in a new format with an integrity tag
  (`ENC2`). Older firmware cannot read them: after a downgrade WiFi, the HTTP password,
  MQTT and Telegram credentials have to be entered again.

### Added

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
