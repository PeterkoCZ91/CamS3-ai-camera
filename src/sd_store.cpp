#include "sd_store.h"

#ifdef INCLUDE_SD_CARD

#include "config.h"
#include "ws_log.h"
#include <SD.h>
#include <esp_task_wdt.h>
#include <stdlib.h>

#ifdef INCLUDE_EVENT_LOG
#include "event_log.h"
#endif

static const char* TAG = "SDStore";

static uint32_t consecutiveFailures = 0;
static bool     sessionDisabled     = false;
static bool     warnedFull          = false;
static uint32_t nameSeq             = 0;

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
//
// One directory scan collects the SD_STORE_MAX_DELETES oldest candidates (kept
// sorted, oldest first); they are then removed in order until usage is low enough.
// The old version rescanned the whole directory for every single deletion.
struct RotateCand {
    time_t t;
    char   name[64];
};

static int rotateDirectory(const char* dir, int targetPct) {
    RotateCand* cand = (RotateCand*)malloc(sizeof(RotateCand) * SD_STORE_MAX_DELETES);
    if (!cand) return 0;
    int n = 0;

    File d = SD.open(dir);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        free(cand);
        return 0;
    }

    File e = d.openNextFile();
    while (e) {
        esp_task_wdt_reset();
        if (!e.isDirectory()) {
            time_t t = e.getLastWrite();
            const char* nm = e.name();
            if (nm && strlen(nm) < sizeof(cand[0].name) &&
                (n < SD_STORE_MAX_DELETES || t < cand[n - 1].t)) {
                // Insert keeping ascending order by time; drop the newest when full.
                int pos = (n < SD_STORE_MAX_DELETES) ? n : n - 1;
                while (pos > 0 && cand[pos - 1].t > t) {
                    if (pos < SD_STORE_MAX_DELETES) cand[pos] = cand[pos - 1];
                    pos--;
                }
                cand[pos].t = t;
                strlcpy(cand[pos].name, nm, sizeof(cand[pos].name));
                if (n < SD_STORE_MAX_DELETES) n++;
            }
        }
        e.close();
        e = d.openNextFile();
    }
    d.close();

    int removed = 0;
    for (int i = 0; i < n; i++) {
        esp_task_wdt_reset();
        if (sdStoreUsagePercent() <= targetPct) break;

        // e.name() may come back as a bare name or a full path depending on the
        // core version; normalise to an absolute path either way.
        char path[160];
        if (cand[i].name[0] == '/') {
            strlcpy(path, cand[i].name, sizeof(path));
        } else {
            snprintf(path, sizeof(path), "%s/%s", dir, cand[i].name);
        }

        if (!SD.remove(path)) {
            logCapture("[%s] Rotation could not remove %s\n", TAG, path);
            break;
        }
        removed++;
    }
    free(cand);

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
            // Rotation did not help — the space is used by something outside this
            // directory (usage is measured for the whole card, rotation only touches
            // `dir`). That is a full card, not a broken one, so it must not trip the
            // breaker; log once and skip this write.
            if (!warnedFull) {
                warnedFull = true;
                logCapture("[%s] Card over %d%% and %s has nothing left to rotate — skipping writes\n",
                           TAG, limit, dir);
            }
            return false;
        }
    }
    warnedFull = false;

    char path[128];
    snprintf(path, sizeof(path), "%s/%s%lu.jpg", dir, prefix, (unsigned long)millis());
    // millis() restarts at every boot and FILE_WRITE truncates: never overwrite an
    // existing capture.
    for (int tries = 0; SD.exists(path) && tries < 100; tries++) {
        snprintf(path, sizeof(path), "%s/%s%lu_%lu.jpg", dir, prefix,
                 (unsigned long)millis(), (unsigned long)(++nameSeq));
    }

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
