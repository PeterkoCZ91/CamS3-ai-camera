#include "person_detection.h"
#include "ws_log.h"

#ifdef INCLUDE_PERSON_DETECT

#include "config.h"
#include "camera_manager.h"
#include "cz_text.h"
#include "image_utils.h"
#include "tracker.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_heap_caps.h"
#include <string.h>

// Override Edge Impulse's heap allocator. The default ei_malloc uses
// MALLOC_CAP_DEFAULT which resolves to internal DRAM — the tensor arena
// (~140 kB for this FOMO model) eats internal heap that async web server,
// TLS handshake and camera buffers all compete for. Steering it to PSRAM
// frees that much DRAM for the rest of the system. Fall back to internal
// if PSRAM allocation fails (keeps small allocations working at boot).
extern "C" void* ei_malloc(size_t size) {
    void* p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_DEFAULT);
    return p;
}
extern "C" void* ei_calloc(size_t nitems, size_t size) {
    size_t total = nitems * size;
    void* p = heap_caps_aligned_alloc(16, total, MALLOC_CAP_SPIRAM);
    if (!p) {
        p = heap_caps_calloc(nitems, size, MALLOC_CAP_DEFAULT);
    } else {
        memset(p, 0, total);
    }
    return p;
}
extern "C" void ei_free(void* ptr) { heap_caps_free(ptr); }

// Edge Impulse FOMO integration.
// Direct include — PlatformIO LDF needs to see the header reference to pull the
// library onto the link line. When lib/ei-person-fomo is a real EI Arduino export,
// EI_CLASSIFIER_* macros are defined and EI_FOMO_AVAILABLE is set below.
//
// The model is deliberately NOT in this repository (24 MB of generated code — see
// docs/fomo_setup.md), so a fresh clone hits the #error below rather than a bare
// "file not found". Failing loudly is intentional: a silently model-less build would
// flash a camera that never detects anyone.
#if defined(__has_include)
#  if !__has_include(<Person_detection_FOMO_inferencing.h>)
#    error "Edge Impulse FOMO model not found in lib/ei-person-fomo/. Export your model as an Arduino library (see docs/fomo_setup.md), or comment out -DINCLUDE_PERSON_DETECT in platformio.ini to build without person detection."
#  endif
#endif
#include <Person_detection_FOMO_inferencing.h>
#if defined(EI_CLASSIFIER_INPUT_WIDTH) && defined(EI_CLASSIFIER_INPUT_HEIGHT)
  #define EI_FOMO_AVAILABLE 1
#endif

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#endif

#ifdef INCLUDE_TELEGRAM
#include "telegram.h"
#endif

#ifdef INCLUDE_MOTION_DETECT
#include "motion_detect.h"
#endif

#ifdef INCLUDE_MQTT
#include "mqtt_handler.h"
#endif

#ifdef INCLUDE_EVENT_LOG
#include "event_log.h"
#endif

static const char* TAG = "PersonDet";

// Semaphore from motion detection cascade
static SemaphoreHandle_t motionSem = NULL;

// Buffers (PSRAM). Sized for the sensor's largest frame at 1/8 scale (QSXGA ->
// 320x240) so no frame_size can overflow them. The previous 256x160 sizing was
// derived from a hand-maintained frame_size table that never learned about
// QXGA/QSXGA, and selecting those wrote ~72 kB past the end of the buffer.
#define PD_DECODE_BUF_SZ  IMG_DECODE_MAX_SZ
#define PD_GRAY_BUF_SZ    (IMG_DECODE_MAX_W * IMG_DECODE_MAX_H)

// Inference input size. When an EI model is linked in, we honour the
// dimensions it was trained on; otherwise fall back to PD_INPUT_SIZE from config.
#ifdef EI_FOMO_AVAILABLE
  #define PD_MODEL_W  EI_CLASSIFIER_INPUT_WIDTH
  #define PD_MODEL_H  EI_CLASSIFIER_INPUT_HEIGHT
#else
  #define PD_MODEL_W  PD_INPUT_SIZE
  #define PD_MODEL_H  PD_INPUT_SIZE
