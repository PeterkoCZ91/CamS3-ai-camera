#include "camera_manager.h"
#include "ws_log.h"
#include "config.h"
#include "board_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

static const char* TAG = "CameraMgr";

// Ring buffer: 3 slots in PSRAM for zero-copy multi-consumer frame access
#define RING_BUF_SLOTS 3

struct FrameSlot {
    uint8_t* data;
    size_t   len;
    uint16_t width;          // sensor dimensions of THIS frame, not of the
    uint16_t height;         // current config — decoders size buffers from these
    uint32_t timestamp_ms;
    volatile int ref_count;  // -1 = writing, 0 = free, >0 = readers
};

static FrameSlot ringBuffer[RING_BUF_SLOTS];
static volatile int writeIndex = 0;
static volatile int latestIndex = -1;
static volatile uint32_t ringDroppedFrames = 0;

// ref_count protocol (all accesses through __atomic_*):
//   -1 = writer owns the slot   0 = free   >0 = number of active readers
// Claim a slot for writing. Fails if anyone is reading it or a writer already has it.
static inline bool ringSlotAcquireWrite(int idx) {
    int expected = 0;
    return __atomic_compare_exchange_n(&ringBuffer[idx].ref_count, &expected, -1,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// Take a read reference. Bounded retry: the CAS can lose a race against another
// reader, but the writer only holds a slot for the duration of one memcpy, so a
// handful of yields is always enough. Returns false if the writer owns the slot.
static inline bool ringSlotAcquireRead(int idx) {
    for (int attempt = 0; attempt < 16; attempt++) {
        int expected = __atomic_load_n(&ringBuffer[idx].ref_count, __ATOMIC_SEQ_CST);
        if (expected < 0) return false;
        if (__atomic_compare_exchange_n(&ringBuffer[idx].ref_count, &expected, expected + 1,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            return true;
        }
        taskYIELD();
    }
    return false;
}

// Drop a read reference, refusing to go below zero if a handle is released twice.
static inline void ringSlotReleaseRead(int idx) {
    for (;;) {
        int expected = __atomic_load_n(&ringBuffer[idx].ref_count, __ATOMIC_SEQ_CST);
        if (expected <= 0) return;
        if (__atomic_compare_exchange_n(&ringBuffer[idx].ref_count, &expected, expected - 1,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            return;
        }
    }
}
// tjpgd (the JPEG decoder behind jpg2rgb565 / fmt2rgb888) keeps decode
// state in static globals — concurrent calls from different tasks corrupt
// each other. Serialize all JPEG decodes through this mutex.
static SemaphoreHandle_t decodeMutex = NULL;

// Stats
static volatile uint32_t captureCount = 0;
static volatile uint32_t captureErrors = 0;
static volatile uint32_t lastCaptureMs = 0;
static volatile uint32_t lastPublishedMs = 0;      // last frame really copied into the ring
static volatile uint32_t oversizeDroppedFrames = 0;
static volatile float captureFps = 0.0f;
static uint32_t fpsCountStart = 0;
static uint32_t fpsFrameCount = 0;

// Stream client tracking
static volatile int streamClients = 0;
static volatile int detectionStreamClients = 0;
static SemaphoreHandle_t clientMutex = NULL;

// Capture task handle
static TaskHandle_t captureTaskHandle = NULL;
static volatile bool captureRunning = false;
// Set by the capture task itself right before it deletes itself, so teardown can
// tell "asked to stop" apart from "actually gone". Tearing the camera driver down
// while the task is still inside esp_camera_fb_get() corrupts the DMA state.
static volatile bool captureTaskExited = true;

// Reinit request flag (set by capture task, handled by main loop / healthWatchdog)
static volatile bool reinitRequested = false;
// Capture was running when a reinit started and has not been restarted yet. Survives
// failed attempts: stopCaptureTask() clears captureRunning, so re-reading that on the
// next attempt wrongly concluded capture had never been running.
static bool captureWantedAfterReinit = false;
// Deferred sensor-settings apply, consumed by the capture task between frames.
static volatile bool settingsApplyRequested = false;

// ---- Read-only live sensor register view (GET /api/sensor) -------------------
// Register reads go over SCCB, so they happen ONLY in the capture task, in the same
// idle window as the deferred settings apply. Readers get a seq-lock protected copy.
// The default list is the standard OV5640 map, a HYPOTHESIS: the sensor is described
// as "PY260 / OV5640-derivative" and the meaning of these registers on PY260 is
// UNVERIFIED. They are exposed as raw values keyed by address, nothing more.
static const uint16_t SENSOR_REGS[SENSOR_VIEW_REGS] = {
    0x3500, 0x3501, 0x3502, 0x3503, 0x350A, 0x350B,
    0x3A0F, 0x3A10, 0x3A11, 0x3A1B, 0x3A1E, 0x3A1F,
    0x5688, 0x5689, 0x568A, 0x568B, 0x568C, 0x568D, 0x568E, 0x568F
};
#define SENSOR_SAMPLE_EVERY 10
static SensorView sensorView;                       // written by capture task only
static volatile uint32_t sensorViewSeq = 0;         // odd while a write is in progress
static volatile bool sensorOneShotPending = false;  // set by HTTP, cleared by capture task
static volatile uint16_t sensorOneShotReg = 0;

static void sensorViewSample() {
    sensor_t* s = esp_camera_sensor_get();
    SensorView v = sensorView;  // keep previous one-shot result
    v.valid = false;
    if (s && s->get_reg) {
        v.valid = true;
        v.pid = s->id.PID; v.ver = s->id.VER;
        v.midh = s->id.MIDH; v.midl = s->id.MIDL;
        v.frame = captureCount;
        for (int i = 0; i < SENSOR_VIEW_REGS; i++) {
            int r = s->get_reg(s, SENSOR_REGS[i], 0xFF);
            v.regs[i] = r < 0 ? 0xFFFF : (uint16_t)(r & 0xFF);
        }
        if (sensorOneShotPending) {
            uint16_t reg = sensorOneShotReg;
            int r = s->get_reg(s, reg, 0xFF);
            v.custom_reg = reg;
            v.custom_val = r < 0 ? 0xFFFF : (uint16_t)(r & 0xFF);
            v.custom_frame = v.frame;
            v.custom_set = true;
        }
    }
    sensorOneShotPending = false;
    sensorViewSeq = sensorViewSeq + 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    sensorView = v;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    sensorViewSeq = sensorViewSeq + 1;
}

bool cameraSensorViewGet(SensorView* out) {
    for (int tries = 0; tries < 5; tries++) {
        uint32_t a = sensorViewSeq;
        if (a & 1) { delayMicroseconds(50); continue; }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        *out = sensorView;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (a == sensorViewSeq) return out->valid;
    }
    return false;
}

uint16_t cameraSensorViewRegAddr(int i) {
    return (i >= 0 && i < SENSOR_VIEW_REGS) ? SENSOR_REGS[i] : 0;
}

void cameraSensorViewRequestReg(uint16_t reg) {
    sensorOneShotReg = reg;
    sensorOneShotPending = true;
}

bool cameraSensorViewOneShotPending() { return sensorOneShotPending; }

// Max JPEG buffer size per slot (256KB should cover up to UXGA JPEG)
#define MAX_FRAME_SIZE (256 * 1024)

// Allocate the ring buffer once per boot and keep it for the process lifetime.
// The slots are plain PSRAM buffers with no camera-driver dependency, so a camera
// reinit does not need to (and must not) free them: readers hand out raw pointers
// into these slots, and freeing underneath a streaming client was a use-after-free.
// Keeping them also avoids re-fragmenting PSRAM with 3x256 kB churn on every reinit.
// Created at the top of cameraInit(), before anything can fail, so a failed camera
// init cannot leave NULL mutexes for the first stream client to take.
static bool ensureMutexes() {
    if (!clientMutex) clientMutex = xSemaphoreCreateMutex();
    if (!decodeMutex) decodeMutex = xSemaphoreCreateMutex();
    if (!clientMutex || !decodeMutex) {
        logCapture("[%s] Failed to create mutexes\n", TAG);
        return false;
    }
    return true;
}

static bool initRingBuffer() {
    bool firstInit = (ringBuffer[0].data == NULL);

    for (int i = 0; i < RING_BUF_SLOTS; i++) {
        if (!ringBuffer[i].data) {
            ringBuffer[i].data = (uint8_t*)ps_malloc(MAX_FRAME_SIZE);
            if (!ringBuffer[i].data) {
                logCapture("[%s] Failed to allocate ring buffer slot %d\n", TAG, i);
                return false;
            }
            ringBuffer[i].ref_count = 0;
        }
        // Drop stale frame metadata; leave ref_count alone so readers that are
        // mid-transfer across a reinit can still release their handles.
        ringBuffer[i].len = 0;
        ringBuffer[i].width = 0;
        ringBuffer[i].height = 0;
        ringBuffer[i].timestamp_ms = 0;
    }
    writeIndex = 0;
    latestIndex = -1;

    if (firstInit) {
        logCapture("[%s] Ring buffer: %d slots x %dKB in PSRAM\n",
                      TAG, RING_BUF_SLOTS, MAX_FRAME_SIZE / 1024);
    }
    return true;
}

bool cameraInit() {
    ensureMutexes();  // best effort; users NULL-guard (see clientLock)
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
        logCapture("[%s] PSRAM found: %dKB free\n", TAG, ESP.getFreePsram() / 1024);
    } else {
        config.frame_size   = FRAMESIZE_SVGA;
        config.jpeg_quality = 16;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
        logCapture("[%s] WARNING: No PSRAM, using limited config\n", TAG);
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        logCapture("[%s] Camera init failed: 0x%x\n", TAG, err);
        return false;
    }

    // Detect sensor
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        logCapture("[%s] Sensor PID: 0x%04X\n", TAG, s->id.PID);
    }

    // Apply saved settings
    applyConfigToCamera();

    // Initialize ring buffer
    if (!initRingBuffer()) {
        logCapture("[%s] Ring buffer init failed\n", TAG);
        return false;
    }

    logCapture("[%s] Camera initialized OK\n", TAG);
    return true;
}

bool cameraDeinit() {
    if (!stopCaptureTask()) {
        // The task is wedged somewhere in the driver. Deinitializing now would
        // pull the frame buffers out from under it; leave the camera as-is and let
        // the caller decide (healthWatchdog escalates to a reboot after retries).
        logCapture("[%s] Capture task did not stop — skipping deinit\n", TAG);
        return false;
    }

    // Invalidate published frames so no consumer picks up a stale slot across the
    // reinit. Buffers themselves stay allocated (see initRingBuffer). Readers that
    // already hold a handle keep working from their own copy of buf/len.
    __atomic_store_n(&latestIndex, -1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < RING_BUF_SLOTS; i++) ringBuffer[i].len = 0;

    esp_camera_deinit();
    logCapture("[%s] Camera deinitialized\n", TAG);
    return true;
}

bool cameraReinit() {
    logCapture("[%s] Reinitializing camera...\n", TAG);
    if (captureRunning) captureWantedAfterReinit = true;
    if (!cameraDeinit()) return false;
    delay(500);

    bool ok = cameraInit();
    if (ok && captureWantedAfterReinit) {
        startCaptureTask();
        if (captureRunning) captureWantedAfterReinit = false;  // cleared only once really restarted
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
    logCapture("[%s] Capture task started on core %d\n", TAG, xPortGetCoreID());
    captureTaskExited = false;
    fpsCountStart = millis();
    fpsFrameCount = 0;
    esp_task_wdt_add(NULL);

    while (captureRunning) {
        esp_task_wdt_reset();
        // Adaptive frame rate based on connected clients
        int clients = getTotalStreamClientCount();
        int targetDelay;
        if (clients > 0) {
            targetDelay = 1000 / max(1, appConfig.active_fps);
        } else {
            targetDelay = 1000 / max(1, appConfig.idle_fps);
        }

        uint32_t loopStart = millis();
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            captureErrors++;
            logCapture("[%s] Capture failed (errors: %lu)\n", TAG, captureErrors);
            vTaskDelay(pdMS_TO_TICKS(100));

            // Ask main loop to reinit (it will stop this task first)
            if (captureErrors > 0 && (captureErrors % 10) == 0) {
                logCapture("[%s] Too many errors, requesting reinit\n", TAG);
                reinitRequested = true;
            }
            continue;
        }

        // Publish the frame into the ring buffer. Lock-free: the writer claims a
        // slot with a CAS 0 -> -1 and copies into it without holding any mutex.
        // The previous version held ringMutex across the whole memcpy (up to 256 kB,
        // ~2 ms), and every reader — two MJPEG streams plus three detection tasks —
        // had to queue behind it just to look up the latest frame.
        if (fb->len > MAX_FRAME_SIZE) {
            // Oversized frame: nothing can consume it, but dropping it without a
            // trace made it look like the camera had simply gone quiet.
            ringDroppedFrames++;
            oversizeDroppedFrames++;
            if ((ringDroppedFrames % 50) == 1) {
                logCapture("[%s] Frame %u B exceeds slot size %u B, dropped (total %lu)\n",
                           TAG, (unsigned)fb->len, (unsigned)MAX_FRAME_SIZE,
                           (unsigned long)ringDroppedFrames);
            }
        } else {
            int slot = -1;
            for (int i = 0; i < RING_BUF_SLOTS; i++) {
                int idx = (writeIndex + i) % RING_BUF_SLOTS;
                if (ringSlotAcquireWrite(idx)) { slot = idx; break; }
            }

            if (slot >= 0) {
                uint32_t capturedAt = millis();
                memcpy(ringBuffer[slot].data, fb->buf, fb->len);
                ringBuffer[slot].len = fb->len;
                ringBuffer[slot].width = fb->width;
                ringBuffer[slot].height = fb->height;
                ringBuffer[slot].timestamp_ms = capturedAt;

                // Release the slot BEFORE publishing the index: a reader that sees
                // the new latestIndex must find ref_count >= 0, never -1.
                __atomic_store_n(&ringBuffer[slot].ref_count, 0, __ATOMIC_SEQ_CST);
                __atomic_store_n(&latestIndex, slot, __ATOMIC_SEQ_CST);
                writeIndex = (slot + 1) % RING_BUF_SLOTS;
                lastPublishedMs = capturedAt;
            } else {
                // All slots held by readers. Dropping the frame is correct, but it
                // used to happen silently — a stalled consumer looked like low FPS.
                ringDroppedFrames++;
                if ((ringDroppedFrames % 50) == 1) {
                    logCapture("[%s] Ring buffer full, frame dropped (total %lu)\n",
                               TAG, (unsigned long)ringDroppedFrames);
                }
            }
        }

        esp_camera_fb_return(fb);
        captureCount++;
        lastCaptureMs = millis();

        // SCCB is idle right here — the only point in the loop where writing sensor
        // registers cannot collide with an in-flight frame grab. Frame size is
        // deliberately excluded: it needs the DMA buffers rebuilt, which is what the
        // reinit request path does.
        if (settingsApplyRequested) {
            settingsApplyRequested = false;
            applyConfigToCamera(false);
        }

        // Same idle SCCB window: refresh the read-only register view.
        if (sensorOneShotPending || (captureCount % SENSOR_SAMPLE_EVERY) == 0) {
            sensorViewSample();
        }

        // FPS calculation (every 2 seconds)
        fpsFrameCount++;
        uint32_t elapsed = millis() - fpsCountStart;
        if (elapsed >= 2000) {
            captureFps = (float)fpsFrameCount * 1000.0f / (float)elapsed;
            fpsFrameCount = 0;
            fpsCountStart = millis();
        }

        // Frame rate limiting. Measure from the top of the iteration — subtracting
        // lastCaptureMs (just set to millis()) always yielded 0, so every frame paid
        // the full targetDelay on top of its capture time and the real rate came out
        // well under the configured FPS.
        int captureTime = (int)(millis() - loopStart);
        int waitTime = targetDelay - captureTime;
        if (waitTime > 0) {
            vTaskDelay(pdMS_TO_TICKS(waitTime));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));  // Yield
        }
    }

    logCapture("[%s] Capture task stopped\n", TAG);
    esp_task_wdt_delete(NULL);
    captureTaskExited = true;
    vTaskDelete(NULL);
}

