#include "telegram.h"
#include "ws_log.h"

#ifdef INCLUDE_TELEGRAM

#include "config.h"
#include "camera_manager.h"
#include "wifi_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#include <FS.h>
#define TG_PENDING_DIR       "/telegram_pending"
#define TG_PENDING_MAX       50     // cap so we don't fill the card
#define TG_PENDING_MAGIC     0x54475057UL  // "TGPW"
struct TgPendingHeader {
    uint32_t magic;
    uint32_t caption_len;   // bytes, no null terminator
    uint32_t jpeg_len;      // bytes
    uint32_t reserved;
};
#endif

static const char* TAG = "Telegram";

// Forward declarations
extern void saveSecretsToNVS();

// Queue message structure
#define TELEGRAM_MSG_MAX_LEN 512
#define TELEGRAM_QUEUE_SIZE  5

struct TelegramMessage {
    char text[TELEGRAM_MSG_MAX_LEN];
    bool has_photo;
    uint8_t* photo_data;    // PSRAM allocated, freed after send
    size_t photo_len;
};

static QueueHandle_t tgQueue = NULL;
static int64_t lastUpdateId = 0;
static unsigned long lastTgSendTime = 0;

// Silent mode — when non-zero, notifications are suppressed until this
// millis() timestamp, after which an auto-resume message is sent.
// Runtime-only (lost across reboot on purpose — safer default).
static unsigned long silentUntilMs = 0;
static bool silentSavedNotifyMotion = false;
static bool silentSavedNotifyFace   = false;
static bool silentSavedNotifyPerson = false;

// HTTP status of the last sendPhotoSync (0 = network/timeout failure).
static int lastPhotoHttpStatus = 0;
// Permanent client error: retrying the same payload can never succeed (429 excepted).
static bool isPermanentHttpError(int code) { return code >= 400 && code < 500 && code != 429; }

