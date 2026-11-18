# Roadmap

What is planned next, in order. Each item names the known issue it closes
(`docs/known_issues.md`) and how we will know it is done. This is a plan, not a
promise; reorder freely.

## Phase 0: finish the arduino-esp32 3.3.9 port (now)

The port to pioarduino 55.03.39 (arduino-esp32 3.3.9 / ESP-IDF 5.5.4) builds and
boots. The prebuilt 3.3.9 core keeps Wi-Fi/lwIP buffers in internal RAM, which
cost ~30 kB of free internal heap (largest block 85 kB -> 30 kB). The
`idf5-hybrid` branch puts them back in PSRAM via `custom_sdkconfig`.

1. **Measure the hybrid build on the device.** Done when free internal heap is
   back near 100 kB, the largest free block near 85 kB, `psramFound()` is true,
   and a Telegram photo plus an MQTT-over-TLS publish both succeed after 24 h.
2. **Pick the branch and merge.** Hybrid if step 1 passes, plain `idf5`
   otherwise. Push, and confirm the first (cold, ~10 min) CI run is green.
3. **Release 2.2.0.** CHANGELOG entry for the core move, and how to downgrade
   (stored secrets stay readable; partition table is unchanged).
4. **Update stale docs.** `known_issues.md` §1, §2 and §12 still describe the
   2.0.17 situation (for example "do not upgrade to 3.1+").

## Phase 1: things the new core may already fix (cheap to check)

5. **Re-test resolutions above UXGA (§2).** The limit came from the 2.0.17
   camera driver; 3.3.9 ships esp32-camera 2.1.6. Try QXGA/QSXGA with
   `jpeg_quality` 10-12 for 30 min. If capture holds, widen the clamp and the
   settings page. Watch the 256 kB ring-slot limit (§9) at the same time.
6. **Frame size vs ring slot (§9).** If larger frames become usable, size
   `MAX_FRAME_SIZE` from the selected frame size instead of a fixed 256 kB
   (PSRAM has room: 3 slots x 512 kB = 1.5 MB of 8 MB).

## Phase 2: features that are half built

7. **Face detection via a FOMO model (§1, §12).** Train a face model in Edge
   Impulse with the existing person-detection pipeline (`docs/fomo_setup.md`) and
   run it through `src/person_detection.cpp`. Removes the dead esp-dl code path.
   Done when a face event reaches A12 and Telegram on the device.
8. **AVI recording (§6).** Add a recorder task that drives `avi_writer.cpp` from
   the ring buffer to `/recordings` on SD, triggered by motion/person events and
   `POST /record`. Write a real `idx1` index (or drop `AVIF_HASINDEX`), emit
   `recording_started/stopped` events, report a live `is_recording`.

## Phase 3: security hardening

9. **Optional auth for port 81 streams (§3).** Basic Auth or a token query
   parameter behind a setting, off by default so existing Home Assistant / A12
   consumers keep working; A12's detection consumer gets the credential.
10. **Optional certificate validation for Telegram (§4).** Use the ESP-IDF CA
    bundle behind a setting. Only after Phase 0 confirms the heap headroom: the
    handshake with validation needs more of it.

## Phase 4: image quality

11. **Backlit faces (§15).** Use the person/face box as the auto-exposure region
    (or a fixed lower-centre ROI) so subjects against a bright background are
    exposed correctly. Hardware fix (front light) stays the real answer.

## Not planned

- Microphone/audio, RTSP, light sensor (§16): the board has no hardware or the
  cost is out of proportion. A12 settings stay as documented.
- Translations of notification text (§10): Czech only until someone needs another
  language.
