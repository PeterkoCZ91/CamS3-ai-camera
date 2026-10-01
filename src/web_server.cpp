#include "web_server.h"
#include "config.h"
#include "camera_manager.h"
#include "wifi_manager.h"
#include "board_config.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

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
#include "sd_store.h"
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

#ifdef INCLUDE_ZONES
#include "zone_manager.h"
#endif

#ifdef INCLUDE_EVENT_LOG
#include "event_log.h"
#endif

#include "ws_log.h"
#include "system_stats.h"

static const char* TAG = "WebSrv";
static AsyncWebServer server(HTTP_PORT);
static AsyncWebSocket ws("/ws");

// Forward declarations
static void setupApiRoutes();
static void setupWebSocket();
static void setupStaticFiles();
extern void saveSecretsToNVS();

// HTTP Basic Auth check for sensitive endpoints.
// Returns true when authorized; otherwise sends 401 and returns false.
static bool requireAuth(AsyncWebServerRequest* request) {
    const String& user = appConfig.auth.http_user;
    const String& pass = appConfig.auth.http_pass;
    // Empty credentials = auth disabled (keeps local dev simple; production should set them).
    if (user.length() == 0 || pass.length() == 0) return true;
    if (!request->authenticate(user.c_str(), pass.c_str())) {
        request->requestAuthentication();
        return false;
    }
    return true;
}

// Session-lifetime CSRF token. Rotates on reboot; held in RAM only.
static char csrfToken[33] = {0};
static void csrfGenerate() {
    const char* alpha = "0123456789abcdef";
    for (int i = 0; i < 32; i++) csrfToken[i] = alpha[esp_random() & 0x0F];
    csrfToken[32] = 0;
}

// Returns true when CSRF is disabled OR the request carries the right X-CSRF-Token header.
// When required and missing/wrong: sends 403 and returns false.
static bool requireCsrf(AsyncWebServerRequest* request) {
    if (!appConfig.auth.csrf_required) return true;

    // The legacy endpoints (/settings, /record, /ir-control, /save_frame) predate
    // /api/csrf and are used by scripted integrations that cannot fetch a token.
    // Exempting them wholesale left a real CSRF hole, because a browser that has
    // cached Basic Auth for this device will happily submit a cross-site form POST.
    //
    // Split the difference on origin: browsers always attach Origin (and modern ones
    // Sec-Fetch-Site) to a cross-origin POST; curl, requests and similar clients
    // send neither. So token-less requests are accepted only when they clearly did
    // not come from a page in someone's browser.
    if (!request->url().startsWith("/api/")) {
        bool browserOriginated = request->hasHeader("Origin") ||
                                 request->hasHeader("Sec-Fetch-Site");
        if (!browserOriginated) return true;
    }
    if (!request->hasHeader("X-CSRF-Token")) {
        request->send(403, "application/json", "{\"error\":\"CSRF token missing\"}");
        return false;
    }
    if (request->header("X-CSRF-Token") != String(csrfToken)) {
        request->send(403, "application/json", "{\"error\":\"CSRF token invalid\"}");
        return false;
    }
    return true;
}

#ifdef INCLUDE_SD_CARD
static void setupSdRoutes();
#endif

// Firmware identity — lazily computed once per boot. SHA-256 of the running
// OTA partition lets the operator verify an upload matches their build output.
// Separate from any build-time elf hash (esp_app_desc_t::app_elf_sha256, which
// covers the ELF, not the flashed .bin).
static String fwSha256Hex;
static String fwAppVersion;
static String fwCompileTime;

static String bytesToHex(const uint8_t* buf, size_t len) {
    static const char* hex = "0123456789abcdef";
    String out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out += hex[(buf[i] >> 4) & 0xF];
        out += hex[buf[i] & 0xF];
    }
    return out;
}

static void computeRunningFirmwareIdentity() {
    if (fwSha256Hex.length() > 0) return;  // already computed

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        uint8_t sha[32] = {0};
        if (esp_partition_get_sha256(running, sha) == ESP_OK) {
            fwSha256Hex = bytesToHex(sha, 32);
        }
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(running, &desc) == ESP_OK) {
            fwAppVersion  = String(desc.version);
            fwCompileTime = String(desc.date) + " " + String(desc.time);
        }
    }
}

// Post-OTA callback: compute SHA-256 of the newly-flashed partition and log it
// so the operator can compare against their build output (fwVerify.py etc.).
// Called on the async task context after Update.end() succeeds — safe to read flash.
static void logNewFirmwareSha() {
    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        logCapture("[%s] OTA: cannot locate next update partition\n", TAG);
        return;
    }
    uint8_t sha[32] = {0};
    if (esp_partition_get_sha256(next, sha) != ESP_OK) {
        logCapture("[%s] OTA: SHA-256 computation failed\n", TAG);
        return;
    }
    String hex = bytesToHex(sha, 32);
    logCapture("[%s] OTA complete — new partition SHA-256: %s\n", TAG, hex.c_str());
}

