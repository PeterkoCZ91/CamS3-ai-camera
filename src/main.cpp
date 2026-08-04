#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config.h"
#include "board_config.h"
#include "camera_manager.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "stream_server.h"

#ifdef INCLUDE_MOTION_DETECT
#include "motion_detect.h"
#endif

#if defined(INCLUDE_FACE_DETECT) && defined(INCLUDE_MOTION_DETECT)
#include "face_detect.h"
#endif

#ifdef INCLUDE_TIMELAPSE
#include "timelapse.h"
#endif

#ifdef INCLUDE_TELEGRAM
#include "telegram.h"
#endif

#ifdef INCLUDE_PERSON_DETECT
#include "person_detection.h"
#include "tracker.h"
#endif

#ifdef INCLUDE_MQTT
#include "mqtt_handler.h"
#endif

#ifdef INCLUDE_ZONES
#include "zone_manager.h"
#endif

#ifdef INCLUDE_EVENT_LOG
#include "event_log.h"
#endif

#include "ws_log.h"
#include "system_stats.h"

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#include <SPI.h>
#endif

static const char* TAG = "Main";

// Health watchdog
static unsigned long lastHealthCheck = 0;
static const unsigned long HEALTH_CHECK_INTERVAL = 30000;  // 30s
static int consecutiveCaptureFails = 0;
// cameraReinit() can now refuse to run (capture task wedged inside the driver).
// If it keeps refusing there is nothing left to try in-process — reboot.
static int failedReinits = 0;
static const int MAX_FAILED_REINITS = 3;

// LED status indication
#ifdef INCLUDE_LED_CONTROL
static void ledInit() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
}

static void ledBlink(int count, int onMs, int offMs) {
    for (int i = 0; i < count; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(onMs);
        digitalWrite(LED_PIN, LOW);
        if (i < count - 1) delay(offMs);
    }
}

static void ledSet(bool on) {
    if (appConfig.led_enabled) {
        digitalWrite(LED_PIN, on ? HIGH : LOW);
    } else {
        digitalWrite(LED_PIN, LOW);
    }
}
#endif

// Forward declaration
extern void webSocketBroadcastStatus();

#ifdef INCLUDE_SD_CARD
static bool sdInit() {
    SPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    // Try 20 MHz first; on breadboard/loose contacts this often fails — retry at 4 MHz.
    bool ok = SD.begin(SD_CS_PIN, SPI, 20000000);
    if (!ok) {
        logCapture("[%s] SD 20MHz mount failed, retrying @4MHz\n", TAG);
        SD.end();
        delay(50);
        ok = SD.begin(SD_CS_PIN, SPI, 4000000);
    }
    if (!ok) {
        logCapture("[%s] SD card mount failed (no card / bad contact / unformatted FAT)\n", TAG);
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        logCapture("[%s] No SD card inserted\n", TAG);
        SD.end();
        return false;
    }

    const char* typeStr = "UNKNOWN";
    if (cardType == CARD_MMC) typeStr = "MMC";
    else if (cardType == CARD_SD) typeStr = "SD";
    else if (cardType == CARD_SDHC) typeStr = "SDHC";

    logCapture("[%s] SD card: %s, %lluMB total, %lluMB used\n",
                  TAG, typeStr,
                  SD.totalBytes() / (1024 * 1024),
                  SD.usedBytes() / (1024 * 1024));

    // Create capture directories
    if (!SD.exists("/captures"))    SD.mkdir("/captures");
    if (!SD.exists("/timelapse"))   SD.mkdir("/timelapse");
    if (!SD.exists("/recordings"))  SD.mkdir("/recordings");

    return true;
}
#endif

// Run a camera reinit and escalate to a reboot if the camera cannot be recovered.
static void attemptCameraRecovery(const char* reason) {
    logCapture("[%s] Camera recovery (%s)\n", TAG, reason);
    if (cameraReinit()) {
        failedReinits = 0;
        consecutiveCaptureFails = 0;
        return;
    }

    failedReinits++;
    logCapture("[%s] Camera reinit failed (%d/%d)\n", TAG, failedReinits, MAX_FAILED_REINITS);
    if (failedReinits >= MAX_FAILED_REINITS) {
        #ifdef INCLUDE_EVENT_LOG
        logEvent(EVT_UNKNOWN, "camera unrecoverable, rebooting");
        #endif
        logCapture("[%s] Camera unrecoverable — rebooting\n", TAG);
        delay(200);
        ESP.restart();
    }
}

