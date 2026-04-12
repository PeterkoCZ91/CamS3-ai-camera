#include "face_detect.h"

#if defined(INCLUDE_FACE_DETECT) && defined(INCLUDE_MOTION_DETECT)

#include "config.h"
#include "camera_manager.h"
#include "motion_detect.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#endif

#ifdef INCLUDE_TELEGRAM
#include "telegram.h"
#endif

static const char* TAG = "FaceDet";

// Decode buffer for face detection (1/8 scale for speed, closer to model sweet spot)
// Max input: 1600x1200 / 8 = 200x150, RGB565 = 60000 bytes
// Note: 200x150 is close to model's optimal ~120x160 and much faster than 400x300
#define FACE_DECODE_MAX_W  200
#define FACE_DECODE_MAX_H  150
#define FACE_DECODE_BUF_SIZE (FACE_DECODE_MAX_W * FACE_DECODE_MAX_H * 2)

static uint8_t* faceDecodeBuf = NULL;

// Detectors (allocated on init)
static HumanFaceDetectMSR01* detector1 = NULL;
static HumanFaceDetectMNP01* detector2 = NULL;

// State
static volatile bool faceDetected = false;
static volatile unsigned long lastFaceTime = 0;
static volatile uint32_t faceEventCount = 0;
static FaceDetectResult lastResult = {};
static portMUX_TYPE faceMux = portMUX_INITIALIZER_UNLOCKED;

void faceDetectInit() {
    faceDecodeBuf = (uint8_t*)ps_malloc(FACE_DECODE_BUF_SIZE);
    if (!faceDecodeBuf) {
        Serial.printf("[%s] Failed to allocate decode buffer (%d bytes)\n", TAG, FACE_DECODE_BUF_SIZE);
        return;
    }

    // Create detectors following Espressif CameraWebServer pattern:
    // Two-stage: MSR01 uses low threshold (0.1) as coarse filter, MNP01 refines with user threshold
    // One-stage: MSR01 uses user threshold directly
    if (appConfig.face_detect.two_stage) {
        detector1 = new HumanFaceDetectMSR01(0.1f, 0.5f, 10, 0.2f);
        detector2 = new HumanFaceDetectMNP01(
            appConfig.face_detect.score_threshold,
            appConfig.face_detect.nms_threshold,
            5  // top_k
        );
    } else {
        detector1 = new HumanFaceDetectMSR01(
            appConfig.face_detect.score_threshold,
            0.5f,   // nms_threshold
            10,     // top_k
            0.2f    // resize_scale
        );
    }

    Serial.printf("[%s] Face detection initialized (two_stage=%s)\n",
                  TAG, detector2 ? "yes" : "no");
}