static int clampInt(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

static float clampFloat(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

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

static uint32_t getFrameAgeMs() {
    // Age of the last frame actually published to the ring (what streams and
    // detectors can see), not of the last sensor grab: oversized or dropped frames
    // advance lastCaptureMs but never reach a consumer.
    uint32_t last = getLastPublishedMs();
    if (last == 0) return 0xFFFFFFFF;
    uint32_t now = millis();
    return now >= last ? (now - last) : 0;
}

static String httpBaseUrl() {
    return "http://" + getIPAddress();
}

static String streamBaseUrl() {
    return httpBaseUrl() + ":" + String(STREAM_PORT);
}

// Build status JSON (shared by /api/status and WebSocket)
static String buildStatusJson() {
    JsonDocument doc;

    // System info
    computeRunningFirmwareIdentity();
    doc["version"]     = FIRMWARE_VERSION;
    doc["fw_sha256"]   = fwSha256Hex;
    doc["fw_version"]  = fwAppVersion;
    doc["fw_built"]    = fwCompileTime;
    doc["device"]      = DEVICE_NAME;
    doc["uptime"]      = formatUptime();
    doc["uptime_sec"]  = millis() / 1000;
    doc["free_heap"]   = ESP.getFreeHeap();
    doc["free_psram"]  = ESP.getFreePsram();
    // UI banner hint: true while http_pass is still the compiled-in default.
    doc["default_password"] = (appConfig.auth.http_pass == String(DEFAULT_HTTP_PASS));
    doc["total_psram"] = ESP.getPsramSize();
    doc["cpu_freq"]    = ESP.getCpuFreqMHz();
    doc["flash_size"]  = ESP.getFlashChipSize();
    doc["sdk"]         = ESP.getSdkVersion();
    // Restart history + reset reason: tells you whether this uptime is the first
    // attempt or the thirty-first.
    systemStatsToJson(doc);

    // WiFi
    doc["ip"]   = getIPAddress();
    doc["rssi"] = getRSSI();
    doc["mac"]  = getMACAddress();
    doc["wifi_mode"] = isWiFiConnected() ? "STA" : "AP";
    doc["hostname"]  = appConfig.wifi.hostname;

    // Camera
    int streamClients = getStreamClientCount();
    int detectionClients = getDetectionStreamClientCount();
    doc["capture_fps"]    = getCaptureFps();
    doc["capture_count"]  = getCaptureCount();
    doc["capture_errors"] = getCaptureErrors();
    doc["ring_dropped"]   = getRingDroppedFrames();
    doc["last_capture_ms"] = getLastCaptureMs();
    doc["frame_age_ms"]   = getFrameAgeMs();
    doc["stream_clients"] = streamClients;
    doc["detection_clients"] = detectionClients;
    doc["total_stream_clients"] = streamClients + detectionClients;
    doc["stream_url"] = streamBaseUrl() + "/stream";
    doc["detection_stream_url"] = streamBaseUrl() + "/detection-stream";
    doc["snapshot_url"] = httpBaseUrl() + "/frame";

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
    doc["aec2"]          = appConfig.camera.aec2;
    doc["agc"]           = appConfig.camera.agc;
    doc["awb"]           = appConfig.camera.awb;
    doc["bpc"]           = appConfig.camera.bpc;
    doc["wpc"]           = appConfig.camera.wpc;
    doc["raw_gma"]       = appConfig.camera.raw_gma;
    doc["lenc"]          = appConfig.camera.lenc;

    // Motion
    doc["motion_enabled"]  = appConfig.motion.enabled;
    doc["motion_threshold"] = appConfig.motion.threshold;
    doc["motion_cooldown"]  = appConfig.motion.cooldown_sec;
    doc["motion_max_area"]  = appConfig.motion.max_area_pct;
    doc["motion_min_area"]  = appConfig.motion.min_area_pct;
    doc["motion_ema_alpha_day"]   = appConfig.motion.ema_alpha_day;
    doc["motion_ema_alpha_night"] = appConfig.motion.ema_alpha_night;
    doc["motion_training_frames"] = appConfig.motion.training_frames;
    doc["motion_agc_gain_factor"] = appConfig.motion.agc_gain_factor;
    doc["motion_brightness_min"]  = appConfig.motion.brightness_min;
    doc["motion_temporal_filter"] = appConfig.motion.temporal_filter;
    doc["motion_spatial_filter"]  = appConfig.motion.spatial_filter;
    doc["motion_night_suppress"]  = appConfig.motion.night_suppress;
    doc["motion_save_sd"] = appConfig.motion.save_to_sd;
    #ifdef INCLUDE_MOTION_DETECT
    doc["motion_detected"]    = isMotionDetected();
    doc["motion_event_count"] = getMotionEventCount();
    #endif

    // Face detection
    doc["face_detect_enabled"]   = appConfig.face_detect.enabled;
    doc["face_detect_two_stage"] = appConfig.face_detect.two_stage;
    doc["face_detect_cooldown"]  = appConfig.face_detect.cooldown_sec;
    doc["face_detect_save_sd"]   = appConfig.face_detect.save_to_sd;
    doc["face_score_threshold"]  = appConfig.face_detect.score_threshold;
    doc["face_nms_threshold"]    = appConfig.face_detect.nms_threshold;
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
    doc["person_detect_save_sd"]    = appConfig.person_detect.save_to_sd;
    doc["person_confident_threshold"] = appConfig.person_detect.confident_threshold;
    doc["tracker_confirm_hits"]     = appConfig.tracker.confirm_hits;
    doc["tracker_max_misses"]       = appConfig.tracker.max_misses;
    doc["tracker_match_dist"]       = appConfig.tracker.match_dist;
    #ifdef INCLUDE_PERSON_DETECT
    doc["person_detected"]    = isPersonDetected();
    doc["person_event_count"] = getPersonEventCount();
    PersonDetectResult pdRes = getPersonDetectResult();
    doc["person_count"]       = pdRes.person_count;
    doc["person_inference_ms"] = pdRes.inference_ms;
    doc["person_track_count"]  = pdRes.track_count;
    doc["person_top_score"]    = pdRes.top_score;
    doc["person_decision"]     = personDecisionToString(pdRes.decision);
    #endif

    // Timelapse
    doc["timelapse_enabled"]  = appConfig.timelapse.enabled;
    doc["timelapse_interval"] = appConfig.timelapse.interval_sec;
    doc["timelapse_save_sd"]  = appConfig.timelapse.save_to_sd;

    // MQTT
    doc["mqtt_enabled"] = appConfig.mqtt.enabled;
    doc["mqtt_server"] = appConfig.mqtt.server;
    doc["mqtt_port"] = appConfig.mqtt.port;
    doc["mqtt_topic_prefix"] = appConfig.mqtt.topic_prefix;
    doc["mqtt_tls_enabled"] = appConfig.mqtt.tls_enabled;
    doc["mqtt_user"] = appConfig.mqtt.user;            // username is not a secret
    doc["mqtt_pass_set"] = appConfig.mqtt.password.length() > 0;
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
    // Lets the UI show "token stored" without ever shipping the token itself.
    doc["telegram_token_set"]        = appConfig.telegram.bot_token.length() > 0;
    doc["telegram_chat_id_set"]      = appConfig.telegram.chat_id.length() > 0;

    // LED
    doc["led_enabled"] = appConfig.led_enabled;
    doc["active_fps"] = appConfig.active_fps;
    doc["idle_fps"] = appConfig.idle_fps;

    // Compatibility aliases for the DFR1154 v3.12.x embedded GUI and A12 tools.
    doc["uptime_seconds"] = millis() / 1000;
    doc["max_alloc_heap"] = ESP.getMaxAllocHeap();
    doc["flash_used"] = ESP.getSketchSize();
    doc["flash_usage_pct"] = ESP.getFlashChipSize() > 0
        ? ((float)ESP.getSketchSize() / (float)ESP.getFlashChipSize()) * 100.0f : 0.0f;
    doc["wifi_rssi"] = getRSSI();
    doc["wifi_channel"] = WiFi.channel();
    doc["clients"] = streamClients;
    doc["detection_clients"] = detectionClients;
    doc["stream_fps"] = getCaptureFps();
    doc["chip_temp_c"] = temperatureRead();
    doc["psram_usage_pct"] = ESP.getPsramSize() > 0
        ? ((float)(ESP.getPsramSize() - ESP.getFreePsram()) / (float)ESP.getPsramSize()) * 100.0f : 0.0f;

    doc["motion_detection_enabled"] = appConfig.motion.enabled;
    doc["motion_telegram_photo"] = appConfig.telegram.photo_on_motion;
    doc["motion_telegram_video"] = false;
    doc["motion_telegram_cooldown"] = appConfig.motion.cooldown_sec;
    doc["motion_notify_start_hour"] = appConfig.telegram.active_start_hour;
    doc["motion_notify_end_hour"] = appConfig.telegram.active_end_hour;

    doc["person_detection_enabled"] = appConfig.person_detect.enabled;
    doc["person_telegram_photo"] = appConfig.telegram.photo_on_person;
    doc["person_confidence_threshold"] = appConfig.person_detect.confidence_threshold;
    doc["person_detection_cooldown"] = appConfig.person_detect.cooldown_sec;
    doc["person_recheck_interval"] = appConfig.person_detect.temporal_frames;
    #ifdef INCLUDE_PERSON_DETECT
    doc["person_last_detected"] = pdRes.detected;
    doc["person_last_confidence"] = pdRes.top_score;
    doc["person_last_detections"] = pdRes.raw_detections;
    doc["person_last_inference_ms"] = pdRes.inference_ms;
    doc["person_last_decision"] = personDecisionToString(pdRes.decision);
    #else
    doc["person_last_detected"] = false;
    doc["person_last_confidence"] = 0.0f;
    doc["person_last_detections"] = 0;
    doc["person_last_inference_ms"] = 0;
    doc["person_last_decision"] = "NONE";
    #endif

    float ambient = 0.0f;
    #ifdef INCLUDE_MOTION_DETECT
    MotionDebugInfo mdi = getMotionDebugInfo();
    ambient = mdi.avg_brightness;
    #endif
    doc["ambient_light_lux"] = ambient;
    doc["camera_profile"] = ambient < 15.0f ? "NIGHT" : (ambient < 40.0f ? "DUSK" : "DAY");
    doc["ir_led"] = false;
    doc["is_recording"] = false;
    doc["telegram_queue_depth"] = 0;
    doc["telegram_queue_ready"] = true;
    doc["telegram_task_ready"] = true;
    doc["telegram_uploading"] = false;
    doc["telegram_sent"] = 0;
    doc["telegram_fail"] = 0;
    doc["telegram_drops"] = 0;

    // SD card status
    #ifdef INCLUDE_SD_CARD
    doc["sd_mounted"] = SD.cardType() != CARD_NONE;
    if (SD.cardType() != CARD_NONE) {
        doc["sd_total_mb"] = SD.totalBytes() / (1024 * 1024);
        doc["sd_used_mb"]  = SD.usedBytes() / (1024 * 1024);
        doc["sd_usage_pct"] = sdStoreUsagePercent();
    }
    // Surfaced so a card that stopped accepting writes is visible in the UI rather
    // than looking like "detection just stopped saving anything".
    doc["sd_write_disabled"] = sdStoreDisabled();
    doc["sd_write_failures"] = sdStoreFailureCount();
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

// API: GET /api/sensor[?reg=0xNNNN]
// Read-only. Registers are sampled by the capture task (never here, SCCB is not
// ours to touch); this handler only copies the cache. ?reg= queues a one-shot read
// that the capture task services on its next frame. The handler cannot wait for it
// without blocking the async server, so it answers at once with the last cached
// one-shot result and "reg_pending":true; poll again to get the fresh value.
static void handleApiSensor(AsyncWebServerRequest* request) {
    bool wantReg = request->hasParam("reg");
    if (wantReg) {
        if (!requireAuth(request)) return;
        String arg = request->getParam("reg")->value();
        char* end = nullptr;
        unsigned long r = strtoul(arg.c_str(), &end, 16);  // accepts "0x" prefix
        if (arg.length() == 0 || arg.length() > 6 || *end != '\0' || r > 0xFFFF) {
            request->send(400, "application/json",
                          "{\"success\":false,\"message\":\"reg must be hex 0x0000-0xFFFF\"}");
            return;
        }
        cameraSensorViewRequestReg((uint16_t)r);
    }

    SensorView v;
    if (!cameraSensorViewGet(&v)) {
        request->send(503, "application/json",
                      "{\"success\":false,\"message\":\"no sensor sample yet\"}");
        return;
    }
    char b[8];
    JsonDocument doc;
    doc["frame"] = v.frame;
    doc["semantics_verified"] = false;
    JsonObject id = doc["id"].to<JsonObject>();
    snprintf(b, sizeof b, "0x%04X", v.pid);  id["pid"]  = String(b);
    snprintf(b, sizeof b, "0x%02X", v.ver);  id["ver"]  = String(b);
    snprintf(b, sizeof b, "0x%02X", v.midh); id["midh"] = String(b);
    snprintf(b, sizeof b, "0x%02X", v.midl); id["midl"] = String(b);
    JsonObject regs = doc["regs"].to<JsonObject>();
    for (int i = 0; i < SENSOR_VIEW_REGS; i++) {
        char k[8];
        snprintf(k, sizeof k, "0x%04X", cameraSensorViewRegAddr(i));
        if (v.regs[i] == 0xFFFF) { regs[k] = nullptr; }
        else { snprintf(b, sizeof b, "0x%02X", v.regs[i]); regs[k] = String(b); }
    }
    if (wantReg || v.custom_set) {
        JsonObject c = doc["reg"].to<JsonObject>();
        snprintf(b, sizeof b, "0x%04X", v.custom_reg);
        c["addr"] = String(b);
        if (v.custom_set && v.custom_val != 0xFFFF) {
            snprintf(b, sizeof b, "0x%02X", v.custom_val); c["value"] = String(b);
        } else { c["value"] = nullptr; }
        c["frame"] = v.custom_frame;
        c["reg_pending"] = cameraSensorViewOneShotPending();
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// Largest JSON body we accept on a POST. The settings payload is a couple of kB;
// the cap exists because `total` comes straight from a client-supplied
// Content-Length, and malloc'ing that unbounded is a one-request OOM.
#define MAX_JSON_BODY 16384

// Accumulate a chunked request body into request->_tempObject.
// Returns 1 when the body is complete, 0 when more chunks are pending, and -1 on
// error (a response has already been sent in that case).
static int accumulateJsonBody(AsyncWebServerRequest* request, uint8_t* data,
                              size_t len, size_t index, size_t total) {
    if (index == 0) {
        if (request->_tempObject) { free(request->_tempObject); request->_tempObject = NULL; }
        if (total == 0 || total > MAX_JSON_BODY) {
            request->send(413, "application/json", "{\"success\":false,\"message\":\"Body too large\"}");
            return -1;
        }
        request->_tempObject = malloc(total);
    }
    if (!request->_tempObject) {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"OOM\"}");
        return -1;
    }
    if (index + len > total) {  // client contradicting its own Content-Length
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Bad body length\"}");
        return -1;
    }
    memcpy((uint8_t*)request->_tempObject + index, data, len);
    return (index + len == total) ? 1 : 0;
}

// API: POST /api/settings - apply camera and app settings
static void handleApiSettings(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;

    // AsyncWebServer calls this per TCP chunk; the settings payload exceeds one MTU.
    if (accumulateJsonBody(request, data, len, index, total) != 1) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, (const uint8_t*)request->_tempObject, total);
    // _tempObject is auto-freed by AsyncWebServer when the request ends; don't double free.

    if (err) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    bool needCameraApply = false;
    bool needSave = false;

    // Keys present in the body but not applied (unknown name or wrong type) are
    // reported back in `ignored`; `ok` records every key a branch below accepted.
    JsonDocument appliedKeys;
    auto ok = [&appliedKeys](const char* key, bool match) {
        if (match) appliedKeys[key] = true;
        return match;
    };

    // Camera settings. Everything numeric is clamped to the range the sensor driver
    // and the detection buffers actually accept — these values arrive from the
    // network and used to be written through to the driver unchecked.
    int prevFrameSize = appConfig.camera.frame_size;
    if (ok("frame_size", doc["frame_size"].is<int>()))   { appConfig.camera.frame_size   = clampInt(doc["frame_size"], FRAMESIZE_QVGA, FRAMESIZE_UXGA); needCameraApply = true; }
    if (ok("jpeg_quality", doc["jpeg_quality"].is<int>())) { appConfig.camera.jpeg_quality = clampInt(doc["jpeg_quality"], 4, 63);  needCameraApply = true; }
    if (ok("vflip", doc["vflip"].is<bool>()))       { appConfig.camera.vflip        = doc["vflip"];         needCameraApply = true; }
    if (ok("hmirror", doc["hmirror"].is<bool>()))     { appConfig.camera.hmirror      = doc["hmirror"];       needCameraApply = true; }
    if (ok("brightness", doc["brightness"].is<int>()))   { appConfig.camera.brightness   = clampInt(doc["brightness"], -2, 2);    needCameraApply = true; }
    if (ok("contrast", doc["contrast"].is<int>()))     { appConfig.camera.contrast     = clampInt(doc["contrast"], -2, 2);      needCameraApply = true; }
    if (ok("saturation", doc["saturation"].is<int>()))   { appConfig.camera.saturation   = clampInt(doc["saturation"], -2, 2);    needCameraApply = true; }
    if (ok("sharpness", doc["sharpness"].is<int>()))    { appConfig.camera.sharpness    = clampInt(doc["sharpness"], -3, 3);     needCameraApply = true; }
    if (ok("denoise", doc["denoise"].is<int>()))      { appConfig.camera.denoise      = clampInt(doc["denoise"], 0, 8);        needCameraApply = true; }
    if (ok("ae_level", doc["ae_level"].is<int>()))     { appConfig.camera.ae_level     = clampInt(doc["ae_level"], -2, 2);      needCameraApply = true; }
    if (ok("aec_value", doc["aec_value"].is<int>()))    { appConfig.camera.aec_value    = clampInt(doc["aec_value"], 0, 1200);   needCameraApply = true; }
    if (ok("agc_gain", doc["agc_gain"].is<int>()))     { appConfig.camera.agc_gain     = clampInt(doc["agc_gain"], 0, 30);      needCameraApply = true; }
    if (ok("gainceiling", doc["gainceiling"].is<int>()))  { appConfig.camera.gainceiling  = clampInt(doc["gainceiling"], 0, 6);    needCameraApply = true; }
    if (ok("wb_mode", doc["wb_mode"].is<int>()))      { appConfig.camera.wb_mode      = clampInt(doc["wb_mode"], 0, 4);        needCameraApply = true; }
    if (ok("aec2", doc["aec2"].is<bool>()))            { appConfig.camera.aec2         = doc["aec2"];          needCameraApply = true; }
    if (ok("aec", doc["aec"].is<bool>()))         { appConfig.camera.aec          = doc["aec"];           needCameraApply = true; }
    if (ok("agc", doc["agc"].is<bool>()))         { appConfig.camera.agc          = doc["agc"];           needCameraApply = true; }
    if (ok("awb", doc["awb"].is<bool>()))         { appConfig.camera.awb          = doc["awb"];           needCameraApply = true; }
    if (ok("bpc", doc["bpc"].is<bool>()))         { appConfig.camera.bpc          = doc["bpc"];           needCameraApply = true; }
    if (ok("wpc", doc["wpc"].is<bool>()))         { appConfig.camera.wpc          = doc["wpc"];           needCameraApply = true; }
    if (ok("raw_gma", doc["raw_gma"].is<bool>()))     { appConfig.camera.raw_gma     = doc["raw_gma"];       needCameraApply = true; }
    if (ok("lenc", doc["lenc"].is<bool>()))        { appConfig.camera.lenc         = doc["lenc"];          needCameraApply = true; }

    // Motion settings
    if (ok("motion_enabled", doc["motion_enabled"].is<bool>()))          { appConfig.motion.enabled          = doc["motion_enabled"];          needSave = true; }
    if (ok("motion_detection_enabled", doc["motion_detection_enabled"].is<bool>())) { appConfig.motion.enabled         = doc["motion_detection_enabled"]; needSave = true; }
    if (ok("motion_threshold", doc["motion_threshold"].is<int>()))         { appConfig.motion.threshold        = clampInt(doc["motion_threshold"], 5, 80);        needSave = true; }
    if (ok("motion_cooldown", doc["motion_cooldown"].is<int>()))          { appConfig.motion.cooldown_sec     = clampInt(doc["motion_cooldown"], 0, 3600);       needSave = true; }
    if (ok("motion_telegram_cooldown", doc["motion_telegram_cooldown"].is<int>())) { appConfig.motion.cooldown_sec     = clampInt(doc["motion_telegram_cooldown"], 0, 3600); needSave = true; }
    if (ok("motion_min_area", doc["motion_min_area"].is<int>()))          { appConfig.motion.min_area_pct     = clampInt(doc["motion_min_area"], 1, 100);        needSave = true; }
    // Upper reject: above this share of changed blocks the frame is treated as a
    // global lighting change. 100 effectively disables the reject; the old code
    // clamped to the 50 % default, so the value could never be raised at all.
    if (ok("motion_max_area", doc["motion_max_area"].is<int>()))          { appConfig.motion.max_area_pct     = clampInt(doc["motion_max_area"], 10, 100);       needSave = true; }
    if (ok("motion_temporal_filter", doc["motion_temporal_filter"].is<bool>()))  { appConfig.motion.temporal_filter  = doc["motion_temporal_filter"];  needSave = true; }
    if (ok("motion_spatial_filter", doc["motion_spatial_filter"].is<bool>()))   { appConfig.motion.spatial_filter   = doc["motion_spatial_filter"];   needSave = true; }
    if (ok("motion_night_suppress", doc["motion_night_suppress"].is<bool>()))   { appConfig.motion.night_suppress   = doc["motion_night_suppress"];   needSave = true; }
    if (ok("motion_save_sd", doc["motion_save_sd"].is<bool>()))          { appConfig.motion.save_to_sd       = doc["motion_save_sd"];          needSave = true; }
    // Expert knobs: persisted since day one but previously only reachable by hand-
    // editing /config.json.
    if (ok("motion_ema_alpha_day", doc["motion_ema_alpha_day"].is<float>()))   { appConfig.motion.ema_alpha_day    = clampFloat(doc["motion_ema_alpha_day"], 0.50f, 0.999f);   needSave = true; }
    if (ok("motion_ema_alpha_night", doc["motion_ema_alpha_night"].is<float>())) { appConfig.motion.ema_alpha_night  = clampFloat(doc["motion_ema_alpha_night"], 0.50f, 0.999f); needSave = true; }
    if (ok("motion_training_frames", doc["motion_training_frames"].is<int>()))   { appConfig.motion.training_frames  = clampInt(doc["motion_training_frames"], 1, 120);  needSave = true; }
    if (ok("motion_agc_gain_factor", doc["motion_agc_gain_factor"].is<float>())) { appConfig.motion.agc_gain_factor  = clampFloat(doc["motion_agc_gain_factor"], 0.0f, 4.0f); needSave = true; }
    if (ok("motion_brightness_min", doc["motion_brightness_min"].is<int>()))    { appConfig.motion.brightness_min   = clampInt(doc["motion_brightness_min"], 0, 128);   needSave = true; }

    // Face detection settings
    if (ok("face_detect_enabled", doc["face_detect_enabled"].is<bool>()))     { appConfig.face_detect.enabled         = doc["face_detect_enabled"];     needSave = true; }
    if (ok("face_detect_two_stage", doc["face_detect_two_stage"].is<bool>()))   { appConfig.face_detect.two_stage       = doc["face_detect_two_stage"];   needSave = true; }
    if (ok("face_detect_cooldown", doc["face_detect_cooldown"].is<int>()))     { appConfig.face_detect.cooldown_sec    = clampInt(doc["face_detect_cooldown"], 0, 3600); needSave = true; }
    if (ok("face_detect_save_sd", doc["face_detect_save_sd"].is<bool>()))     { appConfig.face_detect.save_to_sd      = doc["face_detect_save_sd"];     needSave = true; }
    if (ok("face_score_threshold", doc["face_score_threshold"].is<float>()))   { appConfig.face_detect.score_threshold = clampFloat(doc["face_score_threshold"], 0.05f, 0.99f); needSave = true; }
    if (ok("face_nms_threshold", doc["face_nms_threshold"].is<float>()))     { appConfig.face_detect.nms_threshold   = clampFloat(doc["face_nms_threshold"], 0.05f, 0.99f);   needSave = true; }

    // Person detection
    if (ok("person_detect_enabled", doc["person_detect_enabled"].is<bool>()))       { appConfig.person_detect.enabled              = doc["person_detect_enabled"];       needSave = true; }
    if (ok("person_detection_enabled", doc["person_detection_enabled"].is<bool>()))    { appConfig.person_detect.enabled              = doc["person_detection_enabled"];    needSave = true; }
    if (ok("person_detect_confidence", doc["person_detect_confidence"].is<float>()))   { appConfig.person_detect.confidence_threshold  = clampFloat(doc["person_detect_confidence"], 0.05f, 0.99f);   needSave = true; }
    if (ok("person_confidence_threshold", doc["person_confidence_threshold"].is<float>())) { appConfig.person_detect.confidence_threshold = clampFloat(doc["person_confidence_threshold"], 0.05f, 0.99f); needSave = true; }
    if (ok("person_confident_threshold", doc["person_confident_threshold"].is<float>())) { appConfig.person_detect.confident_threshold   = clampFloat(doc["person_confident_threshold"], 0.05f, 1.0f);  needSave = true; }
    if (ok("person_detect_temporal", doc["person_detect_temporal"].is<int>()))       { appConfig.person_detect.temporal_frames       = clampInt(doc["person_detect_temporal"], 1, 10);   needSave = true; }
    if (ok("person_recheck_interval", doc["person_recheck_interval"].is<int>()))      { appConfig.person_detect.temporal_frames       = clampInt(doc["person_recheck_interval"], 1, 10);  needSave = true; }
    if (ok("person_detect_cooldown", doc["person_detect_cooldown"].is<int>()))       { appConfig.person_detect.cooldown_sec          = clampInt(doc["person_detect_cooldown"], 0, 3600); needSave = true; }
    if (ok("person_detection_cooldown", doc["person_detection_cooldown"].is<int>()))    { appConfig.person_detect.cooldown_sec          = clampInt(doc["person_detection_cooldown"], 0, 3600); needSave = true; }
    if (ok("person_detect_save_sd", doc["person_detect_save_sd"].is<bool>()))       { appConfig.person_detect.save_to_sd            = doc["person_detect_save_sd"];       needSave = true; }

    // Tracker (person detection track lifecycle)
    if (ok("tracker_confirm_hits", doc["tracker_confirm_hits"].is<int>())) { appConfig.tracker.confirm_hits = clampInt(doc["tracker_confirm_hits"], 1, 10);  needSave = true; }
    if (ok("tracker_max_misses", doc["tracker_max_misses"].is<int>()))   { appConfig.tracker.max_misses   = clampInt(doc["tracker_max_misses"], 1, 30);    needSave = true; }
    if (ok("tracker_match_dist", doc["tracker_match_dist"].is<int>()))   { appConfig.tracker.match_dist   = clampInt(doc["tracker_match_dist"], 5, 200);   needSave = true; }

    // Timelapse
    if (ok("timelapse_enabled", doc["timelapse_enabled"].is<bool>())) { appConfig.timelapse.enabled      = doc["timelapse_enabled"];  needSave = true; }
    if (ok("timelapse_interval", doc["timelapse_interval"].is<int>())) { appConfig.timelapse.interval_sec = clampInt(doc["timelapse_interval"], 1, 86400); needSave = true; }
    if (ok("timelapse_save_sd", doc["timelapse_save_sd"].is<bool>())) { appConfig.timelapse.save_to_sd   = doc["timelapse_save_sd"];  needSave = true; }

    // MQTT
    if (ok("mqtt_enabled", doc["mqtt_enabled"].is<bool>()))        { appConfig.mqtt.enabled      = doc["mqtt_enabled"];      needSave = true; }
    // Same "empty = unchanged" rule: a blank field on the settings page must not
    // silently disconnect the broker.
    if (ok("mqtt_server", doc["mqtt_server"].is<const char*>())) {
        String srv = doc["mqtt_server"].as<String>();
        if (srv.length() > 0) { appConfig.mqtt.server = srv; needSave = true; }
    }
    if (ok("mqtt_port", doc["mqtt_port"].is<int>()))             { appConfig.mqtt.port         = clampInt(doc["mqtt_port"], 1, 65535); needSave = true; }
    if (ok("mqtt_topic_prefix", doc["mqtt_topic_prefix"].is<const char*>())) { appConfig.mqtt.topic_prefix = doc["mqtt_topic_prefix"].as<String>(); needSave = true; }
    if (ok("mqtt_tls_enabled", doc["mqtt_tls_enabled"].is<bool>()))     { appConfig.mqtt.tls_enabled  = doc["mqtt_tls_enabled"];  needSave = true; }
    // Broker credentials live in encrypted NVS like the other secrets; without
    // these keys an authenticated broker was simply unusable from the UI.
    if (ok("mqtt_user", doc["mqtt_user"].is<const char*>())) {
        String u = doc["mqtt_user"].as<String>();
        if (u.length() > 0) { appConfig.mqtt.user = u; saveSecretsToNVS(); }
    }
    if (ok("mqtt_pass", doc["mqtt_pass"].is<const char*>())) {
        String p = doc["mqtt_pass"].as<String>();
        if (p.length() > 0) { appConfig.mqtt.password = p; saveSecretsToNVS(); }
    }

    // Network identity
    if (ok("hostname", doc["hostname"].is<const char*>())) {
        String h = doc["hostname"].as<String>();
        if (h.length() > 0 && h.length() <= 32) { appConfig.wifi.hostname = h; needSave = true; }
    }

    // Telegram
    if (ok("telegram_enabled", doc["telegram_enabled"].is<bool>()))           { appConfig.telegram.enabled           = doc["telegram_enabled"];           needSave = true; }
    if (ok("telegram_notify_on_motion", doc["telegram_notify_on_motion"].is<bool>()))  { appConfig.telegram.notify_on_motion  = doc["telegram_notify_on_motion"];  needSave = true; }
    if (ok("telegram_notify_on_face", doc["telegram_notify_on_face"].is<bool>()))    { appConfig.telegram.notify_on_face    = doc["telegram_notify_on_face"];    needSave = true; }
    if (ok("telegram_photo_on_motion", doc["telegram_photo_on_motion"].is<bool>()))   { appConfig.telegram.photo_on_motion   = doc["telegram_photo_on_motion"];   needSave = true; }
    if (ok("motion_telegram_photo", doc["motion_telegram_photo"].is<bool>()))      { appConfig.telegram.photo_on_motion   = doc["motion_telegram_photo"];      needSave = true; }
    if (ok("telegram_photo_on_face", doc["telegram_photo_on_face"].is<bool>()))     { appConfig.telegram.photo_on_face     = doc["telegram_photo_on_face"];     needSave = true; }
    if (ok("telegram_notify_on_person", doc["telegram_notify_on_person"].is<bool>()))  { appConfig.telegram.notify_on_person  = doc["telegram_notify_on_person"];  needSave = true; }
    if (ok("telegram_photo_on_person", doc["telegram_photo_on_person"].is<bool>()))   { appConfig.telegram.photo_on_person   = doc["telegram_photo_on_person"];   needSave = true; }
    if (ok("person_telegram_photo", doc["person_telegram_photo"].is<bool>()))      { appConfig.telegram.photo_on_person   = doc["person_telegram_photo"];      needSave = true; }
    if (ok("telegram_cooldown", doc["telegram_cooldown"].is<int>()))           { appConfig.telegram.cooldown_sec      = doc["telegram_cooldown"];          needSave = true; }
    if (ok("telegram_active_start", doc["telegram_active_start"].is<int>()))       { appConfig.telegram.active_start_hour = doc["telegram_active_start"];      needSave = true; }
    if (ok("telegram_active_end", doc["telegram_active_end"].is<int>()))         { appConfig.telegram.active_end_hour   = doc["telegram_active_end"];        needSave = true; }
    if (ok("motion_notify_start_hour", doc["motion_notify_start_hour"].is<int>()))    { appConfig.telegram.active_start_hour = doc["motion_notify_start_hour"];   needSave = true; }
    if (ok("motion_notify_end_hour", doc["motion_notify_end_hour"].is<int>()))      { appConfig.telegram.active_end_hour   = doc["motion_notify_end_hour"];     needSave = true; }
    if (ok("telegram_poll_interval", doc["telegram_poll_interval"].is<int>()))      { appConfig.telegram.poll_interval_ms  = doc["telegram_poll_interval"];     needSave = true; }
    // Secrets follow the same "empty means leave unchanged" rule as http_pass.
    // The settings page posts every field on every save, so treating "" as a value
    // wiped the bot token and chat id whenever anyone touched an unrelated slider.
    // Clearing is done through POST /api/secrets/clear?target=telegram.
    if (ok("telegram_bot_token", doc["telegram_bot_token"].is<const char*>()))  {
        String t = doc["telegram_bot_token"].as<String>();
        if (t.length() > 0) {
            appConfig.telegram.bot_token = t;
            saveSecretsToNVS();
        }
    }
    if (ok("telegram_chat_id", doc["telegram_chat_id"].is<const char*>()))    {
        String c = doc["telegram_chat_id"].as<String>();
        if (c.length() > 0) {
            appConfig.telegram.chat_id = c;
            saveSecretsToNVS();
        }
    }

    // Auth (HTTP user/password). Password change persists to NVS immediately.
    if (ok("http_user", doc["http_user"].is<const char*>())) {
        String u = doc["http_user"].as<String>();
        if (u.length() > 0 && u.length() <= 32) {
            appConfig.auth.http_user = u;
            needSave = true;
        }
    }
    if (ok("http_pass", doc["http_pass"].is<const char*>())) {
        String p = doc["http_pass"].as<String>();
        // Empty field = leave unchanged. Minimum 4 chars to prevent trivial passwords.
        if (p.length() >= 4 && p.length() <= 64) {
            appConfig.auth.http_pass = p;
            saveSecretsToNVS();
        }
    }

    // LED
    if (ok("led_enabled", doc["led_enabled"].is<bool>())) { appConfig.led_enabled = doc["led_enabled"]; needSave = true; }

    // Frame rates
    if (ok("idle_fps", doc["idle_fps"].is<int>()))   { appConfig.idle_fps   = doc["idle_fps"];   needSave = true; }
    if (ok("active_fps", doc["active_fps"].is<int>())) { appConfig.active_fps = doc["active_fps"]; needSave = true; }

    // Sensor writes are handed to the capture task instead of being done here — the
    // async server task must not touch SCCB while a frame grab is in flight.
    bool frameSizeChanged = (appConfig.camera.frame_size != prevFrameSize);
    if (needCameraApply) {
        if (frameSizeChanged) {
            // New resolution needs the driver's DMA buffers rebuilt; the main loop
            // performs the reinit, where stopping the capture task is safe.
            cameraRequestReinit();
        } else {
            cameraRequestSettingsApply();
        }
        needSave = true;
    }

    if (needSave) {
        saveConfig();
    }

    JsonDocument resp;
    resp["success"] = true;
    resp["message"] = frameSizeChanged ? "Settings applied, camera restarting" : "Settings applied";
    if (frameSizeChanged) resp["camera_restart"] = true;
    JsonArray ignored = resp["ignored"].to<JsonArray>();
    for (JsonPair kv : doc.as<JsonObject>()) {
        if (!appliedKeys[kv.key()].is<bool>()) ignored.add(kv.key().c_str());
    }
    String respOut;
    serializeJson(resp, respOut);
    request->send(200, "application/json", respOut);

    // Notify WebSocket clients
    ws.textAll(buildStatusJson());
}

// API: GET /api/snapshot - returns JPEG
static void handleApiSnapshot(AsyncWebServerRequest* request) {
    const uint8_t* buf = NULL;
    size_t len = 0;

    int rh = ringBufferGetLatest(&buf, &len);
    if (rh >= 0) {
        // Copy data because ring buffer must be released before async send completes.
        // PSRAM, not the internal heap: a UXGA JPEG is ~100-250 kB and taking that
        // out of DRAM starved the async server / TLS handshakes (and simply failed
        // once the heap was fragmented).
        uint8_t* copy = (uint8_t*)ps_malloc(len);
        if (copy) {
            memcpy(copy, buf, len);
            ringBufferRelease(rh);

            // Track whether the lambda's free ran. If the client disconnects mid-transfer
            // the lambda is never called to completion; onDisconnect guarantees cleanup.
            struct SnapCtx { uint8_t* buf; size_t len; bool freed; };
            SnapCtx* ctx = new SnapCtx{copy, len, false};

            AsyncWebServerResponse* response = request->beginResponse(
                "image/jpeg", len,
                [ctx](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
                    size_t remaining = ctx->len - index;
                    size_t toSend = min(remaining, maxLen);
                    memcpy(buffer, ctx->buf + index, toSend);
                    if (index + toSend >= ctx->len && !ctx->freed) {
                        free(ctx->buf);
                        ctx->freed = true;
                    }
                    return toSend;
                }
            );
            response->addHeader("Content-Disposition", "inline; filename=snapshot.jpg");
            response->addHeader("Cache-Control", "no-cache");
            request->onDisconnect([ctx]() {
                if (!ctx->freed && ctx->buf) free(ctx->buf);
                delete ctx;
            });
            request->send(response);
        } else {
            ringBufferRelease(rh);
            request->send(503, "application/json", "{\"error\":\"Memory allocation failed\"}");
        }
    } else {
        request->send(503, "application/json", "{\"error\":\"No frame available\"}");
    }
}

// API: POST /api/wifi - connect to network
static void handleApiWifi(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;

    if (accumulateJsonBody(request, data, len, index, total) != 1) return;

    JsonDocument doc;
    if (deserializeJson(doc, (const uint8_t*)request->_tempObject, total)) {
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

    // Defer the blocking WiFi connect to main loop — never block async handler.
    wifiRequestConnect(ssid, pass);
}

// Helper: schedule a reboot from a throwaway FreeRTOS task (keeps async handler responsive).
static void scheduleRebootTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();
}
static void scheduleReboot(uint32_t delayMs = 500) {
    (void)delayMs;
    xTaskCreate(scheduleRebootTask, "reboot", 2048, NULL, 1, NULL);
}

// API: GET /api/wifi/scan
static void handleApiWifiScan(AsyncWebServerRequest* request) {
    request->send(200, "application/json", scanNetworksJson());
}

// API: POST /api/reboot
static void handleApiReboot(AsyncWebServerRequest* request) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Rebooting...\"}");
    scheduleReboot();
}