// Copy at most dstSize-1 bytes, never cutting a UTF-8 multibyte sequence in half
// (a split character makes Telegram answer 400 forever).
static void copyUtf8Truncated(char* dst, size_t dstSize, const char* src) {
    if (!dstSize) return;
    size_t n = src ? strlen(src) : 0;
    if (n > dstSize - 1) {
        n = dstSize - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;  // back up to a lead byte
    }
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

// Telegram parse_mode=HTML: dynamic text must have &, <, > escaped.
static String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

// Forward decls used by the boot sequence / command menu
#ifdef INCLUDE_MQTT
#include "mqtt_handler.h"
#endif
static bool sendTextSync(const char* text);
static bool sendPhotoSync(const uint8_t* jpeg, size_t jpegLen, const char* caption);
static void registerBotCommands();
static void sendBootMessage();

#ifdef INCLUDE_SD_CARD
// Spill a photo to SD with a small header so one file carries the full
// caption+payload. Used when the in-RAM queue is full or the send fails.
static bool persistPhotoToSd(const uint8_t* jpeg, size_t jpegLen, const char* caption) {
    if (SD.cardType() == CARD_NONE) return false;
    if (!SD.exists(TG_PENDING_DIR)) SD.mkdir(TG_PENDING_DIR);

    // Rotate: if we already have too many, drop the oldest.
    {
        File dir = SD.open(TG_PENDING_DIR);
        if (dir) {
            int count = 0;
            String oldest;
            uint64_t oldestMtime = UINT64_MAX;
            File f = dir.openNextFile();
            while (f) {
                count++;
                time_t t = f.getLastWrite();
                if ((uint64_t)t < oldestMtime) { oldestMtime = t; oldest = String(f.name()); }
                f.close();
                f = dir.openNextFile();
            }
            dir.close();
            if (count >= TG_PENDING_MAX && oldest.length()) {
                String path = String(TG_PENDING_DIR) + "/" + oldest;
                SD.remove(path);
                logCapture("[%s] SD queue full, dropped oldest %s\n", TAG, oldest.c_str());
            }
        }
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%lu.tgp", TG_PENDING_DIR, (unsigned long)millis());
    File out = SD.open(path, FILE_WRITE);
    if (!out) return false;

    TgPendingHeader hdr = {
        .magic = TG_PENDING_MAGIC,
        .caption_len = (uint32_t)(caption ? strlen(caption) : 0),
        .jpeg_len = (uint32_t)jpegLen,
        .reserved = 0,
    };
    size_t w = out.write((const uint8_t*)&hdr, sizeof(hdr));
    if (caption && hdr.caption_len) w += out.write((const uint8_t*)caption, hdr.caption_len);
    w += out.write(jpeg, jpegLen);
    out.close();

    bool ok = (w == sizeof(hdr) + hdr.caption_len + jpegLen);
    if (!ok) { SD.remove(path); return false; }
    logCapture("[%s] Photo spilled to SD: %s (%u bytes)\n", TAG, path, (unsigned)jpegLen);
    return true;
}

// Pop the oldest pending file and try to send it. Returns true if one
// was processed (sent OR unrecoverable + deleted); caller can call again
// to drain multiple per cycle.
static bool flushOnePendingFromSd() {
    if (SD.cardType() == CARD_NONE) return false;
    if (!SD.exists(TG_PENDING_DIR)) return false;

    // Find oldest by mtime.
    File dir = SD.open(TG_PENDING_DIR);
    if (!dir) return false;
    String oldest;
    uint64_t oldestMtime = UINT64_MAX;
    File f = dir.openNextFile();
    while (f) {
        time_t t = f.getLastWrite();
        if ((uint64_t)t < oldestMtime) { oldestMtime = t; oldest = String(f.name()); }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    if (!oldest.length()) return false;

    String path = String(TG_PENDING_DIR) + "/" + oldest;
    File in = SD.open(path, FILE_READ);
    if (!in) return false;

    TgPendingHeader hdr;
    if (in.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != TG_PENDING_MAGIC) {
        in.close();
        SD.remove(path);
        logCapture("[%s] Dropped corrupted pending %s\n", TAG, oldest.c_str());
        return true;
    }

    // Bound sanity: 2 MB JPEG / 512 B caption.
    if (hdr.jpeg_len > 2 * 1024 * 1024 || hdr.caption_len > 512) {
        in.close();
        SD.remove(path);
        return true;
    }

    char caption[513] = {};
    if (hdr.caption_len) {
        in.read((uint8_t*)caption, hdr.caption_len);
        caption[hdr.caption_len] = 0;
    }

    uint8_t* jpeg = (uint8_t*)ps_malloc(hdr.jpeg_len);
    if (!jpeg) { in.close(); return false; }  // try again next cycle when heap eases
    in.read(jpeg, hdr.jpeg_len);
    in.close();

    bool sent = sendPhotoSync(jpeg, hdr.jpeg_len, caption);
    free(jpeg);

    // Retry counter is RAM-only: enough to stop a poisoned file blocking the queue.
    static String failPath;
    static int failCount = 0;
    if (sent) {
        SD.remove(path);
        failCount = 0;
        lastTgSendTime = millis();
        logCapture("[%s] Flushed pending %s (%u bytes)\n", TAG, oldest.c_str(), (unsigned)hdr.jpeg_len);
    } else if (isPermanentHttpError(lastPhotoHttpStatus)) {
        // 4xx will never succeed (bad caption/JPEG) — dead-letter it so the queue moves on.
        SD.remove(path);
        failCount = 0;
        logCapture("[%s] Dropped pending %s: HTTP %d (permanent)\n", TAG, oldest.c_str(), lastPhotoHttpStatus);
        return true;
    } else {
        if (failPath != path) { failPath = path; failCount = 0; }
        if (++failCount >= 5) {
            SD.remove(path);
            failCount = 0;
            logCapture("[%s] Dropped pending %s after 5 failed retries\n", TAG, oldest.c_str());
            return true;
        }
        logCapture("[%s] Flush failed for %s — will retry (%d/5)\n", TAG, oldest.c_str(), failCount);
    }
    return sent;
}
#endif  // INCLUDE_SD_CARD

// --- Active hours helper ---

bool isWithinActiveHours() {
    struct tm ti;
    if (!getLocalTime(&ti, 0)) return true;  // NTP not synced = allow
    int hour = ti.tm_hour;
    int start = appConfig.telegram.active_start_hour;
    int end = appConfig.telegram.active_end_hour;
    if (start <= end) return (hour >= start && hour <= end);
    return (hour >= start || hour <= end);  // overnight wrap
}

bool isTelegramConnected() {
    return appConfig.telegram.bot_token.length() > 0 &&
           appConfig.telegram.chat_id.length() > 0;
}

// --- Synchronous send functions (called from task only) ---

static bool sendTextSync(const char* text) {
    if (!isTelegramConnected() || !isWiFiConnected()) return false;

    String url = "https://api.telegram.org/bot" + appConfig.telegram.bot_token + "/sendMessage";

    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    if (!http.begin(client, url)) {
        logCapture("[%s] HTTP begin failed\n", TAG);
        return false;
    }

    http.setTimeout(10000);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["chat_id"] = appConfig.telegram.chat_id;
    doc["text"] = text;
    doc["parse_mode"] = "HTML";

    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    http.end();

    if (code == 200) {
        logCapture("[%s] Text sent OK\n", TAG);
        return true;
    }
    logCapture("[%s] sendMessage failed: %d\n", TAG, code);
    return false;
}

static bool sendPhotoSync(const uint8_t* jpeg, size_t jpegLen, const char* caption) {
    lastPhotoHttpStatus = 0;
    if (!isTelegramConnected() || !isWiFiConnected()) return false;

    WiFiClientSecure client;
    client.setInsecure();

    if (!client.connect("api.telegram.org", 443)) {
        logCapture("[%s] TLS connect failed\n", TAG);
        return false;
    }

    String boundary = "----CamS3Boundary";
    String path = "/bot" + appConfig.telegram.bot_token + "/sendPhoto";

    // Build multipart body parts
    String partHeader = "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
        appConfig.telegram.chat_id + "\r\n";

    if (caption && caption[0]) {
        partHeader += "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" +
            String(caption) + "\r\n";
    }

    String fileHeader = "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"capture.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";

    String tail = "\r\n--" + boundary + "--\r\n";

    size_t contentLen = partHeader.length() + fileHeader.length() + jpegLen + tail.length();

    // Send HTTP request
    client.printf("POST %s HTTP/1.1\r\n", path.c_str());
    client.println("Host: api.telegram.org");
    client.printf("Content-Length: %u\r\n", contentLen);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
    client.println("Connection: close");
    client.println();

    // Send body
    client.print(partHeader);
    client.print(fileHeader);

    // Send JPEG in 1KB chunks
    size_t sent = 0;
    while (sent < jpegLen) {
        size_t chunk = min((size_t)1024, jpegLen - sent);
        size_t written = client.write(jpeg + sent, chunk);
        if (written == 0) {
            logCapture("[%s] Photo write stalled at %u/%u\n", TAG, sent, jpegLen);
            client.stop();
            return false;
        }
        sent += written;
        vTaskDelay(1);  // yield to WDT
    }

    client.print(tail);
    client.flush();

    // Read response status
    unsigned long timeout = millis() + 15000;
    while (!client.available() && millis() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    bool success = false;
    if (client.available()) {
        String statusLine = client.readStringUntil('\n');
        // "HTTP/1.1 400 Bad Request" -> 400
        int sp = statusLine.indexOf(' ');
        lastPhotoHttpStatus = sp >= 0 ? statusLine.substring(sp + 1).toInt() : 0;
        success = lastPhotoHttpStatus == 200;
        if (!success) {
            logCapture("[%s] sendPhoto failed: %s\n", TAG, statusLine.c_str());
        } else {
            logCapture("[%s] Photo sent OK (%u bytes)\n", TAG, jpegLen);
        }
    } else {
        logCapture("[%s] sendPhoto timeout\n", TAG);
    }

    client.stop();
    return success;
}

// --- Command handling ---

// While muted the notify_on_* flags are temporarily false; persist the user's real
// values instead so a reboot or any save never turns a timed mute into a permanent one.
static void saveConfigUnmuted() {
    if (silentUntilMs == 0) { saveConfig(); return; }
    bool m = appConfig.telegram.notify_on_motion, f = appConfig.telegram.notify_on_face,
         p = appConfig.telegram.notify_on_person;
    appConfig.telegram.notify_on_motion = silentSavedNotifyMotion;
    appConfig.telegram.notify_on_face   = silentSavedNotifyFace;
    appConfig.telegram.notify_on_person = silentSavedNotifyPerson;
    saveConfig();
    appConfig.telegram.notify_on_motion = m;
    appConfig.telegram.notify_on_face   = f;
    appConfig.telegram.notify_on_person = p;
}

static void handleCommand(const String& cmd, const String& args) {
    logCapture("[%s] Command: /%s %s\n", TAG, cmd.c_str(), args.c_str());

    if (cmd == "foto" || cmd == "photo") {
        const uint8_t* buf = NULL;
        size_t len = 0;
        int rh = ringBufferGetLatest(&buf, &len);
        if (rh >= 0) {
            // Copy to PSRAM for sync send
            uint8_t* copy = (uint8_t*)ps_malloc(len);
            if (copy) {
                memcpy(copy, buf, len);
                ringBufferRelease(rh);
                sendPhotoSync(copy, len, "Snapshot");
                free(copy);
            } else {
                ringBufferRelease(rh);
                sendTextSync("Chyba: nedostatek PSRAM");
            }
        } else {
            sendTextSync("Žádný snímek k dispozici");
        }
    }
    else if (cmd == "status" || cmd == "stav") {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<b>CamS3 — stav</b>\n"
            "IP: %s\n"
            "Uptime: %lus\n"
            "Heap: %dKB\n"
            "PSRAM: %dMB\n"
            "WiFi RSSI: %d dBm\n"
            "FPS: %.1f\n"
            "Stream klientů: %d\n"
            "Pohyb: %s\n"
            "Obličeje: %s\n"
            "Osoby (FOMO): %s\n"
            "Telegram – pohyb: %s\n"
            "Telegram – obličeje: %s\n"
            "Telegram – osoby: %s\n"
            "Práh pohybu: %d\n"
            "Cooldown: %ds\n"
            "Aktivní hodiny: %d–%d\n"
            "Interval dotazů: %d ms",
            getIPAddress().c_str(),
            millis() / 1000,
            ESP.getFreeHeap() / 1024,
            ESP.getFreePsram() / (1024 * 1024),
            getRSSI(),
            getCaptureFps(),
            getStreamClientCount(),
            appConfig.motion.enabled ? "ZAP" : "VYP",
            appConfig.face_detect.enabled ? "ZAP" : "VYP",
            appConfig.person_detect.enabled ? "ZAP" : "VYP",
            appConfig.telegram.notify_on_motion ? "ZAP" : "VYP",
            appConfig.telegram.notify_on_face ? "ZAP" : "VYP",
            appConfig.telegram.notify_on_person ? "ZAP" : "VYP",
            appConfig.motion.threshold,
            appConfig.telegram.cooldown_sec,
            appConfig.telegram.active_start_hour,
            appConfig.telegram.active_end_hour,
            appConfig.telegram.poll_interval_ms
        );
        sendTextSync(buf);
    }
    else if (cmd == "detekce") {
        appConfig.telegram.notify_on_motion = !appConfig.telegram.notify_on_motion;
        saveConfigUnmuted();
        sendTextSync(appConfig.telegram.notify_on_motion ?
            "Telegram – pohyb: ZAP" : "Telegram – pohyb: VYP");
    }
    else if (cmd == "obliceje") {
        appConfig.telegram.notify_on_face = !appConfig.telegram.notify_on_face;
        saveConfigUnmuted();
        sendTextSync(appConfig.telegram.notify_on_face ?
            "Telegram – obličeje: ZAP" : "Telegram – obličeje: VYP");
    }
    else if (cmd == "osoba") {
        appConfig.telegram.notify_on_person = !appConfig.telegram.notify_on_person;
        saveConfigUnmuted();
        sendTextSync(appConfig.telegram.notify_on_person ?
            "Telegram – osoby: ZAP" : "Telegram – osoby: VYP");
    }
    else if (cmd == "prah") {
        int val = args.toInt();
        // Same range the HTTP API clamps to (see handleApiSettings) — the two used
        // to disagree, so /prah 60 was rejected while the web UI accepted it.
        if (val >= 5 && val <= 80) {
            appConfig.motion.threshold = val;
            saveConfigUnmuted();
            char buf[64];
            snprintf(buf, sizeof(buf), "Práh pohybu nastaven na %d", val);
            sendTextSync(buf);
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "Použití: /prah N (5–80, nyní %d)", appConfig.motion.threshold);
            sendTextSync(buf);
        }
    }
    else if (cmd == "hlidej") {
        silentUntilMs = 0;  // explicit re-arm cancels a pending timed mute (else auto-resume would undo it)
        appConfig.telegram.notify_on_motion = true;
        appConfig.telegram.notify_on_face = true;
        appConfig.telegram.notify_on_person = true;
        appConfig.telegram.photo_on_motion = true;
        appConfig.telegram.photo_on_face = true;
        appConfig.telegram.photo_on_person = true;
        appConfig.telegram.active_start_hour = 0;
        appConfig.telegram.active_end_hour = 23;
        appConfig.motion.enabled = true;
        appConfig.face_detect.enabled = true;
        appConfig.person_detect.enabled = true;
        saveConfigUnmuted();
        sendTextSync("Režim HLÍDEJ aktivován (vše ZAP, 24/7)");
    }
    else if (cmd == "ticho") {
        // Timed silence: "/ticho 30" mutes for 30 minutes, then auto-resumes.
        // "/ticho" with no arg = permanent mute (persisted to config).
        int minutes = args.toInt();
        if (minutes > 0 && minutes <= 1440) {
            // Remember originals so auto-resume can restore them — but not when already
            // muted, or the (already false) flags would overwrite them = permanent mute.
            if (silentUntilMs == 0) {
                silentSavedNotifyMotion = appConfig.telegram.notify_on_motion;
                silentSavedNotifyFace   = appConfig.telegram.notify_on_face;
                silentSavedNotifyPerson = appConfig.telegram.notify_on_person;
            }
            appConfig.telegram.notify_on_motion = false;
            appConfig.telegram.notify_on_face   = false;
            appConfig.telegram.notify_on_person = false;
            silentUntilMs = millis() + (unsigned long)minutes * 60UL * 1000UL;
            // Not saved to config — the silence is intentionally runtime-only
            // so a crash/reboot cancels it rather than accidentally muting for hours.
            char buf[128];
            snprintf(buf, sizeof(buf),
                "\xF0\x9F\x94\x95 Ticho na %d min, pak se notifikace samy obnoví.",
                minutes);
            sendTextSync(buf);
        } else {
            silentUntilMs = 0;
            appConfig.telegram.notify_on_motion = false;
            appConfig.telegram.notify_on_face   = false;
            appConfig.telegram.notify_on_person = false;
            appConfig.telegram.photo_on_motion  = false;
            appConfig.telegram.photo_on_face    = false;
            appConfig.telegram.photo_on_person  = false;
            saveConfigUnmuted();
            sendTextSync("\xF0\x9F\x94\x95 Režim TICHO (trvalý). Zapnutí přes /hlidej.");
        }
    }
    else if (cmd == "arm") {
        // Alias for /hlidej — shorter, matches common alarm-system UX.
        silentUntilMs = 0;
        appConfig.telegram.notify_on_person = true;
        appConfig.telegram.notify_on_motion = true;
        appConfig.telegram.notify_on_face   = true;
        appConfig.motion.enabled = true;
        appConfig.person_detect.enabled = true;
        appConfig.face_detect.enabled = true;
        saveConfigUnmuted();
        sendTextSync("\xF0\x9F\x94\x94 ARMED — notifikace ZAP.");
    }
    else if (cmd == "disarm") {
        silentUntilMs = 0;
        appConfig.telegram.notify_on_person = false;
        appConfig.telegram.notify_on_motion = false;
        appConfig.telegram.notify_on_face   = false;
        saveConfigUnmuted();
        sendTextSync("\xF0\x9F\x94\x95 DISARMED — notifikace VYP. Detektory běží dál.");
    }
    else if (cmd == "hodiny") {
        // Parse "HH-HH" format
        int start = -1, end = -1;
        if (sscanf(args.c_str(), "%d-%d", &start, &end) == 2 &&
            start >= 0 && start <= 23 && end >= 0 && end <= 23) {
            appConfig.telegram.active_start_hour = start;
            appConfig.telegram.active_end_hour = end;
            saveConfigUnmuted();
            char buf[64];
            snprintf(buf, sizeof(buf), "Aktivní hodiny: %d:00 – %d:00", start, end);
            sendTextSync(buf);
        } else {
            sendTextSync("Použití: /hodiny HH-HH (např. /hodiny 22-7)");
        }
    }
    else if (cmd == "cooldown") {
        int val = args.toInt();
        if (val >= 5 && val <= 3600) {
            appConfig.telegram.cooldown_sec = val;
            saveConfigUnmuted();
            char buf[64];
            snprintf(buf, sizeof(buf), "Cooldown nastaven na %d s", val);
            sendTextSync(buf);
        } else {
            sendTextSync("Použití: /cooldown N (5–3600 sekund)");
        }
    }
    else if (cmd == "ip") {
        char buf[384];
        snprintf(buf, sizeof(buf),
            "IP: %s\n"
            "Hostname: %s\n"
            "Web: http://%s\n"
            "Stream: http://%s:%d/stream",
            getIPAddress().c_str(),
            htmlEscape(appConfig.wifi.hostname).c_str(),
            getIPAddress().c_str(),
            getIPAddress().c_str(),
            STREAM_PORT
        );
        sendTextSync(buf);
    }
    else if (cmd == "restart" || cmd == "reboot") {
        // The update was already acknowledged server-side in checkTelegramUpdates,
        // otherwise /restart would be redelivered after every boot (reboot loop).
        sendTextSync("Restartuji…");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
    }
    else if (cmd == "help" || cmd == "napoveda" || cmd == "start") {
        sendTextSync(
            "<b>CamS3 Telegram Bot</b>\n\n"
            "<b>Rychlé</b>\n"
            "/foto – poslat snímek\n"
            "/status – informace o systému\n"
            "/arm – zapnout hlídání\n"
            "/disarm – vypnout hlídání\n"
            "/ticho N – ticho na N minut (pak auto-obnova)\n\n"
            "<b>Detekce</b>\n"
            "/detekce – přepnout pohyb\n"
            "/obliceje – přepnout obličeje\n"
            "/osoba – přepnout osoby (FOMO)\n"
            "/prah N – práh pohybu (5–80)\n\n"
            "<b>Režim</b>\n"
            "/hlidej – vše ZAP (24/7, s fotkou)\n"
            "/hodiny HH-HH – aktivní hodiny\n"
            "/cooldown N – cooldown (5–3600 s)\n\n"
            "<b>Systém</b>\n"
            "/ip – IP a URL adresy\n"
            "/restart – restartovat zařízení\n"
            "/help – tato nápověda"
        );
    }
    else {
        sendTextSync("Neznámý příkaz. Použij /help");
    }
}

// --- Poll for incoming updates ---

// Confirm updates up to `id` to Telegram (offset=id+1) so they are never redelivered.
static void tgAckUpdates(int64_t id) {
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=0&limit=1",
        appConfig.telegram.bot_token.c_str(), (long long)(id + 1));
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, url)) return;
    http.setTimeout(10000);
    http.GET();
    http.end();
}