void startCaptureTask() {
    if (captureRunning) return;
    captureRunning = true;
    captureTaskExited = false;
    if (xTaskCreatePinnedToCore(captureTask, "capture", CAPTURE_TASK_STACK,
                                NULL, CAPTURE_TASK_PRIO, &captureTaskHandle, 1) != pdPASS) {
        captureRunning = false;
        captureTaskExited = true;
        captureTaskHandle = NULL;
        logCapture("[%s] Failed to create capture task\n", TAG);
    }
}

// Returns true once the capture task has really left its loop and deleted itself.
// The old version only waited a flat 200 ms, which is shorter than a single
// esp_camera_fb_get() at UXGA — so a reinit could pull the driver out from under a
// task that was still mid-DMA.
bool stopCaptureTask() {
    captureRunning = false;
    if (!captureTaskHandle) return true;

    const int WAIT_STEP_MS = 20;
    const int WAIT_MAX_MS  = 3000;
    for (int waited = 0; waited < WAIT_MAX_MS && !captureTaskExited; waited += WAIT_STEP_MS) {
        delay(WAIT_STEP_MS);
    }

    if (!captureTaskExited) {
        logCapture("[%s] Capture task still running after %dms\n", TAG, WAIT_MAX_MS);
        return false;
    }
    captureTaskHandle = NULL;
    return true;
}