// API: POST /api/reset
static void handleApiReset(AsyncWebServerRequest* request) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    resetConfig();
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Factory reset, rebooting...\"}");
    scheduleReboot();
}

// API: POST /api/secrets/clear?target=telegram|mqtt
// Explicit way to erase stored credentials, now that an empty field in the
// settings form means "leave unchanged" rather than "delete".
static void handleApiSecretsClear(AsyncWebServerRequest* request) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;

    String target = request->hasParam("target") ? request->getParam("target")->value() : "";
    if (target == "telegram") {
        appConfig.telegram.bot_token = "";
        appConfig.telegram.chat_id = "";
    } else if (target == "mqtt") {
        appConfig.mqtt.user = "";
        appConfig.mqtt.password = "";
    } else {
        request->send(400, "application/json",
                      "{\"success\":false,\"message\":\"target must be telegram or mqtt\"}");
        return;
    }
    saveSecretsToNVS();
    request->send(200, "application/json", "{\"success\":true}");
}

// API: POST /api/camera/reinit
static void handleApiCameraReinit(AsyncWebServerRequest* request) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    // Don't reinit inline: stopping the capture task now waits up to 3s for it to
    // leave the driver, and blocking the async server task for that long stalls
    // every other connection. The main loop performs it on the next pass.
    cameraRequestReinit();
    request->send(202, "application/json",
                  "{\"success\":true,\"message\":\"Camera reinit scheduled\"}");
}

