#include "sd_store.h"

#ifdef INCLUDE_SD_CARD

#include "config.h"
#include "ws_log.h"
#include <SD.h>

#ifdef INCLUDE_EVENT_LOG
#include "event_log.h"
#endif

static const char* TAG = "SDStore";

static uint32_t consecutiveFailures = 0;
static bool     sessionDisabled     = false;

bool sdStoreDisabled() { return sessionDisabled; }
uint32_t sdStoreFailureCount() { return consecutiveFailures; }

void sdStoreReset() {
    consecutiveFailures = 0;
    sessionDisabled = false;
    logCapture("[%s] Breaker re-armed\n", TAG);
}

bool sdStoreAvailable() {
    if (sessionDisabled) return false;
    return SD.cardType() != CARD_NONE;
}

int sdStoreUsagePercent() {
    if (SD.cardType() == CARD_NONE) return 0;
    uint64_t total = SD.totalBytes();
    if (total == 0) return 0;
    return (int)((SD.usedBytes() * 100ULL) / total);
}

static void noteFailure(const char* what) {
    consecutiveFailures++;
    logCapture("[%s] %s failed (%lu/%d)\n", TAG, what,
               (unsigned long)consecutiveFailures, SD_STORE_MAX_FAILURES);

    if (consecutiveFailures >= SD_STORE_MAX_FAILURES && !sessionDisabled) {
        sessionDisabled = true;
        logCapture("[%s] Too many failures — SD writes disabled for this session\n", TAG);
        #ifdef INCLUDE_EVENT_LOG
        logEvent(EVT_SD_FAILURE, "writes disabled");
        #endif
    }
}

// Delete the oldest entries in `dir` until usage is under the limit. Returns the
// number of files removed. Only regular files directly in `dir` are considered —
// this must never wander into another directory's data.
static int rotateDirectory(const char* dir, int targetPct) {
    int removed = 0;

    for (int pass = 0; pass < SD_STORE_MAX_DELETES; pass++) {
        if (sdStoreUsagePercent() <= targetPct) break;

        File d = SD.open(dir);
        if (!d || !d.isDirectory()) {
            if (d) d.close();
            break;
        }

        // One scan per deletion: the directory is small (bounded by rotation) and
        // holding a full listing in RAM inside a detection task is worse.
        char oldestName[64] = {0};
        time_t oldestTime = 0;
        File e = d.openNextFile();
        while (e) {
            if (!e.isDirectory()) {
                time_t t = e.getLastWrite();
                if (oldestName[0] == '\0' || t < oldestTime) {
                    oldestTime = t;
                    strlcpy(oldestName, e.name(), sizeof(oldestName));
                }
            }
            e.close();
            e = d.openNextFile();
        }
        d.close();

        if (oldestName[0] == '\0') break;  // nothing left to delete

        // e.name() may come back as a bare name or a full path depending on the
        // core version; normalise to an absolute path either way.
        char path[128];
        if (oldestName[0] == '/') {
            strlcpy(path, oldestName, sizeof(path));
        } else {
            snprintf(path, sizeof(path), "%s/%s", dir, oldestName);
        }

        if (!SD.remove(path)) {
            logCapture("[%s] Rotation could not remove %s\n", TAG, path);
            break;
        }
        removed++;
    }

    if (removed > 0) {
        logCapture("[%s] Rotated %s: removed %d file(s), usage now %d%%\n",
                   TAG, dir, removed, sdStoreUsagePercent());
    }
    return removed;
}

bool sdStoreWriteJpeg(const char* dir, const char* prefix,
                      const uint8_t* data, size_t len,
                      char* outPath, size_t outPathLen) {
    if (!data || len == 0) return false;
    if (!sdStoreAvailable()) return false;

    if (!SD.exists(dir) && !SD.mkdir(dir)) {
        noteFailure("mkdir");
        return false;
    }

    // Rotate before writing, so a full card frees space instead of failing.
    int limit = SD_MAX_USAGE_PERCENT;
    if (sdStoreUsagePercent() > limit) {
        int target = limit - SD_STORE_FREE_MARGIN_PCT;
        if (target < 10) target = 10;
        rotateDirectory(dir, target);

        if (sdStoreUsagePercent() > limit) {
            // Rotation did not help — the space is used by something we do not
            // manage. Treat it as a failure so the breaker eventually trips.
            noteFailure("rotation");
            return false;
        }
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s%lu.jpg", dir, prefix, (unsigned long)millis());

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        noteFailure("open");
        return false;
    }
    size_t written = f.write(data, len);
    f.close();

    if (written != len) {
        SD.remove(path);  // don't leave a truncated JPEG behind
        noteFailure("write");
        return false;
    }

    consecutiveFailures = 0;
    if (outPath && outPathLen) strlcpy(outPath, path, outPathLen);
    logCapture("[%s] Saved %s (%u B, usage %d%%)\n",
               TAG, path, (unsigned)len, sdStoreUsagePercent());
    return true;
}

#endif // INCLUDE_SD_CARD
