#include "motion_detect.h"

#ifdef INCLUDE_MOTION_DETECT

#include "config.h"
#include "camera_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_camera.h"
#include "img_converters.h"

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#endif

#ifdef INCLUDE_TELEGRAM
#include "telegram.h"
#endif

static const char* TAG = "Motion";

// PSRAM buffers
static uint8_t*  decodeBuffer = NULL;  // RGB565 80x60 = 9600 bytes
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
static MotionDebugInfo debugInfo = {};
static portMUX_TYPE debugMux = portMUX_INITIALIZER_UNLOCKED;

// Cascade semaphore — signals person detection task
static SemaphoreHandle_t cascadeSemaphore = NULL;

// Convert RGB565 pixel to grayscale using BT.601
static inline uint8_t rgb565ToGray(uint16_t pixel) {
    // RGB565: RRRRR GGGGGG BBBBB (big-endian from decoder)
    uint8_t r = (pixel >> 11) & 0x1F;
    uint8_t g = (pixel >> 5) & 0x3F;
    uint8_t b = pixel & 0x1F;
    // Scale to 8-bit and apply BT.601 weights
    // Y = 0.299R + 0.587G + 0.114B (fixed point: 77R + 150G + 29B) >> 8
    return (uint8_t)((77 * (r << 3) + 150 * (g << 2) + 29 * (b << 3)) >> 8);
}

// Convert full RGB565 frame to grayscale
static void rgb565ToGrayscale(const uint8_t* rgb565, uint8_t* gray, int w, int h) {
    const uint16_t* pixels = (const uint16_t*)rgb565;
    for (int i = 0; i < w * h; i++) {
        gray[i] = rgb565ToGray(pixels[i]);
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
static int spatialFilter(const uint8_t* mask, uint8_t* filtered) {
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
            // Keep block only if it has at least 1 neighbor
            filtered[idx] = (neighbors >= 1) ? 1 : 0;
            if (filtered[idx]) clustered++;
        }
    }
    return clustered;
}

void motionDetectInit() {
    decodeBuffer = (uint8_t*)ps_malloc(MOTION_DECODE_W * MOTION_DECODE_H * 2);  // RGB565
    grayFrame    = (uint8_t*)ps_malloc(MOTION_DECODE_W * MOTION_DECODE_H);
    blockGrid    = (uint8_t*)ps_malloc(MOTION_GRID_SIZE);
    bgModel      = (float*)ps_malloc(MOTION_GRID_SIZE * sizeof(float));
    diffMask     = (uint8_t*)ps_calloc(MOTION_GRID_SIZE, 1);
    prevDiffMask = (uint8_t*)ps_calloc(MOTION_GRID_SIZE, 1);

    if (!decodeBuffer || !grayFrame || !blockGrid || !bgModel || !diffMask || !prevDiffMask) {
        Serial.printf("[%s] Failed to allocate PSRAM buffers!\n", TAG);
        return;
    }

    // Initialize background model
    for (int i = 0; i < MOTION_GRID_SIZE; i++) {
        bgModel[i] = 128.0f;
    }
    trainingCount = 0;

    Serial.printf("[%s] Advanced motion detection initialized (decode %dx%d, grid %dx%d)\n",
                  TAG, MOTION_DECODE_W, MOTION_DECODE_H, MOTION_GRID_W, MOTION_GRID_H);
}

void motionDetectSetSemaphore(SemaphoreHandle_t sem) {
    cascadeSemaphore = sem;
}

void motionDetectResetBackground() {
    bgResetRequested = true;
}

