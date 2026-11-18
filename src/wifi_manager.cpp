#include "wifi_manager.h"
#include "ws_log.h"
#include "config.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <esp_task_wdt.h>

#ifdef INCLUDE_MDNS
#include <ESPmDNS.h>
#endif

#ifdef INCLUDE_EVENT_LOG
#include "event_log.h"
#endif

static const char* TAG = "WiFiMgr";
static DNSServer dnsServer;
static bool captivePortalActive = false;
static AppWiFiMode currentMode = APP_WIFI_NONE;
static unsigned long lastReconnectAttempt = 0;
static const unsigned long RECONNECT_INTERVAL = 30000;  // 30s between reconnect attempts

// Setup-mode STA retry: a slow router after a power cut must not strand the camera
// in the CamS3-Setup portal forever. Non-blocking, runs in AP+STA so the portal stays up.
static const unsigned long SETUP_RETRY_INTERVAL_MS = 45000;
static const unsigned long SETUP_RETRY_TIMEOUT_MS  = 15000;
static unsigned long lastSetupRetryMs  = 0;
static unsigned long setupRetryStartMs = 0;
static bool          setupRetryActive  = false;

// Wi-Fi scan, run from the main loop. A synchronous scan took ~4 s inside the async
// web handler and froze every other request on port 80 (status, /health for A12)
// while it ran. The handler now only asks; wifiLoop() starts the scan when the radio
// is not busy with a connect or a setup-mode retry, polls it, and keeps the result
// for the next request. Results are cached JSON behind a mutex because the loop
// writes them and the async_tcp task reads them.
enum WifiScanState { SCAN_IDLE, SCAN_REQUESTED, SCAN_RUNNING, SCAN_DONE, SCAN_FAILED };
static volatile WifiScanState scanState = SCAN_IDLE;
static unsigned long scanStartedMs = 0;
static unsigned long scanDoneMs    = 0;
static String scanResultJson;
static SemaphoreHandle_t scanMutex = NULL;
static const unsigned long SCAN_RESULT_FRESH_MS = 10000;  // reuse a scan this young
static const unsigned long SCAN_TIMEOUT_MS      = 20000;  // a scan never takes this long
static void scanCancel();
static void wifiScanLoop();

// Silent-death watchdog. WiFi.status() can stick at WL_CONNECTED after the
// link is actually dead (common on ESP32 when the AP vanishes without a
// disconnect notification). Track last time we observed signs of life
// (non-zero IP + non-zero RSSI) and force a reconnect if stuck too long.
static unsigned long lastGoodWiFiMs = 0;
static const unsigned long WIFI_STALE_TIMEOUT_MS = 90000;  // 90s -> forced reconnect

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            logCapture("[%s] Got IP: %s (RSSI %d)\n", TAG,
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            lastGoodWiFiMs = millis();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // Reason code is logged here so /log shows why a join failed (15 = bad password,
            // 201 = SSID not found, 2/202 = auth fail) -- also covers setup-mode retries.
            logCapture("[%s] Disconnected (reason %d), auto-reconnecting\n",
                          TAG, info.wifi_sta_disconnected.reason);
            // setAutoReconnect + WiFi.reconnect() covers most cases; the
            // watchdog in wifiLoop() catches the ones where the event never fires.
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            logCapture("[%s] Associated with AP\n", TAG);
            break;
        default:
            break;
    }
}

// Deferred connect request (set by async handler, consumed by wifiLoop)
static volatile bool     pendingConnect = false;
// Fixed buffers + spinlock: the writer (async web task) and the reader (loop task)
// run on different cores, and String assignment is not atomic.
static char              pendingSsid[33];
static char              pendingPass[65];
static portMUX_TYPE      pendingMux = portMUX_INITIALIZER_UNLOCKED;

