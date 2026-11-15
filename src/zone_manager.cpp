#include "zone_manager.h"
#include "ws_log.h"
#include "motion_detect.h"

#ifdef INCLUDE_ZONES

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define ZONES_FILE    "/zones.json"
#define ROI_MASK_FILE "/roi_mask.txt"

static Zone zones[ZONE_MAX_COUNT];
static int  zone_count = 0;

// Zones are mutated from the async web task and read from the motion task; without
// a lock a delete shifting the array under getActiveZones() reads torn entries.
static SemaphoreHandle_t zone_mutex = NULL;
struct ZoneLock {
    bool ok;
    ZoneLock() { ok = zone_mutex && xSemaphoreTake(zone_mutex, pdMS_TO_TICKS(200)) == pdTRUE; }
    ~ZoneLock() { if (ok) xSemaphoreGive(zone_mutex); }
};

// Clamp rects to the motion grid so a hostile or corrupt w/h cannot make the
// hit-test loop run billions of iterations (WDT boot loop).
static void clampRect(ZoneRect& r) {
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.x > MOTION_GRID_W - 1) r.x = MOTION_GRID_W - 1;
    if (r.y > MOTION_GRID_H - 1) r.y = MOTION_GRID_H - 1;
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    if (r.w > MOTION_GRID_W - r.x) r.w = MOTION_GRID_W - r.x;
    if (r.h > MOTION_GRID_H - r.y) r.h = MOTION_GRID_H - r.y;
}

void clampZoneRect(ZoneRect& r) { clampRect(r); }

// Write to a temp file and rename so a power cut mid-write cannot leave a truncated
// file (loadZones would then drop every zone).
static bool atomicWrite(const char* path, const uint8_t* data, size_t len) {
    String tmp = String(path) + ".tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f) return false;
    size_t w = len ? f.write(data, len) : 0;
    f.close();
    if (w != len) { LittleFS.remove(tmp); return false; }
    if (!LittleFS.rename(tmp, path)) { LittleFS.remove(tmp); return false; }
    return true;
}

static bool loadZones() {
    if (!LittleFS.exists(ZONES_FILE)) return false;
    File f = LittleFS.open(ZONES_FILE, "r");
    if (!f) return false;

    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return false; }
    f.close();

    JsonArray arr = doc.as<JsonArray>();
    zone_count = 0;
    for (JsonObject obj : arr) {
        if (zone_count >= ZONE_MAX_COUNT) break;
        Zone& z = zones[zone_count];
        strlcpy(z.name,  obj["name"]  | "", ZONE_NAME_LEN);
        strlcpy(z.color, obj["color"] | "#ffffff", 8);
        z.alert = obj["alert"] | false;
        z.rect_count = 0;
        JsonArray rects = obj["rects"];
        if (rects) {
            for (JsonObject r : rects) {
                if (z.rect_count >= ZONE_MAX_RECTS) break;
                ZoneRect& zr = z.rects[z.rect_count++];
                zr.x = r["x"] | 0; zr.y = r["y"] | 0;
                zr.w = r["w"] | 1; zr.h = r["h"] | 1;
                clampRect(zr);
            }
        }
        zone_count++;
    }
    return true;
}

// Caller holds zone_mutex (or runs before tasks start).
static bool saveZonesLocked() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < zone_count; i++) {
        Zone& z = zones[i];
        JsonObject obj = arr.add<JsonObject>();
        obj["name"]  = z.name;
        obj["color"] = z.color;
        obj["alert"] = z.alert;
        JsonArray rects = obj["rects"].to<JsonArray>();
        for (int r = 0; r < z.rect_count; r++) {
            JsonObject ro = rects.add<JsonObject>();
            ro["x"] = z.rects[r].x; ro["y"] = z.rects[r].y;
            ro["w"] = z.rects[r].w; ro["h"] = z.rects[r].h;
        }
    }
    String out;
    serializeJson(doc, out);
    return atomicWrite(ZONES_FILE, (const uint8_t*)out.c_str(), out.length());
}

bool saveZones() {
    ZoneLock lk;
    if (!lk.ok) return false;
    return saveZonesLocked();
}

void initZoneManager() {
    if (!zone_mutex) zone_mutex = xSemaphoreCreateMutex();
    loadZones();
    logCapture("[Zones] %d zone(s) loaded\n", zone_count);
}

