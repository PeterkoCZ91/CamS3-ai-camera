#include "camera_manager.h"
#include "config.h"
#include "board_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

static const char* TAG = "CameraMgr";

// Ring buffer: 3 slots in PSRAM for zero-copy multi-consumer frame access
#define RING_BUF_SLOTS 3

struct FrameSlot {
    uint8_t* data;
    size_t   len;
    volatile int ref_count;  // -1 = writing, 0 = free, >0 = readers
};

static FrameSlot ringBuffer[RING_BUF_SLOTS];
static volatile int writeIndex = 0;
static volatile int latestIndex = -1;
static SemaphoreHandle_t ringMutex = NULL;

// Stats
static volatile uint32_t captureCount = 0;
static volatile uint32_t captureErrors = 0;
static volatile uint32_t lastCaptureMs = 0;
static volatile float captureFps = 0.0f;
static uint32_t fpsCountStart = 0;
static uint32_t fpsFrameCount = 0;

// Stream client tracking
static volatile int streamClients = 0;
static SemaphoreHandle_t clientMutex = NULL;

// Capture task handle
static TaskHandle_t captureTaskHandle = NULL;
static volatile bool captureRunning = false;

// Max JPEG buffer size per slot (256KB should cover up to UXGA JPEG)
#define MAX_FRAME_SIZE (256 * 1024)

static bool initRingBuffer() {
    ringMutex = xSemaphoreCreateMutex();
    clientMutex = xSemaphoreCreateMutex();

    for (int i = 0; i < RING_BUF_SLOTS; i++) {
        ringBuffer[i].data = (uint8_t*)ps_malloc(MAX_FRAME_SIZE);
        if (!ringBuffer[i].data) {
            Serial.printf("[%s] Failed to allocate ring buffer slot %d\n", TAG, i);
            return false;
        }
        ringBuffer[i].len = 0;
        ringBuffer[i].ref_count = 0;
    }

    Serial.printf("[%s] Ring buffer: %d slots x %dKB in PSRAM\n",
                  TAG, RING_BUF_SLOTS, MAX_FRAME_SIZE / 1024);
    return true;
}

bool cameraInit() {
    camera_config_t config;
    memset(&config, 0, sizeof(config));

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = XCLK_FREQ;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    // Use PSRAM for frame buffers
    if (psramFound()) {
        config.frame_size   = (framesize_t)appConfig.camera.frame_size;
        config.jpeg_quality = appConfig.camera.jpeg_quality;
        config.fb_count     = FB_COUNT;
        config.fb_location  = CAMERA_FB_IN_PSRAM;
        Serial.printf("[%s] PSRAM found: %dKB free\n", TAG, ESP.getFreePsram() / 1024);
    } else {
        config.frame_size   = FRAMESIZE_SVGA;
        config.jpeg_quality = 16;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
        Serial.printf("[%s] WARNING: No PSRAM, using limited config\n", TAG);
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[%s] Camera init failed: 0x%x\n", TAG, err);
        return false;
    }

    // Detect sensor
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        Serial.printf("[%s] Sensor PID: 0x%04X\n", TAG, s->id.PID);
    }

    // Apply saved settings
    applyConfigToCamera();

    // Initialize ring buffer
    if (!initRingBuffer()) {
        Serial.printf("[%s] Ring buffer init failed\n", TAG);
        return false;
    }

    Serial.printf("[%s] Camera initialized OK\n", TAG);
    return true;
}

void cameraDeinit() {
    stopCaptureTask();
    esp_camera_deinit();

    for (int i = 0; i < RING_BUF_SLOTS; i++) {
        if (ringBuffer[i].data) {
            free(ringBuffer[i].data);
            ringBuffer[i].data = NULL;
        }
    }
    Serial.printf("[%s] Camera deinitialized\n", TAG);
}

bool cameraReinit() {
    Serial.printf("[%s] Reinitializing camera...\n", TAG);
    bool wasRunning = captureRunning;
    cameraDeinit();
    delay(500);

    bool ok = cameraInit();
    if (ok && wasRunning) {
        startCaptureTask();
    }
    return ok;
}

camera_fb_t* captureFrame() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        captureErrors++;
        return NULL;
    }
    captureCount++;
    lastCaptureMs = millis();
    return fb;
}

void releaseFrame(camera_fb_t* fb) {
    if (fb) esp_camera_fb_return(fb);
}

