# A12 companion integration

The firmware is designed to run on its own, but it can also act as the edge half of a
two-stage detection pipeline. The camera does cheap, always-on detection (motion, then
a small FOMO person model); a companion service — referred to here as **A12** — runs
the expensive models (YOLO-class detection, face recognition) on frames the camera is
unsure about.

This document is the contract between the two. Replace `<camera-ip>` with the
camera's address, or use its mDNS name (`cams3.local` by default, from
`wifi.hostname`).

## Why two stages

The FOMO model on the ESP32-S3 is ~64×64 grayscale and runs in a few hundred
milliseconds. It is good enough to say "probably nobody" or "definitely somebody", but
it spends a lot of its time in between. Rather than guess, the firmware classifies
every confirmed detection into three states (`PersonDecision` in
`include/person_detection.h`):

| Decision | Condition | What the firmware does |
|---|---|---|
| `NONE` | score < `person_detect_confidence` | Nothing |
| `UNCERTAIN` | between the two thresholds | Publishes MQTT `person_uncertain` — this is A12's cue |
| `CONFIDENT` | score ≥ `person_confident_threshold` | Notifies directly (Telegram / MQTT) |

Both thresholds are runtime-configurable (`POST /api/settings` →
`person_detect_confidence`, `person_confident_threshold`), so you can tune how much
work you push to A12 without reflashing.

## HTTP endpoints A12 uses

| Purpose | Endpoint |
|---|---|
| Readiness / telemetry | `http://<camera-ip>/a12/status` (alias `/api/a12/status`) |
| Detection MJPEG stream | `http://<camera-ip>:81/detection-stream` |
| Operator MJPEG stream | `http://<camera-ip>:81/stream` |
| Single frame | `http://<camera-ip>/frame` |
| Stream statistics | `http://<camera-ip>/stream-stats` |
| Health probe | `http://<camera-ip>/health` |

None of these require authentication, which is deliberate for the stream path but is
also a limitation — see `docs/known_issues.md`.

### Use `/detection-stream`, not `/stream`

There are two MJPEG routes on port 81 and they are not interchangeable:

- The detection route has its own client counter (`detection_clients`), so the camera
  can tell an automated consumer apart from someone watching the dashboard.
- Either route being connected switches the capture task to `active_fps`, so A12 alone
  is enough to raise the frame rate — the operator does not have to keep a browser open.
- Each MJPEG part carries `X-Timestamp` and `X-Frame-Age` headers, so A12 can drop
  stale frames instead of analysing them.
- The stream never re-sends a frame it already sent: it waits for a newer entry in the
  ring buffer. Duplicate-frame analysis is wasted GPU time.
- The route is capped at 2 concurrent clients (`MAX_DETECTION_STREAM_CLIENTS`), so a
  reconnect storm cannot starve the capture task. A third client gets HTTP 503.

## Readiness check

Treat the camera as ready when `/a12/status` reports:

```
ok              == true
wifi_mode       == "STA"
capture_count   >  0
frame_age_ms    <  10000
capture_errors  not increasing over time
```

`ok` is computed by the firmware as "Wi-Fi connected AND at least one frame captured
AND the newest frame is younger than 10 s", so checking `ok` alone is usually enough;
the individual fields are there to tell you *why* it is false.

## `/a12/status` fields

Identity and link: `ok`, `device`, `version`, `ip`, `mac`, `wifi_mode`, `wifi_rssi`,
`uptime_sec`.

Streaming: `stream_url`, `detection_stream_url`, `snapshot_url`, `stream_port`,
`stream_clients`, `detection_clients`, `total_stream_clients`.

Capture: `capture_fps`, `capture_count`, `capture_errors`, `last_capture_ms`,
`frame_age_ms`, `active_fps`, `idle_fps`, `frame_size`, `jpeg_quality`.

Detection: `motion_enabled`, `motion_detected`, `motion_changed_pct`,
`motion_avg_brightness`, `motion_night_mode`, `motion_training`, `person_enabled`,
`person_detected`, `person_decision`, `person_count`, `person_track_count`,
`person_top_score`, `person_inference_ms`.