// discardOnly: boot-time pass — advance lastUpdateId past stale updates, run nothing.
// Returns number of updates seen.
static int checkTelegramUpdates(bool discardOnly = false) {
    if (!isTelegramConnected() || !isWiFiConnected()) return 0;

    // Build URL without String concat to avoid heap fragmentation
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=0&limit=5",
        appConfig.telegram.bot_token.c_str(), (long long)(lastUpdateId + 1));

    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    if (!http.begin(client, url)) return 0;
    http.setTimeout(10000);

    int code = http.GET();
    if (code != 200) {
        http.end();
        return 0;
    }

    // Read response stream directly into JSON parser to avoid large String alloc
    WiFiClient* stream = http.getStreamPtr();
    JsonDocument doc;
    if (deserializeJson(doc, *stream)) {
        http.end();
        return 0;
    }
    http.end();

    if (!doc["ok"].as<bool>()) return 0;

    JsonArray results = doc["result"].as<JsonArray>();
    int seen = 0;
    for (JsonObject update : results) {
        seen++;
        int64_t updateId = update["update_id"].as<int64_t>();
        if (updateId > lastUpdateId) lastUpdateId = updateId;
        if (discardOnly) continue;

        JsonObject msg = update["message"];
        if (!msg) continue;

        // Security: verify chat_id (compare as int64 — no String alloc)
        int64_t fromChatId = msg["chat"]["id"].as<int64_t>();
        int64_t ourChatId = atoll(appConfig.telegram.chat_id.c_str());
        if (fromChatId != ourChatId) {
            logCapture("[%s] Unauthorized chat_id: %lld\n", TAG, (long long)fromChatId);
            continue;
        }

        const char* text = msg["text"] | "";
        if (text[0] == '\0' || text[0] != '/') continue;

        // Parse command: skip '/', extract cmd and args using fixed buffers
        char cmdBuf[32] = {};
        char argsBuf[128] = {};
        const char* p = text + 1;  // skip '/'

        // Find end of command (space or @)
        int ci = 0;
        while (*p && *p != ' ' && *p != '@' && ci < (int)sizeof(cmdBuf) - 1) {
            cmdBuf[ci++] = tolower(*p);
            p++;
        }
        // Skip @botname if present
        if (*p == '@') { while (*p && *p != ' ') p++; }
        // Skip space, copy args
        if (*p == ' ') {
            p++;
            while (*p == ' ') p++;  // trim leading spaces
            strncpy(argsBuf, p, sizeof(argsBuf) - 1);
            // trim trailing spaces
            int len = strlen(argsBuf);
            while (len > 0 && argsBuf[len-1] == ' ') argsBuf[--len] = '\0';
        }

        // Ack before a restart: ESP.restart() never returns, so the offset would be lost.
        if (!strcmp(cmdBuf, "restart") || !strcmp(cmdBuf, "reboot")) tgAckUpdates(updateId);

        handleCommand(String(cmdBuf), String(argsBuf));
    }
    return seen;
}