// WebSocket events
static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        logCapture("[%s] WS client connected: %u\n", TAG, client->id());
        client->text(buildStatusJson());
    } else if (type == WS_EVT_DISCONNECT) {
        logCapture("[%s] WS client disconnected: %u\n", TAG, client->id());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            // Log the frame without writing a terminator past the end of the
            // library's receive buffer (the old `data[len] = 0` did exactly that).
            logCapture("[%s] WS message: %.*s\n", TAG, (int)len, (const char*)data);
        }
    }
}

#ifdef INCLUDE_SD_CARD
// Card paths accepted from HTTP are restricted to the directories the firmware
// itself creates, and must not contain ".." — otherwise "file=/config.json" style
// requests could read or delete anything the SD driver can reach.
static const char* SD_ALLOWED_PREFIXES[] = {
    "/captures", "/timelapse", "/recordings", "/telegram_pending", "/logs"
};

static bool sdPathAllowed(const String& path) {
    if (path.length() == 0 || path[0] != '/') return false;
    if (path.indexOf("..") >= 0) return false;
    for (const char* prefix : SD_ALLOWED_PREFIXES) {
        if (path == prefix) return true;
        if (path.startsWith(String(prefix) + "/")) return true;
    }
    return false;
}

// Directory listing additionally allows "/" so the UI can discover the roots.
static bool sdListPathAllowed(const String& path) {
    return path == "/" || sdPathAllowed(path);
}

