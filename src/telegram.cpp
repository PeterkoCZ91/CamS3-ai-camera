#include "telegram.h"

#ifdef INCLUDE_TELEGRAM

#include "config.h"
#include "camera_manager.h"
#include "wifi_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

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
        Serial.printf("[%s] HTTP begin failed\n", TAG);
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
        Serial.printf("[%s] Text sent OK\n", TAG);
        return true;
    }
    Serial.printf("[%s] sendMessage failed: %d\n", TAG, code);
    return false;
}

static bool sendPhotoSync(const uint8_t* jpeg, size_t jpegLen, const char* caption) {
    if (!isTelegramConnected() || !isWiFiConnected()) return false;

    WiFiClientSecure client;
    client.setInsecure();

    if (!client.connect("api.telegram.org", 443)) {
        Serial.printf("[%s] TLS connect failed\n", TAG);
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
            Serial.printf("[%s] Photo write stalled at %u/%u\n", TAG, sent, jpegLen);
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
        success = statusLine.indexOf("200") >= 0;
        if (!success) {
            Serial.printf("[%s] sendPhoto failed: %s\n", TAG, statusLine.c_str());
        } else {
            Serial.printf("[%s] Photo sent OK (%u bytes)\n", TAG, jpegLen);
        }
    } else {
        Serial.printf("[%s] sendPhoto timeout\n", TAG);
    }

    client.stop();
    return success;
}

// --- Command handling ---

