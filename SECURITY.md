# Security policy

## Reporting a vulnerability

Open a GitHub issue. If the finding is sensitive, open an issue that says only that
you have something to report and ask for a private channel — do not paste the details
publicly first.

This is a hobby project maintained in spare time. There is no SLA, but security
reports are handled before feature work.

## Threat model

This firmware is designed for a **trusted LAN or an isolated camera VLAN**. It is not
hardened for direct exposure to the internet, and it should not be port-forwarded.
Concretely:

- The MJPEG streams on port 81 (`/stream`, `/detection-stream`) have **no
  authentication**. Anyone who can reach the port can watch the camera.
- The WebSocket at `/ws` has no authentication. It carries status only (uptime, heap,
  detection flags) — no credentials — but it does reveal that the camera exists and
  what it is doing.
- `GET /api/status` is unauthenticated and exposes device identity (MAC, IP, hostname,
  firmware version and hash) plus every non-secret configuration value.
- Outbound TLS to the Telegram API does not validate the server certificate
  (`setInsecure()`), so an attacker who can already intercept traffic leaving your
  network could read or alter notifications.

If you need any of these closed, put the camera on its own VLAN with no inbound
internet route, and reach it through a reverse proxy that enforces authentication.

## What is protected

- **Admin surface**: HTTP Basic Auth on configuration, camera control, SD access, OTA
  and factory reset. The UI shows a persistent banner while the default password is
  still in place.
- **CSRF**: mutating `/api/*` requests require an `X-CSRF-Token` obtained from
  `GET /api/csrf`. Legacy non-`/api/` endpoints, which exist for scripted
  integrations, require the token only when the request carries browser origin headers
  (`Origin` / `Sec-Fetch-Site`) — so a cross-site form POST from someone's browser is
  rejected while a curl-style client still works.
- **Secrets at rest**: Wi-Fi password, HTTP password, MQTT credentials and the
  Telegram bot token are stored in NVS encrypted with AES-256-CTR. The key is derived
  (HMAC-SHA256) from the eFuse MAC and a per-device random salt. This defends against
  a casual flash dump; it does **not** defend against an attacker who can read both
  the eFuse MAC and the salt from the device. It is not a substitute for flash
  encryption.
- **Secrets in transit**: credentials are never written to `config.json`, never logged,
  and never returned by any API. Endpoints report only whether a secret is set
  (`telegram_token_set`, `mqtt_pass_set`).
- **SD access**: file endpoints accept only paths under a fixed whitelist
  (`/captures`, `/timelapse`, `/recordings`, `/telegram_pending`, `/logs`) and reject
  any path containing `..`.
- **Request size**: JSON bodies are capped (`MAX_JSON_BODY`, 16 kB) so a
  client-supplied `Content-Length` cannot exhaust the heap.
- **OTA**: protected by the same Basic Auth; the SHA-256 of the newly flashed
  partition is logged so you can verify an upload matches your build output.

## Hardening checklist for a deployment

1. Change the default password on first boot (`admin` / `admin`).
2. Put the camera on a VLAN or subnet with no inbound internet route.
3. Leave `csrf_required` enabled.
4. If you use MQTT over TLS, provide a CA certificate at `/ca.pem` on the
   filesystem — without it the client falls back to not validating the broker.
5. Do not commit `data/ca.pem`, deployment notes or anything with addresses and
   credentials; `.gitignore` already excludes the usual suspects.