// API: GET /api/sd/list?path=/
static void handleSdList(AsyncWebServerRequest* request) {
    if (!requireAuth(request)) return;
    String path = request->hasParam("path") ? request->getParam("path")->value() : "/";
    if (!sdListPathAllowed(path)) {
        request->send(403, "application/json", "{\"error\":\"Path not allowed\"}");
        return;
    }
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
    if (!requireAuth(request)) return;
    if (!request->hasParam("file")) {
        request->send(400, "application/json", "{\"error\":\"file param required\"}");
        return;
    }
    String path = request->getParam("file")->value();
    if (!sdPathAllowed(path)) {
        request->send(403, "application/json", "{\"error\":\"Path not allowed\"}");
        return;
    }
    if (!SD.exists(path)) {
        request->send(404, "application/json", "{\"error\":\"File not found\"}");
        return;
    }
    request->send(SD, path, "application/octet-stream");
}

// API: DELETE /api/sd/delete?file=/path
static void handleSdDelete(AsyncWebServerRequest* request) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    if (!request->hasParam("file")) {
        request->send(400, "application/json", "{\"error\":\"file param required\"}");
        return;
    }
    String path = request->getParam("file")->value();
    if (!sdPathAllowed(path)) {
        request->send(403, "application/json", "{\"error\":\"Path not allowed\"}");
        return;
    }
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
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
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