// --- Public API ---

void telegramInit() {
    tgQueue = xQueueCreate(TELEGRAM_QUEUE_SIZE, sizeof(TelegramMessage));
    if (!tgQueue) {
        logCapture("[%s] Queue creation failed!\n", TAG);
    }
    logCapture("[%s] Initialized (queue=%d)\n", TAG, TELEGRAM_QUEUE_SIZE);
}

// Cooldown applies to notifications (photos + texts triggered by events).
// Command-response sends should bypass the cooldown — internal helpers go through
// the queue unconditionally, the public API enforces the limit.
static bool cooldownElapsed() {
    unsigned long cd = (unsigned long)appConfig.telegram.cooldown_sec * 1000UL;
    if (cd == 0) return true;
    return (millis() - lastTgSendTime) >= cd;
}

void telegramSendText(const char* msg) {
    if (!tgQueue || !msg) return;

    if (!cooldownElapsed()) {
        logCapture("[%s] Cooldown active, text dropped\n", TAG);
        return;
    }

    TelegramMessage tgMsg = {};
    copyUtf8Truncated(tgMsg.text, TELEGRAM_MSG_MAX_LEN, msg);
    tgMsg.has_photo = false;
    tgMsg.photo_data = NULL;
    tgMsg.photo_len = 0;

    if (xQueueSend(tgQueue, &tgMsg, 0) != pdTRUE) {
        logCapture("[%s] Queue full, text dropped\n", TAG);
    }
}