int ringBufferGetLatest(const uint8_t** buf, size_t* len,
                        uint16_t* width, uint16_t* height) {
    int idx = __atomic_load_n(&latestIndex, __ATOMIC_SEQ_CST);
    if (idx < 0 || idx >= RING_BUF_SLOTS) return -1;

    // Holding a read reference is what makes the slot's contents stable: the writer
    // can only claim slots whose ref_count is exactly 0.
    if (!ringSlotAcquireRead(idx)) return -1;

    if (ringBuffer[idx].len == 0) {
        ringSlotReleaseRead(idx);
        return -1;
    }

    *buf = ringBuffer[idx].data;
    *len = ringBuffer[idx].len;
    if (width)  *width  = ringBuffer[idx].width;
    if (height) *height = ringBuffer[idx].height;
    return idx;
}

uint32_t ringBufferGetTimestamp(int handle) {
    if (handle < 0 || handle >= RING_BUF_SLOTS) return 0;
    // Caller holds a read reference for this handle, so the field cannot change.
    return ringBuffer[handle].timestamp_ms;
}

void ringBufferRelease(int handle) {
    if (handle < 0 || handle >= RING_BUF_SLOTS) return;
    ringSlotReleaseRead(handle);
}

uint32_t getRingDroppedFrames() { return ringDroppedFrames; }