#endif

static uint8_t* decodeBuffer = NULL;   // RGB565 from JPEG decode
static uint8_t* grayBuffer = NULL;     // Grayscale intermediate
static uint8_t* fomoInput = NULL;      // PD_MODEL_W x PD_MODEL_H grayscale for FOMO

// State
static volatile bool personDetected = false;
static volatile unsigned long lastPersonTime = 0;
static volatile uint32_t personEventCount = 0;
static PersonDetectResult lastResult = {};
static portMUX_TYPE pdMux = portMUX_INITIALIZER_UNLOCKED;

// Temporal filter
static int consecutiveDetections = 0;
static unsigned long lastUncertainPublishTime = 0;
// Timestamp of the last notification we actually sent (photo or text).
static volatile unsigned long lastPersonNotifyTime = 0;

PersonDecision classifyDecision(float confidence) {
    // Both thresholds come from config now; the upper one used to be the
    // compile-time PD_CONFIDENT_THRESHOLD_DEFAULT and could not be tuned.
    float confidentAt = appConfig.person_detect.confident_threshold;
    float uncertainAt = appConfig.person_detect.confidence_threshold;
    if (confidentAt < uncertainAt) confidentAt = uncertainAt;  // never invert the bands
    if (confidence >= confidentAt) return PersonDecision::CONFIDENT;
    if (confidence >= uncertainAt) return PersonDecision::UNCERTAIN;
    return PersonDecision::NONE;
}

const char* personDecisionToString(PersonDecision decision) {
    switch (decision) {
        case PersonDecision::CONFIDENT: return "CONFIDENT";
        case PersonDecision::UNCERTAIN: return "UNCERTAIN";
        case PersonDecision::NONE:
        default: return "NONE";
    }
}

// --- Image processing helpers ---

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

// Contrast-stretch the model input using the 1st/99th percentile of its histogram.
//
// FOMO was trained on well-exposed images. A night frame from this sensor occupies a
// narrow band of the range (say 20..70), so every feature the model looks for is
// compressed into a fraction of the input scale and scores hover just above the
// UNCERTAIN threshold. Stretching that band to 0..255 recovers the contrast the model
// expects, without touching the frame that gets saved or sent.
//
// Guard rails: if the band is already wide (>200) there is nothing to gain, and if it
// is extremely narrow (<20) the "signal" is sensor noise and stretching it would
// manufacture detail that is not there. Both cases are left alone.
#define PD_STRETCH_MIN_RANGE  20
#define PD_STRETCH_MAX_RANGE  200

static void stretchContrast(uint8_t* img, int pixels) {
    uint32_t hist[256] = {0};
    for (int i = 0; i < pixels; i++) hist[img[i]]++;

    const uint32_t tail = (uint32_t)(pixels * 0.01f);  // 1 % on each side
    uint32_t acc = 0;
    int lo = 0, hi = 255;
    for (int v = 0; v < 256; v++) { acc += hist[v]; if (acc > tail) { lo = v; break; } }
    acc = 0;
    for (int v = 255; v >= 0; v--) { acc += hist[v]; if (acc > tail) { hi = v; break; } }

    int range = hi - lo;
    if (range < PD_STRETCH_MIN_RANGE || range > PD_STRETCH_MAX_RANGE) return;

    // Precompute the mapping — 256 divisions instead of one per pixel.
    uint8_t lut[256];
    for (int v = 0; v < 256; v++) {
        int out = ((v - lo) * 255) / range;
        lut[v] = (uint8_t)(out < 0 ? 0 : (out > 255 ? 255 : out));
    }
    for (int i = 0; i < pixels; i++) img[i] = lut[img[i]];
}