void telegramSendPhoto(const uint8_t* jpeg, size_t len, const char* caption) {
    if (!tgQueue || !jpeg || len == 0) return;

    if (!cooldownElapsed()) {
        logCapture("[%s] Cooldown active, photo dropped\n", TAG);
        return;
    }

    // Copy JPEG to PSRAM (freed by task after send)
    uint8_t* copy = (uint8_t*)ps_malloc(len);
    if (!copy) {
        logCapture("[%s] PSRAM alloc failed for photo (%u bytes)\n", TAG, len);
        return;
    }
    memcpy(copy, jpeg, len);

    TelegramMessage tgMsg = {};
    if (caption) copyUtf8Truncated(tgMsg.text, TELEGRAM_MSG_MAX_LEN, caption);
    tgMsg.has_photo = true;
    tgMsg.photo_data = copy;
    tgMsg.photo_len = len;

    if (xQueueSend(tgQueue, &tgMsg, 0) != pdTRUE) {
        logCapture("[%s] Queue full, spilling photo to SD\n", TAG);
#ifdef INCLUDE_SD_CARD
        persistPhotoToSd(copy, len, tgMsg.text);
#endif
        free(copy);
    }
}

// Rich boot notification sent once Telegram API is reachable. Includes
// system state the user actually cares about when the device reconnects.
static void sendBootMessage() {
    char buf[768];
    snprintf(buf, sizeof(buf),
        "\xF0\x9F\x9A\x80 <b>CamS3 online</b>\n"
        "━━━━━━━━━━━━━\n"
        "IP: <code>%s</code>\n"
        "Host: %s\n"
        "FW: %s\n"
        "Heap: %d kB free / %d kB min\n"
        "PSRAM: %d MB free\n"
        "WiFi: %d dBm\n"
        "━━━━━━━━━━━━━\n"
        "Pohyb: %s\n"
        "Obličeje: %s\n"
        "Osoby (FOMO): %s\n"
        "MQTT: %s\n"
        "━━━━━━━━━━━━━\n"
        "/help – příkazy  |  /foto – snímek",
        getIPAddress().c_str(),
        htmlEscape(appConfig.wifi.hostname).c_str(),
        htmlEscape(String(FIRMWARE_VERSION)).c_str(),
        (int)(ESP.getFreeHeap() / 1024),
        (int)(ESP.getMinFreeHeap() / 1024),
        (int)(ESP.getFreePsram() / (1024 * 1024)),
        getRSSI(),
        appConfig.motion.enabled ? "ZAP" : "VYP",
        appConfig.face_detect.enabled ? "ZAP" : "VYP",
        appConfig.person_detect.enabled ? "ZAP" : "VYP",
#ifdef INCLUDE_MQTT
        isMqttConnected() ? "connected" : (appConfig.mqtt.enabled ? "connecting" : "VYP")
#else
        "N/A"
#endif
    );
    sendTextSync(buf);
}