bool wifiInit() {
    if (!scanMutex) scanMutex = xSemaphoreCreateMutex();

    WiFi.setHostname(appConfig.wifi.hostname.c_str());
    WiFi.onEvent(onWiFiEvent);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);  // don't spam NVS with credential writes

    if (appConfig.wifi.ssid.length() > 0) {
        // Try STA mode
        logCapture("[%s] Connecting to: %s\n", TAG, appConfig.wifi.ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(appConfig.wifi.ssid.c_str(), appConfig.wifi.password.c_str());

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            esp_task_wdt_reset();
            Serial.print(".");
            attempts++;
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            currentMode = APP_WIFI_STA;
            lastGoodWiFiMs = millis();
            logCapture("[%s] Connected! IP: %s\n", TAG, WiFi.localIP().toString().c_str());
            logCapture("[%s] RSSI: %d dBm\n", TAG, WiFi.RSSI());

            #ifdef INCLUDE_MDNS
            if (MDNS.begin(appConfig.wifi.hostname.c_str())) {
                MDNS.addService("http", "tcp", HTTP_PORT);
                logCapture("[%s] mDNS: %s.local\n", TAG, appConfig.wifi.hostname.c_str());
            }
            #endif

            return true;
        }

        logCapture("[%s] STA connection failed, falling back to AP\n", TAG);
    }

    // AP mode with captive portal
    logCapture("[%s] Starting AP: %s\n", TAG, DEFAULT_AP_SSID);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
    delay(100);

    // Start DNS server for captive portal
    dnsServer.start(53, "*", WiFi.softAPIP());
    captivePortalActive = true;
    currentMode = APP_WIFI_SETUP;
    lastSetupRetryMs = millis();

    logCapture("[%s] AP IP: %s\n", TAG, WiFi.softAPIP().toString().c_str());
    return true;
}