void motionDetectTask(void* param) {
    Serial.printf("[%s] Motion detection task started\n", TAG);

    while (true) {
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
            bgResetRequested = false;
            Serial.printf("[%s] Background model reset\n", TAG);
        }

        // Get latest frame from ring buffer
        const uint8_t* buf = NULL;
        size_t len = 0;
        if (!ringBufferGetLatest(&buf, &len)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t t0 = millis();

        // Step 1: JPEG decode to RGB565 with 1/8 scaling
        // Source: e.g. 1600x1200 -> 200x150 (JPG_SCALE_8X gives approx 1/8)
        // We need 80x60, the decoder gives the closest supported size.
        // jpg2rgb565 decodes into the buffer; actual output size depends on JPEG.
        bool decoded = jpg2rgb565(buf, len, decodeBuffer, JPG_SCALE_8X);
        ringBufferRelease();

        if (!decoded) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t decodeTime = millis() - t0;
        uint32_t t1 = millis();

        // Step 2: RGB565 -> Grayscale
        rgb565ToGrayscale(decodeBuffer, grayFrame, MOTION_DECODE_W, MOTION_DECODE_H);

        // Step 3: 4x4 block averaging
        computeBlockGrid(grayFrame, blockGrid);

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

        // Step 7: AGC-adaptive threshold
        float adaptiveThreshold = (float)appConfig.motion.threshold +
                                  (float)sensorGain * appConfig.motion.agc_gain_factor;

        // Step 8: Compare against background model
        // Save previous diff mask for temporal filtering
        memcpy(prevDiffMask, diffMask, MOTION_GRID_SIZE);

        int activeBlocks = 0;
        for (int i = 0; i < MOTION_GRID_SIZE; i++) {
            float diff = fabs((float)blockGrid[i] - bgModel[i]);
            if (diff > adaptiveThreshold) {
                diffMask[i] = 1;
                activeBlocks++;
            } else {
                diffMask[i] = 0;
                // Update background model only for stable blocks
                bgModel[i] = bgModel[i] * alpha + (float)blockGrid[i] * (1.0f - alpha);
            }
        }

        // Step 9: Calculate changed percentage
        float changedPct = (float)activeBlocks * 100.0f / (float)MOTION_GRID_SIZE;

        // Step 10: Upper reject - if too many blocks changed, it's likely a lighting change
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

        // Step 11: Night suppression
        if (appConfig.motion.night_suppress && isNight) {
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
            debugInfo.night_mode = true;
            debugInfo.training = false;
            debugInfo.decode_ms = decodeTime;
            debugInfo.analysis_ms = analysisTime;
            portEXIT_CRITICAL(&debugMux);

            // Still update background model slowly
            for (int i = 0; i < MOTION_GRID_SIZE; i++) {
                bgModel[i] = bgModel[i] * alpha + (float)blockGrid[i] * (1.0f - alpha);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Step 12: Spatial filter - reject isolated blocks
        int clusteredBlocks = activeBlocks;
        if (appConfig.motion.spatial_filter && activeBlocks > 0) {
            uint8_t filteredMask[MOTION_GRID_SIZE];
            clusteredBlocks = spatialFilter(diffMask, filteredMask);
            memcpy(diffMask, filteredMask, MOTION_GRID_SIZE);
        }

        float clusteredPct = (float)clusteredBlocks * 100.0f / (float)MOTION_GRID_SIZE;

        // Step 13: Temporal filter - require 2+ consecutive frames
        bool motionConfirmed = (clusteredPct >= (float)appConfig.motion.min_area_pct);

        if (appConfig.motion.temporal_filter) {
            if (motionConfirmed) {
                // Check if previous frame also had motion in similar areas
                int overlapCount = 0;
                for (int i = 0; i < MOTION_GRID_SIZE; i++) {
                    if (diffMask[i] && prevDiffMask[i]) overlapCount++;
                }
                consecutiveMotionFrames++;
                // Require at least 2 consecutive frames
                motionConfirmed = (consecutiveMotionFrames >= 2);
            } else {
                consecutiveMotionFrames = 0;
            }
        }

        // Step 14: Trigger motion event
        if (motionConfirmed) {
            unsigned long now = millis();
            unsigned long cooldownMs = appConfig.motion.cooldown_sec * 1000UL;

            if (!motionDetected || (now - lastMotionTime > cooldownMs)) {
                motionDetected = true;
                lastMotionTime = now;
                motionEventCount++;
                Serial.printf("[%s] Motion detected! (%.1f%% clustered, gain=%d, thresh=%.1f, event #%lu)\n",
                              TAG, clusteredPct, sensorGain, adaptiveThreshold, motionEventCount);

                // Signal person detection task via cascade semaphore
                if (cascadeSemaphore) {
                    xSemaphoreGive(cascadeSemaphore);
                }

                #ifdef INCLUDE_SD_CARD
                if (appConfig.motion.save_to_sd && SD.cardType() != CARD_NONE) {
                    // Grab a fresh frame for saving (full resolution)
                    const uint8_t* saveBuf = NULL;
                    size_t saveLen = 0;
                    if (ringBufferGetLatest(&saveBuf, &saveLen)) {
                        char filename[64];
                        snprintf(filename, sizeof(filename), "/captures/motion_%lu.jpg", now);
                        File f = SD.open(filename, FILE_WRITE);
                        if (f) {
                            f.write(saveBuf, saveLen);
                            f.close();
                            Serial.printf("[%s] Saved to %s\n", TAG, filename);
                        }
                        ringBufferRelease();
                    }
                }
                #endif

                #ifdef INCLUDE_TELEGRAM
                if (appConfig.telegram.enabled && appConfig.telegram.notify_on_motion && isWithinActiveHours()) {
                    if (appConfig.telegram.photo_on_motion) {
                        const uint8_t* tgBuf = NULL;
                        size_t tgLen = 0;
                        if (ringBufferGetLatest(&tgBuf, &tgLen)) {
                            telegramSendPhoto(tgBuf, tgLen, "Pohyb detekovan");
                            ringBufferRelease();
                        }
                    } else {
                        telegramSendText("Pohyb detekovan");
                    }
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