// Register the slash-command menu so Telegram shows the "Menu" button
// with clickable commands. Called once per boot; Telegram stores them.
static void registerBotCommands() {
    if (!isTelegramConnected() || !isWiFiConnected()) return;

    String url = "https://api.telegram.org/bot" + appConfig.telegram.bot_token + "/setMyCommands";

    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, url)) return;
    http.setTimeout(8000);
    http.addHeader("Content-Type", "application/json");

    // Keep short — Telegram shows max ~8 comfortably in the slide-up menu.
    static const char* body =
        "{\"commands\":["
        "{\"command\":\"foto\",\"description\":\"Snímek z kamery\"},"
        "{\"command\":\"status\",\"description\":\"Stav a metriky\"},"
        "{\"command\":\"arm\",\"description\":\"Zapnout hlídání\"},"
        "{\"command\":\"disarm\",\"description\":\"Vypnout hlídání\"},"
        "{\"command\":\"ticho\",\"description\":\"Ticho N minut (pak auto-obnova)\"},"
        "{\"command\":\"osoba\",\"description\":\"Přepnout notifikace osob\"},"
        "{\"command\":\"ip\",\"description\":\"IP a URL adresy\"},"
        "{\"command\":\"help\",\"description\":\"Seznam všech příkazů\"}"
        "]}";

    int code = http.POST(body);
    http.end();
    logCapture("[%s] setMyCommands: HTTP %d\n", TAG, code);
}

