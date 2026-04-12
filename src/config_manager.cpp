#include "config.h"
#include <LittleFS.h>
#include <Preferences.h>
#include "esp_camera.h"
#include "board_config.h"

static const char* TAG = "ConfigMgr";
static const char* CONFIG_FILE = "/config.json";
static Preferences nvs;

AppConfig appConfig;

bool loadConfig() {
    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.printf("[%s] No config file, using defaults\n", TAG);
        return false;
    }

    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) {
        Serial.printf("[%s] Failed to open config file\n", TAG);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[%s] JSON parse error: %s\n", TAG, err.c_str());
        return false;
    }

    int ver = doc["version"] | 0;
    if (ver < CONFIG_VERSION) {
        Serial.printf("[%s] Config migration from v%d to v%d\n", TAG, ver, CONFIG_VERSION);
    }

    // Camera
    JsonObject cam = doc["camera"];
    if (cam) {
        appConfig.camera.frame_size   = cam["frame_size"]   | FRAME_SIZE_DEFAULT;
        appConfig.camera.jpeg_quality = cam["jpeg_quality"]  | JPEG_QUALITY_DEFAULT;
        appConfig.camera.vflip        = cam["vflip"]         | false;
        appConfig.camera.hmirror      = cam["hmirror"]       | false;
        appConfig.camera.brightness   = cam["brightness"]    | 0;
        appConfig.camera.contrast     = cam["contrast"]      | 0;
        appConfig.camera.saturation   = cam["saturation"]    | 0;
        appConfig.camera.sharpness    = cam["sharpness"]     | 0;
        appConfig.camera.denoise      = cam["denoise"]       | 0;
        appConfig.camera.ae_level     = cam["ae_level"]      | 0;
        appConfig.camera.aec_value    = cam["aec_value"]     | 300;
        appConfig.camera.agc_gain     = cam["agc_gain"]      | 0;
        appConfig.camera.gainceiling  = cam["gainceiling"]   | 0;
        appConfig.camera.wb_mode      = cam["wb_mode"]       | 0;
        appConfig.camera.aec          = cam["aec"]           | true;
        appConfig.camera.agc          = cam["agc"]           | true;
        appConfig.camera.awb          = cam["awb"]           | true;
        appConfig.camera.bpc          = cam["bpc"]           | true;
        appConfig.camera.wpc          = cam["wpc"]           | true;
        appConfig.camera.raw_gma      = cam["raw_gma"]       | true;
        appConfig.camera.lenc         = cam["lenc"]          | true;
    }

    // WiFi (hostname only; SSID/pass in NVS)
    JsonObject wifi = doc["wifi"];
    if (wifi) {
        appConfig.wifi.hostname = wifi["hostname"] | String("cams3");
    }

    // Motion
    JsonObject mot = doc["motion"];
    if (mot) {
        appConfig.motion.enabled          = mot["enabled"]          | false;
        appConfig.motion.threshold        = mot["threshold"]        | MOTION_THRESHOLD_DEFAULT;
        appConfig.motion.cooldown_sec     = mot["cooldown_sec"]     | MOTION_COOLDOWN_DEFAULT;
        appConfig.motion.min_area_pct     = mot["min_area_pct"]     | MOTION_MIN_AREA_DEFAULT;
        appConfig.motion.max_area_pct     = mot["max_area_pct"]     | MOTION_MAX_AREA_DEFAULT;
        appConfig.motion.ema_alpha_day    = mot["ema_alpha_day"]    | MOTION_EMA_ALPHA_DAY;
        appConfig.motion.ema_alpha_night  = mot["ema_alpha_night"]  | MOTION_EMA_ALPHA_NIGHT;
        appConfig.motion.training_frames  = mot["training_frames"]  | MOTION_TRAINING_FRAMES;
        appConfig.motion.agc_gain_factor  = mot["agc_gain_factor"]  | MOTION_AGC_GAIN_FACTOR;
        appConfig.motion.brightness_min   = mot["brightness_min"]   | MOTION_BRIGHTNESS_MIN;
        appConfig.motion.temporal_filter  = mot["temporal_filter"]  | true;
        appConfig.motion.spatial_filter   = mot["spatial_filter"]   | true;
        appConfig.motion.night_suppress   = mot["night_suppress"]   | true;
        appConfig.motion.save_to_sd       = mot["save_to_sd"]       | false;
    }

    // Face detection
    JsonObject fd = doc["face_detect"];
    if (fd) {
        appConfig.face_detect.enabled         = fd["enabled"]         | false;
        appConfig.face_detect.score_threshold  = fd["score_threshold"]  | FACE_SCORE_THRESHOLD_DEFAULT;
        appConfig.face_detect.nms_threshold    = fd["nms_threshold"]    | FACE_NMS_THRESHOLD_DEFAULT;
        appConfig.face_detect.two_stage        = fd["two_stage"]        | true;
        appConfig.face_detect.cooldown_sec     = fd["cooldown_sec"]     | FACE_COOLDOWN_DEFAULT;
        appConfig.face_detect.save_to_sd       = fd["save_to_sd"]       | false;
    }

    // Person detection
    JsonObject pd = doc["person_detect"];
    if (pd) {
        appConfig.person_detect.enabled              = pd["enabled"]              | false;
        appConfig.person_detect.confidence_threshold  = pd["confidence_threshold"]  | PD_CONFIDENCE_THRESHOLD_DEFAULT;
        appConfig.person_detect.temporal_frames       = pd["temporal_frames"]       | PD_TEMPORAL_FRAMES_DEFAULT;
        appConfig.person_detect.cooldown_sec          = pd["cooldown_sec"]          | PD_COOLDOWN_DEFAULT;
        appConfig.person_detect.save_to_sd            = pd["save_to_sd"]            | false;
    }

    // Timelapse
    JsonObject tl = doc["timelapse"];
    if (tl) {
        appConfig.timelapse.enabled      = tl["enabled"]      | false;
        appConfig.timelapse.interval_sec = tl["interval_sec"] | TIMELAPSE_INTERVAL_DEFAULT;
        appConfig.timelapse.save_to_sd   = tl["save_to_sd"]   | true;
    }

    // MQTT
    JsonObject mq = doc["mqtt"];
    if (mq) {
        appConfig.mqtt.enabled      = mq["enabled"]      | false;
        appConfig.mqtt.server       = mq["server"]       | String("");
        appConfig.mqtt.port         = mq["port"]         | MQTT_PORT_DEFAULT;
        appConfig.mqtt.topic_prefix = mq["topic_prefix"] | String("cams3");
    }

    // Telegram
    JsonObject tg = doc["telegram"];
    if (tg) {
        appConfig.telegram.enabled           = tg["enabled"]           | false;
        appConfig.telegram.notify_on_motion  = tg["notify_on_motion"]  | true;
        appConfig.telegram.notify_on_face    = tg["notify_on_face"]    | true;
        appConfig.telegram.photo_on_motion   = tg["photo_on_motion"]   | true;
        appConfig.telegram.photo_on_face     = tg["photo_on_face"]     | true;
        appConfig.telegram.notify_on_person  = tg["notify_on_person"]  | true;
        appConfig.telegram.photo_on_person   = tg["photo_on_person"]   | true;
        appConfig.telegram.cooldown_sec      = tg["cooldown_sec"]      | TELEGRAM_COOLDOWN_DEFAULT;
        appConfig.telegram.active_start_hour = tg["active_start_hour"] | TELEGRAM_ACTIVE_START_DEFAULT;
        appConfig.telegram.active_end_hour   = tg["active_end_hour"]   | TELEGRAM_ACTIVE_END_DEFAULT;
        appConfig.telegram.poll_interval_ms  = tg["poll_interval_ms"]  | TELEGRAM_POLL_INTERVAL_MS_DEFAULT;
    }

    // Auth (passwords from NVS for security)
    appConfig.auth.http_user = doc["auth"]["user"] | String(DEFAULT_HTTP_USER);

    // Misc
    appConfig.led_enabled = doc["led_enabled"] | true;
    appConfig.idle_fps    = doc["idle_fps"]    | 1;
    appConfig.active_fps  = doc["active_fps"]  | 15;

    // Load secrets from NVS
    nvs.begin("cams3", true);  // read-only
    appConfig.wifi.ssid     = nvs.getString("wifi_ssid", "");
    appConfig.wifi.password = nvs.getString("wifi_pass", "");
    appConfig.auth.http_pass = nvs.getString("http_pass", DEFAULT_HTTP_PASS);
    appConfig.mqtt.user     = nvs.getString("mqtt_user", "");
    appConfig.mqtt.password = nvs.getString("mqtt_pass", "");
    appConfig.telegram.bot_token = nvs.getString("tg_token", "");
    appConfig.telegram.chat_id   = nvs.getString("tg_chat_id", "");
    nvs.end();

    Serial.printf("[%s] Config loaded (v%d)\n", TAG, appConfig.version);
    return true;
}