static void healthWatchdog() {
    // Handle out-of-band reinit request from capture task (repeated fb_get failures)
    if (cameraReinitRequested()) {
        cameraClearReinitRequest();
        attemptCameraRecovery("requested by capture task");
        lastHealthCheck = millis();
        return;
    }

    if (millis() - lastHealthCheck < HEALTH_CHECK_INTERVAL) return;
    lastHealthCheck = millis();

    systemStatsTick();

    // Check capture health
    uint32_t lastCapture = getLastCaptureMs();
    if (lastCapture > 0 && (millis() - lastCapture) > 10000) {
        consecutiveCaptureFails++;
        logCapture("[%s] WARNING: No capture for %lums (fails: %d)\n",
                      TAG, millis() - lastCapture, consecutiveCaptureFails);

        if (consecutiveCaptureFails >= 3) {
            attemptCameraRecovery("no capture for 30s");
        }
    } else {
        consecutiveCaptureFails = 0;
    }

    // Health line. Free heap alone hides the failure mode that actually bites on
    // this board: the heap stays "big enough" while the largest contiguous block
    // shrinks below what a TLS handshake or a JPEG copy needs. Log fragmentation
    // (1 - largest_block/free) and the drift against the first measurement so a
    // slow leak is visible without external tooling.
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    int fragPct = freeHeap > 0 ? (int)(100 - (uint64_t)maxBlock * 100 / freeHeap) : 0;

    static uint32_t heapBaseline = 0;
    if (heapBaseline == 0) heapBaseline = freeHeap;
    int32_t drift = (int32_t)freeHeap - (int32_t)heapBaseline;

    logCapture("[%s] Health: heap=%uKB (min=%uKB max_block=%uKB frag=%d%% drift=%+dKB) "
               "psram=%uKB fps=%.1f clients=%d/%d errors=%lu dropped=%lu\n",
               TAG,
               (unsigned)(freeHeap / 1024),
               (unsigned)(ESP.getMinFreeHeap() / 1024),
               (unsigned)(maxBlock / 1024),
               fragPct,
               (int)(drift / 1024),
               (unsigned)(ESP.getFreePsram() / 1024),
               getCaptureFps(),
               getStreamClientCount(),
               getDetectionStreamClientCount(),
               getCaptureErrors(),
               (unsigned long)getRingDroppedFrames());

    // Detection pipeline counters — the numbers you need when tuning thresholds.
    #if defined(INCLUDE_MOTION_DETECT) || defined(INCLUDE_PERSON_DETECT)
    {
        #ifdef INCLUDE_MOTION_DETECT
        MotionDebugInfo mdi = getMotionDebugInfo();
        #endif
        logCapture("[%s] Perf:"
                   #ifdef INCLUDE_MOTION_DETECT
                   " motion_events=%lu changed=%.1f%% bright=%.0f night=%d decode=%lums analysis=%lums"
                   #endif
                   #ifdef INCLUDE_PERSON_DETECT
                   " person_events=%lu tracks=%d infer=%lums last=%s"
                   #endif
                   "\n",
                   TAG
                   #ifdef INCLUDE_MOTION_DETECT
                   , getMotionEventCount(), mdi.changed_pct, mdi.avg_brightness,
                   mdi.night_mode ? 1 : 0, (unsigned long)mdi.decode_ms,
                   (unsigned long)mdi.analysis_ms
                   #endif
                   #ifdef INCLUDE_PERSON_DETECT
                   , getPersonEventCount(), getPersonDetectResult().track_count,
                   (unsigned long)getPersonDetectResult().inference_ms,
                   personDecisionToString(getPersonDetectResult().decision)
                   #endif
                   );
    }
    #endif

    // Low heap is worth a persisted event, not just a serial line — it is the
    // usual precursor to a watchdog reboot and EVT_LOW_MEMORY was never emitted.
    #ifdef INCLUDE_EVENT_LOG
    static unsigned long lastLowMemEvent = 0;
    const uint32_t LOW_HEAP_KB = 40;
    if (freeHeap / 1024 < LOW_HEAP_KB &&
        (lastLowMemEvent == 0 || millis() - lastLowMemEvent > 600000)) {  // max 1 per 10 min
        lastLowMemEvent = millis();
        char detail[EVENT_DETAIL_LEN];
        snprintf(detail, sizeof(detail), "heap=%uKB frag=%d%%",
                 (unsigned)(freeHeap / 1024), fragPct);
        logEvent(EVT_LOW_MEMORY, detail);
    }
    #endif
}