static void handleCommand(const String& cmd, const String& args) {
    Serial.printf("[%s] Command: /%s %s\n", TAG, cmd.c_str(), args.c_str());

    if (cmd == "foto" || cmd == "photo") {
        const uint8_t* buf = NULL;
        size_t len = 0;
        if (ringBufferGetLatest(&buf, &len)) {
            // Copy to PSRAM for sync send
            uint8_t* copy = (uint8_t*)ps_malloc(len);
            if (copy) {
                memcpy(copy, buf, len);
                ringBufferRelease();
                sendPhotoSync(copy, len, "Snapshot");
                free(copy);
            } else {
                ringBufferRelease();
                sendTextSync("Chyba: nedostatek PSRAM");
            }
        } else {
            sendTextSync("Zadny snimek k dispozici");
        }
    }
    else if (cmd == "status" || cmd == "stav") {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<b>CamS3 Status</b>\n"
            "IP: %s\n"
            "Uptime: %lus\n"
            "Heap: %dKB\n"
            "PSRAM: %dMB\n"
            "WiFi RSSI: %d dBm\n"
            "FPS: %.1f\n"
            "Stream klientu: %d\n"
            "Pohyb: %s\n"
            "Obliceje: %s\n"
            "Osoby (FOMO): %s\n"
            "TG notify pohyb: %s\n"
            "TG notify obliceje: %s\n"
            "TG notify osoby: %s\n"
            "Prah pohybu: %d\n"
            "Cooldown: %ds\n"
            "Aktivni hodiny: %d-%d\n"
            "Poll interval: %dms",
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
        saveConfig();
        sendTextSync(appConfig.telegram.notify_on_motion ?
            "TG notifikace pohybu: ZAP" : "TG notifikace pohybu: VYP");
    }
    else if (cmd == "obliceje") {
        appConfig.telegram.notify_on_face = !appConfig.telegram.notify_on_face;
        saveConfig();
        sendTextSync(appConfig.telegram.notify_on_face ?
            "TG notifikace obliceju: ZAP" : "TG notifikace obliceju: VYP");
    }
    else if (cmd == "osoba") {
        appConfig.telegram.notify_on_person = !appConfig.telegram.notify_on_person;
        saveConfig();
        sendTextSync(appConfig.telegram.notify_on_person ?
            "TG notifikace osob: ZAP" : "TG notifikace osob: VYP");
    }
    else if (cmd == "prah") {
        int val = args.toInt();
        if (val >= 5 && val <= 50) {
            appConfig.motion.threshold = val;
            saveConfig();
            char buf[64];
            snprintf(buf, sizeof(buf), "Prah pohybu nastaven na %d", val);
            sendTextSync(buf);
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "Pouziti: /prah N (5-50, nyni: %d)", appConfig.motion.threshold);
            sendTextSync(buf);
        }
    }
    else if (cmd == "hlidej") {
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
        saveConfig();
        sendTextSync("Rezim HLIDEJ aktivovan (vse ZAP, 24/7)");
    }
    else if (cmd == "ticho") {
        appConfig.telegram.notify_on_motion = false;
        appConfig.telegram.notify_on_face = false;
        appConfig.telegram.notify_on_person = false;
        appConfig.telegram.photo_on_motion = false;
        appConfig.telegram.photo_on_face = false;
        appConfig.telegram.photo_on_person = false;
        saveConfig();
        sendTextSync("Rezim TICHO aktivovan (notifikace VYP)");
    }
    else if (cmd == "hodiny") {
        // Parse "HH-HH" format
        int start = -1, end = -1;
        if (sscanf(args.c_str(), "%d-%d", &start, &end) == 2 &&
            start >= 0 && start <= 23 && end >= 0 && end <= 23) {
            appConfig.telegram.active_start_hour = start;
            appConfig.telegram.active_end_hour = end;
            saveConfig();
            char buf[64];
            snprintf(buf, sizeof(buf), "Aktivni hodiny: %d:00 - %d:00", start, end);
            sendTextSync(buf);
        } else {
            sendTextSync("Pouziti: /hodiny HH-HH (napr. /hodiny 22-7)");
        }
    }
    else if (cmd == "cooldown") {
        int val = args.toInt();
        if (val >= 5 && val <= 3600) {
            appConfig.telegram.cooldown_sec = val;
            saveConfig();
            char buf[64];
            snprintf(buf, sizeof(buf), "Cooldown nastaven na %ds", val);
            sendTextSync(buf);
        } else {
            sendTextSync("Pouziti: /cooldown N (5-3600 sekund)");
        }
    }
    else if (cmd == "ip") {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "IP: %s\n"
            "Hostname: %s\n"
            "Web: http://%s\n"
            "Stream: http://%s:%d/stream",
            getIPAddress().c_str(),
            appConfig.wifi.hostname.c_str(),
            getIPAddress().c_str(),
            getIPAddress().c_str(),
            STREAM_PORT
        );
        sendTextSync(buf);
    }
    else if (cmd == "restart" || cmd == "reboot") {
        sendTextSync("Restartuji...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
    }
    else if (cmd == "help" || cmd == "napoveda" || cmd == "start") {
        sendTextSync(
            "<b>CamS3 Telegram Bot</b>\n\n"
            "/foto - Poslat snapshot\n"
            "/status - Info o systemu\n"
            "/detekce - Prepnout notifikace pohybu\n"
            "/obliceje - Prepnout notifikace obliceju\n"
            "/osoba - Prepnout notifikace osob (FOMO)\n"
            "/prah N - Prah pohybu (5-50)\n"
            "/hlidej - Vse ZAP (24/7)\n"
            "/ticho - Vse VYP\n"
            "/hodiny HH-HH - Aktivni hodiny\n"
            "/cooldown N - Cooldown (5-3600s)\n"
            "/ip - IP a URL adresy\n"
            "/restart - Restartovat zarizeni\n"
            "/help - Tato napoveda"
        );
    }
    else {
        sendTextSync("Neznamy prikaz. Pouzij /help");
    }
}

// --- Poll for incoming updates ---