bool saveConfig() {
    JsonDocument doc;
    doc["version"] = CONFIG_VERSION;

    // Camera
    JsonObject cam = doc["camera"].to<JsonObject>();
    cam["frame_size"]   = appConfig.camera.frame_size;
    cam["jpeg_quality"]  = appConfig.camera.jpeg_quality;
    cam["vflip"]         = appConfig.camera.vflip;
    cam["hmirror"]       = appConfig.camera.hmirror;
    cam["brightness"]    = appConfig.camera.brightness;
    cam["contrast"]      = appConfig.camera.contrast;
    cam["saturation"]    = appConfig.camera.saturation;
    cam["sharpness"]     = appConfig.camera.sharpness;
    cam["denoise"]       = appConfig.camera.denoise;
    cam["ae_level"]      = appConfig.camera.ae_level;
    cam["aec_value"]     = appConfig.camera.aec_value;
    cam["agc_gain"]      = appConfig.camera.agc_gain;
    cam["gainceiling"]   = appConfig.camera.gainceiling;
    cam["wb_mode"]       = appConfig.camera.wb_mode;
    cam["aec"]           = appConfig.camera.aec;
    cam["agc"]           = appConfig.camera.agc;
    cam["awb"]           = appConfig.camera.awb;
    cam["bpc"]           = appConfig.camera.bpc;
    cam["wpc"]           = appConfig.camera.wpc;
    cam["raw_gma"]       = appConfig.camera.raw_gma;
    cam["lenc"]          = appConfig.camera.lenc;

    // WiFi
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["hostname"] = appConfig.wifi.hostname;

    // Motion
    JsonObject mot = doc["motion"].to<JsonObject>();
    mot["enabled"]          = appConfig.motion.enabled;
    mot["threshold"]        = appConfig.motion.threshold;
    mot["cooldown_sec"]     = appConfig.motion.cooldown_sec;
    mot["min_area_pct"]     = appConfig.motion.min_area_pct;
    mot["max_area_pct"]     = appConfig.motion.max_area_pct;
    mot["ema_alpha_day"]    = appConfig.motion.ema_alpha_day;
    mot["ema_alpha_night"]  = appConfig.motion.ema_alpha_night;
    mot["training_frames"]  = appConfig.motion.training_frames;
    mot["agc_gain_factor"]  = appConfig.motion.agc_gain_factor;
    mot["brightness_min"]   = appConfig.motion.brightness_min;
    mot["temporal_filter"]  = appConfig.motion.temporal_filter;
    mot["spatial_filter"]   = appConfig.motion.spatial_filter;
    mot["night_suppress"]   = appConfig.motion.night_suppress;
    mot["save_to_sd"]       = appConfig.motion.save_to_sd;

    // Face detection
    JsonObject fd = doc["face_detect"].to<JsonObject>();
    fd["enabled"]         = appConfig.face_detect.enabled;
    fd["score_threshold"] = appConfig.face_detect.score_threshold;
    fd["nms_threshold"]   = appConfig.face_detect.nms_threshold;
    fd["two_stage"]       = appConfig.face_detect.two_stage;
    fd["cooldown_sec"]    = appConfig.face_detect.cooldown_sec;
    fd["save_to_sd"]      = appConfig.face_detect.save_to_sd;

    // Person detection
    JsonObject pd = doc["person_detect"].to<JsonObject>();
    pd["enabled"]              = appConfig.person_detect.enabled;
    pd["confidence_threshold"] = appConfig.person_detect.confidence_threshold;
    pd["temporal_frames"]      = appConfig.person_detect.temporal_frames;
    pd["cooldown_sec"]         = appConfig.person_detect.cooldown_sec;
    pd["save_to_sd"]           = appConfig.person_detect.save_to_sd;

    // Timelapse
    JsonObject tl = doc["timelapse"].to<JsonObject>();
    tl["enabled"]      = appConfig.timelapse.enabled;
    tl["interval_sec"] = appConfig.timelapse.interval_sec;
    tl["save_to_sd"]   = appConfig.timelapse.save_to_sd;

    // MQTT
    JsonObject mq = doc["mqtt"].to<JsonObject>();
    mq["enabled"]      = appConfig.mqtt.enabled;
    mq["server"]       = appConfig.mqtt.server;
    mq["port"]         = appConfig.mqtt.port;
    mq["topic_prefix"] = appConfig.mqtt.topic_prefix;

    // Telegram (token + chat_id stored in NVS, not JSON)
    JsonObject tg = doc["telegram"].to<JsonObject>();
    tg["enabled"]           = appConfig.telegram.enabled;
    tg["notify_on_motion"]  = appConfig.telegram.notify_on_motion;
    tg["notify_on_face"]    = appConfig.telegram.notify_on_face;
    tg["photo_on_motion"]   = appConfig.telegram.photo_on_motion;
    tg["photo_on_face"]     = appConfig.telegram.photo_on_face;
    tg["notify_on_person"]  = appConfig.telegram.notify_on_person;
    tg["photo_on_person"]   = appConfig.telegram.photo_on_person;
    tg["cooldown_sec"]      = appConfig.telegram.cooldown_sec;
    tg["active_start_hour"] = appConfig.telegram.active_start_hour;
    tg["active_end_hour"]   = appConfig.telegram.active_end_hour;
    tg["poll_interval_ms"]  = appConfig.telegram.poll_interval_ms;

    // Auth (user only; password in NVS)
    JsonObject auth = doc["auth"].to<JsonObject>();
    auth["user"] = appConfig.auth.http_user;

    // Misc
    doc["led_enabled"] = appConfig.led_enabled;
    doc["idle_fps"]    = appConfig.idle_fps;
    doc["active_fps"]  = appConfig.active_fps;

    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) {
        Serial.printf("[%s] Failed to create config file\n", TAG);
        return false;
    }

    serializeJsonPretty(doc, f);
    f.close();

    Serial.printf("[%s] Config saved\n", TAG);
    return true;
}

