#include "motion_detect.h"
#include "ws_log.h"

#ifdef INCLUDE_MOTION_DETECT

#include "config.h"
#include "camera_manager.h"
#include "image_utils.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <string.h>
#include "esp_camera.h"
#include "img_converters.h"

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#include "sd_store.h"
#endif

#ifdef INCLUDE_TELEGRAM
#include "telegram.h"
#endif

#if defined(INCLUDE_PERSON_DETECT) && !defined(LITE_MODE)
#include "person_detection.h"
#endif

#ifdef INCLUDE_ZONES
#include "zone_manager.h"
#endif

#ifdef INCLUDE_EVENT_LOG
#include "event_log.h"
#endif

static const char* TAG = "Motion";

// PSRAM buffers
// decodeBuffer is sized for the MAX decoded JPEG output at JPG_SCALE_8X
// (tjpgd writes source_W/8 * source_H/8 * 2 bytes and does not honour any
// target-size hint). grayFrame stays at the analysis size — we subsample.
static uint8_t*  decodeBuffer = NULL;  // RGB565 up to MAX_W*MAX_H*2
static uint8_t*  grayFrame    = NULL;  // Grayscale 80x60 = 4800 bytes
static uint8_t*  blockGrid    = NULL;  // Block averages 20x15 = 300 bytes
static float*    bgModel      = NULL;  // EMA background model 20x15 floats
static uint8_t*  diffMask     = NULL;  // Current diff mask 20x15
static uint8_t*  prevDiffMask = NULL;  // Previous diff mask for temporal filter

// State
static volatile bool motionDetected = false;
static volatile unsigned long lastMotionTime = 0;
static volatile uint32_t motionEventCount = 0;
static int trainingCount = 0;
static int consecutiveMotionFrames = 0;
static bool bgResetRequested = false;
static int prevAvgBrightness = -1;
static MotionDebugInfo debugInfo = {};
static portMUX_TYPE debugMux = portMUX_INITIALIZER_UNLOCKED;

// Cascade semaphore — signals person detection task
static SemaphoreHandle_t cascadeSemaphore = NULL;

// Region of interest. roiMask[i] == 0 means "ignore this block".
// roiActiveBlocks is the denominator for the trigger percentage, so masking off
// half the frame does not halve the effective sensitivity in the rest of it.
static uint8_t roiMask[MOTION_GRID_SIZE];
static int     roiActiveBlocks = MOTION_GRID_SIZE;

void motionDetectClearRoiMask() {
    memset(roiMask, 1, sizeof(roiMask));
    roiActiveBlocks = MOTION_GRID_SIZE;
}

bool motionDetectSetRoiMask(const char* mask, int len) {
    if (!mask || len != MOTION_GRID_SIZE) {
        motionDetectClearRoiMask();
        return false;
    }
    int active = 0;
    for (int i = 0; i < MOTION_GRID_SIZE; i++) {
        roiMask[i] = (mask[i] == '0') ? 0 : 1;
        if (roiMask[i]) active++;
    }
    // An all-zero mask would make detection impossible; treat it as "no mask".
    if (active == 0) {
        motionDetectClearRoiMask();
        return false;
    }
    roiActiveBlocks = active;
    return true;
}

int motionDetectRoiActiveBlocks() { return roiActiveBlocks; }

#ifdef INCLUDE_TELEGRAM
// millis() when a motion notification was deferred pending the person verdict.
// 0 = nothing pending.
static unsigned long fallbackArmedAt = 0;

static void sendMotionNotification() {
    if (appConfig.telegram.photo_on_motion) {
        const uint8_t* tgBuf = NULL;
        size_t tgLen = 0;
        int th = ringBufferGetLatest(&tgBuf, &tgLen);
        if (th >= 0) {
            telegramSendPhoto(tgBuf, tgLen, "Pohyb detekován");
            ringBufferRelease(th);
        }
    } else {
        telegramSendText("Pohyb detekován");
    }
}

