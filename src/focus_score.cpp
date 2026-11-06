#include "focus_score.h"
#include "ws_log.h"
#include "config.h"
#include "camera_manager.h"
#include "image_utils.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include "esp_camera.h"
#include "img_converters.h"

static const char* TAG = "Focus";

static const uint32_t FOCUS_INTERVAL_MS = 500;    // cheap: at most 2 decodes/s
static const uint32_t FOCUS_SESSION_MS  = 10000;  // stop computing after this idle time

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t lastTouch = 0;   // millis() of last request, 0 = never
static volatile bool     taskStarted = false;
static bool     resetPending = false;
static float    curScore = 0, curPeak = 0;
static uint32_t scoreAt = 0;
static bool     haveScore = false;

// The motion task's buffer is only 80x60 after subsampling: the centre crop would be
// ~40x30 px, too coarse to see focus change. So we decode our own 1/8 frame (up to
// 320x240) only while someone is polling, and free the buffer when they stop.
static uint8_t* decBuf = NULL;

static float centreLaplacianVariance(const uint8_t* rgb565, int w, int h) {
    const uint16_t* px = (const uint16_t*)rgb565;
    // Centre half of each axis: the owner aims the subject at the middle, and the
    // border (vignetting, lens edge softness) would otherwise dominate the number.
    int x0 = w / 4, x1 = w - w / 4, y0 = h / 4, y1 = h - h / 4;
    if (x1 - x0 < 3 || y1 - y0 < 3) return 0;
    double sum = 0, sum2 = 0;
    int n = 0;
    for (int y = y0 + 1; y < y1 - 1; y++) {
        for (int x = x0 + 1; x < x1 - 1; x++) {
            int c = imgRgb565ToGray(px[y * w + x]);
            int l = imgRgb565ToGray(px[y * w + x - 1]);
            int r = imgRgb565ToGray(px[y * w + x + 1]);
            int u = imgRgb565ToGray(px[(y - 1) * w + x]);
            int d = imgRgb565ToGray(px[(y + 1) * w + x]);
            int lap = l + r + u + d - 4 * c;
            sum += lap;
            sum2 += (double)lap * lap;
            n++;
        }
    }
    if (n == 0) return 0;
    double mean = sum / n;
    return (float)(sum2 / n - mean * mean);
}

static void focusTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FOCUS_INTERVAL_MS));
        uint32_t now = millis();
        uint32_t t = lastTouch;
        if (t == 0 || now - t > FOCUS_SESSION_MS) {
            // Idle: give the 150 kB PSRAM back so normal operation pays nothing.
            if (decBuf) { free(decBuf); decBuf = NULL; }
            continue;
        }
        if (!decBuf) {
            decBuf = (uint8_t*)ps_malloc(IMG_DECODE_MAX_SZ);
            if (!decBuf) { logCapture("[%s] PSRAM alloc failed\n", TAG); continue; }
        }
        const uint8_t* buf = NULL;
        size_t len = 0;
        uint16_t srcW = 0, srcH = 0;
        int rh = ringBufferGetLatest(&buf, &len, &srcW, &srcH);
        if (rh < 0) continue;
        int decW = 0, decH = 0;
        // jpg2rgb565 has no output bound: size comes from this frame's own dims.
        bool ok = imgDecodeFits(srcW, srcH, IMG_DECODE_MAX_SZ, decW, decH);
        if (ok) {
            ok = false;
            if (cameraDecodeLock()) {
                ok = jpg2rgb565(buf, len, decBuf, JPG_SCALE_8X);
                cameraDecodeUnlock();
            }
        }
        ringBufferRelease(rh);  // decoded into our own buffer, slot not needed any more
        if (!ok) continue;

        float s = centreLaplacianVariance(decBuf, decW, decH);
        portENTER_CRITICAL(&mux);
        if (resetPending) { curPeak = 0; resetPending = false; }
        curScore = s;
        if (s > curPeak) curPeak = s;
        scoreAt = millis();
        haveScore = true;
        portEXIT_CRITICAL(&mux);
    }
}

void focusScoreTouch(bool resetPeak) {
    uint32_t now = millis();
    portENTER_CRITICAL(&mux);
    // A new session (previous one expired) restarts the peak: a stale maximum from
    // another lens position would make the number meaningless.
    if (resetPeak || lastTouch == 0 || now - lastTouch > FOCUS_SESSION_MS) {
        resetPending = true;
        curPeak = 0;
        haveScore = false;
    }
    lastTouch = now ? now : 1;
    bool start = !taskStarted;
    taskStarted = true;
    portEXIT_CRITICAL(&mux);
    if (start && xTaskCreate(focusTask, "focus", 4096, NULL, 1, NULL) != pdPASS) {
        taskStarted = false;
        logCapture("[%s] task create failed\n", TAG);
    }
}

FocusScore focusScoreGet() {
    FocusScore f;
    portENTER_CRITICAL(&mux);
    f.score = curScore;
    f.peak = curPeak;
    f.valid = haveScore;
    f.age_ms = haveScore ? millis() - scoreAt : 0;
    portEXIT_CRITICAL(&mux);
    return f;
}
