#include "ws_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Ring buffer in PSRAM — see the note in ws_log.h about DRAM pressure.
typedef char LogLine[LOG_LINE_MAX_LEN];
static LogLine* logRing = NULL;
static int logHead = 0;      // Next write position
static int logCount = 0;     // Total lines stored (max LOG_RING_LINES)
static SemaphoreHandle_t logMutex = NULL;

void logInit() {
    logMutex = xSemaphoreCreateMutex();
    logHead = 0;
    logCount = 0;

    logRing = (LogLine*)ps_calloc(LOG_RING_LINES, sizeof(LogLine));
    if (!logRing) {
        // No PSRAM (or it is exhausted): keep logging to Serial, just without
        // history. Better than failing to boot or eating 25 kB of DRAM.
        Serial.println("[Log] PSRAM ring allocation failed — /log history disabled");
    }
}

void logCapture(const char* fmt, ...) {
    char buf[LOG_LINE_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Serial always gets the line, even before logInit() ran or without PSRAM.
    Serial.print(buf);

    if (!logMutex || !logRing) return;

    // Strip trailing newline for clean storage
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[--len] = '\0';
    }
    if (len == 0) return;

    if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(logRing[logHead], buf, LOG_LINE_MAX_LEN - 1);
        logRing[logHead][LOG_LINE_MAX_LEN - 1] = '\0';
        logHead = (logHead + 1) % LOG_RING_LINES;
        if (logCount < LOG_RING_LINES) logCount++;
        xSemaphoreGive(logMutex);
    }
}

char* logGetAll() {
    if (!logMutex || !logRing) return NULL;

    // Allocate in PSRAM: worst case LOG_RING_LINES * (LOG_LINE_MAX_LEN + 1)
    size_t maxSize = LOG_RING_LINES * (LOG_LINE_MAX_LEN + 1);
    char* result = (char*)ps_malloc(maxSize);
    if (!result) return NULL;

    result[0] = '\0';
    size_t pos = 0;

    if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Read from oldest to newest
        int start = (logCount < LOG_RING_LINES) ? 0 : logHead;
        for (int i = 0; i < logCount; i++) {
            int idx = (start + i) % LOG_RING_LINES;
            int len = strlen(logRing[idx]);
            if (pos + len + 2 < maxSize) {
                memcpy(result + pos, logRing[idx], len);
                pos += len;
                result[pos++] = '\n';
            }
        }
        result[pos] = '\0';
        xSemaphoreGive(logMutex);
    }

    return result;
}

int logGetCount() {
    return logCount;
}
