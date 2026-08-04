# Contributing

## Before you start

This firmware runs unattended on a camera that people rely on. A change that adds a
feature but makes the device reboot at 3 a.m. is a net loss. Correctness and
predictability come before functionality.

## Setting up

```bash
git clone <this-repo>
cd CamS3-Firmware
pio run                 # builds the default configuration
```

Person detection needs an Edge Impulse FOMO model that is not in the repository — see
[docs/fomo_setup.md](docs/fomo_setup.md). Without it the build stops with a message
telling you where to get one, or you can comment out `-DINCLUDE_PERSON_DETECT`.

Flashing and first boot: [docs/FIRST_FLASH.md](docs/FIRST_FLASH.md).

## What to verify before opening a PR

CI runs the `ci` build environment and the web UI checks
(`.github/workflows/build.yml`). Run the same things locally first — plus the parts CI
cannot cover:

```bash
pio run                    # default env (needs a FOMO model)
pio run -e ci              # what CI builds: everything except person detection
pio run -t buildfs         # web UI → LittleFS image
node tools/check_www.js    # inline script syntax + translation parity
```

1. **Build the configurations you affected, not just yours.** Feature flags in
   `platformio.ini` gate whole files, and code behind a flag you left off still has to
   compile for everyone else. CI builds one combination; if you touched anything under
   `#ifdef INCLUDE_FACE_DETECT`, `INCLUDE_AVI_WRITER` or `LITE_MODE`, build with those
   enabled too:
   ```bash
   PLATFORMIO_BUILD_FLAGS="-DINCLUDE_FACE_DETECT" pio run
   PLATFORMIO_BUILD_FLAGS="-DLITE_MODE" pio run
   ```
2. **Web UI changes**: `node tools/check_www.js` covers inline `<script>` syntax,
   `en`/`cs` key parity in `data/www/i18n.js`, and that every key referenced from markup
   (`data-i18n*`) or page code (`T('…')`) exists. A missing key renders as the raw key
   on the device — the firmware build will not tell you.
3. **Docs must match the code.** If you add, rename or remove an endpoint or a settings
   key, update [docs/API.md](docs/API.md) in the same commit. Documented-but-nonexistent
   endpoints are worse than undocumented ones.

## Code conventions

- Match the surrounding style: 4-space indent, `camelCase` functions, `snake_case`
  config fields, `static` for file-local symbols.
- **Comments explain why, not what.** The repo is deliberately heavy on comments that
  record the reason a piece of code looks odd — a race it avoids, a driver bug it works
  around, a limit it respects. If you fix something subtle, leave that note behind;
  the next person will otherwise "simplify" it back into a bug.
- Log through `logCapture()`, not `Serial.printf()`. It echoes to serial *and* feeds the
  ring buffer that `/log` and `/log-viewer` read.
- Anything user-facing goes through the translation table, including `aria-label` and
  `alt` text. Do not branch on the current language in page code.
- Czech user-facing strings use proper diacritics and agree with their count — see
  `include/cz_text.h` for the plural helper.

## Concurrency rules worth knowing

These have all caused real bugs here:

- **Never touch the camera sensor (SCCB/I2C) outside the capture task.** HTTP handlers
  request a deferred apply with `cameraRequestSettingsApply()`; the capture task applies
  it between frames. Changing the frame size needs the driver's buffers rebuilt, so it
  goes through `cameraRequestReinit()` instead.
- **Never block an async web handler.** No `delay()`, no SD I/O, no camera teardown.
  Schedule work for the main loop and answer the request immediately.
- **Ring buffer reads are reference-counted.** Every `ringBufferGetLatest()` needs a
  matching `ringBufferRelease()` on the returned handle, and you must not hold a slot
  across a long operation — copy the frame out first. Holding slots starves the capture
  task, which then drops frames (`ring_dropped` in `/api/status`).
- **`jpg2rgb565` has no output bound.** Derive the decoded size from the dimensions the
  ring buffer reports for *that frame* and check it fits your buffer before decoding.
  Never derive it from the current configuration — it can change under you.

## Commits and PRs

- One logical change per commit, with a message that says what changed and why.
- Do not commit build output, the Edge Impulse export, or anything with addresses,
  credentials or personal deployment details.