void saveSecretsToNVS() {
    nvs.begin("cams3", false);
    nvs.putString("wifi_ssid", appConfig.wifi.ssid);
    nvs.putString("wifi_pass", appConfig.wifi.password);
    nvs.putString("http_pass", appConfig.auth.http_pass);
    nvs.putString("mqtt_user", appConfig.mqtt.user);
    nvs.putString("mqtt_pass", appConfig.mqtt.password);
    nvs.putString("tg_token", appConfig.telegram.bot_token);
    nvs.putString("tg_chat_id", appConfig.telegram.chat_id);
    nvs.end();
}

void resetConfig() {
    appConfig = AppConfig();
    LittleFS.remove(CONFIG_FILE);
    nvs.begin("cams3", false);
    nvs.clear();
    nvs.end();
    Serial.printf("[%s] Config reset to defaults\n", TAG);
}

void applyConfigToCamera() {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        Serial.printf("[%s] Camera sensor not available\n", TAG);
        return;
    }

    s->set_framesize(s, (framesize_t)appConfig.camera.frame_size);
    s->set_quality(s, appConfig.camera.jpeg_quality);
    s->set_vflip(s, appConfig.camera.vflip ? 1 : 0);
    s->set_hmirror(s, appConfig.camera.hmirror ? 1 : 0);
    s->set_brightness(s, appConfig.camera.brightness);
    s->set_contrast(s, appConfig.camera.contrast);
    s->set_saturation(s, appConfig.camera.saturation);
    s->set_sharpness(s, appConfig.camera.sharpness);
    s->set_denoise(s, appConfig.camera.denoise);
    s->set_ae_level(s, appConfig.camera.ae_level);
    s->set_aec_value(s, appConfig.camera.aec_value);
    s->set_agc_gain(s, appConfig.camera.agc_gain);
    s->set_gainceiling(s, (gainceiling_t)appConfig.camera.gainceiling);
    s->set_wb_mode(s, appConfig.camera.wb_mode);
    s->set_aec2(s, appConfig.camera.aec ? 1 : 0);
    s->set_gain_ctrl(s, appConfig.camera.agc ? 1 : 0);
    s->set_whitebal(s, appConfig.camera.awb ? 1 : 0);
    s->set_bpc(s, appConfig.camera.bpc ? 1 : 0);
    s->set_wpc(s, appConfig.camera.wpc ? 1 : 0);
    s->set_raw_gma(s, appConfig.camera.raw_gma ? 1 : 0);
    s->set_lenc(s, appConfig.camera.lenc ? 1 : 0);

    Serial.printf("[%s] Camera settings applied\n", TAG);
}
