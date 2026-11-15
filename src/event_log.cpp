#include "event_log.h"
#include "ws_log.h"

#ifdef INCLUDE_EVENT_LOG

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define EVENTS_FILE "/events.jsonl"

static EventEntry ring[EVENT_LOG_SIZE];
static int ring_head  = 0;
static int ring_count = 0;
static portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;

// Lines currently in the file, tracked in RAM. Counting them by re-reading the
// whole file after every event (which is what this module used to do) meant each
// motion event triggered a full read plus, past the cap, a full rewrite — from the
// motion task, on LittleFS.
static int file_lines = 0;

// Serialises every access to EVENTS_FILE and file_lines: logEvent() is called from
// motion, web and wifi tasks concurrently; interleaved append/rotate corrupted lines.
static SemaphoreHandle_t file_mutex = NULL;

// Escape a detail string for embedding in a JSON string literal (zone names can
// contain quotes/backslashes/control chars which broke both the API and events.jsonl).
static void appendEscaped(String& out, const char* s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { out += ' '; }
        else out += (char)c;
    }
}

// Rotate in batches instead of on every line over the cap: one rewrite per
// ROTATE_SLACK events rather than one per event.
#define EVENT_LOG_ROTATE_SLACK 32

const char* eventTypeName(EventType type) {
    switch (type) {
        case EVT_BOOT:              return "boot";
        case EVT_MOTION:            return "motion";
        case EVT_PERSON:            return "person";
        case EVT_FACE:              return "face";
        case EVT_ARMED:             return "armed";
        case EVT_DISARMED:          return "disarmed";
        case EVT_RECORDING_STARTED: return "recording_started";
        case EVT_RECORDING_STOPPED: return "recording_stopped";
        case EVT_TELEGRAM_FAILED:   return "telegram_failed";
        case EVT_WIFI_RECONNECT:    return "wifi_reconnect";
        case EVT_LOW_MEMORY:        return "low_memory";
        case EVT_SD_FAILURE:        return "sd_failure";
        default:                    return "unknown";
    }
}

static void writeLine(File& f, const EventEntry& e) {
    String d;
    appendEscaped(d, e.detail);
    f.printf("{\"u\":%lu,\"t\":\"%s\",\"d\":\"%s\"}\n",
             (unsigned long)e.uptime_s, eventTypeName(e.type), d.c_str());
}

// Rewrite the file from the in-RAM ring, which by definition already holds exactly
// the newest EVENT_LOG_SIZE entries. No read-back, no tail parsing.
//
// The snapshot (EVENT_LOG_SIZE * sizeof(EventEntry) = 5.6 kB) must NOT live on the
// stack: rotation runs from whichever task logged the event (motion: 6 kB stack) and
// at boot from setup() inside loopTask (8 kB, already deep). On the stack it tripped
// the canary, the panic hit before the file was rewritten, so every following boot
// found the file still over the cap and crashed again — a permanent boot loop
// (2026-11-11, ~51k panics before it was caught).
static void rotateFile() {
    EventEntry* snapshot = (EventEntry*)heap_caps_malloc(
        sizeof(EventEntry) * EVENT_LOG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) snapshot = (EventEntry*)malloc(sizeof(EventEntry) * EVENT_LOG_SIZE);
    if (!snapshot) return;  // try again on the next event; better than crashing
    int count, head;
    portENTER_CRITICAL(&ring_mux);
    count = ring_count;
    head  = ring_head;
    memcpy(snapshot, ring, sizeof(EventEntry) * EVENT_LOG_SIZE);
    portEXIT_CRITICAL(&ring_mux);

    // Write a temp file and rename over the original: a power cut mid-rotation used
    // to leave a truncated/empty events.jsonl.
    const char* tmpName = "/events.jsonl.tmp";
    File w = LittleFS.open(tmpName, "w");
    if (!w) { free(snapshot); return; }
    int start = (count < EVENT_LOG_SIZE) ? 0 : head;
    for (int i = 0; i < count; i++) {
        writeLine(w, snapshot[(start + i) % EVENT_LOG_SIZE]);
    }
    w.close();
    free(snapshot);
    if (!LittleFS.rename(tmpName, EVENTS_FILE)) { LittleFS.remove(tmpName); return; }
    file_lines = count;
}

