#include "face_detect.h"
#include "ws_log.h"

#if defined(INCLUDE_FACE_DETECT) && defined(INCLUDE_MOTION_DETECT)

#include "config.h"
#include "camera_manager.h"
#include "cz_text.h"
#include "image_utils.h"
#include "motion_detect.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#include "sd_store.h"
#endif

#ifdef INCLUDE_TELEGRAM
#include "telegram.h"
#endif

#ifdef INCLUDE_EVENT_LOG
#include "event_log.h"
#endif

static const char* TAG = "FaceDet";

// Decode buffer for face detection (1/8 scale for speed, closer to model sweet spot).
// Sized for the sensor's largest frame (QSXGA / 8 = 320x240) so no frame_size can
// overflow it — the old 256x160 sizing came from a frame_size table that lacked
// QXGA/QSXGA entries and silently fell through to a too-small "default".
#define FACE_DECODE_BUF_SIZE IMG_DECODE_MAX_SZ

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

bool faceDetectInit() {
    // esp-dl tie728 SIMD kernels require 16-byte aligned input; plain ps_malloc
    // is only 4-byte aligned, which triggered LoadStoreError in MSR01's first
    // depthwise conv (EXCVADDR landing in I-cache space on unaligned vector load).
    faceDecodeBuf = (uint8_t*)heap_caps_aligned_alloc(16, FACE_DECODE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!faceDecodeBuf) {
        logCapture("[%s] Failed to allocate decode buffer (%d bytes)\n", TAG, FACE_DECODE_BUF_SIZE);
        return false;
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

    logCapture("[%s] Face detection initialized (two_stage=%s)\n",
                  TAG, detector2 ? "yes" : "no");
    return detector1 != NULL;
}

void faceDetectTask(void* param) {
    logCapture("[%s] Face detection task started\n", TAG);
    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();
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

        // Get latest frame, with the dimensions it was captured at
        const uint8_t* buf = NULL;
        size_t len = 0;
        uint16_t srcW = 0, srcH = 0;
        int rh = ringBufferGetLatest(&buf, &len, &srcW, &srcH);
        if (rh < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Bound-check before decoding — jpg2rgb565 has no output limit.
        int decW = 0, decH = 0;
        if (!imgDecodeFits(srcW, srcH, FACE_DECODE_BUF_SIZE, decW, decH)) {
            ringBufferRelease(rh);
            logCapture("[%s] Frame %ux%u would decode to %dx%d, over buffer — skipping\n",
                          TAG, srcW, srcH, decW, decH);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint32_t t0 = millis();

        // Decode JPEG to RGB565 at 1/8 scale. tjpgd isn't reentrant → serialize.
        bool decoded = false;
        if (cameraDecodeLock()) {
            decoded = jpg2rgb565(buf, len, faceDecodeBuf, JPG_SCALE_8X);
            cameraDecodeUnlock();
        }

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

        ringBufferRelease(rh);

        if (!decoded) {
            if (saveBuf) free(saveBuf);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Stage 1: MSR01 detection (decW/decH validated before the decode)
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
            logCapture("[%s] %d face(s) detected (%.2f score, %dms, event #%lu)\n",
                          TAG, faceCount, result.largest_score, inferenceMs, faceEventCount);

            #ifdef INCLUDE_EVENT_LOG
            {
                char detail[EVENT_DETAIL_LEN];
                snprintf(detail, sizeof(detail), "n=%d score=%.2f %lums",
                         faceCount, result.largest_score, (unsigned long)inferenceMs);
                logEvent(EVT_FACE, detail);
            }
            #endif

            #ifdef INCLUDE_SD_CARD
            if (saveBuf && saveLen > 0 && sdStoreAvailable()) {
                sdStoreWriteJpeg("/captures", "face_", saveBuf, saveLen);
            }
            #endif

            #ifdef INCLUDE_TELEGRAM
            if (appConfig.telegram.enabled && appConfig.telegram.notify_on_face && isWithinActiveHours()) {
                // Czech agrees the verb and the noun with the count, so build both
                // forms instead of the old "1 oblicej/u" slash placeholder.
                char caption[64];
                snprintf(caption, sizeof(caption), "%s %d %s",
                         czPlural(faceCount, "Detekován", "Detekovány", "Detekováno"),
                         faceCount,
                         czPlural(faceCount, "obličej", "obličeje", "obličejů"));
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