static void checkTelegramUpdates() {
    if (!isTelegramConnected() || !isWiFiConnected()) return;

    // Build URL without String concat to avoid heap fragmentation
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=0&limit=5",
        appConfig.telegram.bot_token.c_str(), (long long)(lastUpdateId + 1));

    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    if (!http.begin(client, url)) return;
    http.setTimeout(10000);

    int code = http.GET();
    if (code != 200) {
        http.end();
        return;
    }

    // Read response stream directly into JSON parser to avoid large String alloc
    WiFiClient* stream = http.getStreamPtr();
    JsonDocument doc;
    if (deserializeJson(doc, *stream)) {
        http.end();
        return;
    }
    http.end();

    if (!doc["ok"].as<bool>()) return;

    JsonArray results = doc["result"].as<JsonArray>();
    for (JsonObject update : results) {
        int64_t updateId = update["update_id"].as<int64_t>();
        if (updateId > lastUpdateId) lastUpdateId = updateId;

        JsonObject msg = update["message"];
        if (!msg) continue;

        // Security: verify chat_id (compare as int64 — no String alloc)
        int64_t fromChatId = msg["chat"]["id"].as<int64_t>();
        int64_t ourChatId = atoll(appConfig.telegram.chat_id.c_str());
        if (fromChatId != ourChatId) {
            Serial.printf("[%s] Unauthorized chat_id: %lld\n", TAG, (long long)fromChatId);
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

        handleCommand(String(cmdBuf), String(argsBuf));
    }
}

// --- Public API ---

void telegramInit() {
    tgQueue = xQueueCreate(TELEGRAM_QUEUE_SIZE, sizeof(TelegramMessage));
    if (!tgQueue) {
        Serial.printf("[%s] Queue creation failed!\n", TAG);
    }
    Serial.printf("[%s] Initialized (queue=%d)\n", TAG, TELEGRAM_QUEUE_SIZE);
}

void telegramSendText(const char* msg) {
    if (!tgQueue || !msg) return;

    TelegramMessage tgMsg = {};
    strncpy(tgMsg.text, msg, TELEGRAM_MSG_MAX_LEN - 1);
    tgMsg.has_photo = false;
    tgMsg.photo_data = NULL;
    tgMsg.photo_len = 0;

    if (xQueueSend(tgQueue, &tgMsg, 0) != pdTRUE) {
        Serial.printf("[%s] Queue full, text dropped\n", TAG);
    }
}

void telegramSendPhoto(const uint8_t* jpeg, size_t len, const char* caption) {
    if (!tgQueue || !jpeg || len == 0) return;

    // Copy JPEG to PSRAM (freed by task after send)
    uint8_t* copy = (uint8_t*)ps_malloc(len);
    if (!copy) {
        Serial.printf("[%s] PSRAM alloc failed for photo (%u bytes)\n", TAG, len);
        return;
    }
    memcpy(copy, jpeg, len);

    TelegramMessage tgMsg = {};
    if (caption) strncpy(tgMsg.text, caption, TELEGRAM_MSG_MAX_LEN - 1);
    tgMsg.has_photo = true;
    tgMsg.photo_data = copy;
    tgMsg.photo_len = len;

    if (xQueueSend(tgQueue, &tgMsg, 0) != pdTRUE) {
        Serial.printf("[%s] Queue full, photo dropped\n", TAG);
        free(copy);
    }
}

void telegramTask(void* param) {
    Serial.printf("[%s] Telegram task started\n", TAG);

    // Wait for WiFi before doing anything
    while (!isWiFiConnected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Clear any pending updates from before boot
    if (isTelegramConnected()) {
        checkTelegramUpdates();
        Serial.printf("[%s] Cleared pending updates\n", TAG);
    }

    while (true) {
        if (!appConfig.telegram.enabled || !isTelegramConnected()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        TelegramMessage msg;
        if (xQueueReceive(tgQueue, &msg, pdMS_TO_TICKS(appConfig.telegram.poll_interval_ms)) == pdTRUE) {
            // Cooldown check for notifications (not for command responses)
            if (msg.has_photo) {
                sendPhotoSync(msg.photo_data, msg.photo_len, msg.text);
                free(msg.photo_data);
            } else if (msg.text[0]) {
                sendTextSync(msg.text);
            }
            lastTgSendTime = millis();
        } else {
            // Queue timeout - poll for incoming commands
            checkTelegramUpdates();
        }
    }
}

#endif // INCLUDE_TELEGRAM
