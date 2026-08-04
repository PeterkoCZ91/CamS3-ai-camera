#ifndef SD_STORE_H
#define SD_STORE_H

#include <Arduino.h>

#ifdef INCLUDE_SD_CARD

// Guarded writes for everything the firmware saves to the SD card.
//
// Why this module exists: SD_MAX_USAGE_PERCENT was defined in config.h and never
// read. Motion captures, face captures and manual snapshots all wrote straight into
// /captures with no rotation, so the card filled up, SD.open() started returning a
// false File, and the failure was swallowed — the device kept reporting healthy
// while silently storing nothing. That is the worst failure mode for a camera.
//
// Two mechanisms:
//   * rotation      — before writing, delete the oldest files in the target
//                     directory until usage drops below the configured limit
//   * circuit breaker — after SD_STORE_MAX_FAILURES consecutive write failures,
//                     stop trying for the rest of the session and raise
//                     EVT_SD_FAILURE, so a dead/unwritable card is visible instead
//                     of costing a filesystem operation on every event

#define SD_STORE_MAX_FAILURES   3
// Free up a bit past the limit so we are not rotating on every single write.
#define SD_STORE_FREE_MARGIN_PCT 5
// Bound the work per call — deleting hundreds of files inside a detection task
// would block it for seconds.
#define SD_STORE_MAX_DELETES    20

// True when the card is mounted and the breaker has not tripped.
bool sdStoreAvailable();

// Write a JPEG as <dir>/<prefix><millis>.jpg, rotating the directory first.
// outPath (optional) receives the path written. Returns false if the card is
// unavailable, rotation could not free space, or the write failed.
bool sdStoreWriteJpeg(const char* dir, const char* prefix,
                      const uint8_t* data, size_t len,
                      char* outPath = nullptr, size_t outPathLen = 0);

// Current SD usage in percent (0 when unavailable).
int sdStoreUsagePercent();

// Consecutive write failures, and whether the breaker has tripped this session.
uint32_t sdStoreFailureCount();
bool sdStoreDisabled();

// Re-arm the breaker (e.g. after the operator swaps the card and reboots is not
// an option). Exposed so the admin UI can offer a retry.
void sdStoreReset();

#endif // INCLUDE_SD_CARD
#endif // SD_STORE_H
