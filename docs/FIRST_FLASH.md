# First flash — from clone to a running camera

Step by step, for an M5Stack Unit CamS3 (ESP32-S3-WROOM-1-N16R8, PY260 5 MP sensor)
that has never run this firmware. Every command is run from the repository root.

If you are **upgrading** an existing install, read
[§10 Upgrading from an older build](#10-upgrading-from-an-older-build) first — the
partition table changed and you must reflash the filesystem.

---

## 1. What you need

| Item | Notes |
| --- | --- |
| M5Stack Unit CamS3 5 MP | 16 MB flash, 8 MB octal PSRAM |
| USB-C data cable | The board exposes native USB CDC (`ARDUINO_USB_CDC_ON_BOOT=1`), so no external USB-serial adapter is needed |
| Python 3.9+ | For PlatformIO |
| PlatformIO Core | CLI is enough; the VS Code extension bundles it |
| microSD card (optional) | FAT32. Needed only for saving captures, timelapse and the gallery |

Install PlatformIO Core if you do not have it:

```bash
python3 -m pip install --user platformio
pio --version
```

If `pio` is not on your `PATH`, it is at `~/.platformio/penv/bin/pio`.

On Linux you also need permission to talk to the serial port — add yourself to the
`dialout` group (`sudo usermod -aG dialout "$USER"`, then log out and back in) and
install PlatformIO's udev rules:

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## 2. Get the source

```bash
git clone <repository-url> CamS3-Firmware
cd CamS3-Firmware
```

The first build downloads the toolchain, the pinned `espressif32@6.12.0` platform and
the four libraries from `lib_deps` into `.pio/` — expect a few hundred MB and several
minutes once.

## 3. Provide the person detection model (or turn it off)

**A fresh clone does not build with the default flags.** `lib/ei-person-fomo/` is
gitignored, so `-DINCLUDE_PERSON_DETECT` has no model to compile against and the
build stops on purpose:

```
error: "Edge Impulse FOMO model not found in lib/ei-person-fomo/. Export your
model as an Arduino library (see docs/fomo_setup.md), or comment out
-DINCLUDE_PERSON_DETECT in platformio.ini to build without person detection."
```

Pick one:

* **Bring a model** — follow `docs/fomo_setup.md` (train or clone a FOMO object
  detection model in Edge Impulse, deploy it as an Arduino library, extract it into
  `lib/ei-person-fomo/`). This is the full-featured path: motion → person cascade,
  tracker, `CONFIDENT`/`UNCERTAIN` decisions, Telegram and MQTT notifications.
* **Build without it** — comment out this line in `platformio.ini`:

  ```ini
  ; -DINCLUDE_PERSON_DETECT
  ```

  You still get streaming, motion detection, zones/ROI, timelapse, SD storage,
  Telegram, MQTT and the whole web UI.

Face detection is already off by default and should stay off — see
`docs/known_issues.md` §1.

## 4. Build

```bash
pio run
```

A successful build ends with a RAM/Flash summary. The `Flash` line is the one to
check:

```
Flash: [=====     ]  45.9% (used 1444480 bytes from 3145728 bytes)
```

That percentage is against the 3 MB `app0` partition. Around 46 % is normal for a
default build; ~67 % means you enabled face detection (`docs/known_issues.md` §1).

## 5. Flash the firmware

Connect the board over USB-C and run:

```bash
pio run -t upload
```

PlatformIO auto-detects the port. If it picks the wrong one, name it explicitly:

```bash
pio run -t upload --upload-port /dev/ttyACM0        # Linux
pio run -t upload --upload-port /dev/cu.usbmodem1101 # macOS
pio run -t upload --upload-port COM5                 # Windows
```

If the board is not detected at all, put it in download mode: hold **BOOT**, tap
**RESET**, release **BOOT**, then run the command again.

Upload speed is 460800 baud. If you see checksum errors on a long or unpowered
cable, lower it: `pio run -t upload --upload-speed 115200`.

## 6. Flash the filesystem — this step is mandatory

```bash
pio run -t uploadfs
```

**Do not skip this.** `pio run -t upload` writes only the application. The entire web
interface — `index.html`, `settings.html`, `admin.html`, `gallery.html`, `wifi.html`,
`zones.html`, `style.css`, `common.js`, `i18n.js` — lives in a LittleFS image built
from `data/`, and `serveStatic("/", LittleFS, "/www/")` is the only thing that serves
it. Without `uploadfs` the firmware boots, joins WiFi and answers the JSON API, but
every page request returns `404 Not Found`: there are no files to serve. It looks
exactly like a broken build.

The same image also holds the runtime state files the firmware writes:
`/config.json`, `/zones.json`, `/roi_mask.txt`, `/events.jsonl`, `/sysstats.bin`, and
optionally `/ca.pem` for MQTT TLS.

`uploadfs` runs `tools/gzip_www.py` first. That script stages a copy of `data/` under
`.pio/build/.../fsdata`, gzips the text assets there and points the image builder at
the copy — the sources in `data/` are never modified, and nothing new appears in
`git status`. Expect roughly a 4× reduction, which is both flash saved and TCP
segments saved on every page load. Only the `.gz` files are shipped; the async web
server prefers them automatically and adds `Content-Encoding: gzip`.

One consequence worth remembering: because only the gzipped copy is on the device, a
plain `curl http://<camera-ip>/index.html` receives raw gzip bytes. Use
`curl --compressed`, or just use a browser.

Build the image without flashing (useful for OTA later):

```bash
pio run -t buildfs      # -> .pio/build/m5stack-cams3/littlefs.bin
```

## 7. First boot — AP mode and the captive portal

Open the serial console:

```bash
pio device monitor
```

(115200 baud, set in `platformio.ini`.) You should see a banner, then the boot
sequence: PSRAM detected, LittleFS mounted, restart accounting, config loaded, SD
probe, camera init, WiFi.

With no WiFi credentials stored, the firmware starts its own access point:

| | |
| --- | --- |
| **SSID** | `CamS3-Setup` |
| **Password** | `cams3admin` |
| **Address** | The ESP32 softAP default, `192.168.4.1`, unless your build changes it — the serial log prints `AP IP: …` |

Both values are `DEFAULT_AP_SSID` / `DEFAULT_AP_PASS` in `include/config.h`. They are
compile-time constants: change them there and rebuild if you want something else.

A DNS server answers every lookup with the device's own address, so joining the
network should pop the **captive portal** automatically. Android probes
`/generate_204`, iOS and macOS probe `/hotspot-detect.html`, Windows probes
`/fwlink`; all three redirect to the setup page, and while the portal is active any
unknown path redirects there too.

If the portal does not appear by itself, browse to `http://192.168.4.1/` manually.

## 8. Join your WiFi, then lock the device down

### 8.1 Connect to your network

In the portal open the **WiFi** page. It calls `GET /api/wifi/scan`, lists the
networks it can see with signal strength, and posts your choice to
`POST /api/wifi`.

The response comes back immediately (`{"success":true,"message":"Connecting..."}`) —
that is by design, because the blocking connect happens in the main loop, not in the
HTTP handler. Watch the serial console for `Connected! IP: …`, or poll
`/api/status` and look at `wifi_mode` and `ip`.

The credentials are written to **AES-256-CTR-encrypted NVS**, not to the JSON config.
If the connection fails, the setup AP and captive portal come back so you can try
again.

Once connected, the device is reachable at `http://<camera-ip>/` and, with mDNS
enabled (default), at `http://cams3.local/`. The hostname comes from
`appConfig.wifi.hostname` — change it in Settings if you run more than one camera.

### 8.2 Change the default HTTP password

The firmware ships with:

| | |
| --- | --- |
| **User** | `admin` |
| **Password** | `admin` |

(`DEFAULT_HTTP_USER` / `DEFAULT_HTTP_PASS` in `include/config.h`.)

These protect every mutating endpoint, the SD browser, the credentials view and the
OTA page. **Change the password before the camera goes on a network you share with
anyone.**

The firmware makes this hard to forget: at boot it logs

```
[Main] WARNING: default admin password in use — change it in Settings
```

and `/api/status` reports `"default_password": true`, which the web UI turns into a
persistent banner until you change it.

Do it in **Settings → Security**, or by hand:

```bash
TOKEN=$(curl -s -u admin:admin http://<camera-ip>/api/csrf | jq -r .token)
curl -s -u admin:admin -X POST http://<camera-ip>/api/settings \
     -H 'Content-Type: application/json' \
     -H "X-CSRF-Token: $TOKEN" \
     -d '{"http_pass":"a-real-password"}'
```

Rules worth knowing (all silent — an ignored value still returns `success: true`):

* an **empty** `http_pass` means “leave unchanged”, so posting the settings form
  without touching the field does not wipe your password;
* shorter than 4 or longer than 64 characters is **ignored**, not rejected;
* the new password goes straight to encrypted NVS and takes effect immediately —
  your next request needs it;
* an empty `http_user` **or** empty `http_pass` disables authentication entirely for
  the whole device. Do not do that on a shared network.

The firmware deliberately does **not** generate a random password on first boot: a
user with no serial console would be locked out of their own camera.

### 8.3 Optional next steps

* **SD card** — insert a FAT32 card and reboot. The firmware creates `/captures`,
  `/timelapse` and `/recordings`, and `/api/status` starts reporting `sd_mounted`,
  `sd_total_mb`, `sd_used_mb`. Writes are rotated below `SD_MAX_USAGE_PERCENT`
  (90 %) and a circuit breaker stops trying after 3 consecutive failures.
* **Telegram** — paste the bot token and chat id in Settings. Both are stored
  encrypted in NVS and never returned by the API (`telegram_token_set` /
  `telegram_chat_id_set` report presence only). Read `docs/known_issues.md` §4 for
  the TLS trade-off before you use it.
* **MQTT** — broker address, port, credentials and topic prefix in Settings. For
  validated TLS, put your CA in `data/ca.pem` and re-run `pio run -t uploadfs`;
  without it, TLS falls back to unvalidated.
* **Motion tuning** — the **Zones** page draws the ROI mask and alert zones on the
  20 × 15 block grid, and `/api/motion/debug` shows exactly what the detector is
  computing. Both are documented in `docs/API.md` §5.6.

## 9. Later updates — OTA

Once the camera is on your network you do not need the cable again.

Open `http://<camera-ip>/update` (Basic Auth applies whenever both credentials are
set) and upload:

| Image | File | Mode |
| --- | --- | --- |
| Firmware | `.pio/build/m5stack-cams3/firmware.bin` | Firmware |
| Filesystem | `.pio/build/m5stack-cams3/littlefs.bin` | Filesystem |

Build them with `pio run` and `pio run -t buildfs` respectively.

After a successful firmware upload the device logs the **SHA-256 of the newly flashed
partition**:

```
[WebSrv] OTA complete — new partition SHA-256: <64 hex chars>
```

Compare it against the running partition's hash in `/api/status` (`fw_sha256`) after
the reboot — they should match, which confirms the image on the device is the image
you built. This is a hash of the flashed `.bin`, deliberately not the build-time ELF
hash.

Two OTA slots exist (`app0` and `app1`, 3 MB each), so a failed upload leaves the
previous firmware bootable.

## 10. Upgrading from an older build

> **The partition table changed. You must reflash the filesystem, not just the
> firmware.**

`partitions.csv` now reserves a **64 kB coredump partition** at the end of flash:

```
nvs,       data, nvs,      0x9000,   0x5000,
otadata,   data, ota,      0xE000,   0x2000,
app0,      app,  ota_0,    0x10000,  0x300000,
app1,      app,  ota_1,    0x310000, 0x300000,
spiffs,    data, spiffs,   0x610000, 0x9E0000,   <- 64 kB smaller than before
coredump,  data, coredump, 0xFF0000, 0x10000,    <- new
```

The Arduino core is built with `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, so without
this partition every panic dump was silently discarded and the panic handler was
useless. The 64 kB came out of the filesystem: **9.938 MiB → 9.875 MiB**.

### What happens if you flash only the firmware

The existing LittleFS image no longer matches its partition, so the mount fails. And
because `setup()` calls `LittleFS.begin(true)` — format-on-failure, which is the
right behaviour for a first boot — the device **formats the filesystem** to recover.
Result:

* the web UI is gone — every page returns `404 Not Found` (the JSON API still works,
  which makes it look like a UI bug rather than a missing filesystem);
* `/config.json` is gone, so **all settings revert to defaults**: camera parameters,
  motion tuning, FPS, Telegram and MQTT toggles, hostname;
* `/zones.json`, `/roi_mask.txt`, `/events.jsonl` and `/sysstats.bin` are gone —
  zones, ROI mask, event history and restart counters with them.

### What survives

**NVS keeps its offset and size** (`0x9000`, `0x5000`), so everything in encrypted
NVS survives untouched:

* WiFi SSID and password;
* the HTTP password;
* the Telegram bot token and chat id;
* MQTT username and password.

Practical consequence: after the upgrade the camera **rejoins your WiFi and still
wants your old password**, while every non-secret setting is back to factory values.
That combination is confusing if you are not expecting it.

### The correct upgrade sequence

```bash
git pull
pio run
pio run -t upload      # application
pio run -t uploadfs    # filesystem — REQUIRED on this upgrade
pio device monitor     # confirm: "LittleFS mounted"
```

Over OTA: upload the firmware image, then the filesystem image, then reboot. Either
way, expect to re-enter your non-secret settings once. Export them first if you care
— `GET /api/status` returns every current value, and `GET /api/zones` plus
`GET /api/roi` return the zone and ROI definitions, so you can replay them through
`POST /api/settings`, `POST /api/zones` and `POST /api/roi` afterwards.

---

## 11. Troubleshooting

### The camera does not initialize

Serial shows:

```
[Main] CRITICAL: Camera init failed!
```

and the status LED (GPIO 14) blinks five times. The firmware carries on so you can
still reach the web UI and the log for diagnostics.

Checks, in order:

1. **The ribbon cable.** By far the most common cause. Reseat it at both ends; make
   sure it is fully home and the connector latch is closed. A partially seated cable
   often gives an init that succeeds and then fails within a minute.
2. **Power.** The 5 MP sensor plus WiFi draws real current. A weak USB port or a thin
   cable shows up as brownout reboots — check `brownout_restarts` in `/api/status`.
   Try a powered hub or a different supply.
3. **PSRAM.** The boot banner must say `PSRAM: OK (8MB)`. If it says `NOT FOUND`,
   the build flags do not match the board — `qio_opi`,
   `-DCONFIG_SPIRAM_MODE_OCT=1` and `-DBOARD_HAS_PSRAM` must all be present.
4. **Resolution.** If you set `frame_size` above 13 through the API, the PY260 driver
   produces nothing (`docs/known_issues.md` §2). Post `{"frame_size": 13}` to
   `/api/settings`, or factory-reset with `POST /api/reset`.

The health watchdog reinits the camera automatically after 30 s with no capture, and
reboots the device after three failed reinits, so a transient fault self-heals. A
permanent fault turns into a reboot loop — `total_restarts` and `reset_reason` in
`/api/status` will show it.

### The SD card does not mount

Serial shows:

```
[Main] SD 20MHz mount failed, retrying @4MHz
[Main] SD card mount failed (no card / bad contact / unformatted FAT)
```

The firmware already retries: it tries SPI at **20 MHz** first and, on failure, ends
the bus and retries at **4 MHz**, because loose or long connections frequently do not
tolerate the higher clock. If both fail:

* **Format the card as FAT32.** exFAT and ext4 are not supported. Cards over 32 GB
  often ship exFAT out of the box.
* **Reseat the card** and check for debris in the slot. The SPI pins are CLK 39,
  MOSI 38, MISO 40, CS 9.
* **Try a different card.** Cheap and worn cards fail this test disproportionately.

If the card mounts but stops accepting writes later, look at `sd_write_disabled` and
`sd_write_failures` in `/api/status`: after 3 consecutive write failures a circuit
breaker stops all writes for the rest of the session and raises an `sd_failure`
event, so a dead card is visible instead of looking like “detection stopped saving”.
A reboot re-arms it.

### `/log` is empty

Two distinct causes:

1. **`503 Log buffer unavailable`** — the ~25 kB PSRAM allocation for the log
   snapshot failed. That means PSRAM is exhausted or badly fragmented; check
   `free_psram` and `psram_usage_pct` in `/api/status` and reboot.
2. **`200` with an empty or nearly empty body** — the ring genuinely has nothing in
   it. Only output written through `logCapture()` is captured; a module using
   `Serial.printf()` directly appears on the serial console but never in `/log`. If
   you added code and cannot find its output here, that is why. Capacity is
   `LOG_RING_LINES` = 100 lines of up to 256 characters, so a chatty boot can also
   have already pushed out what you were looking for.

`/log-viewer` polls `/log` every 5 seconds and shows the same content in a browser.

### The stream is stuck or will not start

* **`503 Too many stream clients`** — you hit the cap. `/stream` allows 3 concurrent
  clients (`MAX_STREAM_CLIENTS`), `/detection-stream` allows 2. Check
  `stream_clients` and `detection_clients` in `/api/status`. A browser tab that was
  closed uncleanly can hold a slot until the 3-second socket send timeout expires.
* **Frozen image, healthy `capture_fps`** — look at `frame_age_ms`. If it keeps
  growing while `capture_fps` looks fine, frames are not reaching the ring buffer.
  Two candidates: `ring_dropped` climbing means a consumer is falling behind (too
  many detectors plus streams for the configured FPS); `ring_dropped` **not**
  climbing while the stream is frozen means frames are exceeding the 256 kB
  per-slot limit and being dropped uncounted — raise `jpeg_quality` (higher number =
  more compression). See `docs/known_issues.md` §9.
* **First frame takes a second** — expected. `idle_fps` defaults to 1, and the
  handler never sends the same frame twice, so a fresh viewer waits for the next
  capture before the rate climbs to `active_fps` (15).
* **Nothing at all on port 81** — confirm you are using the right port. Streams live
  on `http://<camera-ip>:81/stream`, not on port 80. `/api/status` gives you the
  exact URLs in `stream_url` and `detection_stream_url`.
* **After changing the resolution** — `POST /api/settings` answers with
  `"camera_restart": true` when `frame_size` changed, because the driver's DMA
  buffers have to be rebuilt. The stream drops for 1–3 seconds; reconnect.

### The web UI is blank or returns 404

You almost certainly skipped `pio run -t uploadfs`, or you flashed only the firmware
over an older build and LittleFS reformatted itself (§10). Check the serial log for
`LittleFS mounted`, then flash the filesystem.

### Recovering a device that will not behave

```bash
pio run -t erase                 # erase the whole chip — this DOES wipe NVS secrets
pio run -t upload
pio run -t uploadfs
```

A full erase clears NVS too, so WiFi credentials, the HTTP password and the Telegram
and MQTT secrets are gone and the device comes back up in `CamS3-Setup` AP mode.
That is the point: it is a genuine factory state, unlike `POST /api/reset`, which
clears only `/config.json` and leaves the encrypted secrets in place.