void cameraRequestReinit() { reinitRequested = true; }
bool cameraReinitRequested() { return reinitRequested; }
void cameraClearReinitRequest() { reinitRequested = false; }
void cameraRequestSettingsApply() { settingsApplyRequested = true; }

bool cameraDecodeLock(TickType_t timeout) {
    if (!decodeMutex) return true;  // not initialized yet → best-effort
    return xSemaphoreTake(decodeMutex, timeout) == pdTRUE;
}

void cameraDecodeUnlock() {
    if (decodeMutex) xSemaphoreGive(decodeMutex);
}

// NULL-safe: if the mutex was never created, count unlocked rather than assert.
static bool clientLock() {
    return !clientMutex || xSemaphoreTake(clientMutex, pdMS_TO_TICKS(100)) == pdTRUE;
}
static void clientUnlock() { if (clientMutex) xSemaphoreGive(clientMutex); }

void streamClientConnected() {
    if (clientLock()) {
        streamClients++;
        logCapture("[%s] Stream client connected (gui: %d, detection: %d)\n", TAG, streamClients, detectionStreamClients);
        clientUnlock();
    }
}

void streamClientDisconnected() {
    if (clientLock()) {
        if (streamClients > 0) streamClients--;
        logCapture("[%s] Stream client disconnected (gui: %d, detection: %d)\n", TAG, streamClients, detectionStreamClients);
        clientUnlock();
    }
}

void detectionStreamClientConnected() {
    if (clientLock()) {
        detectionStreamClients++;
        logCapture("[%s] Detection stream client connected (gui: %d, detection: %d)\n", TAG, streamClients, detectionStreamClients);
        clientUnlock();
    }
}

void detectionStreamClientDisconnected() {
    if (clientLock()) {
        if (detectionStreamClients > 0) detectionStreamClients--;
        logCapture("[%s] Detection stream client disconnected (gui: %d, detection: %d)\n", TAG, streamClients, detectionStreamClients);
        clientUnlock();
    }
}

int getStreamClientCount()  { return streamClients; }
int getDetectionStreamClientCount() { return detectionStreamClients; }
int getTotalStreamClientCount() { return streamClients + detectionStreamClients; }
uint32_t getCaptureCount()  { return captureCount; }
float getCaptureFps()       { return captureFps; }
uint32_t getLastCaptureMs() { return lastCaptureMs; }
uint32_t getCaptureErrors() { return captureErrors; }
uint32_t getLastPublishedMs() { return lastPublishedMs; }
uint32_t getOversizeDroppedFrames() { return oversizeDroppedFrames; }
