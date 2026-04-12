#include "web_server.h"
#include "config.h"
#include "camera_manager.h"
#include "wifi_manager.h"
#include "board_config.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>

#ifdef INCLUDE_OTA
#include <ElegantOTA.h>
#endif

#ifdef INCLUDE_MOTION_DETECT
#include "motion_detect.h"
#endif

#if defined(INCLUDE_FACE_DETECT) && defined(INCLUDE_MOTION_DETECT)
#include "face_detect.h"
#endif

#ifdef INCLUDE_SD_CARD
#include <SD.h>
#include <SPI.h>
#endif

#ifdef INCLUDE_TELEGRAM
#include "telegram.h"
#endif

#ifdef INCLUDE_PERSON_DETECT
#include "person_detection.h"
#endif

#ifdef INCLUDE_MQTT
#include "mqtt_handler.h"
#endif

#include "ws_log.h"

static const char* TAG = "WebSrv";
static AsyncWebServer server(HTTP_PORT);
static AsyncWebSocket ws("/ws");

// Forward declarations
static void setupApiRoutes();
static void setupWebSocket();
static void setupStaticFiles();
extern void saveSecretsToNVS();

#ifdef INCLUDE_SD_CARD
static void setupSdRoutes();
#endif

// Uptime calculation
static String formatUptime() {
    unsigned long sec = millis() / 1000;
    unsigned long d = sec / 86400; sec %= 86400;
    unsigned long h = sec / 3600;  sec %= 3600;
    unsigned long m = sec / 60;    sec %= 60;
    char buf[32];
    if (d > 0)
        snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu", d, h, m, sec);
    else
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, sec);
    return String(buf);
}

