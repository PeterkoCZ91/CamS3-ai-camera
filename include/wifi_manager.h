#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

enum AppWiFiMode {
    APP_WIFI_NONE,
    APP_WIFI_STA,
    APP_WIFI_AP,
    APP_WIFI_SETUP  // Captive portal for initial config
};

bool wifiInit();
void wifiLoop();  // Handle reconnection, DNS for captive portal

AppWiFiMode getWiFiCurrentMode();
bool isWiFiConnected();
String getIPAddress();
int getRSSI();
String getMACAddress();

// Captive portal
bool isCaptivePortalActive();

// Scan networks
String scanNetworksJson();

// Connect to new network (blocking — up to ~10s). Do not call from async web handlers.
bool wifiConnect(const String& ssid, const String& password);

// Request a deferred connect from main loop — safe from async contexts.
void wifiRequestConnect(const String& ssid, const String& password);

#endif // WIFI_MANAGER_H
