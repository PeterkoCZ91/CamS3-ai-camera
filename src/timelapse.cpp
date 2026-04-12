#include "timelapse.h"

#ifdef INCLUDE_TIMELAPSE

#include "config.h"
#include "camera_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#endif

static const char* TAG = "Timelapse";

static volatile uint32_t timelapseCount = 0;
static unsigned long lastCapture = 0;

void timelapseInit() {
    Serial.printf("[%s] Timelapse initialized (interval: %ds)\n",
                  TAG, appConfig.timelapse.interval_sec);
}

void timelapseTask(void* param) {
    Serial.printf("[%s] Timelapse task started\n", TAG);

    while (true) {
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

            if (ringBufferGetLatest(&buf, &len)) {
                #ifdef INCLUDE_SD_CARD
                if (appConfig.timelapse.save_to_sd && SD.cardType() != CARD_NONE) {
                    // Generate sequential filename
                    char filename[64];
                    snprintf(filename, sizeof(filename), "/timelapse/tl_%06lu.jpg", timelapseCount);
                    File f = SD.open(filename, FILE_WRITE);
                    if (f) {
                        f.write(buf, len);
                        f.close();
                        Serial.printf("[%s] Captured #%lu -> %s (%u bytes)\n",
                                      TAG, timelapseCount, filename, (unsigned)len);
                    } else {
                        Serial.printf("[%s] Failed to write %s\n", TAG, filename);
                    }
                }
                #endif

                timelapseCount++;
                ringBufferRelease();
            } else {
                Serial.printf("[%s] No frame available for timelapse\n", TAG);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

uint32_t getTimelapseCount() { return timelapseCount; }

#endif // INCLUDE_TIMELAPSE