bool personDetectInit() {
    decodeBuffer = (uint8_t*)ps_malloc(PD_DECODE_BUF_SZ);
    grayBuffer   = (uint8_t*)ps_malloc(PD_GRAY_BUF_SZ);
    fomoInput    = (uint8_t*)ps_malloc(PD_MODEL_W * PD_MODEL_H);

    if (!decodeBuffer || !grayBuffer || !fomoInput) {
        logCapture("[%s] Failed to allocate PSRAM buffers!\n", TAG);
        if (decodeBuffer) { free(decodeBuffer); decodeBuffer = NULL; }
        if (grayBuffer)   { free(grayBuffer);   grayBuffer   = NULL; }
        if (fomoInput)    { free(fomoInput);    fomoInput    = NULL; }
        return false;
    }

    trackerInit();

#ifdef EI_FOMO_AVAILABLE
    logCapture("[%s] Person detection initialized — EI FOMO model %dx%d, %d class(es)\n",
                  TAG, PD_MODEL_W, PD_MODEL_H, EI_CLASSIFIER_LABEL_COUNT);
#else
    logCapture("[%s] Person detection initialized — NO MODEL (placeholder, see lib/ei-person-fomo)\n",
                  TAG);
#endif
    return true;
}

void personDetectSetSemaphore(SemaphoreHandle_t sem) {
    motionSem = sem;
}