// Build status JSON (shared by /api/status and WebSocket)
static String buildStatusJson() {
    JsonDocument doc;

    // System info
    doc["version"]     = FIRMWARE_VERSION;
    doc["device"]      = DEVICE_NAME;
    doc["uptime"]      = formatUptime();
    doc["uptime_sec"]  = millis() / 1000;
    doc["free_heap"]   = ESP.getFreeHeap();
    doc["free_psram"]  = ESP.getFreePsram();
    doc["total_psram"] = ESP.getPsramSize();
    doc["cpu_freq"]    = ESP.getCpuFreqMHz();
    doc["flash_size"]  = ESP.getFlashChipSize();
    doc["sdk"]         = ESP.getSdkVersion();

    // WiFi
    doc["ip"]   = getIPAddress();
    doc["rssi"] = getRSSI();
    doc["mac"]  = getMACAddress();
    doc["wifi_mode"] = isWiFiConnected() ? "STA" : "AP";
    doc["hostname"]  = appConfig.wifi.hostname;

    // Camera
    doc["capture_fps"]    = getCaptureFps();
    doc["capture_count"]  = getCaptureCount();
    doc["capture_errors"] = getCaptureErrors();
    doc["stream_clients"] = getStreamClientCount();

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        doc["sensor_pid"] = s->id.PID;
    }

    // Camera settings (current)
    doc["frame_size"]    = appConfig.camera.frame_size;
    doc["jpeg_quality"]  = appConfig.camera.jpeg_quality;
    doc["brightness"]    = appConfig.camera.brightness;
    doc["contrast"]      = appConfig.camera.contrast;
    doc["saturation"]    = appConfig.camera.saturation;
    doc["sharpness"]     = appConfig.camera.sharpness;
    doc["denoise"]       = appConfig.camera.denoise;
    doc["ae_level"]      = appConfig.camera.ae_level;
    doc["aec_value"]     = appConfig.camera.aec_value;
    doc["agc_gain"]      = appConfig.camera.agc_gain;
    doc["gainceiling"]   = appConfig.camera.gainceiling;
    doc["wb_mode"]       = appConfig.camera.wb_mode;
    doc["vflip"]         = appConfig.camera.vflip;
    doc["hmirror"]       = appConfig.camera.hmirror;
    doc["aec"]           = appConfig.camera.aec;
    doc["agc"]           = appConfig.camera.agc;
    doc["awb"]           = appConfig.camera.awb;
    doc["bpc"]           = appConfig.camera.bpc;
    doc["wpc"]           = appConfig.camera.wpc;
    doc["raw_gma"]       = appConfig.camera.raw_gma;
    doc["lenc"]          = appConfig.camera.lenc;

    // Motion
    doc["motion_enabled"]  = appConfig.motion.enabled;
    doc["motion_threshold"] = appConfig.motion.threshold;
    doc["motion_max_area"]  = appConfig.motion.max_area_pct;
    doc["motion_temporal_filter"] = appConfig.motion.temporal_filter;
    doc["motion_spatial_filter"]  = appConfig.motion.spatial_filter;
    doc["motion_night_suppress"]  = appConfig.motion.night_suppress;
    #ifdef INCLUDE_MOTION_DETECT
    doc["motion_detected"]    = isMotionDetected();
    doc["motion_event_count"] = getMotionEventCount();
    #endif

    // Face detection
    doc["face_detect_enabled"]   = appConfig.face_detect.enabled;
    doc["face_detect_two_stage"] = appConfig.face_detect.two_stage;
    doc["face_detect_cooldown"]  = appConfig.face_detect.cooldown_sec;
    doc["face_detect_save_sd"]   = appConfig.face_detect.save_to_sd;
    #if defined(INCLUDE_FACE_DETECT) && defined(INCLUDE_MOTION_DETECT)
    doc["face_detected"]    = isFaceDetected();
    doc["face_event_count"] = getFaceEventCount();
    FaceDetectResult faceRes = getFaceDetectResult();
    doc["face_count"]       = faceRes.face_count;
    doc["face_inference_ms"] = faceRes.inference_ms;
    #endif

    // Person detection (FOMO)
    doc["person_detect_enabled"]    = appConfig.person_detect.enabled;
    doc["person_detect_confidence"] = appConfig.person_detect.confidence_threshold;
    doc["person_detect_temporal"]   = appConfig.person_detect.temporal_frames;
    doc["person_detect_cooldown"]   = appConfig.person_detect.cooldown_sec;
    #ifdef INCLUDE_PERSON_DETECT
    doc["person_detected"]    = isPersonDetected();
    doc["person_event_count"] = getPersonEventCount();
    PersonDetectResult pdRes = getPersonDetectResult();
    doc["person_count"]       = pdRes.person_count;
    doc["person_inference_ms"] = pdRes.inference_ms;
    doc["person_track_count"]  = pdRes.track_count;
    #endif

    // Timelapse
    doc["timelapse_enabled"]  = appConfig.timelapse.enabled;
    doc["timelapse_interval"] = appConfig.timelapse.interval_sec;

    // MQTT
    doc["mqtt_enabled"] = appConfig.mqtt.enabled;
    #ifdef INCLUDE_MQTT
    doc["mqtt_connected"] = isMqttConnected();
    #endif

    // Telegram
    doc["telegram_enabled"]         = appConfig.telegram.enabled;
    #ifdef INCLUDE_TELEGRAM
    doc["telegram_connected"]       = isTelegramConnected();
    #else
    doc["telegram_connected"]       = false;
    #endif
    doc["telegram_notify_on_motion"] = appConfig.telegram.notify_on_motion;
    doc["telegram_notify_on_face"]   = appConfig.telegram.notify_on_face;
    doc["telegram_notify_on_person"] = appConfig.telegram.notify_on_person;
    doc["telegram_photo_on_motion"]  = appConfig.telegram.photo_on_motion;
    doc["telegram_photo_on_face"]    = appConfig.telegram.photo_on_face;
    doc["telegram_photo_on_person"]  = appConfig.telegram.photo_on_person;
    doc["telegram_cooldown"]         = appConfig.telegram.cooldown_sec;
    doc["telegram_active_start"]     = appConfig.telegram.active_start_hour;
    doc["telegram_active_end"]       = appConfig.telegram.active_end_hour;
    doc["telegram_poll_interval"]    = appConfig.telegram.poll_interval_ms;

    // LED
    doc["led_enabled"] = appConfig.led_enabled;

    // SD card status
    #ifdef INCLUDE_SD_CARD
    doc["sd_mounted"] = SD.cardType() != CARD_NONE;
    if (SD.cardType() != CARD_NONE) {
        doc["sd_total_mb"] = SD.totalBytes() / (1024 * 1024);
        doc["sd_used_mb"]  = SD.usedBytes() / (1024 * 1024);
    }
    #else
    doc["sd_mounted"] = false;
    #endif

    String result;
    serializeJson(doc, result);
    return result;
}

