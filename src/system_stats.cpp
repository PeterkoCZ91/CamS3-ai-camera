#include "system_stats.h"
#include "ws_log.h"
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_core_dump.h>

static const char* TAG = "SysStats";
static const char* STATS_FILE = "/sysstats.bin";

// Magic + version guard the on-flash layout: adding a counter later must not make
// an old file deserialize into garbage.
static const uint32_t STATS_MAGIC   = 0x43533353;  // "CS3S"
static const uint16_t STATS_VERSION = 1;

// Rewriting the file on every health check would wear the flash for no reason;
// the uptime record only needs coarse resolution.
static const unsigned long SAVE_INTERVAL_MS = 10UL * 60UL * 1000UL;

struct StatsFile {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    SystemStats stats;
};

SystemStats sysStats = {};
static unsigned long lastSaveMs = 0;

const char* resetReasonName(uint32_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_UNKNOWN:
        default:                return "UNKNOWN";
    }
}

static bool statsLoad() {
    if (!LittleFS.exists(STATS_FILE)) return false;
    File f = LittleFS.open(STATS_FILE, "r");
    if (!f) return false;

    StatsFile sf = {};
    size_t got = f.read((uint8_t*)&sf, sizeof(sf));
    f.close();

    if (got != sizeof(sf) || sf.magic != STATS_MAGIC || sf.version != STATS_VERSION) {
        logCapture("[%s] Stats file unusable (magic/version/size) — starting fresh\n", TAG);
        return false;
    }
    sysStats = sf.stats;
    return true;
}

static bool statsSave() {
    StatsFile sf = { STATS_MAGIC, STATS_VERSION, 0, sysStats };
    File f = LittleFS.open(STATS_FILE, "w");
    if (!f) {
        logCapture("[%s] Cannot open %s for write\n", TAG, STATS_FILE);
        return false;
    }
    size_t written = f.write((const uint8_t*)&sf, sizeof(sf));
    f.close();
    lastSaveMs = millis();
    return written == sizeof(sf);
}

void systemStatsBegin() {
    statsLoad();  // absent/corrupt file just means we start from zeroes

    uint32_t reason = (uint32_t)esp_reset_reason();
    sysStats.last_reset_reason = reason;
    sysStats.total_restarts++;

    switch (reason) {
        case ESP_RST_POWERON:  sysStats.poweron_restarts++;  break;
        case ESP_RST_BROWNOUT: sysStats.brownout_restarts++; break;
        case ESP_RST_PANIC:    sysStats.panic_restarts++;    break;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      sysStats.wdt_restarts++;      break;
        case ESP_RST_SW:       sysStats.sw_restarts++;       break;
        default:               sysStats.other_restarts++;    break;
    }

    statsSave();

    logCapture("[%s] Boot #%lu, reset reason: %s (poweron=%lu brownout=%lu wdt=%lu panic=%lu sw=%lu)\n",
               TAG, (unsigned long)sysStats.total_restarts, resetReasonName(reason),
               (unsigned long)sysStats.poweron_restarts, (unsigned long)sysStats.brownout_restarts,
               (unsigned long)sysStats.wdt_restarts, (unsigned long)sysStats.panic_restarts,
               (unsigned long)sysStats.sw_restarts);

    // A stored core dump means the last panic was captured and can be pulled off
    // the device — say so, otherwise nobody would think to look.
    size_t cdAddr = 0, cdSize = 0;
    if (esp_core_dump_image_get(&cdAddr, &cdSize) == ESP_OK && cdSize > 0) {
        logCapture("[%s] Core dump present in flash: %u bytes at 0x%x\n",
                   TAG, (unsigned)cdSize, (unsigned)cdAddr);
    }
}

void systemStatsTick() {
    uint32_t uptime = millis() / 1000;
    if (uptime <= sysStats.longest_uptime_s) return;

    sysStats.longest_uptime_s = uptime;
    if (lastSaveMs == 0 || millis() - lastSaveMs >= SAVE_INTERVAL_MS) {
        statsSave();
    }
}

void systemStatsToJson(JsonDocument& doc) {
    doc["total_restarts"]     = sysStats.total_restarts;
    doc["reset_reason"]       = resetReasonName(sysStats.last_reset_reason);
    doc["reset_reason_code"]  = sysStats.last_reset_reason;
    doc["poweron_restarts"]   = sysStats.poweron_restarts;
    doc["brownout_restarts"]  = sysStats.brownout_restarts;
    doc["wdt_restarts"]       = sysStats.wdt_restarts;
    doc["panic_restarts"]     = sysStats.panic_restarts;
    doc["sw_restarts"]        = sysStats.sw_restarts;
    doc["other_restarts"]     = sysStats.other_restarts;
    doc["longest_uptime_s"]   = sysStats.longest_uptime_s;

    size_t cdAddr = 0, cdSize = 0;
    bool haveDump = (esp_core_dump_image_get(&cdAddr, &cdSize) == ESP_OK && cdSize > 0);
    doc["coredump_present"]   = haveDump;
    doc["coredump_size"]      = haveDump ? (uint32_t)cdSize : 0;
}