// ── Zone manager API ─────────────────────────────────────────────────────────
#ifdef INCLUDE_ZONES
static void handleApiZonesGet(AsyncWebServerRequest* request) {
    request->send(200, "application/json", getZonesJSON());
}

static void handleApiZonesPost(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    Zone z;
    strlcpy(z.name,  doc["name"]  | "", ZONE_NAME_LEN);
    strlcpy(z.color, doc["color"] | "#ffffff", 8);
    z.alert = doc["alert"] | false;
    z.rect_count = 0;
    JsonArray rects = doc["rects"];
    if (rects) {
        for (JsonObject r : rects) {
            if (z.rect_count >= ZONE_MAX_RECTS) break;
            ZoneRect& zr = z.rects[z.rect_count++];
            zr.x = r["x"] | 0; zr.y = r["y"] | 0;
            zr.w = r["w"] | 1; zr.h = r["h"] | 1;
        }
    }
    if (z.name[0] == '\0') {
        request->send(400, "application/json", "{\"error\":\"Zone name required\"}");
        return;
    }
    bool ok = addOrUpdateZone(z);
    request->send(ok ? 200 : 500, "application/json",
                  ok ? "{\"success\":true}" : "{\"error\":\"Save failed\"}");
}

static void handleApiZonesDelete(AsyncWebServerRequest* request) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    if (!request->hasParam("name")) {
        request->send(400, "application/json", "{\"error\":\"name param required\"}");
        return;
    }
    String name = request->getParam("name")->value();
    bool ok = deleteZone(name.c_str());
    request->send(ok ? 200 : 404, "application/json",
                  ok ? "{\"success\":true}" : "{\"error\":\"Zone not found\"}");
}

static void handleApiRoiGet(AsyncWebServerRequest* request) {
    char buf[MOTION_GRID_SIZE + 1] = {0};
    JsonDocument doc;
    doc["mask"] = loadROIMask(buf, sizeof(buf)) ? buf : "";
    // Tell the client the grid it must produce a mask for, and how much of the
    // frame is currently active, so a ROI editor does not have to guess.
    doc["grid_w"] = MOTION_GRID_W;
    doc["grid_h"] = MOTION_GRID_H;
    #ifdef INCLUDE_MOTION_DETECT
    doc["active_blocks"] = motionDetectRoiActiveBlocks();
    #endif
    doc["total_blocks"] = MOTION_GRID_SIZE;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

static void handleApiRoiPost(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
    }
    const char* mask = doc["mask"] | "";
    size_t maskLen = strlen(mask);
    // Empty mask = clear (whole frame active). Anything else must match the grid
    // exactly, otherwise the detector would silently ignore it.
    if (maskLen != 0 && maskLen != (size_t)MOTION_GRID_SIZE) {
        char err[96];
        snprintf(err, sizeof(err),
                 "{\"error\":\"mask must be %d chars (%dx%d grid) or empty\"}",
                 MOTION_GRID_SIZE, MOTION_GRID_W, MOTION_GRID_H);
        request->send(400, "application/json", err);
        return;
    }

    saveROIMask(mask, maskLen);
    #ifdef INCLUDE_MOTION_DETECT
    // Apply immediately — no reboot needed.
    if (maskLen == 0) motionDetectClearRoiMask();
    else              motionDetectSetRoiMask(mask, (int)maskLen);
    #endif
    request->send(200, "application/json", "{\"success\":true}");
}
#endif // INCLUDE_ZONES

// ── Event log API ─────────────────────────────────────────────────────────────
#ifdef INCLUDE_EVENT_LOG
static void handleApiEvents(AsyncWebServerRequest* request) {
    request->send(200, "application/json", getEventsJSON());
}
#endif


static void handleCompatJson(AsyncWebServerRequest* request) {
    request->send(200, "application/json", buildStatusJson());
}