// API: GET /api/status
static void handleApiStatus(AsyncWebServerRequest* request) {
    request->send(200, "application/json", buildStatusJson());
}

// API: POST /api/settings - apply camera and app settings
static void handleApiSettings(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (index + len != total) return;  // Wait for complete body

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    bool needCameraApply = false;
    bool needSave = false;

    // Camera settings
    if (doc["frame_size"].is<int>())   { appConfig.camera.frame_size   = doc["frame_size"];   needCameraApply = true; }
    if (doc["jpeg_quality"].is<int>()) { appConfig.camera.jpeg_quality = doc["jpeg_quality"];  needCameraApply = true; }
    if (doc["vflip"].is<bool>())       { appConfig.camera.vflip        = doc["vflip"];         needCameraApply = true; }
    if (doc["hmirror"].is<bool>())     { appConfig.camera.hmirror      = doc["hmirror"];       needCameraApply = true; }
    if (doc["brightness"].is<int>())   { appConfig.camera.brightness   = doc["brightness"];    needCameraApply = true; }
    if (doc["contrast"].is<int>())     { appConfig.camera.contrast     = doc["contrast"];      needCameraApply = true; }
    if (doc["saturation"].is<int>())   { appConfig.camera.saturation   = doc["saturation"];    needCameraApply = true; }
    if (doc["sharpness"].is<int>())    { appConfig.camera.sharpness    = doc["sharpness"];     needCameraApply = true; }
    if (doc["denoise"].is<int>())      { appConfig.camera.denoise      = doc["denoise"];       needCameraApply = true; }
    if (doc["ae_level"].is<int>())     { appConfig.camera.ae_level     = doc["ae_level"];      needCameraApply = true; }
    if (doc["aec_value"].is<int>())    { appConfig.camera.aec_value    = doc["aec_value"];     needCameraApply = true; }
    if (doc["agc_gain"].is<int>())     { appConfig.camera.agc_gain     = doc["agc_gain"];      needCameraApply = true; }
    if (doc["gainceiling"].is<int>())  { appConfig.camera.gainceiling  = doc["gainceiling"];   needCameraApply = true; }
    if (doc["wb_mode"].is<int>())      { appConfig.camera.wb_mode      = doc["wb_mode"];       needCameraApply = true; }
    if (doc["aec"].is<bool>())         { appConfig.camera.aec          = doc["aec"];           needCameraApply = true; }
    if (doc["agc"].is<bool>())         { appConfig.camera.agc          = doc["agc"];           needCameraApply = true; }
    if (doc["awb"].is<bool>())         { appConfig.camera.awb          = doc["awb"];           needCameraApply = true; }
    if (doc["bpc"].is<bool>())         { appConfig.camera.bpc          = doc["bpc"];           needCameraApply = true; }
    if (doc["wpc"].is<bool>())         { appConfig.camera.wpc          = doc["wpc"];           needCameraApply = true; }
    if (doc["raw_gma"].is<bool>())     { appConfig.camera.raw_gma     = doc["raw_gma"];       needCameraApply = true; }
    if (doc["lenc"].is<bool>())        { appConfig.camera.lenc         = doc["lenc"];          needCameraApply = true; }

    // Motion settings
    if (doc["motion_enabled"].is<bool>())          { appConfig.motion.enabled          = doc["motion_enabled"];          needSave = true; }
    if (doc["motion_threshold"].is<int>())         { appConfig.motion.threshold        = doc["motion_threshold"];        needSave = true; }
    if (doc["motion_cooldown"].is<int>())          { appConfig.motion.cooldown_sec     = doc["motion_cooldown"];         needSave = true; }
    if (doc["motion_max_area"].is<int>())          { appConfig.motion.max_area_pct     = doc["motion_max_area"];         needSave = true; }
    if (doc["motion_temporal_filter"].is<bool>())  { appConfig.motion.temporal_filter  = doc["motion_temporal_filter"];  needSave = true; }
    if (doc["motion_spatial_filter"].is<bool>())   { appConfig.motion.spatial_filter   = doc["motion_spatial_filter"];   needSave = true; }
    if (doc["motion_night_suppress"].is<bool>())   { appConfig.motion.night_suppress   = doc["motion_night_suppress"];   needSave = true; }
    if (doc["motion_save_sd"].is<bool>())          { appConfig.motion.save_to_sd       = doc["motion_save_sd"];          needSave = true; }

    // Face detection settings
    if (doc["face_detect_enabled"].is<bool>())     { appConfig.face_detect.enabled         = doc["face_detect_enabled"];     needSave = true; }
    if (doc["face_detect_two_stage"].is<bool>())   { appConfig.face_detect.two_stage       = doc["face_detect_two_stage"];   needSave = true; }
    if (doc["face_detect_cooldown"].is<int>())     { appConfig.face_detect.cooldown_sec    = doc["face_detect_cooldown"];    needSave = true; }
    if (doc["face_detect_save_sd"].is<bool>())     { appConfig.face_detect.save_to_sd      = doc["face_detect_save_sd"];     needSave = true; }

    // Person detection
    if (doc["person_detect_enabled"].is<bool>())      { appConfig.person_detect.enabled              = doc["person_detect_enabled"];      needSave = true; }
    if (doc["person_detect_confidence"].is<float>())   { appConfig.person_detect.confidence_threshold  = doc["person_detect_confidence"];   needSave = true; }
    if (doc["person_detect_temporal"].is<int>())       { appConfig.person_detect.temporal_frames       = doc["person_detect_temporal"];     needSave = true; }
    if (doc["person_detect_cooldown"].is<int>())       { appConfig.person_detect.cooldown_sec          = doc["person_detect_cooldown"];     needSave = true; }

    // Timelapse
    if (doc["timelapse_enabled"].is<bool>()) { appConfig.timelapse.enabled      = doc["timelapse_enabled"];  needSave = true; }
    if (doc["timelapse_interval"].is<int>()) { appConfig.timelapse.interval_sec = doc["timelapse_interval"]; needSave = true; }

    // MQTT
    if (doc["mqtt_enabled"].is<bool>())        { appConfig.mqtt.enabled      = doc["mqtt_enabled"];      needSave = true; }
    if (doc["mqtt_server"].is<const char*>())   { appConfig.mqtt.server       = doc["mqtt_server"].as<String>(); needSave = true; }
    if (doc["mqtt_port"].is<int>())             { appConfig.mqtt.port         = doc["mqtt_port"];         needSave = true; }
    if (doc["mqtt_topic_prefix"].is<const char*>()) { appConfig.mqtt.topic_prefix = doc["mqtt_topic_prefix"].as<String>(); needSave = true; }

    // Telegram
    if (doc["telegram_enabled"].is<bool>())           { appConfig.telegram.enabled           = doc["telegram_enabled"];           needSave = true; }
    if (doc["telegram_notify_on_motion"].is<bool>())  { appConfig.telegram.notify_on_motion  = doc["telegram_notify_on_motion"];  needSave = true; }
    if (doc["telegram_notify_on_face"].is<bool>())    { appConfig.telegram.notify_on_face    = doc["telegram_notify_on_face"];    needSave = true; }
    if (doc["telegram_photo_on_motion"].is<bool>())   { appConfig.telegram.photo_on_motion   = doc["telegram_photo_on_motion"];   needSave = true; }
    if (doc["telegram_photo_on_face"].is<bool>())     { appConfig.telegram.photo_on_face     = doc["telegram_photo_on_face"];     needSave = true; }
    if (doc["telegram_notify_on_person"].is<bool>())   { appConfig.telegram.notify_on_person  = doc["telegram_notify_on_person"];  needSave = true; }
    if (doc["telegram_photo_on_person"].is<bool>())   { appConfig.telegram.photo_on_person   = doc["telegram_photo_on_person"];   needSave = true; }
    if (doc["telegram_cooldown"].is<int>())           { appConfig.telegram.cooldown_sec      = doc["telegram_cooldown"];          needSave = true; }
    if (doc["telegram_active_start"].is<int>())       { appConfig.telegram.active_start_hour = doc["telegram_active_start"];      needSave = true; }
    if (doc["telegram_active_end"].is<int>())         { appConfig.telegram.active_end_hour   = doc["telegram_active_end"];        needSave = true; }
    if (doc["telegram_poll_interval"].is<int>())      { appConfig.telegram.poll_interval_ms  = doc["telegram_poll_interval"];     needSave = true; }
    if (doc["telegram_bot_token"].is<const char*>())  {
        appConfig.telegram.bot_token = doc["telegram_bot_token"].as<String>();
        saveSecretsToNVS();
    }
    if (doc["telegram_chat_id"].is<const char*>())    {
        appConfig.telegram.chat_id = doc["telegram_chat_id"].as<String>();
        saveSecretsToNVS();
    }

    // LED
    if (doc["led_enabled"].is<bool>()) { appConfig.led_enabled = doc["led_enabled"]; needSave = true; }

    // Frame rates
    if (doc["idle_fps"].is<int>())   { appConfig.idle_fps   = doc["idle_fps"];   needSave = true; }
    if (doc["active_fps"].is<int>()) { appConfig.active_fps = doc["active_fps"]; needSave = true; }

    if (needCameraApply) {
        applyConfigToCamera();
        needSave = true;
    }

    if (needSave) {
        saveConfig();
    }

    request->send(200, "application/json", "{\"success\":true,\"message\":\"Settings applied\"}");

    // Notify WebSocket clients
    ws.textAll(buildStatusJson());
}