void faceDetectTask(void* param) {
    Serial.printf("[%s] Face detection task started\n", TAG);

    while (true) {
        // Check if face detection is enabled
        if (!appConfig.face_detect.enabled || !detector1) {
            faceDetected = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Cascade: only run when motion is detected
        if (!isMotionDetected()) {
            // Clear face detection after timeout
            if (faceDetected && (millis() - lastFaceTime > 5000)) {
                faceDetected = false;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Cooldown check
        if (lastFaceTime > 0) {
            unsigned long cooldownMs = appConfig.face_detect.cooldown_sec * 1000UL;
            if (millis() - lastFaceTime < cooldownMs) {
                vTaskDelay(pdMS_TO_TICKS(500));
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

        // Decode JPEG to RGB565 at 1/8 scale (fast, close to model's optimal input size)
        bool decoded = jpg2rgb565(buf, len, faceDecodeBuf, JPG_SCALE_8X);

        // Copy JPEG data for potential SD save before releasing ring buffer
        uint8_t* saveBuf = NULL;
        size_t saveLen = 0;
        #ifdef INCLUDE_SD_CARD
        if (appConfig.face_detect.save_to_sd && decoded) {
            saveBuf = (uint8_t*)malloc(len);
            if (saveBuf) {
                memcpy(saveBuf, buf, len);
                saveLen = len;
            }
        }
        #endif

        ringBufferRelease();

        if (!decoded) {
            if (saveBuf) free(saveBuf);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Determine decoded image dimensions based on source resolution
        // JPG_SCALE_8X divides dimensions by 8 (integer division)
        int decW, decH;
        switch (appConfig.camera.frame_size) {
            case 18: decW = 240; decH = 135; break;  // FHD 1920x1080
            case 13: decW = 200; decH = 150; break;  // UXGA 1600x1200
            case 12: decW = 160; decH = 128; break;  // SXGA 1280x1024
            case 10: decW = 128; decH = 96;  break;  // XGA 1024x768
            case 9:  decW = 100; decH = 75;  break;  // SVGA 800x600
            case 8:  decW = 80;  decH = 60;  break;  // VGA 640x480
            case 5:  decW = 40;  decH = 30;  break;  // QVGA 320x240
            default: decW = 160; decH = 120; break;
        }

        // Stage 1: MSR01 detection
        std::list<dl::detect::result_t>& results1 =
            detector1->infer<uint16_t>((uint16_t*)faceDecodeBuf, {decH, decW, 3});

        std::list<dl::detect::result_t>* finalResults = &results1;

        // Stage 2: MNP01 refinement (if enabled and stage 1 found candidates)
        if (detector2 && !results1.empty()) {
            std::list<dl::detect::result_t>& results2 =
                detector2->infer<uint16_t>((uint16_t*)faceDecodeBuf, {decH, decW, 3}, results1);
            finalResults = &results2;
        }

        uint32_t inferenceMs = millis() - t0;
        int faceCount = finalResults->size();

        // Find largest face
        int largestArea = 0;
        FaceDetectResult result = {};
        result.detected = (faceCount > 0);
        result.face_count = faceCount;
        result.inference_ms = inferenceMs;

        for (auto& det : *finalResults) {
            if (det.box.size() >= 4) {
                int w = det.box[2] - det.box[0];
                int h = det.box[3] - det.box[1];
                int area = w * h;
                if (area > largestArea) {
                    largestArea = area;
                    result.largest_x = det.box[0];
                    result.largest_y = det.box[1];
                    result.largest_w = w;
                    result.largest_h = h;
                    result.largest_score = det.score;
                }
            }
        }

        // Update state
        portENTER_CRITICAL(&faceMux);
        lastResult = result;
        portEXIT_CRITICAL(&faceMux);

        if (faceCount > 0) {
            faceDetected = true;
            lastFaceTime = millis();
            faceEventCount++;
            Serial.printf("[%s] %d face(s) detected (%.2f score, %dms, event #%lu)\n",
                          TAG, faceCount, result.largest_score, inferenceMs, faceEventCount);

            #ifdef INCLUDE_SD_CARD
            if (saveBuf && saveLen > 0 && SD.cardType() != CARD_NONE) {
                char filename[64];
                snprintf(filename, sizeof(filename), "/captures/face_%lu.jpg", millis());
                File f = SD.open(filename, FILE_WRITE);
                if (f) {
                    f.write(saveBuf, saveLen);
                    f.close();
                    Serial.printf("[%s] Saved to %s\n", TAG, filename);
                }
            }
            #endif

            #ifdef INCLUDE_TELEGRAM
            if (appConfig.telegram.enabled && appConfig.telegram.notify_on_face && isWithinActiveHours()) {
                char caption[64];
                snprintf(caption, sizeof(caption), "Oblicej detekovan (%d oblicej/u)", faceCount);
                if (appConfig.telegram.photo_on_face && saveBuf && saveLen > 0) {
                    telegramSendPhoto(saveBuf, saveLen, caption);
                } else {
                    telegramSendText(caption);
                }
            }
            #endif
        } else {
            if (faceDetected && (millis() - lastFaceTime > 5000)) {
                faceDetected = false;
            }
        }

        if (saveBuf) free(saveBuf);

        // Face detection is heavier, run at ~2 fps
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

bool isFaceDetected() { return faceDetected; }
unsigned long getLastFaceTime() { return lastFaceTime; }
uint32_t getFaceEventCount() { return faceEventCount; }

FaceDetectResult getFaceDetectResult() {
    FaceDetectResult r;
    portENTER_CRITICAL(&faceMux);
    r = lastResult;
    portEXIT_CRITICAL(&faceMux);
    return r;
}

#endif // INCLUDE_FACE_DETECT && INCLUDE_MOTION_DETECT
