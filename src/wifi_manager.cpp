#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <DNSServer.h>

#ifdef INCLUDE_MDNS
#include <ESPmDNS.h>
#endif

static const char* TAG = "WiFiMgr";
static DNSServer dnsServer;
static bool captivePortalActive = false;
static AppWiFiMode currentMode = APP_WIFI_NONE;
static unsigned long lastReconnectAttempt = 0;
static const unsigned long RECONNECT_INTERVAL = 30000;  // 30s between reconnect attempts

bool wifiInit() {
    WiFi.setHostname(appConfig.wifi.hostname.c_str());

    if (appConfig.wifi.ssid.length() > 0) {
        // Try STA mode
        Serial.printf("[%s] Connecting to: %s\n", TAG, appConfig.wifi.ssid.c_str());
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
            Serial.printf("[%s] Connected! IP: %s\n", TAG, WiFi.localIP().toString().c_str());
            Serial.printf("[%s] RSSI: %d dBm\n", TAG, WiFi.RSSI());

            #ifdef INCLUDE_MDNS
            if (MDNS.begin(appConfig.wifi.hostname.c_str())) {
                MDNS.addService("http", "tcp", HTTP_PORT);
                MDNS.addService("rtsp", "tcp", 554);
                Serial.printf("[%s] mDNS: %s.local\n", TAG, appConfig.wifi.hostname.c_str());
            }
            #endif

            return true;
        }

        Serial.printf("[%s] STA connection failed, falling back to AP\n", TAG);
    }

    // AP mode with captive portal
    Serial.printf("[%s] Starting AP: %s\n", TAG, DEFAULT_AP_SSID);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
    delay(100);

    // Start DNS server for captive portal
    dnsServer.start(53, "*", WiFi.softAPIP());
    captivePortalActive = true;
    currentMode = APP_WIFI_SETUP;

    Serial.printf("[%s] AP IP: %s\n", TAG, WiFi.softAPIP().toString().c_str());
    return true;
}

void wifiLoop() {
    if (captivePortalActive) {
        dnsServer.processNextRequest();
    }

    // Auto-reconnect in STA mode
    if (currentMode == APP_WIFI_STA && WiFi.status() != WL_CONNECTED) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
            lastReconnectAttempt = now;
            Serial.printf("[%s] WiFi disconnected, reconnecting...\n", TAG);
            WiFi.reconnect();
        }
    }
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
    Serial.printf("[%s] Connecting to new network: %s\n", TAG, ssid.c_str());

    // Stop captive portal if active
    if (captivePortalActive) {
        dnsServer.stop();
        captivePortalActive = false;
    }

    // Save credentials
    appConfig.wifi.ssid = ssid;
    appConfig.wifi.password = password;
    extern void saveSecretsToNVS();
    saveSecretsToNVS();
    saveConfig();

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
        Serial.printf("[%s] Connected! IP: %s\n", TAG, WiFi.localIP().toString().c_str());

        #ifdef INCLUDE_MDNS
        MDNS.begin(appConfig.wifi.hostname.c_str());
        MDNS.addService("http", "tcp", HTTP_PORT);
        #endif

        return true;
    }

    // Failed - restart AP
    Serial.printf("[%s] Connection failed, restarting AP\n", TAG);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
    dnsServer.start(53, "*", WiFi.softAPIP());
    captivePortalActive = true;
    currentMode = APP_WIFI_SETUP;
    return false;
}