// Send the deferred motion notification once the fallback window has elapsed,
// unless person detection notified in the meantime.
static void serviceMotionFallback() {
    if (fallbackArmedAt == 0) return;
    if (millis() - fallbackArmedAt < MOTION_AI_FALLBACK_MS) return;

    unsigned long armedAt = fallbackArmedAt;
    fallbackArmedAt = 0;

    #if defined(INCLUDE_PERSON_DETECT) && !defined(LITE_MODE)
    unsigned long personNotified = getLastPersonNotifyTime();
    // Notified after we armed → the person path already told the user; stay quiet.
    if (personNotified != 0 && (long)(personNotified - armedAt) >= 0) {
        return;
    }
    #endif

    if (!appConfig.telegram.enabled || !appConfig.telegram.notify_on_motion) return;
    if (!isWithinActiveHours()) return;
    logCapture("[%s] Person detection did not confirm in %dms — motion fallback\n",
               TAG, MOTION_AI_FALLBACK_MS);
    sendMotionNotification();
}
#endif  // INCLUDE_TELEGRAM

// Box-filter (area-average) downsample from RGB565 at srcW x srcH to grayscale at
// dstW x dstH. Used to bring the JPEG decoder's actual output (e.g. 200x150 at UXGA)
// down to the motion analysis size (80x60). Averaging every source pixel that falls
// into a destination cell avoids the aliasing / noise amplification that nearest-
// neighbour caused on sensor-noise dominated scenes.
static void rgb565SubsampleToGrayscale(const uint8_t* rgb565, int srcW, int srcH,
                                       uint8_t* gray, int dstW, int dstH) {
    const uint16_t* pixels = (const uint16_t*)rgb565;
    for (int y = 0; y < dstH; y++) {
        int sy0 = (y * srcH) / dstH;
        int sy1 = ((y + 1) * srcH) / dstH;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dstW; x++) {
            int sx0 = (x * srcW) / dstW;
            int sx1 = ((x + 1) * srcW) / dstW;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t sum = 0;
            int count = 0;
            for (int iy = sy0; iy < sy1; iy++) {
                const uint16_t* row = pixels + iy * srcW;
                for (int ix = sx0; ix < sx1; ix++) {
                    sum += imgRgb565ToGray(row[ix]);
                    count++;
                }
            }
            gray[y * dstW + x] = (uint8_t)(sum / count);
        }
    }
}

// 4x4 block averaging: 80x60 grayscale -> 20x15 block grid
static void computeBlockGrid(const uint8_t* gray, uint8_t* blocks) {
    for (int by = 0; by < MOTION_GRID_H; by++) {
        for (int bx = 0; bx < MOTION_GRID_W; bx++) {
            uint32_t sum = 0;
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    int px = bx * 4 + dx;
                    int py = by * 4 + dy;
                    sum += gray[py * MOTION_DECODE_W + px];
                }
            }
            blocks[by * MOTION_GRID_W + bx] = (uint8_t)(sum / 16);
        }
    }
}

// Compute average brightness of block grid
static float computeAvgBrightness(const uint8_t* blocks) {
    uint32_t sum = 0;
    for (int i = 0; i < MOTION_GRID_SIZE; i++) {
        sum += blocks[i];
    }
    return (float)sum / MOTION_GRID_SIZE;
}

// Get current sensor AGC gain
static int getSensorGain() {
    sensor_t* s = esp_camera_sensor_get();
    if (s && s->status.agc_gain >= 0) {
        return s->status.agc_gain;
    }
    return 0;
}

// Spatial filter: count neighbors with motion for each block
static int spatialFilter(const uint8_t* mask, uint8_t* filtered, int minNeighbors) {
    int clustered = 0;
    for (int y = 0; y < MOTION_GRID_H; y++) {
        for (int x = 0; x < MOTION_GRID_W; x++) {
            int idx = y * MOTION_GRID_W + x;
            if (!mask[idx]) {
                filtered[idx] = 0;
                continue;
            }
            // Count active neighbors (8-connectivity)
            int neighbors = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < MOTION_GRID_W && ny >= 0 && ny < MOTION_GRID_H) {
                        if (mask[ny * MOTION_GRID_W + nx]) neighbors++;
                    }
                }
            }
            // Keep block only if it has enough neighboring motion blocks.
            filtered[idx] = (neighbors >= minNeighbors) ? 1 : 0;
            if (filtered[idx]) clustered++;
        }
    }
    return clustered;
}

