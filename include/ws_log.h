#ifndef WS_LOG_H
#define WS_LOG_H

#include <Arduino.h>

// Ring buffer log capture — intercepts Serial.printf style output
// and stores last LOG_RING_LINES lines for web access

#define LOG_RING_LINES    100
#define LOG_LINE_MAX_LEN  200

// Initialize the log ring buffer
void logInit();

// Capture a log line (printf-style). Thread-safe.
void logCapture(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Get all buffered log lines as a single string (caller must free() result)
// Returns PSRAM-allocated string, or NULL on failure.
char* logGetAll();

// Get number of stored lines
int logGetCount();

#endif // WS_LOG_H