// API: GET /api/snapshot - returns JPEG
static void handleApiSnapshot(AsyncWebServerRequest* request) {
    const uint8_t* buf = NULL;
    size_t len = 0;

    if (ringBufferGetLatest(&buf, &len)) {
        // Copy data because ring buffer must be released before async send completes
        uint8_t* copy = (uint8_t*)malloc(len);
        if (copy) {
            memcpy(copy, buf, len);
            ringBufferRelease();

            AsyncWebServerResponse* response = request->beginResponse(
                "image/jpeg", len,
                [copy, len](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
                    size_t remaining = len - index;
                    size_t toSend = min(remaining, maxLen);
                    memcpy(buffer, copy + index, toSend);
                    if (index + toSend >= len) {
                        free(copy);
                    }
                    return toSend;
                }
            );
            response->addHeader("Content-Disposition", "inline; filename=snapshot.jpg");
            response->addHeader("Cache-Control", "no-cache");
            request->send(response);
        } else {
            ringBufferRelease();
            request->send(503, "application/json", "{\"error\":\"Memory allocation failed\"}");
        }
    } else {
        request->send(503, "application/json", "{\"error\":\"No frame available\"}");
    }
}

// API: POST /api/wifi - connect to network
static void handleApiWifi(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (index + len != total) return;

    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    String ssid = doc["ssid"] | "";
    String pass = doc["password"] | "";

    if (ssid.length() == 0) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"SSID required\"}");
        return;
    }

    request->send(200, "application/json", "{\"success\":true,\"message\":\"Connecting...\"}");

    // Connect after response is sent (delay needed)
    delay(500);
    wifiConnect(ssid, pass);
}

