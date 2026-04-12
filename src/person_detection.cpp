#include "person_detection.h"

#ifdef INCLUDE_PERSON_DETECT

#include "config.h"
#include "camera_manager.h"
#include "tracker.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "esp_camera.h"
#include "img_converters.h"

// Edge Impulse FOMO model — uncomment after adding EI library
// #include <ei-person-detection-fomo_inferencing.h>

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#endif

#ifdef INCLUDE_TELEGRAM
#include "telegram.h"
#endif

#ifdef INCLUDE_MOTION_DETECT
#include "motion_detect.h"
#endif

static const char* TAG = "PersonDet";

// Semaphore from motion detection cascade
static SemaphoreHandle_t motionSem = NULL;

// Buffers (PSRAM)
static uint8_t* decodeBuffer = NULL;   // RGB565 from JPEG decode
static uint8_t* grayBuffer = NULL;     // Grayscale intermediate
static uint8_t* fomoInput = NULL;      // 64x64 grayscale for FOMO

// State
static volatile bool personDetected = false;
static volatile unsigned long lastPersonTime = 0;
static volatile uint32_t personEventCount = 0;
static PersonDetectResult lastResult = {};
static portMUX_TYPE pdMux = portMUX_INITIALIZER_UNLOCKED;

// Temporal filter
static int consecutiveDetections = 0;

// --- Image processing helpers ---

static inline uint8_t rgb565ToGray(uint16_t pixel) {
    uint8_t r = (pixel >> 11) & 0x1F;
    uint8_t g = (pixel >> 5) & 0x3F;
    uint8_t b = pixel & 0x1F;
    return (uint8_t)((77 * (r << 3) + 150 * (g << 2) + 29 * (b << 3)) >> 8);
}

// Bilinear resize grayscale image to PD_INPUT_SIZE x PD_INPUT_SIZE
static void bilinearResize(const uint8_t* src, int srcW, int srcH,
                           uint8_t* dst, int dstW, int dstH) {
    float xRatio = (float)(srcW - 1) / (float)(dstW - 1);
    float yRatio = (float)(srcH - 1) / (float)(dstH - 1);

    for (int y = 0; y < dstH; y++) {
        float srcY = y * yRatio;
        int y0 = (int)srcY;
        int y1 = min(y0 + 1, srcH - 1);
        float yFrac = srcY - y0;

        for (int x = 0; x < dstW; x++) {
            float srcX = x * xRatio;
            int x0 = (int)srcX;
            int x1 = min(x0 + 1, srcW - 1);
            float xFrac = srcX - x0;

            float top = src[y0 * srcW + x0] * (1.0f - xFrac) + src[y0 * srcW + x1] * xFrac;
            float bot = src[y1 * srcW + x0] * (1.0f - xFrac) + src[y1 * srcW + x1] * xFrac;
            dst[y * dstW + x] = (uint8_t)(top * (1.0f - yFrac) + bot * yFrac);
        }
    }
}

void personDetectInit() {
    // Allocate PSRAM buffers
    // Decode buffer: max 200x150 RGB565 (from 1600x1200 / 8)
    decodeBuffer = (uint8_t*)ps_malloc(200 * 150 * 2);
    grayBuffer   = (uint8_t*)ps_malloc(200 * 150);
    fomoInput    = (uint8_t*)ps_malloc(PD_INPUT_SIZE * PD_INPUT_SIZE);

    if (!decodeBuffer || !grayBuffer || !fomoInput) {
        Serial.printf("[%s] Failed to allocate PSRAM buffers!\n", TAG);
        return;
    }

    // Initialize tracker
    trackerInit();

    Serial.printf("[%s] Person detection initialized (FOMO %dx%d)\n",
                  TAG, PD_INPUT_SIZE, PD_INPUT_SIZE);
}

void personDetectSetSemaphore(SemaphoreHandle_t sem) {
    motionSem = sem;
}

