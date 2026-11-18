#include "flash_worker.h"
#include "ws_log.h"
#include "config.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

static const char* TAG = "FLASHW";

#define FW_QUEUE_LEN 8
static QueueHandle_t q = NULL;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static FlashJob waiting[FW_QUEUE_LEN];   // jobs queued but not yet started
static volatile bool running = false;

static void workerTask(void*) {
    for (;;) {
        FlashJob job = NULL;
        if (xQueueReceive(q, &job, portMAX_DELAY) != pdTRUE || !job) continue;
        // Clear the "waiting" mark BEFORE running: a request that arrives while the
        // job executes must run again, or the change it announces would be lost.
        portENTER_CRITICAL(&mux);
        for (int i = 0; i < FW_QUEUE_LEN; i++) if (waiting[i] == job) { waiting[i] = NULL; break; }
        portEXIT_CRITICAL(&mux);
        job();
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        static bool warned = false;
        if (hwm < 1024 && !warned) {
            warned = true;
            logCapture("[%s] WARNING stack headroom only %u bytes\n", TAG, (unsigned)hwm);
        }
    }
}

void flashWorkerStart() {
    if (running) return;
    q = xQueueCreate(FW_QUEUE_LEN, sizeof(FlashJob));
    if (!q) return;
    // Internal stack on purpose (that is the whole point of this task).
    if (xTaskCreatePinnedToCore(workerTask, "flashw", 6144, NULL, 1, NULL, 0) == pdPASS) {
        running = true;
    }
}

void flashDefer(FlashJob job) {
    if (!job) return;
    if (!running) { job(); return; }
    bool queue = false;
    portENTER_CRITICAL(&mux);
    bool already = false;
    int freeSlot = -1;
    for (int i = 0; i < FW_QUEUE_LEN; i++) {
        if (waiting[i] == job) { already = true; break; }
        if (!waiting[i] && freeSlot < 0) freeSlot = i;
    }
    if (!already && freeSlot >= 0) { waiting[freeSlot] = job; queue = true; }
    portEXIT_CRITICAL(&mux);
    if (queue && xQueueSend(q, &job, 0) != pdTRUE) {
        portENTER_CRITICAL(&mux);
        for (int i = 0; i < FW_QUEUE_LEN; i++) if (waiting[i] == job) { waiting[i] = NULL; break; }
        portEXIT_CRITICAL(&mux);
        logCapture("[%s] queue full, job dropped\n", TAG);
    }
}

static void saveConfigJob() { saveConfig(); }
void saveConfigDeferred() { flashDefer(saveConfigJob); }