// API: GET /api/wifi/scan
static void handleApiWifiScan(AsyncWebServerRequest* request) {
    request->send(200, "application/json", scanNetworksJson());
}

// API: POST /api/reboot
static void handleApiReboot(AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Rebooting...\"}");
    // Schedule restart after response is sent
    request->onDisconnect([]() {
        delay(200);
        ESP.restart();
    });
}

// API: POST /api/reset
static void handleApiReset(AsyncWebServerRequest* request) {
    resetConfig();
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Factory reset, rebooting...\"}");
    request->onDisconnect([]() {
        delay(200);
        ESP.restart();
    });
}

// API: POST /api/camera/reinit
static void handleApiCameraReinit(AsyncWebServerRequest* request) {
    bool ok = cameraReinit();
    if (ok) {
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Camera reinitialized\"}");
    } else {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Camera reinit failed\"}");
    }
}

// WebSocket events
static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("[%s] WS client connected: %u\n", TAG, client->id());
        client->text(buildStatusJson());
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("[%s] WS client disconnected: %u\n", TAG, client->id());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            // Handle incoming WS commands if needed
            data[len] = 0;
            Serial.printf("[%s] WS message: %s\n", TAG, (char*)data);
        }
    }
}

#ifdef INCLUDE_SD_CARD
// API: GET /api/sd/list?path=/
static void handleSdList(AsyncWebServerRequest* request) {
    String path = request->hasParam("path") ? request->getParam("path")->value() : "/";
    File root = SD.open(path);
    if (!root || !root.isDirectory()) {
        request->send(404, "application/json", "{\"error\":\"Directory not found\"}");
        return;
    }

    JsonDocument doc;
    JsonArray files = doc.to<JsonArray>();

    File entry;
    while ((entry = root.openNextFile())) {
        JsonObject f = files.add<JsonObject>();
        f["name"] = String(entry.name());
        f["size"] = entry.size();
        f["dir"]  = entry.isDirectory();
        entry.close();
    }
    root.close();

    String result;
    serializeJson(doc, result);
    request->send(200, "application/json", result);
}