void personDetectTask(void* param) {
    Serial.printf("[%s] Person detection task started\n", TAG);

    while (true) {
        if (!appConfig.person_detect.enabled) {
            personDetected = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Wait for motion semaphore (cascade from motion task)
        if (motionSem) {
            if (xSemaphoreTake(motionSem, pdMS_TO_TICKS(2000)) != pdTRUE) {
                // Timeout — no motion, clear state
                if (personDetected && (millis() - lastPersonTime > 5000)) {
                    personDetected = false;
                }
                continue;
            }
        } else {
            // Fallback: poll isMotionDetected()
            #ifdef INCLUDE_MOTION_DETECT
            if (!isMotionDetected()) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            #else
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
            #endif
        }

        // Cooldown check
        if (lastPersonTime > 0) {
            unsigned long cooldownMs = appConfig.person_detect.cooldown_sec * 1000UL;
            if (millis() - lastPersonTime < cooldownMs) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        // Get latest frame
        const uint8_t* buf = NULL;
        size_t len = 0;
        if (!ringBufferGetLatest(&buf, &len)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t t0 = millis();

        // Step 1: JPEG -> RGB565 at 1/8 scale
        bool decoded = jpg2rgb565(buf, len, decodeBuffer, JPG_SCALE_8X);
        ringBufferRelease();

        if (!decoded) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Determine decoded dimensions (same logic as face_detect.cpp)
        int decW, decH;
        switch (appConfig.camera.frame_size) {
            case 18: decW = 240; decH = 135; break;
            case 13: decW = 200; decH = 150; break;
            case 12: decW = 160; decH = 128; break;
            case 10: decW = 128; decH = 96;  break;
            case 9:  decW = 100; decH = 75;  break;
            case 8:  decW = 80;  decH = 60;  break;
            case 5:  decW = 40;  decH = 30;  break;
            default: decW = 160; decH = 120; break;
        }

        // Step 2: RGB565 -> Grayscale
        const uint16_t* pixels = (const uint16_t*)decodeBuffer;
        for (int i = 0; i < decW * decH; i++) {
            grayBuffer[i] = rgb565ToGray(pixels[i]);
        }

        // Step 3: Bilinear resize to 64x64
        bilinearResize(grayBuffer, decW, decH, fomoInput, PD_INPUT_SIZE, PD_INPUT_SIZE);

        // Step 4: FOMO inference
        // TODO: Uncomment after adding EI library
        /*
        signal_t signal;
        signal.total_length = PD_INPUT_SIZE * PD_INPUT_SIZE;
        signal.get_data = [](size_t offset, size_t length, float* out) -> int {
            for (size_t i = 0; i < length; i++) {
                out[i] = (float)fomoInput[offset + i] / 255.0f;
            }
            return 0;
        };

        ei_impulse_result_t result = {};
        EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
        if (err != EI_IMPULSE_OK) {
            Serial.printf("[%s] FOMO inference error: %d\n", TAG, err);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Convert FOMO bounding boxes to Detection structs
        Detection dets[TRACKER_MAX_DETECTIONS];
        int numDets = 0;
        for (size_t i = 0; i < result.bounding_boxes_count && numDets < TRACKER_MAX_DETECTIONS; i++) {
            ei_impulse_result_bounding_box_t& bb = result.bounding_boxes[i];
            if (bb.value < appConfig.person_detect.confidence_threshold) continue;
            dets[numDets].x = bb.x + bb.width / 2;
            dets[numDets].y = bb.y + bb.height / 2;
            dets[numDets].w = bb.width;
            dets[numDets].h = bb.height;
            dets[numDets].score = bb.value;
            dets[numDets].label = 0;
            numDets++;
        }
        */

        // Placeholder: no detections until EI model is integrated
        Detection dets[1];
        int numDets = 0;

        uint32_t inferenceMs = millis() - t0;

        // Step 5: Update tracker
        int confirmedCount = trackerUpdate(dets, numDets);

        // Step 6: Temporal filter
        bool confirmed = false;
        if (confirmedCount > 0) {
            consecutiveDetections++;
            if (consecutiveDetections >= appConfig.person_detect.temporal_frames) {
                confirmed = true;
            }
        } else {
            consecutiveDetections = 0;
        }

        // Step 7: Handle confirmed person detection
        if (confirmed) {
            personDetected = true;
            lastPersonTime = millis();
            personEventCount++;

            Serial.printf("[%s] Person confirmed! (%d tracked, %dms, event #%lu)\n",
                          TAG, confirmedCount, inferenceMs, personEventCount);

            // Notify for newly confirmed tracks (prevents duplicate notifications)
            #ifdef INCLUDE_TELEGRAM
            if (appConfig.telegram.enabled && appConfig.telegram.notify_on_person && isWithinActiveHours()) {
                const Track* newTracks[4];
                int newCount = trackerGetNewlyConfirmed(newTracks, 4);

                if (newCount > 0) {
                    char caption[64];
                    snprintf(caption, sizeof(caption), "Osoba detekovana (%d osob/y)", confirmedCount);

                    if (appConfig.telegram.photo_on_person) {
                        const uint8_t* tgBuf = NULL;
                        size_t tgLen = 0;
                        if (ringBufferGetLatest(&tgBuf, &tgLen)) {
                            telegramSendPhoto(tgBuf, tgLen, caption);
                            ringBufferRelease();
                        }
                    } else {
                        telegramSendText(caption);
                    }

                    // Mark notified
                    for (int i = 0; i < newCount; i++) {
                        trackerMarkNotified(newTracks[i]->id);
                    }
                }
            }
            #endif
        } else {
            if (personDetected && (millis() - lastPersonTime > 5000)) {
                personDetected = false;
            }
        }

        // Update result
        int totalTracks = 0;
        trackerGetTracks(&totalTracks);

        PersonDetectResult res = {};
        res.detected = personDetected;
        res.person_count = confirmedCount;
        res.raw_detections = numDets;
        res.top_score = (numDets > 0) ? dets[0].score : 0.0f;
        res.inference_ms = inferenceMs;
        res.track_count = totalTracks;

        portENTER_CRITICAL(&pdMux);
        lastResult = res;
        portEXIT_CRITICAL(&pdMux);

        // ~3-4 fps for person detection
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

bool isPersonDetected() { return personDetected; }
unsigned long getLastPersonTime() { return lastPersonTime; }
uint32_t getPersonEventCount() { return personEventCount; }

PersonDetectResult getPersonDetectResult() {
    PersonDetectResult r;
    portENTER_CRITICAL(&pdMux);
    r = lastResult;
    portEXIT_CRITICAL(&pdMux);
    return r;
}

#endif // INCLUDE_PERSON_DETECT
