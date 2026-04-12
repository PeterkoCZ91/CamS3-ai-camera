#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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

#include "ws_log.h"

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#include <SPI.h>
#endif

static const char* TAG = "Main";

// Health watchdog
static unsigned long lastHealthCheck = 0;
static const unsigned long HEALTH_CHECK_INTERVAL = 30000;  // 30s
static int consecutiveCaptureFails = 0;

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
    if (!SD.begin(SD_CS_PIN)) {
        Serial.printf("[%s] SD card mount failed\n", TAG);
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.printf("[%s] No SD card inserted\n", TAG);
        return false;
    }

    const char* typeStr = "UNKNOWN";
    if (cardType == CARD_MMC) typeStr = "MMC";
    else if (cardType == CARD_SD) typeStr = "SD";
    else if (cardType == CARD_SDHC) typeStr = "SDHC";

    Serial.printf("[%s] SD card: %s, %lluMB total, %lluMB used\n",
                  TAG, typeStr,
                  SD.totalBytes() / (1024 * 1024),
                  SD.usedBytes() / (1024 * 1024));

    // Create capture directories
    if (!SD.exists("/captures")) SD.mkdir("/captures");
    if (!SD.exists("/timelapse")) SD.mkdir("/timelapse");

    return true;
}
#endif

static void healthWatchdog() {
    if (millis() - lastHealthCheck < HEALTH_CHECK_INTERVAL) return;
    lastHealthCheck = millis();

    // Check capture health
    uint32_t lastCapture = getLastCaptureMs();
    if (lastCapture > 0 && (millis() - lastCapture) > 10000) {
        consecutiveCaptureFails++;
        Serial.printf("[%s] WARNING: No capture for %lums (fails: %d)\n",
                      TAG, millis() - lastCapture, consecutiveCaptureFails);

        if (consecutiveCaptureFails >= 3) {
            Serial.printf("[%s] CRITICAL: Reinitializing camera\n", TAG);
            cameraReinit();
            consecutiveCaptureFails = 0;
        }
    } else {
        consecutiveCaptureFails = 0;
    }

    // Log health
    Serial.printf("[%s] Health: heap=%dKB psram=%dMB fps=%.1f clients=%d errors=%lu\n",
                  TAG,
                  ESP.getFreeHeap() / 1024,
                  ESP.getFreePsram() / (1024 * 1024),
                  getCaptureFps(),
                  getStreamClientCount(),
                  getCaptureErrors());
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Initialize log ring buffer early so it captures boot messages
    logInit();

    Serial.println();
    Serial.println("========================================");
    Serial.printf("  %s Firmware v%s\n", DEVICE_NAME, FIRMWARE_VERSION);
    Serial.println("  M5Stack Unit CamS3 5MP");
    Serial.println("========================================");
    Serial.printf("  CPU: %dMHz, SRAM: %dKB\n", ESP.getCpuFreqMHz(), ESP.getHeapSize() / 1024);
    Serial.printf("  PSRAM: %s (%dMB)\n",
                  psramFound() ? "OK" : "NOT FOUND",
                  ESP.getPsramSize() / (1024 * 1024));
    Serial.printf("  Flash: %dMB\n", ESP.getFlashChipSize() / (1024 * 1024));
    Serial.println("========================================");

    #ifdef INCLUDE_LED_CONTROL
    ledInit();
    ledBlink(2, 100, 100);  // Boot indicator
    #endif

    // Initialize LittleFS
    if (!LittleFS.begin(true)) {
        Serial.printf("[%s] LittleFS mount failed!\n", TAG);
    } else {
        Serial.printf("[%s] LittleFS mounted\n", TAG);
    }

    // Load configuration
    loadConfig();

    #ifdef INCLUDE_SD_CARD
    sdInit();
    #endif

    // Initialize camera
    if (!cameraInit()) {
        Serial.printf("[%s] CRITICAL: Camera init failed!\n", TAG);
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
    #ifdef INCLUDE_PERSON_DETECT
    motionDetectSetSemaphore(motionCascadeSem);
    #endif
    xTaskCreatePinnedToCore(motionDetectTask, "motion", 6144, NULL, 3, NULL, 0);
    Serial.printf("[%s] Motion detection task created\n", TAG);
    #endif

    // Start person detection task (FOMO + tracker, cascade from motion)
    #if defined(INCLUDE_PERSON_DETECT) && defined(INCLUDE_MOTION_DETECT) && !defined(LITE_MODE)
    personDetectInit();
    personDetectSetSemaphore(motionCascadeSem);
    xTaskCreatePinnedToCore(personDetectTask, "persondet", 8192, NULL, 2, NULL, 0);
    Serial.printf("[%s] Person detection task created\n", TAG);
    #endif

    // Start face detection task (requires motion detection)
    #if defined(INCLUDE_FACE_DETECT) && defined(INCLUDE_MOTION_DETECT) && !defined(LITE_MODE)
    faceDetectInit();
    xTaskCreatePinnedToCore(faceDetectTask, "facedet", 8192, NULL, 2, NULL, 0);
    Serial.printf("[%s] Face detection task created\n", TAG);
    #endif

    // Start timelapse task
    #if defined(INCLUDE_TIMELAPSE) && !defined(LITE_MODE)
    timelapseInit();
    xTaskCreatePinnedToCore(timelapseTask, "timelapse", 4096, NULL, 2, NULL, 0);
    Serial.printf("[%s] Timelapse task created\n", TAG);
    #endif

    // NTP time sync + Telegram
    #ifdef INCLUDE_TELEGRAM
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");
    Serial.printf("[%s] NTP time sync configured\n", TAG);

    telegramInit();
    xTaskCreatePinnedToCore(telegramTask, "telegram", 8192, NULL, 2, NULL, 0);
    Serial.printf("[%s] Telegram task created\n", TAG);
    #endif

    // Start MQTT task
    #ifdef INCLUDE_MQTT
    mqttInit();
    xTaskCreatePinnedToCore(mqttTask, "mqtt", 6144, NULL, 2, NULL, 0);
    Serial.printf("[%s] MQTT task created\n", TAG);
    #endif

    #ifdef INCLUDE_LED_CONTROL
    ledSet(true);  // Solid LED = running
    #endif

    Serial.printf("[%s] Boot complete! Access at http://%s\n", TAG, getIPAddress().c_str());
    Serial.printf("[%s] Stream at http://%s:%d/stream\n", TAG, getIPAddress().c_str(), STREAM_PORT);

    #ifdef INCLUDE_OTA
    Serial.printf("[%s] OTA at http://%s/update\n", TAG, getIPAddress().c_str());
    #endif

    #ifdef INCLUDE_TELEGRAM
    if (appConfig.telegram.enabled && isTelegramConnected()) {
        char bootMsg[128];
        snprintf(bootMsg, sizeof(bootMsg), "CamS3 online, IP: %s", getIPAddress().c_str());
        telegramSendText(bootMsg);
    }
    #endif
}

void loop() {
    wifiLoop();
    healthWatchdog();
    webSocketBroadcastStatus();

    // Note: ElegantOTA.loop() is called in web_server via the ESPAsyncWebServer

    // Small delay to prevent WDT reset
    delay(10);
}
