# A12 companion integration

The firmware runs standalone, but it is also built to feed **A12**, a companion
service that does the heavy detection off-device: it consumes the camera's MJPEG
stream continuously and runs its own models (a YOLO-class detector over the COCO
classes, optional face recognition, and a vision model for scene descriptions).

Get the division of labour right, because it is the opposite of what "edge AI"
usually implies:

| | Job |
|---|---|
| **Camera** | Always-on, cheap: motion on an adaptive background model, a small FOMO person model to gate notifications, and a stable MJPEG stream. Notifies on its own when it is confident. |
| **A12** | The actual detection. Pulls `/detection-stream` frame after frame and decides what is in the picture, with models the ESP32 could never run. |

The camera is not a detector that occasionally asks for help; it is a well-behaved
frame source that happens to also do useful cheap detection of its own. Both halves
work without the other.

Replace `<camera-ip>` with the camera's address, or use its mDNS name
(`cams3.local` by default, from `wifi.hostname`).

## What A12 consumes

| Purpose | Endpoint | Notes |
|---|---|---|
| Frame source | `http://<camera-ip>:81/detection-stream` | The primary integration. Continuous MJPEG. |
| Liveness | `http://<camera-ip>/health` | What A12 polls. Small, cheap, stable field set. |
| Rich telemetry | `http://<camera-ip>/a12/status` (alias `/api/a12/status`) | Offered by the firmware for automation; A12 does not currently use it. Useful for dashboards and for diagnosing why the camera is unhealthy. |
| Single frame | `http://<camera-ip>/frame` | Recovery snapshots, tests. |
| Stream statistics | `http://<camera-ip>/stream-stats` | Client counts, FPS, frame age. |

None of these require authentication. That is deliberate for the stream path and is
also a limitation — see [`known_issues.md`](known_issues.md).

### Use `/detection-stream`, not `/stream`

There are two MJPEG routes on port 81 and they are not interchangeable:

- The detection route has its own client counter (`detection_clients`), so the camera
  can tell an automated consumer apart from someone watching the dashboard.
- Either route being connected switches the capture task to `active_fps`, so A12 alone
  keeps the frame rate up — nobody has to leave a browser open.
- Each MJPEG part carries `X-Timestamp` and `X-Frame-Age` headers, so A12 can drop
  stale frames instead of spending inference on them.
- The stream never re-sends a frame it already sent: it waits for a newer entry in the
  ring buffer. Duplicate-frame analysis is wasted GPU time.
- The route is capped at 2 concurrent clients (`MAX_DETECTION_STREAM_CLIENTS`), so a
  reconnect storm cannot starve the capture task. A third client gets HTTP 503.

## Liveness

A12 polls `/health`. Treat the camera as usable when it reports:

```
ok              == true          (equivalently overall_health == "ok")
wifi_connected  == true
capture_fps     >  0
frame_age_ms    <  10000
capture_errors  not increasing over time
```

When something is wrong, `issues` names it — `wifi down`, `stale frame (>10s)`,
`capture stalled`, `low heap`, `sd writes disabled` — so an alert can say what broke
instead of just that something did.

`/health` also carries the restart history: `total_restarts`, per-cause counters and
`power_health`, which turns `"suspect"` when the restart pattern points at the power
supply rather than at software. Combined with `uptime_seconds` this distinguishes a
camera that is browning out right now from one that did so last month.

**Field naming:** the endpoint emits `uptime_seconds`, `overall_health`,
`power_health`, `last_restart_reason_name` and `power_restarts_poweron` /
`power_restarts_brownout` — the exact names A12 reads — alongside the shorter
firmware-native forms (`uptime_sec`, `reset_reason`). Earlier firmware emitted only
the short forms, so A12 polled `/health`, found nothing it recognised, and its health
and power alerts never fired. If you change these names, A12 goes quiet without
reporting a problem.

`/a12/status` is the same picture plus detection state, stream URLs and capture
configuration, and it computes an `ok` flag for you (Wi-Fi up AND at least one frame
captured AND newest frame younger than 10 s). Prefer the URLs it reports
(`stream_url`, `detection_stream_url`, `snapshot_url`) over building them yourself —
they stay correct behind a proxy or on a non-default port.

## The camera's own detection

The FOMO model on the ESP32-S3 is ~64×64 grayscale and runs in a few hundred
milliseconds. Good enough to say "probably nobody" or "definitely somebody", and it
spends a lot of its time in between — so every confirmed detection is classified into
three states (`PersonDecision` in `include/person_detection.h`):

