#include "timelapse.h"
#include "ws_log.h"

#ifdef INCLUDE_TIMELAPSE

#include "config.h"
#include "camera_manager.h"
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
        unsigned long interval = appConfig.timelapse.interval_sec * 1000UL;

        if (now - lastCapture >= interval) {
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
                if (appConfig.timelapse.save_to_sd && sdStoreAvailable()) {
                    if (!sdStoreWriteJpeg("/timelapse", "tl_", buf, len)) {
                        logCapture("[%s] Capture #%lu not stored\n", TAG, timelapseCount);
                    }
                }
                #endif

                timelapseCount++;
                ringBufferRelease(rh);
            } else {
                logCapture("[%s] No frame available for timelapse\n", TAG);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

uint32_t getTimelapseCount() { return timelapseCount; }

#endif // INCLUDE_TIMELAPSE