void wifiLoop() {
    if (captivePortalActive) {
        dnsServer.processNextRequest();
    }

    // Handle deferred connect request (from async web handler)
    if (pendingConnect) {
        char ssidBuf[sizeof(pendingSsid)];
        char passBuf[sizeof(pendingPass)];
        portENTER_CRITICAL(&pendingMux);
        memcpy(ssidBuf, pendingSsid, sizeof(ssidBuf));
        memcpy(passBuf, pendingPass, sizeof(passBuf));
        pendingSsid[0] = '\0';
        pendingPass[0] = '\0';
        pendingConnect = false;
        portEXIT_CRITICAL(&pendingMux);
        String ssid(ssidBuf);
        String pass(passBuf);
        memset(passBuf, 0, sizeof(passBuf));
        scanCancel();   // the connect switches the radio mode under a running scan
        wifiConnect(ssid, pass);
    }

    wifiScanLoop();

    // Setup/AP fallback is not terminal: periodically retry the stored SSID without
    // tearing down the portal (AP+STA), and leave setup mode once it connects.
    if (currentMode == APP_WIFI_SETUP && appConfig.wifi.ssid.length() > 0) {
        unsigned long now = millis();
        if (setupRetryActive) {
            if (WiFi.status() == WL_CONNECTED) {
                setupRetryActive = false;
                dnsServer.stop();
                captivePortalActive = false;
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                currentMode = APP_WIFI_STA;
                lastGoodWiFiMs = now;
                logCapture("[%s] Setup-mode retry succeeded, IP: %s -- portal stopped\n",
                           TAG, WiFi.localIP().toString().c_str());
                #ifdef INCLUDE_MDNS
                if (MDNS.begin(appConfig.wifi.hostname.c_str())) {
                    MDNS.addService("http", "tcp", HTTP_PORT);
                    }
                #endif
                return;
            }
            if (now - setupRetryStartMs > SETUP_RETRY_TIMEOUT_MS) {
                // Disconnect reason was already logged by onWiFiEvent.
                setupRetryActive = false;
                lastSetupRetryMs = now;
                WiFi.disconnect(false, false);
                WiFi.mode(WIFI_AP);
                logCapture("[%s] Setup-mode STA retry failed, staying in portal\n", TAG);
            }
        } else if (!pendingConnect && scanState != SCAN_REQUESTED && scanState != SCAN_RUNNING
                   && now - lastSetupRetryMs > SETUP_RETRY_INTERVAL_MS) {
            lastSetupRetryMs = now;
            // Don't yank the radio (AP channel may move) while someone is using the portal.
            if (WiFi.softAPgetStationNum() == 0) {
                logCapture("[%s] Setup mode: retrying STA '%s'\n", TAG, appConfig.wifi.ssid.c_str());
                WiFi.mode(WIFI_AP_STA);
                WiFi.begin(appConfig.wifi.ssid.c_str(), appConfig.wifi.password.c_str());
                setupRetryActive  = true;
                setupRetryStartMs = now;
            }
        }
    }

    // Auto-reconnect in STA mode. Two paths:
    //   (1) status flipped to non-connected — clean case, try reconnect.
    //   (2) status still CONNECTED but IP is 0.0.0.0 or RSSI dead for >90s —
    //       the "silent death" where the stack thinks we're up but nothing
    //       actually works. Force a disconnect + reconnect cycle.
    if (currentMode == APP_WIFI_STA) {
        unsigned long now = millis();
        bool statusOk = (WiFi.status() == WL_CONNECTED);
        bool ipOk     = (uint32_t)WiFi.localIP() != 0;
        bool rssiOk   = (WiFi.RSSI() != 0);

        if (statusOk && ipOk && rssiOk) {
            lastGoodWiFiMs = now;
        }

        // Case 1: hard disconnect (status flipped)
        if (!statusOk) {
            if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
                lastReconnectAttempt = now;
                logCapture("[%s] WiFi disconnected, reconnecting...\n", TAG);
                #ifdef INCLUDE_EVENT_LOG
                logEvent(EVT_WIFI_RECONNECT, "status lost");
                #endif
                WiFi.reconnect();
            }
        }
        // Case 2: silent death — status lies. Forced full reset.
        else if (lastGoodWiFiMs > 0 && (now - lastGoodWiFiMs) > WIFI_STALE_TIMEOUT_MS) {
            if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
                lastReconnectAttempt = now;
                logCapture("[%s] WiFi silent death (no IP/RSSI %lus) — forcing reconnect\n",
                              TAG, (now - lastGoodWiFiMs) / 1000);
                #ifdef INCLUDE_EVENT_LOG
                logEvent(EVT_WIFI_RECONNECT, "silent death");
                #endif
                WiFi.disconnect(false, true);
                delay(100);
                WiFi.begin(appConfig.wifi.ssid.c_str(), appConfig.wifi.password.c_str());
                lastGoodWiFiMs = now;  // grace period to avoid immediate re-trigger
            }
        }
    }
}

void wifiRequestConnect(const String& ssid, const String& password) {
    if (ssid.length() >= sizeof(pendingSsid) || password.length() >= sizeof(pendingPass)) {
        logCapture("[%s] Connect request ignored: SSID/password too long\n", TAG);
        return;
    }
    portENTER_CRITICAL(&pendingMux);
    if (pendingConnect) {   // a request is already queued: ignore this one, never overwrite
        portEXIT_CRITICAL(&pendingMux);
        logCapture("[%s] Connect request ignored: another one is pending\n", TAG);
        return;
    }
    strlcpy(pendingSsid, ssid.c_str(), sizeof(pendingSsid));
    strlcpy(pendingPass, password.c_str(), sizeof(pendingPass));
    pendingConnect = true;
    portEXIT_CRITICAL(&pendingMux);
}

AppWiFiMode getWiFiCurrentMode() { return currentMode; }
bool isWiFiConnected() { return WiFi.status() == WL_CONNECTED; }

String getIPAddress() {
    if (currentMode == APP_WIFI_STA) {
        return WiFi.localIP().toString();
    }
    return WiFi.softAPIP().toString();
}

int getRSSI() {
    if (currentMode == APP_WIFI_STA) {
        return WiFi.RSSI();
    }
    return 0;
}

String getMACAddress() {
    return WiFi.macAddress();
}

bool isCaptivePortalActive() { return captivePortalActive; }