Integrations: `mqtt_enabled`, `mqtt_connected`, `telegram_enabled`, `sd_mounted`.

Prefer the URLs the camera reports (`stream_url`, `detection_stream_url`,
`snapshot_url`) over building them yourself — they stay correct if the ports change or
the camera sits behind a proxy.

`GET /api/status` returns a superset of these (~140 keys) including memory,
restart history and every configuration value; `/a12/status` is the stable, smaller
subset intended for automation.

## MQTT contract

Set `mqtt_enabled`, `mqtt_server`, `mqtt_port`, optional `mqtt_user`/`mqtt_pass` and
`mqtt_topic_prefix` (default `cams3`). Below, `<prefix>` is that value.

Published by the camera:

| Topic | Payload | Retained | Notes |
|---|---|---|---|
| `<prefix>/availability` | `online` / `offline` | yes | `offline` is also the MQTT will, so a hard crash marks the camera down |
| `<prefix>/motion/state` | `ON` / `OFF` | yes | Edge-triggered |
| `<prefix>/person/state` | `ON` / `OFF` | yes | Only for `CONFIDENT` |
| `<prefix>/person/attributes` | `{"count":N}` | yes | Confirmed tracks |
| `<prefix>/person_uncertain` | `{"confidence":0.68,"tracks":1}` | no | **A12's trigger to verify** |
| `<prefix>/face/state` | `ON` / `OFF` | yes | Only when face detection is compiled in |
| `<prefix>/status` | JSON: `uptime`, `heap`, `heap_min`, `heap_total`, `psram`, `fps`, `rssi`, `clients` | no | Every 30 s |
| `<prefix>/camera/status/heartbeat` | `{"uptime":…,"free_heap":…}` | no | Every 5 s |
| `<prefix>/camera/status/profile` | `DAY` / `DUSK` / `NIGHT` | yes | Derived from frame brightness (this board has no lux sensor) |

`person_uncertain` is rate-limited to one publish per `person_detect_cooldown`, so a
person loitering in a poorly lit spot will not flood A12.

Subscribed by the camera — A12 can change settings without an HTTP round trip:

```
<prefix>/config/set/motion/enabled     ON | OFF | true | false | 1 | 0
<prefix>/config/set/motion/threshold   5..80
<prefix>/config/set/person/enabled     ON | OFF | true | false | 1 | 0
```

Underscore forms (`motion_enabled`, `motion_threshold`, `person_detect_enabled`) and
the legacy `camera/config/set/#` prefix are accepted as well. Anything else is logged
and ignored. Accepted changes are persisted, so they survive a reboot.

Home Assistant auto-discovery is published on first connect under
`homeassistant/binary_sensor/<hostname>/…` and `homeassistant/sensor/<hostname>/…`
(motion, person, face, uptime, plus FPS / heap / min-heap / PSRAM / RSSI / clients as
diagnostic entities). If A12 and Home Assistant share a broker, the discovery entities
and the raw topics above coexist without interfering.

## Suggested soak test

1. Let A12 consume `/detection-stream` for 12–24 hours.
2. Watch `/stream-stats`: `detection_clients` should stay at 1, `capture_fps` near
   `active_fps`, `frame_age_ms` small, `capture_errors` flat.
3. Open the dashboard at the same time and confirm A12's stream is unaffected — the
   two routes have separate client budgets.
4. Watch `ring_dropped` in `/api/status`. If it climbs, a consumer is holding ring
   buffer slots too long and the capture task is discarding frames.
5. If A12 needs lower latency, reduce `frame_size` or raise `jpeg_quality`'s numeric
   value (lower quality) and compare `frame_age_ms`.

## Resolution note

The PY260 is a 5 MP sensor, but the firmware runs at `frame_size: 13`
(UXGA 1600×1200) because the PY260 driver in arduino-esp32 2.0.x cannot sustain
capture above that. For a verification pipeline UXGA is the pragmatic choice anyway:
lower latency, less PSRAM and Wi-Fi pressure, and A12's models downscale their input
regardless.