int   getZoneCount()       { return zone_count; }
Zone* getZone(int index)   { return (index >= 0 && index < zone_count) ? &zones[index] : nullptr; }

bool addOrUpdateZone(const Zone& zin) {
    ZoneLock lk;
    if (!lk.ok) return false;
    Zone z = zin;
    z.name[ZONE_NAME_LEN - 1] = '\0';
    z.color[sizeof(z.color) - 1] = '\0';
    if (z.rect_count < 0) z.rect_count = 0;
    if (z.rect_count > ZONE_MAX_RECTS) z.rect_count = ZONE_MAX_RECTS;
    for (int r = 0; r < z.rect_count; r++) clampRect(z.rects[r]);
    for (int i = 0; i < zone_count; i++) {
        if (strncmp(zones[i].name, z.name, ZONE_NAME_LEN) == 0) {
            zones[i] = z; return saveZonesLocked();
        }
    }
    if (zone_count >= ZONE_MAX_COUNT) return false;
    zones[zone_count++] = z;
    return saveZonesLocked();
}

bool deleteZone(const char* name) {
    ZoneLock lk;
    if (!lk.ok) return false;
    for (int i = 0; i < zone_count; i++) {
        if (strncmp(zones[i].name, name, ZONE_NAME_LEN) == 0) {
            for (int j = i; j < zone_count - 1; j++) zones[j] = zones[j + 1];
            zone_count--;
            return saveZonesLocked();
        }
    }
    return false;
}

void clearZones() {
    ZoneLock lk;
    if (!lk.ok) return;
    zone_count = 0;
    saveZonesLocked();
}

void getActiveZones(const uint8_t* motion_grid, int gw, int gh,
                    char* out_buf, int buf_len) {
    if (buf_len <= 0) return;
    out_buf[0] = '\0';
    // Motion task: wait is bounded (200 ms); skipping one frame's zone report is harmless.
    ZoneLock lk;
    if (!lk.ok) return;
    int pos = 0;
    for (int i = 0; i < zone_count; i++) {
        Zone& z = zones[i];
        if (!z.alert) continue;
        bool hit = false;
        for (int r = 0; r < z.rect_count && !hit; r++) {
            ZoneRect& zr = z.rects[r];
            for (int y = zr.y; y < zr.y + zr.h && !hit; y++) {
                for (int x = zr.x; x < zr.x + zr.w && !hit; x++) {
                    if (x >= 0 && x < gw && y >= 0 && y < gh && motion_grid[y * gw + x])
                        hit = true;
                }
            }
        }
        if (hit) {
            // Whole names only: a full buffer used to truncate mid-name or lose the
            // separator and glue two names together.
            int len = strnlen(z.name, ZONE_NAME_LEN);
            int need = len + (pos > 0 ? 1 : 0);
            if (pos + need > buf_len - 1) break;
            if (pos > 0) out_buf[pos++] = ',';
            memcpy(out_buf + pos, z.name, len);
            pos += len;
            out_buf[pos] = '\0';
        }
    }
}

String getZonesJSON() {
    ZoneLock lk;
    if (!lk.ok) return "[]";
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < zone_count; i++) {
        Zone& z = zones[i];
        JsonObject obj = arr.add<JsonObject>();
        obj["name"]  = z.name;
        obj["color"] = z.color;
        obj["alert"] = z.alert;
        JsonArray rects = obj["rects"].to<JsonArray>();
        for (int r = 0; r < z.rect_count; r++) {
            JsonObject ro = rects.add<JsonObject>();
            ro["x"] = z.rects[r].x; ro["y"] = z.rects[r].y;
            ro["w"] = z.rects[r].w; ro["h"] = z.rects[r].h;
        }
    }
    String out;
    serializeJson(doc, out);
    return out;
}

void saveROIMask(const char* mask_str, int len) {
    // len == 0 is valid: an empty file is how "clear mask" is persisted.
    if (!mask_str || len < 0) return;
    atomicWrite(ROI_MASK_FILE, (const uint8_t*)mask_str, (size_t)len);
}

bool loadROIMask(char* out_buf, int buf_len) {
    if (!LittleFS.exists(ROI_MASK_FILE)) return false;
    File f = LittleFS.open(ROI_MASK_FILE, "r");
    if (!f) return false;
    int bytes = f.readBytes(out_buf, buf_len - 1);
    out_buf[bytes] = '\0';
    f.close();
    return bytes > 0;
}

#endif // INCLUDE_ZONES