void motionDetectInit() {
    // decodeBuffer must accommodate the JPEG decoder's actual output size
    // (source_W/8 * source_H/8 * 2). Allocate for the sensor's largest frame so
    // no frame_size can ever overflow it (QSXGA 2560x1920 / 8 = 320x240).
    decodeBuffer = (uint8_t*)ps_malloc(IMG_DECODE_MAX_SZ);
    grayFrame    = (uint8_t*)ps_malloc(MOTION_DECODE_W * MOTION_DECODE_H);
    blockGrid    = (uint8_t*)ps_malloc(MOTION_GRID_SIZE);
    bgModel      = (float*)ps_malloc(MOTION_GRID_SIZE * sizeof(float));
    diffMask     = (uint8_t*)ps_calloc(MOTION_GRID_SIZE, 1);
    prevDiffMask = (uint8_t*)ps_calloc(MOTION_GRID_SIZE, 1);

    if (!decodeBuffer || !grayFrame || !blockGrid || !bgModel || !diffMask || !prevDiffMask) {
        logCapture("[%s] Failed to allocate PSRAM buffers!\n", TAG);
        return;
    }

    // Initialize background model
    for (int i = 0; i < MOTION_GRID_SIZE; i++) {
        bgModel[i] = 128.0f;
    }
    trainingCount = 0;

    // Whole frame active unless a stored ROI mask is applied (see main.cpp).
    motionDetectClearRoiMask();

    logCapture("[%s] Advanced motion detection initialized (decode %dx%d, grid %dx%d)\n",
                  TAG, MOTION_DECODE_W, MOTION_DECODE_H, MOTION_GRID_W, MOTION_GRID_H);
}

void motionDetectSetSemaphore(SemaphoreHandle_t sem) {
    cascadeSemaphore = sem;
}

void motionDetectResetBackground() {
    bgResetRequested = true;
}

