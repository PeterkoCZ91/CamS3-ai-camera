#include "event_log.h"
#include "ws_log.h"

#ifdef INCLUDE_EVENT_LOG

#include <ArduinoJson.h>
#include <LittleFS.h>

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
    f.printf("{\"u\":%lu,\"t\":\"%s\",\"d\":\"%s\"}\n",
             (unsigned long)e.uptime_s, eventTypeName(e.type), e.detail);
}

// Rewrite the file from the in-RAM ring, which by definition already holds exactly
// the newest EVENT_LOG_SIZE entries. No read-back, no tail parsing.
static void rotateFile() {
    EventEntry snapshot[EVENT_LOG_SIZE];
    int count, head;
    portENTER_CRITICAL(&ring_mux);
    count = ring_count;
    head  = ring_head;
    memcpy(snapshot, ring, sizeof(snapshot));
    portEXIT_CRITICAL(&ring_mux);

    File w = LittleFS.open(EVENTS_FILE, "w");
    if (!w) return;
    int start = (count < EVENT_LOG_SIZE) ? 0 : head;
    for (int i = 0; i < count; i++) {
        writeLine(w, snapshot[(start + i) % EVENT_LOG_SIZE]);
    }
    w.close();
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

    appendToFile(e);
    logCapture("[EVT] %s %s\n", eventTypeName(type), detail ? detail : "");
}

String getEventsJSON() {
    String out = "[";
    portENTER_CRITICAL(&ring_mux);
    int count = ring_count;
    int head  = ring_head;
    portEXIT_CRITICAL(&ring_mux);

    for (int i = 0; i < count; i++) {
        int idx = (head - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
        const EventEntry& e = ring[idx];
        if (i > 0) out += ',';
        out += "{\"uptime_s\":"; out += e.uptime_s;
        out += ",\"type\":\"";   out += eventTypeName(e.type);
        out += "\",\"detail\":\""; out += e.detail;
        out += "\"}";
    }
    out += "]";
    return out;
}

#endif // INCLUDE_EVENT_LOG
