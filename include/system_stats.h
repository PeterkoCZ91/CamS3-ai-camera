#ifndef SYSTEM_STATS_H
#define SYSTEM_STATS_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Persisted restart accounting.
//
// Why this exists: the device reboots on watchdog panic, brownout and on its own
// camera-recovery escalation, and until now every reboot erased all evidence that
// it happened. Uptime alone cannot distinguish "running fine for an hour" from
// "crashed 30 times and is currently one hour into attempt 31". These counters plus
// esp_reset_reason() answer that from /api/status without a serial console attached.

struct SystemStats {
    uint32_t total_restarts;
    uint32_t poweron_restarts;   // ESP_RST_POWERON — cable pulled, PSU glitch
    uint32_t brownout_restarts;  // ESP_RST_BROWNOUT — undervoltage, bad supply
    uint32_t wdt_restarts;       // task / int / RTC watchdog
    uint32_t panic_restarts;     // ESP_RST_PANIC — crash with core dump
    uint32_t sw_restarts;        // commanded: OTA, /api/reboot, /restart
    uint32_t other_restarts;
    uint32_t last_reset_reason;  // raw esp_reset_reason() of THIS boot
    uint32_t longest_uptime_s;
};

extern SystemStats sysStats;

// Load counters, account for this boot, persist. Call once early in setup(),
// after LittleFS is mounted.
void systemStatsBegin();

// Refresh longest_uptime_s if this boot has outlived the record. Cheap; writes to
// flash at most once per SYSTEM_STATS_SAVE_INTERVAL. Call from the health check.
void systemStatsTick();

const char* resetReasonName(uint32_t reason);

// Add the counters (and coredump presence) to a status document.
void systemStatsToJson(JsonDocument& doc);

#endif // SYSTEM_STATS_H