// GET /health — the liveness endpoint external monitors poll.
//
// The field names here are a contract, not a free choice: the A12 companion reads
// overall_health, power_health, uptime_seconds, last_restart_reason_name and
// power_restarts_* and raises alerts from them. This endpoint used to emit none of
// those (and an always-empty `issues`), so A12's entire health and power alerting
// path was silently dead against this firmware — it polled, parsed, found nothing
// actionable and stayed quiet. Renaming or dropping a field below breaks a monitor
// that has no way to tell you it went blind.
static void handleHealth(AsyncWebServerRequest* request) {
    JsonDocument doc;

    // "Dropping frames" = the counter grew since the previous /health poll, so a
    // long-ago burst does not mark the device degraded forever.
    static uint32_t healthLastDropped = 0;
    static uint32_t healthPrevSeen = 0;
    healthLastDropped = healthPrevSeen;
    healthPrevSeen = getRingDroppedFrames();

    uint32_t uptimeSec = millis() / 1000;
    uint32_t freeHeap  = ESP.getFreeHeap();
    uint32_t frameAge  = getFrameAgeMs();

    // Collect concrete complaints; `issues` is what a monitor shows the operator, so
    // it has to say what is wrong rather than just that something is.
    String issues;
    auto addIssue = [&issues](const char* what) {
        if (issues.length()) issues += "; ";
        issues += what;
    };

    if (!isWiFiConnected())              addIssue("wifi down");
    if (getCaptureCount() == 0)          addIssue("no frame captured since boot");
    else if (frameAge > 10000)           addIssue("stale frame (>10s)");
    // getCaptureFps() is 0 until the first FPS window completes, so skip the check
    // for the first seconds after boot/reinit (a fresh frame proves capture is alive).
    if (getCaptureFps() < 0.1f && millis() > 5000 && frameAge > 3000) addIssue("capture stalled");
    if (getRingDroppedFrames() > healthLastDropped) addIssue("frames being dropped");
    if (freeHeap < 40 * 1024)            addIssue("low heap");
    #ifdef INCLUDE_SD_CARD
    if (sdStoreDisabled())               addIssue("sd writes disabled");
    #endif

    bool degraded = issues.length() > 0;

    doc["ok"] = !degraded;
    doc["overall_health"] = degraded ? "degraded" : "ok";
    doc["issues"] = issues;

    doc["uptime_sec"] = uptimeSec;
    doc["uptime_seconds"] = uptimeSec;   // name A12 reads
    doc["free_heap"] = freeHeap;
    doc["free_psram"] = ESP.getFreePsram();
    doc["wifi_connected"] = isWiFiConnected();
    doc["wifi_rssi"] = getRSSI();
    doc["capture_fps"] = getCaptureFps();
    doc["capture_errors"] = getCaptureErrors();
    doc["ring_dropped"] = getRingDroppedFrames();
    doc["ring_oversize"] = getOversizeDroppedFrames();
    doc["device_name"] = appConfig.wifi.hostname;   // A12 derives esp32cam/<device>/... from it
    doc["frame_age_ms"] = frameAge;
    doc["stream_clients"] = getStreamClientCount();
    doc["detection_clients"] = getDetectionStreamClientCount();

    // Restart history. "suspect" means the restart pattern points at the power
    // supply rather than at software: a brownout was recorded, or the board came up
    // from a bare power-on without anyone asking for a reboot. A monitor combines
    // this with uptime to decide whether the problem is current or historical.
    const char* reasonName = resetReasonName(sysStats.last_reset_reason);
    bool powerSuspect = sysStats.brownout_restarts > 0 ||
                        sysStats.last_reset_reason == ESP_RST_BROWNOUT ||
                        (sysStats.last_reset_reason == ESP_RST_POWERON &&
                         sysStats.total_restarts > 1);

    doc["reset_reason"] = reasonName;
    doc["last_restart_reason_name"] = reasonName;   // name A12 reads
    doc["total_restarts"] = sysStats.total_restarts;
    doc["power_health"] = powerSuspect ? "suspect" : "ok";
    doc["power_restarts_poweron"]  = sysStats.poweron_restarts;
    doc["power_restarts_brownout"] = sysStats.brownout_restarts;
    doc["wdt_restarts"] = sysStats.wdt_restarts;
    doc["panic_restarts"] = sysStats.panic_restarts;

    String result;
    serializeJson(doc, result);
    request->send(200, "application/json", result);
}

static void handleStreamStats(AsyncWebServerRequest* request) {
    JsonDocument doc;
    int streamClients = getStreamClientCount();
    int detectionClients = getDetectionStreamClientCount();
    doc["stream_clients"] = streamClients;
    doc["detection_clients"] = detectionClients;
    doc["total_stream_clients"] = streamClients + detectionClients;
    doc["capture_fps"] = getCaptureFps();
    doc["capture_count"] = getCaptureCount();
    doc["capture_errors"] = getCaptureErrors();
    doc["last_capture_ms"] = getLastCaptureMs();
    doc["frame_age_ms"] = getFrameAgeMs();
    doc["active_fps"] = appConfig.active_fps;
    doc["idle_fps"] = appConfig.idle_fps;
    doc["stream_url"] = streamBaseUrl() + "/stream";
    doc["detection_stream_url"] = streamBaseUrl() + "/detection-stream";
    String result;
    serializeJson(doc, result);
    request->send(200, "application/json", result);
}

static void handleA12Status(AsyncWebServerRequest* request) {
    JsonDocument doc;
    uint32_t frameAge = getFrameAgeMs();

    doc["ok"] = isWiFiConnected() && getCaptureCount() > 0 && frameAge < 10000;
    doc["device"] = DEVICE_NAME;
    doc["version"] = FIRMWARE_VERSION;
    doc["ip"] = getIPAddress();
    doc["mac"] = getMACAddress();
    doc["wifi_mode"] = isWiFiConnected() ? "STA" : "AP";
    doc["wifi_rssi"] = getRSSI();
    doc["uptime_sec"] = millis() / 1000;

    doc["stream_url"] = streamBaseUrl() + "/stream";
    doc["detection_stream_url"] = streamBaseUrl() + "/detection-stream";
    doc["snapshot_url"] = httpBaseUrl() + "/frame";
    int streamClients = getStreamClientCount();
    int detectionClients = getDetectionStreamClientCount();
    doc["stream_port"] = STREAM_PORT;
    doc["stream_clients"] = streamClients;
    doc["detection_clients"] = detectionClients;
    doc["total_stream_clients"] = streamClients + detectionClients;

    doc["capture_fps"] = getCaptureFps();
    doc["capture_count"] = getCaptureCount();
    doc["capture_errors"] = getCaptureErrors();
    doc["last_capture_ms"] = getLastCaptureMs();
    doc["frame_age_ms"] = frameAge;
    doc["active_fps"] = appConfig.active_fps;
    doc["idle_fps"] = appConfig.idle_fps;

    doc["frame_size"] = appConfig.camera.frame_size;
    doc["jpeg_quality"] = appConfig.camera.jpeg_quality;
    doc["camera_vflip"] = appConfig.camera.vflip;
    doc["camera_hmirror"] = appConfig.camera.hmirror;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["free_psram"] = ESP.getFreePsram();
    doc["total_psram"] = ESP.getPsramSize();

    doc["motion_enabled"] = appConfig.motion.enabled;
    #ifdef INCLUDE_MOTION_DETECT
    MotionDebugInfo mdi = getMotionDebugInfo();
    doc["motion_detected"] = isMotionDetected();
    doc["motion_changed_pct"] = mdi.changed_pct;
    doc["motion_avg_brightness"] = mdi.avg_brightness;
    doc["motion_night_mode"] = mdi.night_mode;
    doc["motion_training"] = mdi.training;
    #else
    doc["motion_detected"] = false;
    #endif

    doc["person_enabled"] = appConfig.person_detect.enabled;
    #ifdef INCLUDE_PERSON_DETECT
    PersonDetectResult pdRes = getPersonDetectResult();
    doc["person_detected"] = pdRes.detected;
    doc["person_decision"] = personDecisionToString(pdRes.decision);
    doc["person_count"] = pdRes.person_count;
    doc["person_track_count"] = pdRes.track_count;
    doc["person_top_score"] = pdRes.top_score;
    doc["person_inference_ms"] = pdRes.inference_ms;
    #else
    doc["person_detected"] = false;
    doc["person_decision"] = "NONE";
    doc["person_count"] = 0;
    doc["person_track_count"] = 0;
    doc["person_top_score"] = 0.0f;
    doc["person_inference_ms"] = 0;
    #endif

    doc["mqtt_enabled"] = appConfig.mqtt.enabled;
    #ifdef INCLUDE_MQTT
    doc["mqtt_connected"] = isMqttConnected();
    #else
    doc["mqtt_connected"] = false;
    #endif
    doc["telegram_enabled"] = appConfig.telegram.enabled;
    doc["sd_mounted"] = false;
    #ifdef INCLUDE_SD_CARD
    doc["sd_mounted"] = SD.cardType() != CARD_NONE;
    #endif

    String result;
    serializeJson(doc, result);
    request->send(200, "application/json", result);
}