| Decision | Condition | Firmware behaviour |
|---|---|---|
| `NONE` | score < `person_detect_confidence` | Nothing |
| `UNCERTAIN` | between the two thresholds | Publishes an MQTT hint (below) |
| `CONFIDENT` | score ≥ `person_confident_threshold` | Notifies directly (Telegram / MQTT) |

Both thresholds are runtime-configurable (`POST /api/settings` →
`person_detect_confidence`, `person_confident_threshold`), so you decide how much the
camera handles alone. Push `person_confident_threshold` to 1.0 and the camera never
notifies by itself — every decision is A12's.

## MQTT

Set `mqtt_enabled`, `mqtt_server`, `mqtt_port`, optional `mqtt_user`/`mqtt_pass` and
`mqtt_topic_prefix` (default `cams3`). Below, `<prefix>` is that value.

Published by the camera:

| Topic | Payload | Retained | Notes |
|---|---|---|---|
| `<prefix>/availability` | `online` / `offline` | yes | `offline` is also the MQTT will, so a hard crash marks the camera down |
| `<prefix>/motion/state` | `ON` / `OFF` | yes | Edge-triggered. Home Assistant reads this one. |
| `<prefix>/motion` | `ON` / `OFF` | yes | Same value, flat name — A12 reads this one (see below) |
| `<prefix>/person/state` | `ON` / `OFF` | yes | Only for `CONFIDENT` |
| `<prefix>/person/attributes` | `{"count":N}` | yes | Confirmed tracks |
| `<prefix>/person_uncertain` | `{"confidence":0.68,"tracks":1}` | no | Hint: "I saw something, I am not sure" |
| `<prefix>/face/state` | `ON` / `OFF` | yes | Only when face detection is compiled in |
| `<prefix>/status` | JSON: `uptime`, `heap`, `heap_min`, `heap_total`, `psram`, `fps`, `rssi`, `clients` | no | Every 30 s |
| `<prefix>/camera/status/heartbeat` | `{"uptime":…,"free_heap":…}` | no | Every 5 s |
| `<prefix>/camera/status/profile` | `DAY` / `DUSK` / `NIGHT` | yes | Derived from frame brightness (this board has no lux sensor) |

`person_uncertain` is rate-limited to one publish per `person_detect_cooldown`, so
someone loitering in a badly lit spot cannot flood the broker.

### Topic names do not line up out of the box

A12 subscribes to two hint topics under a **different naming scheme** than this
firmware publishes:

| A12 subscribes to | This firmware publishes |
|---|---|
| `esp32cam/<device>/person_uncertain` | `<prefix>/person_uncertain` |
| `esp32cam/<device>/motion` (`ON`/`OFF`) | `<prefix>/motion` — published alongside `<prefix>/motion/state`, which is what Home Assistant's discovery points at |

`<device>` is A12's `esp32_mqtt_device` setting (default `ESP32-Camera`).

**So set `mqtt_topic_prefix` to `esp32cam/<device>` and both hints line up.** Nothing
else breaks when you do: Home Assistant discovery, availability and the telemetry
topics all follow the same prefix.

These hints are not cosmetic. A12 uses either of them to open a ~15 s window in which
it forces a YOLO pass, and the motion hint is OR'd into its own motion decision. With
a mismatched prefix A12 still works — it does its own frame differencing — but the
camera's motion detector, which sees a clean full-resolution frame before JPEG
transport, contributes nothing to it.

The firmware publishes motion under both names for exactly this reason: earlier
versions emitted only `<prefix>/motion/state`, so the gate never fired no matter how
the prefix was set.

Subscribed by the camera — A12 (or Home Assistant, or anything else) can change
settings without an HTTP round trip:

```
<prefix>/config/set/motion/enabled     ON | OFF | true | false | 1 | 0
<prefix>/config/set/motion/threshold   5..80
<prefix>/config/set/person/enabled     ON | OFF | true | false | 1 | 0
```

Underscore forms (`motion_enabled`, `motion_threshold`, `person_detect_enabled`) and
the legacy `camera/config/set/#` prefix are accepted too — and A12 already subscribes
to `camera/config/set/#`, so if both listen on one broker, be aware that a config
message on that legacy prefix is seen by both. Anything else is logged and ignored.
Accepted changes are persisted and survive a reboot.

Home Assistant auto-discovery is published on first connect under
`homeassistant/binary_sensor/<hostname>/…` and `homeassistant/sensor/<hostname>/…`
(motion, person, face, uptime, plus FPS / heap / min-heap / PSRAM / RSSI / clients as
diagnostic entities).

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
capture above that. For a stream that feeds an off-device detector, UXGA is the
pragmatic choice anyway: lower latency, less PSRAM and Wi-Fi pressure, and A12's
models downscale their input regardless.