// Auto-resume from timed silence. Returns true if we just resumed
// (caller sends a heads-up message), false otherwise.
static bool maybeResumeFromSilence() {
    if (silentUntilMs == 0) return false;
    if ((long)(millis() - silentUntilMs) < 0) return false;
    appConfig.telegram.notify_on_motion = silentSavedNotifyMotion;
    appConfig.telegram.notify_on_face   = silentSavedNotifyFace;
    appConfig.telegram.notify_on_person = silentSavedNotifyPerson;
    silentUntilMs = 0;
    return true;
}

void telegramTask(void* param) {
    logCapture("[%s] Telegram task started\n", TAG);
    esp_task_wdt_add(NULL);

    // Wait for WiFi before doing anything
    while (!isWiFiConnected()) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Give DNS/TLS a moment to warm up on the first connect.
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool bootAnnounced = false;
    bool commandsRegistered = false;

    // Clear any pending updates from before boot (so a /foto queued
    // while the device was offline doesn't fire as a stale command).
    if (isTelegramConnected() && appConfig.telegram.enabled) {
        // Discard only (never execute) — drain in batches of 5, bounded.
        for (int i = 0; i < 20 && checkTelegramUpdates(true) > 0; i++) esp_task_wdt_reset();
        logCapture("[%s] Cleared pending updates\n", TAG);
    }

    while (true) {
        esp_task_wdt_reset();
        if (!appConfig.telegram.enabled || !isTelegramConnected()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // First successful cycle after becoming enabled: register menu + boot msg.
        if (!commandsRegistered) {
            registerBotCommands();
            commandsRegistered = true;
        }
        if (!bootAnnounced) {
            sendBootMessage();
            bootAnnounced = true;
        }

        // Silence timer — auto-resume notifications if the timed mute expired.
        if (maybeResumeFromSilence()) {
            sendTextSync("\xF0\x9F\x94\x94 Ticho skončilo, notifikace obnoveny.");
        }

        TelegramMessage msg;
        if (xQueueReceive(tgQueue, &msg, pdMS_TO_TICKS(appConfig.telegram.poll_interval_ms)) == pdTRUE) {
            bool sent = true;
            if (msg.has_photo) {
                sent = sendPhotoSync(msg.photo_data, msg.photo_len, msg.text);
                // On network failure, spill to SD so the photo isn't lost.
                // (Corrupt JPEG → 400 from Telegram → sent=false but spilling
                //  would just loop; accept that as the trade-off.)
#ifdef INCLUDE_SD_CARD
                if (!sent && isPermanentHttpError(lastPhotoHttpStatus)) {
                    logCapture("[%s] Photo rejected (HTTP %d), not spilling\n", TAG, lastPhotoHttpStatus);
                } else if (!sent) {
                    persistPhotoToSd(msg.photo_data, msg.photo_len, msg.text);
                }
#endif
                free(msg.photo_data);
            } else if (msg.text[0]) {
                sendTextSync(msg.text);
            }
            lastTgSendTime = millis();
        } else {
            // Queue timeout — poll for incoming commands, then drain one pending photo.
            checkTelegramUpdates();
#ifdef INCLUDE_SD_CARD
            flushOnePendingFromSd();
#endif
        }
    }
}

#endif // INCLUDE_TELEGRAM