static void handlePsramStats(AsyncWebServerRequest* request) {
    JsonDocument doc;
    size_t total = ESP.getPsramSize();
    size_t freep = ESP.getFreePsram();
    doc["total"] = total;
    doc["free"] = freep;
    doc["used"] = total > freep ? total - freep : 0;
    doc["usage_pct"] = total > 0 ? ((float)(total - freep) / (float)total) * 100.0f : 0.0f;
    String result;
    serializeJson(doc, result);
    request->send(200, "application/json", result);
}

static void handleAudioStatus(AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"enabled\":false,\"available\":false,\"reason\":\"audio_not_enabled\"}");
}

static void handleIrStatus(AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"ir_led_state\":false,\"auto_mode\":false,\"available\":false}");
}

static void handleIrControl(AsyncWebServerRequest* request, uint8_t*, size_t, size_t, size_t) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    request->send(200, "application/json", "{\"success\":true,\"ir_led_state\":false,\"available\":false}");
}

static void handleRecordControl(AsyncWebServerRequest* request, uint8_t*, size_t, size_t, size_t) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    request->send(501, "application/json", "{\"success\":false,\"message\":\"AVI recording is not enabled on CamS3 yet\"}");
}

static void handleCredentialsGet(AsyncWebServerRequest* request) {
    if (!requireAuth(request)) return;
    JsonDocument doc;
    doc["wifi_ssid"] = appConfig.wifi.ssid;
    doc["wifi_pass_set"] = appConfig.wifi.password.length() > 0;
    doc["telegram_token_set"] = appConfig.telegram.bot_token.length() > 0;
    doc["telegram_chat_id"] = appConfig.telegram.chat_id;
    doc["http_user"] = appConfig.auth.http_user;
    doc["default_password"] = (appConfig.auth.http_pass == String(DEFAULT_HTTP_PASS));
    String result;
    serializeJson(doc, result);
    request->send(200, "application/json", result);
}

#ifdef INCLUDE_SD_CARD
static void handleSaveFrame(AsyncWebServerRequest* request) {
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    if (!sdStoreAvailable()) {
        request->send(503, "application/json",
                      sdStoreDisabled()
                          ? "{\"status\":\"error\",\"message\":\"sd_write_disabled\"}"
                          : "{\"status\":\"error\",\"message\":\"sd_not_mounted\"}");
        return;
    }
    const uint8_t* buf = NULL;
    size_t len = 0;
    int rh = ringBufferGetLatest(&buf, &len);
    if (rh < 0 || !buf || len == 0) {
        request->send(503, "application/json", "{\"status\":\"error\",\"message\":\"no_frame\"}");
        return;
    }
    char filename[128];
    bool ok = sdStoreWriteJpeg("/captures", "manual_", buf, len, filename, sizeof(filename));
    ringBufferRelease(rh);
    if (!ok) {
        request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"write_failed\"}");
        return;
    }
    String body = "{\"status\":\"ok\",\"file\":\"" + String(filename) + "\"}";
    request->send(200, "application/json", body);
}
#endif

static void setupApiRoutes() {
    // CORS headers for all
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/sensor", HTTP_GET, handleApiSensor);
    server.on("/api/snapshot", HTTP_GET, handleApiSnapshot);
    server.on("/api/wifi/scan", HTTP_GET, handleApiWifiScan);
    server.on("/api/csrf", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!requireAuth(request)) return;
        String body = "{\"token\":\"" + String(csrfToken) + "\"}";
        request->send(200, "application/json", body);
    });
    server.on("/api/reboot", HTTP_POST, handleApiReboot);
    // Legacy alias used by the A12 companion; same exemption as /settings and
    // /ir-control (non-/api/ path: no token needed unless the request is browser-originated).
    server.on("/reboot", HTTP_POST, handleApiReboot);
    server.on("/api/reset", HTTP_POST, handleApiReset);
    server.on("/api/camera/reinit", HTTP_POST, handleApiCameraReinit);
    server.on("/api/secrets/clear", HTTP_POST, handleApiSecretsClear);

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

    // Simple endpoints compatible with the DFR1154 reference project.
    server.on("/status", HTTP_GET, handleApiStatus);
    server.on("/settings", HTTP_GET, handleApiStatus);
    server.on("/settings", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        NULL,
        handleApiSettings
    );
    server.on("/settings-page", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("/settings.html");
    });
    server.on("/frame", HTTP_GET, handleApiSnapshot);
    server.on("/snapshot", HTTP_GET, handleApiSnapshot);
    server.on("/health", HTTP_GET, handleHealth);
    server.on("/telemetry", HTTP_GET, handleCompatJson);
    server.on("/stream-stats", HTTP_GET, handleStreamStats);
    server.on("/a12/status", HTTP_GET, handleA12Status);
    server.on("/api/a12/status", HTTP_GET, handleA12Status);
    server.on("/psram-stats", HTTP_GET, handlePsramStats);
    server.on("/audio-status", HTTP_GET, handleAudioStatus);
    server.on("/ir-status", HTTP_GET, handleIrStatus);
    server.on("/ir-control", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        NULL,
        handleIrControl
    );
    server.on("/record", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        NULL,
        handleRecordControl
    );
    server.on("/credentials", HTTP_GET, handleCredentialsGet);

    // Motion detection API
    #ifdef INCLUDE_MOTION_DETECT
    server.on("/api/motion/debug", HTTP_GET, handleApiMotionDebug);
    server.on("/motion-debug", HTTP_GET, handleApiMotionDebug);
    server.on("/api/motion/reset", HTTP_POST, handleApiMotionReset);
    #endif

    // Log viewer
    server.on("/log", HTTP_GET, handleLog);
    server.on("/log-viewer", HTTP_GET, handleLogViewer);

    #ifdef INCLUDE_ZONES
    server.on("/api/zones", HTTP_GET, handleApiZonesGet);
    server.on("/api/zones", HTTP_POST,
        [](AsyncWebServerRequest* r) {}, NULL, handleApiZonesPost);
    server.on("/api/zones", HTTP_DELETE, handleApiZonesDelete);
    server.on("/api/roi", HTTP_GET, handleApiRoiGet);
    server.on("/api/roi", HTTP_POST,
        [](AsyncWebServerRequest* r) {}, NULL, handleApiRoiPost);
    #endif

    #ifdef INCLUDE_EVENT_LOG
    server.on("/api/events", HTTP_GET, handleApiEvents);
    server.on("/events", HTTP_GET, handleApiEvents);
    #endif

    #ifdef INCLUDE_SD_CARD
    setupSdRoutes();
    server.on("/sd-list", HTTP_GET, handleSdList);
    server.on("/save_frame", HTTP_POST, handleSaveFrame);
    #endif
}

static void setupWebSocket() {
    ws.onEvent(onWsEvent);
    // WS broadcast carries only non-secret status (uptime, heap, detection flags).
    // Browsers inconsistently forward cached Basic Auth to the WS upgrade handshake
    // (Safari omits it, Chrome/Firefox depend on version); enforcing auth here
    // leaves the page stuck at "Connecting..." even after a successful HTTP login.
    // Real admin mutations still go through auth-protected POST endpoints.
    server.addHandler(&ws);
}

static void setupStaticFiles() {
    // Serve web UI from LittleFS. Without Cache-Control the browser revalidates
    // every asset on every navigation — five requests to the ESP32 per menu click.
    // A day of caching is fine: the assets only change with a firmware/FS flash,
    // and the handler still answers conditional requests with ETag/304.
    server.serveStatic("/", LittleFS, "/www/")
          .setDefaultFile("index.html")
          .setCacheControl("public, max-age=86400");

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
    csrfGenerate();
    setupApiRoutes();
    setupWebSocket();
    setupStaticFiles();

    #ifdef INCLUDE_OTA
    ElegantOTA.begin(&server);
    if (appConfig.auth.http_user.length() > 0 && appConfig.auth.http_pass.length() > 0) {
        ElegantOTA.setAuth(appConfig.auth.http_user.c_str(), appConfig.auth.http_pass.c_str());
    }
    ElegantOTA.onEnd([](bool success) {
        if (success) logNewFirmwareSha();
        else logCapture("[%s] OTA failed\n", TAG);
    });
    logCapture("[%s] OTA enabled at /update (post-flash SHA-256 logged)\n", TAG);
    #endif

    server.begin();
    logCapture("[%s] Web server started on port %d\n", TAG, HTTP_PORT);
}

void webServerStop() {
    server.end();
    logCapture("[%s] Web server stopped\n", TAG);
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
