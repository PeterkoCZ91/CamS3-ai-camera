#include "wifi_manager.h"
#include "ws_log.h"
#include "config.h"
#include <WiFi.h>
#include <DNSServer.h>

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
static String            pendingSsid;
static String            pendingPass;

bool wifiInit() {
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
                MDNS.addService("rtsp", "tcp", 554);
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
        String ssid = pendingSsid;
        String pass = pendingPass;
        pendingConnect = false;
        pendingSsid = String();
        pendingPass = String();
        wifiConnect(ssid, pass);
    }

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
                    MDNS.addService("rtsp", "tcp", 554);
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
        } else if (!pendingConnect && now - lastSetupRetryMs > SETUP_RETRY_INTERVAL_MS) {
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
    pendingSsid = ssid;
    pendingPass = password;
    pendingConnect = true;
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

String scanNetworksJson() {
    int n = WiFi.scanNetworks(false, false, false, 300);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < n; i++) {
        JsonObject net = arr.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["enc"]  = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }

    WiFi.scanDelete();
    String result;
    serializeJson(doc, result);
    return result;
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