// Capture task: continuously captures frames into ring buffer
static void captureTask(void* param) {
    Serial.printf("[%s] Capture task started on core %d\n", TAG, xPortGetCoreID());
    fpsCountStart = millis();
    fpsFrameCount = 0;

    while (captureRunning) {
        // Adaptive frame rate based on connected clients
        int clients = getStreamClientCount();
        int targetDelay;
        if (clients > 0) {
            targetDelay = 1000 / max(1, appConfig.active_fps);
        } else {
            targetDelay = 1000 / max(1, appConfig.idle_fps);
        }

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            captureErrors++;
            Serial.printf("[%s] Capture failed (errors: %lu)\n", TAG, captureErrors);
            vTaskDelay(pdMS_TO_TICKS(100));

            // Auto-reinit after 10 consecutive errors
            if (captureErrors > 0 && (captureErrors % 10) == 0) {
                Serial.printf("[%s] Too many errors, attempting reinit\n", TAG);
                esp_camera_deinit();
                delay(1000);
                // Re-init will be done in main loop watchdog
            }
            continue;
        }

        // Copy frame to ring buffer
        if (fb->len <= MAX_FRAME_SIZE && xSemaphoreTake(ringMutex, pdMS_TO_TICKS(50))) {
            // Find a free slot (not being read)
            int slot = -1;
            for (int i = 0; i < RING_BUF_SLOTS; i++) {
                int idx = (writeIndex + i) % RING_BUF_SLOTS;
                if (ringBuffer[idx].ref_count == 0) {
                    slot = idx;
                    break;
                }
            }

            if (slot >= 0) {
                ringBuffer[slot].ref_count = -1;  // Mark as writing
                memcpy(ringBuffer[slot].data, fb->buf, fb->len);
                ringBuffer[slot].len = fb->len;
                ringBuffer[slot].ref_count = 0;
                latestIndex = slot;
                writeIndex = (slot + 1) % RING_BUF_SLOTS;
            }

            xSemaphoreGive(ringMutex);
        }

        esp_camera_fb_return(fb);
        captureCount++;
        lastCaptureMs = millis();

        // FPS calculation (every 2 seconds)
        fpsFrameCount++;
        uint32_t elapsed = millis() - fpsCountStart;
        if (elapsed >= 2000) {
            captureFps = (float)fpsFrameCount * 1000.0f / (float)elapsed;
            fpsFrameCount = 0;
            fpsCountStart = millis();
        }

        // Frame rate limiting
        int captureTime = millis() - lastCaptureMs;
        int waitTime = targetDelay - captureTime;
        if (waitTime > 0) {
            vTaskDelay(pdMS_TO_TICKS(waitTime));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));  // Yield
        }
    }

    Serial.printf("[%s] Capture task stopped\n", TAG);
    vTaskDelete(NULL);
}

void startCaptureTask() {
    if (captureRunning) return;
    captureRunning = true;
    xTaskCreatePinnedToCore(captureTask, "capture", CAPTURE_TASK_STACK,
                            NULL, CAPTURE_TASK_PRIO, &captureTaskHandle, 1);
}

void stopCaptureTask() {
    captureRunning = false;
    if (captureTaskHandle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        captureTaskHandle = NULL;
    }
}

bool ringBufferGetLatest(const uint8_t** buf, size_t* len) {
    if (latestIndex < 0) return false;

    if (xSemaphoreTake(ringMutex, pdMS_TO_TICKS(50))) {
        int idx = latestIndex;
        if (idx >= 0 && ringBuffer[idx].ref_count >= 0 && ringBuffer[idx].len > 0) {
            ringBuffer[idx].ref_count++;
            *buf = ringBuffer[idx].data;
            *len = ringBuffer[idx].len;
            xSemaphoreGive(ringMutex);
            return true;
        }
        xSemaphoreGive(ringMutex);
    }
    return false;
}

void ringBufferRelease() {
    if (xSemaphoreTake(ringMutex, pdMS_TO_TICKS(50))) {
        for (int i = 0; i < RING_BUF_SLOTS; i++) {
            if (ringBuffer[i].ref_count > 0) {
                ringBuffer[i].ref_count--;
            }
        }
        xSemaphoreGive(ringMutex);
    }
}

void streamClientConnected() {
    if (xSemaphoreTake(clientMutex, pdMS_TO_TICKS(100))) {
        streamClients++;
        Serial.printf("[%s] Stream client connected (total: %d)\n", TAG, streamClients);
        xSemaphoreGive(clientMutex);
    }
}

void streamClientDisconnected() {
    if (xSemaphoreTake(clientMutex, pdMS_TO_TICKS(100))) {
        if (streamClients > 0) streamClients--;
        Serial.printf("[%s] Stream client disconnected (total: %d)\n", TAG, streamClients);
        xSemaphoreGive(clientMutex);
    }
}

int getStreamClientCount()  { return streamClients; }
uint32_t getCaptureCount()  { return captureCount; }
float getCaptureFps()       { return captureFps; }
uint32_t getLastCaptureMs() { return lastCaptureMs; }
uint32_t getCaptureErrors() { return captureErrors; }