void personDetectTask(void* param) {
    logCapture("[%s] Person detection task started\n", TAG);
    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();
        if (!appConfig.person_detect.enabled || !decodeBuffer || !grayBuffer || !fomoInput) {
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

        // Get latest frame, with the dimensions it was captured at
        const uint8_t* buf = NULL;
        size_t len = 0;
        uint16_t srcW = 0, srcH = 0;
        int rh = ringBufferGetLatest(&buf, &len, &srcW, &srcH);
        if (rh < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Bound-check before decoding — jpg2rgb565 writes as much as the frame needs.
        int decW = 0, decH = 0;
        if (!imgDecodeFits(srcW, srcH, PD_DECODE_BUF_SZ, decW, decH)) {
            ringBufferRelease(rh);
            logCapture("[%s] Frame %ux%u would decode to %dx%d, over buffer — skipping\n",
                          TAG, srcW, srcH, decW, decH);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint32_t t0 = millis();

        // Step 1: JPEG -> RGB565 at 1/8 scale (decode serialized; tjpgd isn't reentrant)
        bool decoded = false;
        if (cameraDecodeLock()) {
            decoded = jpg2rgb565(buf, len, decodeBuffer, JPG_SCALE_8X);
            cameraDecodeUnlock();
        }
        ringBufferRelease(rh);

        if (!decoded) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Step 2: RGB565 -> Grayscale (decW/decH already validated above)
        imgRgb565ToGrayscale(decodeBuffer, grayBuffer, decW, decH);

        // Step 3: Bilinear resize to model input size
        bilinearResize(grayBuffer, decW, decH, fomoInput, PD_MODEL_W, PD_MODEL_H);

        // Step 3b: normalize contrast so low-light frames reach the model with a
        // usable dynamic range (see stretchContrast).
        stretchContrast(fomoInput, PD_MODEL_W * PD_MODEL_H);

        // Step 4: FOMO inference (only when the EI model is linked in)
        Detection dets[TRACKER_MAX_DETECTIONS];
        int numDets = 0;
        float topScore = 0.0f;

#ifdef EI_FOMO_AVAILABLE
        // EI callback pulls normalized grayscale from our fomoInput buffer.
        // FOMO with grayscale input expects a single-channel float per pixel in [0,1].
        signal_t signal;
        signal.total_length = PD_MODEL_W * PD_MODEL_H;
        signal.get_data = [](size_t offset, size_t length, float* out) -> int {
            for (size_t i = 0; i < length; i++) {
                out[i] = (float)fomoInput[offset + i] / 255.0f;
            }
            return 0;
        };

        ei_impulse_result_t result = {};
        EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
        if (err != EI_IMPULSE_OK) {
            logCapture("[%s] FOMO inference error: %d\n", TAG, err);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        for (size_t i = 0; i < result.bounding_boxes_count && numDets < TRACKER_MAX_DETECTIONS; i++) {
            ei_impulse_result_bounding_box_t& bb = result.bounding_boxes[i];
            if (bb.value > topScore) topScore = bb.value;
            if (bb.value < appConfig.person_detect.confidence_threshold) continue;
            dets[numDets].x = bb.x + bb.width / 2;
            dets[numDets].y = bb.y + bb.height / 2;
            dets[numDets].w = bb.width;
            dets[numDets].h = bb.height;
            dets[numDets].score = bb.value;
            dets[numDets].label = 0;
            numDets++;
        }
#endif  // EI_FOMO_AVAILABLE
        // Without the EI model, numDets stays 0 — tracker sees no detections,
        // the task still runs (buffers, motion cascade, cooldown) so it can
        // be smoke-tested end-to-end without the real inference backend.

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

        // Step 7: Classify confirmed tracker output into NONE / UNCERTAIN / CONFIDENT.
        PersonDecision decision = confirmed ? classifyDecision(topScore) : PersonDecision::NONE;
        if (decision == PersonDecision::CONFIDENT) {
            personDetected = true;
            lastPersonTime = millis();
            personEventCount++;

            logCapture("[%s] Person CONFIDENT! (%d tracked, score=%.2f, %dms, event #%lu)\n",
                          TAG, confirmedCount, topScore, inferenceMs, personEventCount);

            // EVT_PERSON existed in the enum but was never emitted, so /api/events
            // only ever showed motion.
            #ifdef INCLUDE_EVENT_LOG
            {
                char detail[EVENT_DETAIL_LEN];
                snprintf(detail, sizeof(detail), "n=%d score=%.2f %lums",
                         confirmedCount, topScore, (unsigned long)inferenceMs);
                logEvent(EVT_PERSON, detail);
            }
            #endif

            // Notify for newly confirmed tracks (prevents duplicate notifications)
            #ifdef INCLUDE_TELEGRAM
            if (appConfig.telegram.enabled && appConfig.telegram.notify_on_person && isWithinActiveHours()) {
                const Track* newTracks[4];
                int newCount = trackerGetNewlyConfirmed(newTracks, 4);

                if (newCount > 0) {
                    // Czech agrees the verb and the noun with the count — see cz_text.h.
                    char caption[96];
                    snprintf(caption, sizeof(caption), "%s %d %s (%.0f %%)",
                             czPlural(confirmedCount, "Detekována", "Detekovány", "Detekováno"),
                             confirmedCount,
                             czPlural(confirmedCount, "osoba", "osoby", "osob"),
                             topScore * 100.0f);

                    if (appConfig.telegram.photo_on_person) {
                        const uint8_t* tgBuf = NULL;
                        size_t tgLen = 0;
                        int th = ringBufferGetLatest(&tgBuf, &tgLen);
                        if (th >= 0) {
                            telegramSendPhoto(tgBuf, tgLen, caption);
                            ringBufferRelease(th);
                        }
                    } else {
                        telegramSendText(caption);
                    }

                    // Tell motion detection a person notification went out, so its
                    // fallback does not follow up with a duplicate photo.
                    lastPersonNotifyTime = millis();

                    for (int i = 0; i < newCount; i++) {
                        trackerMarkNotified(newTracks[i]->id);
                    }
                }
            }
            #endif
        } else if (decision == PersonDecision::UNCERTAIN) {
            personDetected = false;
            unsigned long now = millis();
            unsigned long cooldownMs = appConfig.person_detect.cooldown_sec * 1000UL;
            if (lastUncertainPublishTime == 0 || now - lastUncertainPublishTime >= cooldownMs) {
                lastUncertainPublishTime = now;
                logCapture("[%s] Person UNCERTAIN (tracks=%d, score=%.2f, %dms)\n",
                              TAG, confirmedCount, topScore, inferenceMs);
                #ifdef INCLUDE_MQTT
                mqttPublishPersonUncertain(topScore, confirmedCount);
                #endif
            }
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
        res.top_score = topScore;
        res.inference_ms = inferenceMs;
        res.track_count = totalTracks;
        res.decision = decision;

        portENTER_CRITICAL(&pdMux);
        lastResult = res;
        portEXIT_CRITICAL(&pdMux);

        // ~3-4 fps for person detection
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

unsigned long getLastPersonNotifyTime() { return lastPersonNotifyTime; }
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