void motionDetectTask(void* param) {
    logCapture("[%s] Motion detection task started\n", TAG);
    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();

        #ifdef INCLUDE_TELEGRAM
        // Serviced first and unconditionally: several paths below `continue`, and a
        // deferred notification must not be stranded by one of them.
        serviceMotionFallback();
        #endif

        if (!appConfig.motion.enabled) {
            motionDetected = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Handle background reset request
        if (bgResetRequested) {
            for (int i = 0; i < MOTION_GRID_SIZE; i++) {
                bgModel[i] = 128.0f;
            }
            trainingCount = 0;
            consecutiveMotionFrames = 0;
            prevAvgBrightness = -1;
            bgResetRequested = false;
            logCapture("[%s] Background model reset\n", TAG);
        }

        // Get latest frame from ring buffer, together with the dimensions it was
        // actually captured at — the config can change between capture and decode.
        const uint8_t* buf = NULL;
        size_t len = 0;
        uint16_t srcW = 0, srcH = 0;
        int rh = ringBufferGetLatest(&buf, &len, &srcW, &srcH);
        if (rh < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Refuse to decode anything that would not fit — checked before the
        // decode, not after, because jpg2rgb565 has no output bound.
        int decW = 0, decH = 0;
        if (!imgDecodeFits(srcW, srcH, IMG_DECODE_MAX_SZ, decW, decH)) {
            ringBufferRelease(rh);
            logCapture("[%s] Frame %ux%u would decode to %dx%d, over buffer — skipping\n",
                          TAG, srcW, srcH, decW, decH);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint32_t t0 = millis();

        // Step 1: JPEG decode to RGB565 with 1/8 scaling.
        // tjpgd is not reentrant — serialize with other detection tasks.
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

        uint32_t decodeTime = millis() - t0;
        uint32_t t1 = millis();

        // Step 2: RGB565 (decW x decH) -> Grayscale (MOTION_DECODE_W x MOTION_DECODE_H)
        // with nearest-neighbour subsampling so the whole frame contributes, not
        // just the top-left corner.
        rgb565SubsampleToGrayscale(decodeBuffer, decW, decH,
                                   grayFrame, MOTION_DECODE_W, MOTION_DECODE_H);
        taskYIELD();

        // Step 3: 4x4 block averaging
        computeBlockGrid(grayFrame, blockGrid);
        taskYIELD();

        // Step 4: Compute brightness and sensor state
        float avgBrightness = computeAvgBrightness(blockGrid);
        int sensorGain = getSensorGain();
        bool isNight = (avgBrightness < appConfig.motion.brightness_min);

        // Step 5: Determine EMA alpha
        float alpha = isNight ? appConfig.motion.ema_alpha_night : appConfig.motion.ema_alpha_day;

        // Step 6: Update background model (EMA)
        bool isTraining = (trainingCount < appConfig.motion.training_frames);
        if (isTraining) {
            // Fast learning during training phase
            float trainAlpha = 0.5f;
            for (int i = 0; i < MOTION_GRID_SIZE; i++) {
                bgModel[i] = bgModel[i] * trainAlpha + (float)blockGrid[i] * (1.0f - trainAlpha);
            }
            trainingCount++;
            motionDetected = false;
            vTaskDelay(pdMS_TO_TICKS(200));

            // Update debug info
            portENTER_CRITICAL(&debugMux);
            debugInfo.avg_brightness = avgBrightness;
            debugInfo.training = true;
            debugInfo.decode_ms = decodeTime;
            portEXIT_CRITICAL(&debugMux);
            continue;
        }

        // Step 7: AGC-adaptive threshold + hysteresis.
        // Night suppression keeps detection active, but raises the per-block
        // threshold and later requires denser clusters instead of muting all
        // night-time motion.
        bool nightSuppressionActive = appConfig.motion.night_suppress && isNight;
        float adaptiveThreshold = (float)appConfig.motion.threshold +
                                  (float)sensorGain * appConfig.motion.agc_gain_factor;
        if (motionDetected) {
            adaptiveThreshold *= 0.70f;
        }
        if (nightSuppressionActive) {
            adaptiveThreshold *= MOTION_NIGHT_THRESHOLD_MULT;
        }

        // Sudden frame-wide brightness jumps are global illumination events,
        // not object motion. Reset the model to the current frame immediately.
        int brightnessDelta = (prevAvgBrightness >= 0)
            ? abs((int)avgBrightness - prevAvgBrightness)
            : 0;
        if (prevAvgBrightness >= 0 && brightnessDelta > MOTION_BRIGHTNESS_JUMP) {
            for (int i = 0; i < MOTION_GRID_SIZE; i++) {
                bgModel[i] = (float)blockGrid[i];
            }
            motionDetected = false;
            consecutiveMotionFrames = 0;
            int oldBrightness = prevAvgBrightness;
            prevAvgBrightness = (int)avgBrightness;

            uint32_t analysisTime = millis() - t1;
            portENTER_CRITICAL(&debugMux);
            debugInfo.changed_pct = 0.0f;
            debugInfo.avg_brightness = avgBrightness;
            debugInfo.sensor_gain = sensorGain;
            debugInfo.adaptive_threshold = adaptiveThreshold;
            debugInfo.active_blocks = 0;
            debugInfo.clustered_blocks = 0;
            debugInfo.consecutive_frames = 0;
            debugInfo.night_mode = isNight;
            debugInfo.training = false;
            debugInfo.decode_ms = decodeTime;
            debugInfo.analysis_ms = analysisTime;
            portEXIT_CRITICAL(&debugMux);

            logCapture("[%s] Brightness jump %d -> %.0f, background reset\n",
                          TAG, oldBrightness, avgBrightness);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        prevAvgBrightness = (int)avgBrightness;

        // Step 8: Compare against background model. Motion blocks still drift
        // slowly toward the current frame so gradual light changes do not build
        // permanent false positives.
        memcpy(prevDiffMask, diffMask, MOTION_GRID_SIZE);
        memset(diffMask, 0, MOTION_GRID_SIZE);

        int activeBlocks = 0;
        for (int i = 0; i < MOTION_GRID_SIZE; i++) {
            float diff = fabsf((float)blockGrid[i] - bgModel[i]);
            float relDiff = diff / (bgModel[i] + 1.0f);
            if (diff > adaptiveThreshold && relDiff > MOTION_REL_THRESHOLD) {
                // Masked-out blocks still track the background (so re-enabling the
                // ROI later does not fire a false positive) but never count as motion.
                if (roiMask[i]) {
                    diffMask[i] = 1;
                    activeBlocks++;
                }
                bgModel[i] = bgModel[i] * MOTION_EMA_ALPHA_MOTION +
                             (float)blockGrid[i] * (1.0f - MOTION_EMA_ALPHA_MOTION);
            } else {
                bgModel[i] = bgModel[i] * alpha + (float)blockGrid[i] * (1.0f - alpha);
            }
        }

        // Step 9: Calculate changed percentage, over the ROI area rather than the
        // full grid — otherwise masking off part of the frame silently raises the
        // effective trigger threshold for the part that is still watched.
        float roiArea = (float)(roiActiveBlocks > 0 ? roiActiveBlocks : MOTION_GRID_SIZE);
        float changedPct = (float)activeBlocks * 100.0f / roiArea;

        // Step 10: Upper reject - if too many blocks changed, it is likely a lighting change
        if (changedPct > (float)appConfig.motion.max_area_pct) {
            // Lighting change - force update entire background model
            for (int i = 0; i < MOTION_GRID_SIZE; i++) {
                bgModel[i] = bgModel[i] * 0.7f + (float)blockGrid[i] * 0.3f;
            }
            motionDetected = false;
            consecutiveMotionFrames = 0;

            uint32_t analysisTime = millis() - t1;
            portENTER_CRITICAL(&debugMux);
            debugInfo.changed_pct = changedPct;
            debugInfo.avg_brightness = avgBrightness;
            debugInfo.sensor_gain = sensorGain;
            debugInfo.adaptive_threshold = adaptiveThreshold;
            debugInfo.active_blocks = activeBlocks;
            debugInfo.clustered_blocks = 0;
            debugInfo.consecutive_frames = 0;
            debugInfo.night_mode = isNight;
            debugInfo.training = false;
            debugInfo.decode_ms = decodeTime;
            debugInfo.analysis_ms = analysisTime;
            portEXIT_CRITICAL(&debugMux);

            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Step 12: Spatial filter - reject isolated blocks
        int clusteredBlocks = activeBlocks;
        if (appConfig.motion.spatial_filter && activeBlocks > 0) {
            uint8_t filteredMask[MOTION_GRID_SIZE];
            clusteredBlocks = spatialFilter(diffMask, filteredMask, nightSuppressionActive ? 2 : 1);
            memcpy(diffMask, filteredMask, MOTION_GRID_SIZE);
        }

        float clusteredPct = (float)clusteredBlocks * 100.0f / roiArea;

        // Step 13: Area gate. Night suppression raises the required area as well as
        // the per-block threshold — raising only the threshold left noise-driven
        // single-block clusters able to trigger.
        float requiredPct = (float)appConfig.motion.min_area_pct;
        if (nightSuppressionActive) requiredPct *= MOTION_NIGHT_AREA_MULT;
        bool motionConfirmed = (clusteredPct >= requiredPct);

        // Temporal filter: 2+ consecutive frames whose motion regions overlap.
        // The overlap was already being computed here and then thrown away, which
        // let two unrelated single-frame noise bursts pass as "consecutive motion".
        if (appConfig.motion.temporal_filter) {
            if (motionConfirmed) {
                int overlapCount = 0;
                for (int i = 0; i < MOTION_GRID_SIZE; i++) {
                    if (diffMask[i] && prevDiffMask[i]) overlapCount++;
                }
                consecutiveMotionFrames++;
                motionConfirmed = (consecutiveMotionFrames >= 2) && (overlapCount > 0);
            } else {
                consecutiveMotionFrames = 0;
            }
        }

        // Step 14: Trigger motion event.
        // Event semantics: one event = one episode of motion bounded by quiet
        // periods. While motion continues, we keep motionDetected=true and
        // bump lastMotionTime but don't re-fire the event / notification /
        // cascade — that's what was causing the "10s re-spam" pattern.
        // A new event fires when:
        //   (a) we were idle (motionDetected=false), OR
        //   (b) the configured cooldown has passed since the last trigger,
        //       allowing intentional periodic re-notifications for long events.
        if (motionConfirmed) {
            unsigned long now = millis();
            unsigned long cooldownMs = appConfig.motion.cooldown_sec * 1000UL;
            bool risingEdge = !motionDetected;
            bool cooldownExpired = motionDetected && (now - lastMotionTime > cooldownMs);

            motionDetected = true;
            lastMotionTime = now;

            if (risingEdge || cooldownExpired) {
                motionEventCount++;

                // Zone hit detection — fills comma-separated zone names if any match
                #ifdef INCLUDE_ZONES
                char zoneHits[64] = "";
                getActiveZones(diffMask, MOTION_GRID_W, MOTION_GRID_H, zoneHits, sizeof(zoneHits));
                #endif

                #ifdef INCLUDE_EVENT_LOG
                {
                    char evDetail[EVENT_DETAIL_LEN];
                    #ifdef INCLUDE_ZONES
                    if (zoneHits[0])
                        snprintf(evDetail, sizeof(evDetail), "pct=%.1f zones=%s", clusteredPct, zoneHits);
                    else
                    #endif
                        snprintf(evDetail, sizeof(evDetail), "pct=%.1f", clusteredPct);
                    logEvent(EVT_MOTION, evDetail);
                }
                #endif

                logCapture("[%s] Motion %s! (%.1f%% clustered, gain=%d, thresh=%.1f, event #%lu%s%s)\n",
                              TAG,
                              risingEdge ? "detected" : "ongoing (cooldown re-fire)",
                              clusteredPct, sensorGain, adaptiveThreshold, motionEventCount
                              #ifdef INCLUDE_ZONES
                              , zoneHits[0] ? " zones=" : "", zoneHits[0] ? zoneHits : ""
                              #else
                              , "", ""
                              #endif
                              );

                // Signal person detection task via cascade semaphore
                if (cascadeSemaphore) {
                    xSemaphoreGive(cascadeSemaphore);
                }

                #ifdef INCLUDE_SD_CARD
                // sdStoreWriteJpeg handles rotation and the failure breaker; the old
                // inline SD.open() silently did nothing once the card was full.
                if (appConfig.motion.save_to_sd && sdStoreAvailable()) {
                    // Grab a fresh frame for saving (full resolution)
                    const uint8_t* saveBuf = NULL;
                    size_t saveLen = 0;
                    int sh = ringBufferGetLatest(&saveBuf, &saveLen);
                    if (sh >= 0) {
                        sdStoreWriteJpeg("/captures", "motion_", saveBuf, saveLen);
                        ringBufferRelease(sh);
                    }
                }
                #endif

                #ifdef INCLUDE_TELEGRAM
                if (appConfig.telegram.enabled && appConfig.telegram.notify_on_motion && isWithinActiveHours()) {
                    #if defined(INCLUDE_PERSON_DETECT) && !defined(LITE_MODE)
                    if (appConfig.person_detect.enabled) {
                        // Person detection is about to look at this event — arm the
                        // fallback instead of notifying now (see MOTION_AI_FALLBACK_MS).
                        fallbackArmedAt = now;
                    } else {
                        sendMotionNotification();
                    }
                    #else
                    sendMotionNotification();
                    #endif
                }
                #endif
            }
        } else {
            // Clear motion flag after timeout
            if (motionDetected && (millis() - lastMotionTime > 5000)) {
                motionDetected = false;
            }
        }

        uint32_t analysisTime = millis() - t1;

        // Update debug info (thread-safe)
        portENTER_CRITICAL(&debugMux);
        debugInfo.changed_pct = changedPct;
        debugInfo.avg_brightness = avgBrightness;
        debugInfo.sensor_gain = sensorGain;
        debugInfo.adaptive_threshold = adaptiveThreshold;
        debugInfo.active_blocks = activeBlocks;
        debugInfo.clustered_blocks = clusteredBlocks;
        debugInfo.consecutive_frames = consecutiveMotionFrames;
        debugInfo.night_mode = isNight;
        debugInfo.training = false;
        debugInfo.decode_ms = decodeTime;
        debugInfo.analysis_ms = analysisTime;
        portEXIT_CRITICAL(&debugMux);

        // Sample every 200ms (5 fps motion check)
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

bool isMotionDetected() { return motionDetected; }
unsigned long getLastMotionTime() { return lastMotionTime; }
uint32_t getMotionEventCount() { return motionEventCount; }

MotionDebugInfo getMotionDebugInfo() {
    MotionDebugInfo info;
    portENTER_CRITICAL(&debugMux);
    info = debugInfo;
    portEXIT_CRITICAL(&debugMux);
    return info;
}

#endif // INCLUDE_MOTION_DETECT