// API: GET /api/sd/download?file=/path
static void handleSdDownload(AsyncWebServerRequest* request) {
    if (!request->hasParam("file")) {
        request->send(400, "application/json", "{\"error\":\"file param required\"}");
        return;
    }
    String path = request->getParam("file")->value();
    if (!SD.exists(path)) {
        request->send(404, "application/json", "{\"error\":\"File not found\"}");
        return;
    }
    request->send(SD, path, "application/octet-stream");
}

// API: DELETE /api/sd/delete?file=/path
static void handleSdDelete(AsyncWebServerRequest* request) {
    if (!request->hasParam("file")) {
        request->send(400, "application/json", "{\"error\":\"file param required\"}");
        return;
    }
    String path = request->getParam("file")->value();
    if (SD.remove(path)) {
        request->send(200, "application/json", "{\"success\":true}");
    } else {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Delete failed\"}");
    }
}

static void setupSdRoutes() {
    server.on("/api/sd/list", HTTP_GET, handleSdList);
    server.on("/api/sd/download", HTTP_GET, handleSdDownload);
    server.on("/api/sd/delete", HTTP_DELETE, handleSdDelete);
}
#endif

// API: GET /api/motion/debug - motion detection debug info
#ifdef INCLUDE_MOTION_DETECT
static void handleApiMotionDebug(AsyncWebServerRequest* request) {
    MotionDebugInfo info = getMotionDebugInfo();
    JsonDocument doc;
    doc["changed_pct"]         = info.changed_pct;
    doc["avg_brightness"]      = info.avg_brightness;
    doc["sensor_gain"]         = info.sensor_gain;
    doc["adaptive_threshold"]  = info.adaptive_threshold;
    doc["active_blocks"]       = info.active_blocks;
    doc["clustered_blocks"]    = info.clustered_blocks;
    doc["consecutive_frames"]  = info.consecutive_frames;
    doc["night_mode"]          = info.night_mode;
    doc["training"]            = info.training;
    doc["decode_ms"]           = info.decode_ms;
    doc["analysis_ms"]         = info.analysis_ms;
    doc["motion_detected"]     = isMotionDetected();
    doc["event_count"]         = getMotionEventCount();
    String result;
    serializeJson(doc, result);
    request->send(200, "application/json", result);
}

static void handleApiMotionReset(AsyncWebServerRequest* request) {
    motionDetectResetBackground();
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Background model reset\"}");
}
#endif

// API: GET /log - plaintext log output
static void handleLog(AsyncWebServerRequest* request) {
    char* logs = logGetAll();
    if (logs) {
        request->send(200, "text/plain", logs);
        free(logs);
    } else {
        request->send(503, "text/plain", "Log buffer unavailable");
    }
}