static void appendToFile(const EventEntry& e) {
    File f = LittleFS.open(EVENTS_FILE, "a");
    if (!f) return;
    writeLine(f, e);
    f.close();
    file_lines++;

    if (file_lines > EVENT_LOG_SIZE + EVENT_LOG_ROTATE_SLACK) {
        rotateFile();
    }
}

static void loadFromFile() {
    if (!LittleFS.exists(EVENTS_FILE)) return;
    File f = LittleFS.open(EVENTS_FILE, "r");
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        JsonDocument doc;
        if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
        EventEntry e;
        e.uptime_s = doc["u"] | 0UL;
        e.type = EVT_UNKNOWN;
        const char* tname = doc["t"] | "";
        for (int i = EVT_BOOT; i <= EVT_UNKNOWN; i++) {
            if (strcmp(eventTypeName((EventType)i), tname) == 0) {
                e.type = (EventType)i; break;
            }
        }
        strlcpy(e.detail, doc["d"] | "", EVENT_DETAIL_LEN);
        ring[ring_head] = e;
        ring_head = (ring_head + 1) % EVENT_LOG_SIZE;
        if (ring_count < EVENT_LOG_SIZE) ring_count++;
        file_lines++;
    }
    f.close();

    // The file may have grown past the cap in an older firmware build; fold it back
    // to the ring contents once, at boot, instead of on the next event.
    if (file_lines > EVENT_LOG_SIZE) rotateFile();
}

void initEventLog() {
    if (!file_mutex) file_mutex = xSemaphoreCreateRecursiveMutex();
    memset(ring, 0, sizeof(ring));
    loadFromFile();
    logEvent(EVT_BOOT);
}

void logEvent(EventType type, const char* detail) {
    EventEntry e;
    e.uptime_s = millis() / 1000;
    e.type = type;
    strlcpy(e.detail, detail ? detail : "", EVENT_DETAIL_LEN);

    portENTER_CRITICAL(&ring_mux);
    ring[ring_head] = e;
    ring_head = (ring_head + 1) % EVENT_LOG_SIZE;
    if (ring_count < EVENT_LOG_SIZE) ring_count++;
    portEXIT_CRITICAL(&ring_mux);

    if (file_mutex && xSemaphoreTakeRecursive(file_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        appendToFile(e);
        xSemaphoreGiveRecursive(file_mutex);
    }
    logCapture("[EVT] %s %s\n", eventTypeName(type), detail ? detail : "");
}

String getEventsJSON() {
    // Copy the ring inside the critical section; reading it outside raced with
    // logEvent() overwriting entries (torn detail strings). Heap, not stack (5.6 kB).
    EventEntry* snap = (EventEntry*)malloc(sizeof(EventEntry) * EVENT_LOG_SIZE);
    if (!snap) return "[]";
    int count, head;
    portENTER_CRITICAL(&ring_mux);
    count = ring_count;
    head  = ring_head;
    memcpy(snap, ring, sizeof(EventEntry) * EVENT_LOG_SIZE);
    portEXIT_CRITICAL(&ring_mux);

    String out = "[";
    out.reserve(count * 60 + 4);
    for (int i = 0; i < count; i++) {
        int idx = (head - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
        const EventEntry& e = snap[idx];
        if (i > 0) out += ',';
        out += "{\"uptime_s\":"; out += e.uptime_s;
        out += ",\"type\":\"";   out += eventTypeName(e.type);
        out += "\",\"detail\":\""; appendEscaped(out, e.detail);
        out += "\"}";
    }
    out += "]";
    free(snap);
    return out;
}

#endif // INCLUDE_EVENT_LOG
