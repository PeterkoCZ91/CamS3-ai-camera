#include "timelapse.h"
#include "ws_log.h"

#ifdef INCLUDE_TIMELAPSE

#include "config.h"
#include "camera_manager.h"
#include "image_utils.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#include "sd_store.h"
#endif

static const char* TAG = "Timelapse";

static volatile uint32_t timelapseCount = 0;
static unsigned long lastCapture = 0;
static bool hasLastCapture = false;

#ifdef INCLUDE_SD_CARD
// Allocated once on first use and held: lets us release the ring slot before the
// (slow, breaker-guarded) SD write instead of holding it across card I/O.
static uint8_t* jpegCopy = NULL;
#endif

void timelapseInit() {
    logCapture("[%s] Timelapse initialized (interval: %ds)\n",
                  TAG, appConfig.timelapse.interval_sec);
}

void timelapseTask(void* param) {
    logCapture("[%s] Timelapse task started\n", TAG);
    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();
        if (!appConfig.timelapse.enabled) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        unsigned long now = millis();
        int intervalSec = appConfig.timelapse.interval_sec;
        if (intervalSec < 1) intervalSec = 1;   // 0 / negative would capture every loop
        unsigned long interval = (unsigned long)intervalSec * 1000UL;

        if (!hasLastCapture || now - lastCapture >= interval) {
            hasLastCapture = true;
            lastCapture = now;

            const uint8_t* buf = NULL;
            size_t len = 0;

            int rh = ringBufferGetLatest(&buf, &len);
            if (rh >= 0) {
                #ifdef INCLUDE_SD_CARD
                // Rotation and the write-failure breaker now live in sd_store, so
                // timelapse, motion and face captures all behave the same way. The
                // old inline copy here only rotated one file per capture and only
                // below 5 % free, which lost the race against a filling card.
                // Copy to PSRAM and release the slot first: SD I/O can take
                // hundreds of ms and a held slot starves the capture task.
                size_t saveLen = 0;
                bool wantSave = appConfig.timelapse.save_to_sd && sdStoreAvailable();
                if (wantSave) {
                    if (!jpegCopy) jpegCopy = (uint8_t*)ps_malloc(IMG_JPEG_COPY_MAX);
                    if (jpegCopy && len > 0 && len <= IMG_JPEG_COPY_MAX) {
                        memcpy(jpegCopy, buf, len);
                        saveLen = len;
                    }
                }
                ringBufferRelease(rh);
                if (saveLen > 0) {
                    if (!sdStoreWriteJpeg("/timelapse", "tl_", jpegCopy, saveLen)) {
                        logCapture("[%s] Capture #%lu not stored\n", TAG, (unsigned long)timelapseCount);
                    }
                } else if (wantSave) {
                    logCapture("[%s] Capture #%lu not stored (no copy buffer / oversize)\n",
                               TAG, (unsigned long)timelapseCount);
                }
                #else
                ringBufferRelease(rh);
                #endif

                timelapseCount++;
            } else {
                logCapture("[%s] No frame available for timelapse\n", TAG);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

uint32_t getTimelapseCount() { return timelapseCount; }

#endif // INCLUDE_TIMELAPSE