// API: GET /log-viewer - HTML page with auto-refresh
static void handleLogViewer(AsyncWebServerRequest* request) {
    static const char LOG_VIEWER_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>CamS3 Log</title>
<style>
body{background:#1a1a2e;color:#0f0;font:13px/1.4 monospace;margin:0;padding:10px}
pre{white-space:pre-wrap;word-wrap:break-word}
h2{color:#e94560;margin:0 0 10px}
.info{color:#888;font-size:11px}
</style></head><body>
<h2>CamS3 Live Log</h2>
<p class="info">Auto-refresh: 5s | Lines: <span id="cnt">-</span></p>
<pre id="log">Loading...</pre>
<script>
function r(){fetch('/log').then(r=>r.text()).then(t=>{
document.getElementById('log').textContent=t;
var l=t.split('\n').filter(x=>x).length;
document.getElementById('cnt').textContent=l;
window.scrollTo(0,document.body.scrollHeight);
}).catch(e=>{document.getElementById('log').textContent='Error: '+e})}
r();setInterval(r,5000);
</script></body></html>
)rawliteral";
    request->send(200, "text/html", LOG_VIEWER_HTML);
}

static void setupApiRoutes() {
    // CORS headers for all
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/snapshot", HTTP_GET, handleApiSnapshot);
    server.on("/api/wifi/scan", HTTP_GET, handleApiWifiScan);
    server.on("/api/reboot", HTTP_POST, handleApiReboot);
    server.on("/api/reset", HTTP_POST, handleApiReset);
    server.on("/api/camera/reinit", HTTP_POST, handleApiCameraReinit);

    // Settings (POST with JSON body)
    server.on("/api/settings", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        NULL,
        handleApiSettings
    );

    // WiFi connect (POST with JSON body)
    server.on("/api/wifi", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        NULL,
        handleApiWifi
    );

    // Simple GET endpoints (backward compat with reference project)
    server.on("/status", HTTP_GET, handleApiStatus);
    server.on("/frame", HTTP_GET, handleApiSnapshot);
    server.on("/snapshot", HTTP_GET, handleApiSnapshot);

    // Motion detection API
    #ifdef INCLUDE_MOTION_DETECT
    server.on("/api/motion/debug", HTTP_GET, handleApiMotionDebug);
    server.on("/api/motion/reset", HTTP_POST, handleApiMotionReset);
    #endif

    // Log viewer
    server.on("/log", HTTP_GET, handleLog);
    server.on("/log-viewer", HTTP_GET, handleLogViewer);

    #ifdef INCLUDE_SD_CARD
    setupSdRoutes();
    #endif
}

static void setupWebSocket() {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
}

static void setupStaticFiles() {
    // Serve web UI from LittleFS
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

    // Handle captive portal redirects
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->redirect("http://" + getIPAddress() + "/");
    });
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->redirect("http://" + getIPAddress() + "/");
    });
    server.on("/fwlink", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->redirect("http://" + getIPAddress() + "/");
    });

    // 404 handler - redirect to index in AP mode
    server.onNotFound([](AsyncWebServerRequest* request) {
        if (isCaptivePortalActive()) {
            request->redirect("http://" + getIPAddress() + "/");
        } else {
            request->send(404, "text/plain", "Not Found");
        }
    });
}

void webServerInit() {
    setupApiRoutes();
    setupWebSocket();
    setupStaticFiles();

    #ifdef INCLUDE_OTA
    ElegantOTA.begin(&server);
    Serial.printf("[%s] OTA enabled at /update\n", TAG);
    #endif

    server.begin();
    Serial.printf("[%s] Web server started on port %d\n", TAG, HTTP_PORT);
}

void webServerStop() {
    server.end();
    Serial.printf("[%s] Web server stopped\n", TAG);
}

// Call periodically from main loop to broadcast status via WebSocket
void webSocketBroadcastStatus() {
    static unsigned long lastBroadcast = 0;
    if (millis() - lastBroadcast > 2000 && ws.count() > 0) {
        lastBroadcast = millis();
        ws.textAll(buildStatusJson());
        ws.cleanupClients();
    }
}
