#ifndef WS_LOG_H
#define WS_LOG_H

#include <Arduino.h>

// Ring buffer log capture. logCapture() is the project's logging entry point:
// it echoes to Serial *and* keeps the last LOG_RING_LINES lines so /log and
// /log-viewer have something to show. Modules must call logCapture() rather than
// Serial.printf() — otherwise the ring stays empty, which is exactly how those two
// endpoints ended up always returning nothing.
//
// The ring lives in PSRAM (25 kB), not in BSS, because internal DRAM is the scarce
// resource on this board (async web server + TLS + camera buffers all fight for it).

#define LOG_RING_LINES    100
#define LOG_LINE_MAX_LEN  256

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