void setup() {
    // Ensure the brownout detector is actually enabled (some boards disable it
    // by default via ESP32_DISABLE_BROWNOUT_DETECTOR).
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0x10000); // enable, reset on BOD

    // Task watchdog: 20s timeout, panic on trigger so core dump + reset happen.
    esp_task_wdt_init(20, true);
    esp_task_wdt_add(NULL);  // subscribe loop task

    Serial.begin(115200);
    delay(1000);

    // Initialize log ring buffer early so it captures boot messages
    logInit();

    logCapture("\n========================================\n");
    logCapture("  %s Firmware v%s\n", DEVICE_NAME, FIRMWARE_VERSION);
    logCapture("  M5Stack Unit CamS3 5MP\n");
    logCapture("========================================\n");
    logCapture("  CPU: %dMHz, SRAM: %dKB\n", ESP.getCpuFreqMHz(), ESP.getHeapSize() / 1024);
    logCapture("  PSRAM: %s (%dMB)\n",
                  psramFound() ? "OK" : "NOT FOUND",
                  ESP.getPsramSize() / (1024 * 1024));
    logCapture("  Flash: %dMB\n", ESP.getFlashChipSize() / (1024 * 1024));
    logCapture("========================================\n");

    #ifdef INCLUDE_LED_CONTROL
    ledInit();
    ledBlink(2, 100, 100);  // Boot indicator
    #endif

    // Initialize LittleFS
    if (!LittleFS.begin(true)) {
        logCapture("[%s] LittleFS mount failed!\n", TAG);
    } else {
        logCapture("[%s] LittleFS mounted\n", TAG);
    }

    // Restart accounting needs LittleFS and should run before anything can crash.
    systemStatsBegin();

    // Load configuration
    loadConfig();

    #ifdef INCLUDE_EVENT_LOG
    initEventLog();
    #endif

    #ifdef INCLUDE_ZONES
    initZoneManager();
    #endif

    // Default password still in place? Log it, and the admin UI will show a banner
    // (via /api/status → "default_password": true) until the user changes it.
    // We deliberately do NOT auto-generate — users without UART would get locked out.
    if (appConfig.auth.http_pass == String(DEFAULT_HTTP_PASS)) {
        Serial.println("[Main] WARNING: default admin password in use — change it in Settings");
    }

    #ifdef INCLUDE_SD_CARD
    sdInit();
    #endif

    // Initialize camera
    if (!cameraInit()) {
        logCapture("[%s] CRITICAL: Camera init failed!\n", TAG);
        #ifdef INCLUDE_LED_CONTROL
        ledBlink(5, 200, 200);  // Error indicator
        #endif
        // Continue anyway - WiFi and web server can still work for diagnostics
    }

    // Initialize WiFi
    wifiInit();

    // Start capture task
    startCaptureTask();

    // Start servers
    streamServerInit();
    webServerInit();

    // Create cascade semaphore for motion -> person detection pipeline
    #if defined(INCLUDE_MOTION_DETECT) && defined(INCLUDE_PERSON_DETECT)
    SemaphoreHandle_t motionCascadeSem = xSemaphoreCreateBinary();
    #endif

    // Start motion detection task
    #ifdef INCLUDE_MOTION_DETECT
    motionDetectInit();
    // Apply the stored ROI mask, if any. Before this the mask written through
    // /api/roi was persisted and then never read by the detector.
    #ifdef INCLUDE_ZONES
    {
        char roi[MOTION_GRID_SIZE + 1] = {0};
        if (loadROIMask(roi, sizeof(roi))) {
            int len = strlen(roi);
            if (motionDetectSetRoiMask(roi, len)) {
                logCapture("[%s] ROI mask applied (%d/%d blocks active)\n",
                           TAG, motionDetectRoiActiveBlocks(), MOTION_GRID_SIZE);
            } else {
                logCapture("[%s] Stored ROI mask ignored (len %d, expected %d)\n",
                           TAG, len, MOTION_GRID_SIZE);
            }
        }
    }
    #endif
    #ifdef INCLUDE_PERSON_DETECT
    motionDetectSetSemaphore(motionCascadeSem);
    #endif
    xTaskCreatePinnedToCore(motionDetectTask, "motion", 6144, NULL, 3, NULL, 0);
    logCapture("[%s] Motion detection task created\n", TAG);
    #endif

    // Start person detection task (FOMO + tracker, cascade from motion)
    #if defined(INCLUDE_PERSON_DETECT) && defined(INCLUDE_MOTION_DETECT) && !defined(LITE_MODE)
    if (personDetectInit()) {
        personDetectSetSemaphore(motionCascadeSem);
        xTaskCreatePinnedToCore(personDetectTask, "persondet", 8192, NULL, 2, NULL, 0);
        logCapture("[%s] Person detection task created\n", TAG);
    } else {
        logCapture("[%s] Person detection init failed — task NOT started\n", TAG);
    }
    #endif

    // Start face detection task (requires motion detection)
    #if defined(INCLUDE_FACE_DETECT) && defined(INCLUDE_MOTION_DETECT) && !defined(LITE_MODE)
    if (faceDetectInit()) {
        xTaskCreatePinnedToCore(faceDetectTask, "facedet", 8192, NULL, 2, NULL, 0);
        logCapture("[%s] Face detection task created\n", TAG);
    } else {
        logCapture("[%s] Face detection init failed — task NOT started\n", TAG);
    }
    #endif

    // Start timelapse task
    #if defined(INCLUDE_TIMELAPSE) && !defined(LITE_MODE)
    timelapseInit();
    xTaskCreatePinnedToCore(timelapseTask, "timelapse", 4096, NULL, 2, NULL, 0);
    logCapture("[%s] Timelapse task created\n", TAG);
    #endif

    // NTP time sync + Telegram
    #ifdef INCLUDE_TELEGRAM
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");
    logCapture("[%s] NTP time sync configured\n", TAG);

    telegramInit();
    xTaskCreatePinnedToCore(telegramTask, "telegram", 8192, NULL, 2, NULL, 0);
    logCapture("[%s] Telegram task created\n", TAG);
    #endif

    // Start MQTT task
    #ifdef INCLUDE_MQTT
    mqttInit();
    xTaskCreatePinnedToCore(mqttTask, "mqtt", 6144, NULL, 2, NULL, 0);
    logCapture("[%s] MQTT task created\n", TAG);
    #endif

    // Give tasks a chance to run their first iteration
    delay(2000);

    #ifdef INCLUDE_LED_CONTROL
    ledSet(true);  // Solid LED = running
    #endif

    logCapture("[%s] Boot complete! Access at http://%s\n", TAG, getIPAddress().c_str());
    logCapture("[%s] Stream at http://%s:%d/stream\n", TAG, getIPAddress().c_str(), STREAM_PORT);

    #ifdef INCLUDE_OTA
    logCapture("[%s] OTA at http://%s/update\n", TAG, getIPAddress().c_str());
    #endif

    // Note: rich boot message is sent from telegramTask() once WiFi + TLS
    // are actually up. At this point isTelegramConnected() only checks that
    // creds exist, not that we can reach api.telegram.org.
}

void loop() {
    esp_task_wdt_reset();
    wifiLoop();
    healthWatchdog();
    webSocketBroadcastStatus();

    // Note: ElegantOTA.loop() is called in web_server via the ESPAsyncWebServer

    // Small delay to prevent WDT reset
    delay(10);
}