static void scanStoreResults(int n) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject net = arr.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["enc"]  = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    String json;
    serializeJson(doc, json);
    if (scanMutex && xSemaphoreTake(scanMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        scanResultJson = json;
        xSemaphoreGive(scanMutex);
    }
}

static void scanCancel() {
    if (scanState == SCAN_RUNNING) WiFi.scanDelete();
    scanState = SCAN_IDLE;
}

// Main-loop half of the scan. Never starts while the radio is being reconfigured:
// a mode change in the middle of a scan aborts it, and in setup mode the scan itself
// adds the STA interface the retry logic also drives.
static void wifiScanLoop() {
    unsigned long now = millis();
    if (scanState == SCAN_REQUESTED) {
        if (pendingConnect || setupRetryActive) return;   // wait for the radio
        // Async: returns at once. The radio is off its home channel while it scans,
        // so the camera answers nothing (A12 included) for ~3.5-4 s; that is the
        // radio, not this code. Do not shorten the dwell to save time: the core
        // gives up after dwell x 20 ms, and at 120 ms a scan that still takes ~4 s
        // while connected is reported as failed (measured).
        int r = WiFi.scanNetworks(true, false, false, 300);
        if (r == WIFI_SCAN_FAILED) {
            logCapture("[%s] WiFi scan could not start\n", TAG);
            scanState = SCAN_FAILED;
            return;
        }
        scanStartedMs = now;
        scanState = SCAN_RUNNING;
        return;
    }
    if (scanState == SCAN_RUNNING) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            scanStoreResults(n);
            scanDoneMs = now;
            scanState = SCAN_DONE;
        } else if (n == WIFI_SCAN_FAILED || now - scanStartedMs > SCAN_TIMEOUT_MS) {
            WiFi.scanDelete();
            logCapture("[%s] WiFi scan failed\n", TAG);
            scanState = SCAN_FAILED;
        }
    }
}

int wifiScanPoll(String& out) {
    WifiScanState st = scanState;
    if (st == SCAN_DONE && millis() - scanDoneMs < SCAN_RESULT_FRESH_MS) {
        if (scanMutex && xSemaphoreTake(scanMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            out = scanResultJson;
            xSemaphoreGive(scanMutex);
            return 200;
        }
        return 202;   // the loop is writing a newer result right now
    }
    if (st == SCAN_FAILED) {
        scanState = SCAN_IDLE;   // the next poll starts a fresh attempt
        return 500;
    }
    if (st == SCAN_IDLE || st == SCAN_DONE) scanState = SCAN_REQUESTED;
    return 202;
}

bool wifiConnect(const String& ssid, const String& password) {
    logCapture("[%s] Connecting to new network: %s\n", TAG, ssid.c_str());

    // Stop captive portal if active
    if (captivePortalActive) {
        dnsServer.stop();
        captivePortalActive = false;
    }

    setupRetryActive = false;  // manual connect takes over the radio

    // Connect
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        esp_task_wdt_reset();
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        currentMode = APP_WIFI_STA;
        lastGoodWiFiMs = millis();
        logCapture("[%s] Connected! IP: %s\n", TAG, WiFi.localIP().toString().c_str());

        // Persist only now: a typo in the portal must not overwrite working credentials.
        appConfig.wifi.ssid = ssid;
        appConfig.wifi.password = password;
        extern void saveSecretsToNVS();
        saveSecretsToNVS();
        saveConfig();

        #ifdef INCLUDE_MDNS
        MDNS.begin(appConfig.wifi.hostname.c_str());
        MDNS.addService("http", "tcp", HTTP_PORT);
        #endif

        return true;
    }

    // Failed - restart AP; previous stored credentials are kept untouched.
    logCapture("[%s] Connection failed, restarting AP (stored credentials unchanged)\n", TAG);
    lastSetupRetryMs = millis();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
    dnsServer.start(53, "*", WiFi.softAPIP());
    captivePortalActive = true;
    currentMode = APP_WIFI_SETUP;
    return false;
}
