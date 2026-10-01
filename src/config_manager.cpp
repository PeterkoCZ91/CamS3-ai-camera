#include "config.h"
#include "ws_log.h"
#include <LittleFS.h>
#include <Preferences.h>
#include "esp_camera.h"
#include "board_config.h"
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char* TAG = "ConfigMgr";
static const char* CONFIG_FILE     = "/config.json";
static const char* CONFIG_FILE_TMP = "/config.json.tmp";
static const char* CONFIG_FILE_BAK = "/config.json.bak";
static Preferences nvs;

// One recursive mutex for load/save/reset/secrets: callers are async handlers, the
// telegram and mqtt tasks and the main loop, and the tmp filename + `nvs` handle are shared.
// Recursive because loadConfig -> saveSecretsToNVS (migration) nests.
static SemaphoreHandle_t cfgMutex = nullptr;
static portMUX_TYPE cfgMutexInit = portMUX_INITIALIZER_UNLOCKED;
struct CfgLock {
    CfgLock() {
        if (!cfgMutex) {  // create once, race-safe from any task
            SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
            portENTER_CRITICAL(&cfgMutexInit);
            if (!cfgMutex) { cfgMutex = m; m = nullptr; }
            portEXIT_CRITICAL(&cfgMutexInit);
            if (m) vSemaphoreDelete(m);
        }
        xSemaphoreTakeRecursive(cfgMutex, portMAX_DELAY);
    }
    ~CfgLock() { xSemaphoreGiveRecursive(cfgMutex); }
};

AppConfig appConfig;

// Secrets are AES-256-CTR encrypted in NVS using a device-bound key.
// Threat model: defends against casual flash dumps; not against an attacker
// who can also read the salt + eFuse MAC (derivation inputs) from the device.
// Not a replacement for flash encryption, but raises the bar significantly.
static uint8_t  secretsKey[32];
static bool     secretsKeyReady = false;
static const uint8_t ENC_MAGIC[4]  = { 'E', 'N', 'C', '1' };  // legacy: header + ciphertext
// ENC2 = same 20-byte header + ciphertext + 8-byte HMAC tag, so a wrong key (lost salt)
// is detected instead of decrypting to garbage. ENC1 blobs are still accepted and rewritten.
static const uint8_t ENC_MAGIC2[4] = { 'E', 'N', 'C', '2' };
static const size_t  ENC_TAG_LEN   = 8;

static void deriveSecretsKey() {
    if (secretsKeyReady) return;

    uint8_t mac[8] = {0};
    esp_efuse_mac_get_default(mac);

    Preferences k;
    k.begin("keyring", false);
    uint8_t salt[16];
    size_t saltLen = k.getBytesLength("salt");
    if (saltLen == sizeof(salt)) {
        k.getBytes("salt", salt, sizeof(salt));
    } else {
        esp_fill_random(salt, sizeof(salt));
        bool saltOk = k.putBytes("salt", salt, sizeof(salt)) == sizeof(salt);
        // Loud: any ENC1 secrets stored under the old salt are now unrecoverable garbage.
        logCapture("[%s] Generated new secrets encryption salt (previous len %u, stored=%d) -- existing encrypted secrets cannot be decrypted\n",
                   TAG, (unsigned)saltLen, (int)saltOk);
    }
    k.end();

    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md, mac, 8, salt, sizeof(salt), secretsKey);
    secretsKeyReady = true;
}

// Put an encrypted secret under `key` (binary blob). Empty strings stored as empty blob.
static void encTag(const uint8_t* blob, size_t pl, uint8_t* tag) {
    uint8_t full[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), secretsKey, 32, blob, 20 + pl, full);
    memcpy(tag, full, ENC_TAG_LEN);
}

static bool encPutSecret(Preferences& p, const char* key, const String& plaintext) {
    if (plaintext.length() == 0) {
        // Intentionally empty: the only case where a key is deleted.
        if (!p.isKey(key)) return true;
        return p.remove(key);
    }
    deriveSecretsKey();

    size_t pl = plaintext.length();
    size_t blobLen = 4 + 16 + pl + ENC_TAG_LEN;  // magic + IV + ciphertext + tag
    uint8_t* blob = (uint8_t*)malloc(blobLen);
    if (!blob) return false;

    memcpy(blob, ENC_MAGIC2, 4);
    esp_fill_random(blob + 4, 16);

    uint8_t nonceCounter[16];
    memcpy(nonceCounter, blob + 4, 16);
    uint8_t streamBlock[16] = {0};
    size_t ncOff = 0;

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, secretsKey, 256);
    mbedtls_aes_crypt_ctr(&ctx, pl, &ncOff, nonceCounter, streamBlock,
                          (const unsigned char*)plaintext.c_str(), blob + 20);
    mbedtls_aes_free(&ctx);
    encTag(blob, pl, blob + 20 + pl);

    bool ok = p.putBytes(key, blob, blobLen) == blobLen;
    free(blob);
    return ok;
}

// Set to true by encGetSecret whenever a legacy plaintext entry is read.
// loadSecretsFromNVS uses it to trigger a one-shot re-encryption pass.
static bool secretsNeedMigration = false;

// Read an encrypted secret. If the stored bytes are NOT encrypted (legacy plaintext
// stored via putString), fall back to that and silently upgrade on next save.
static String encGetSecret(Preferences& p, const char* key, const char* defaultVal = "") {
    size_t len = p.getBytesLength(key);
    if (len == 0) {
        // Legacy plaintext written via putString has a different NVS entry type;
        // Preferences::getString handles that path. Keep backward-compat.
        String legacy = p.getString(key, defaultVal);
        if (legacy.length() > 0 && legacy != String(defaultVal)) secretsNeedMigration = true;
        return legacy;
    }

    uint8_t* blob = (uint8_t*)malloc(len);
    if (!blob) return String(defaultVal);
    p.getBytes(key, blob, len);

    // Not our magic → treat as raw string bytes (migrate target).
    bool isV2 = len >= 20 + ENC_TAG_LEN && memcmp(blob, ENC_MAGIC2, 4) == 0;
    bool isV1 = len >= 20 && memcmp(blob, ENC_MAGIC, 4) == 0;
    if (!isV1 && !isV2) {
        String fallback = p.getString(key, defaultVal);
        free(blob);
        if (fallback.length() > 0 && fallback != String(defaultVal)) secretsNeedMigration = true;
        return fallback;
    }

    deriveSecretsKey();
    size_t pl = len - 20 - (isV2 ? ENC_TAG_LEN : 0);
    if (isV2) {
        uint8_t tag[ENC_TAG_LEN];
        encTag(blob, pl, tag);
        if (memcmp(tag, blob + 20 + pl, ENC_TAG_LEN) != 0) {
            // Wrong key (salt regenerated) or corrupt blob: return default, not garbage.
            logCapture("[%s] Secret '%s' failed integrity check, using default\n", TAG, key);
            free(blob);
            return String(defaultVal);
        }
    } else {
        secretsNeedMigration = true;  // ENC1 -> ENC2 rewrite (adds integrity tag)
    }
    uint8_t* out = (uint8_t*)malloc(pl + 1);
    if (!out) { free(blob); return String(defaultVal); }

    uint8_t nonceCounter[16];
    memcpy(nonceCounter, blob + 4, 16);
    uint8_t streamBlock[16] = {0};
    size_t ncOff = 0;

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, secretsKey, 256);
    mbedtls_aes_crypt_ctr(&ctx, pl, &ncOff, nonceCounter, streamBlock,
                          blob + 20, out);
    mbedtls_aes_free(&ctx);

    out[pl] = 0;
    String result = String((char*)out);
    free(out);
    free(blob);
    return result;
}

void saveSecretsToNVS();

static void loadSecretsFromNVS() {
    CfgLock lock;
    nvs.begin("cams3", true);  // read-only
    appConfig.wifi.ssid           = encGetSecret(nvs, "wifi_ssid", "");
    appConfig.wifi.password       = encGetSecret(nvs, "wifi_pass", "");
    appConfig.auth.http_pass      = encGetSecret(nvs, "http_pass", DEFAULT_HTTP_PASS);
    appConfig.mqtt.user           = encGetSecret(nvs, "mqtt_user", "");
    appConfig.mqtt.password       = encGetSecret(nvs, "mqtt_pass", "");
    appConfig.telegram.bot_token  = encGetSecret(nvs, "tg_token", "");
    appConfig.telegram.chat_id    = encGetSecret(nvs, "tg_chat_id", "");
    nvs.end();

    if (secretsNeedMigration) {
        logCapture("[%s] Migrating legacy plaintext secrets -> AES-256-CTR\n", TAG);
        saveSecretsToNVS();
        secretsNeedMigration = false;
    }
}

// Parse a config file into doc. Returns false (and logs) if missing/unreadable/invalid.
static bool readConfigFile(const char* path, JsonDocument& doc) {
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) {
        logCapture("[%s] Failed to open %s\n", TAG, path);
        return false;
    }
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        logCapture("[%s] JSON parse error in %s: %s\n", TAG, path, err.c_str());
        return false;
    }
    return true;
}

bool loadConfig() {
    CfgLock lock;
    // Always load secrets from NVS first — independent of LittleFS health.
    // Previously secrets loaded only at the end of JSON parse; an unmountable
    // filesystem (fresh flash, missing fs image) left WiFi creds empty forever.
    loadSecretsFromNVS();

    // Recovery: previous save was interrupted between tmp-write and rename.
    if (!LittleFS.exists(CONFIG_FILE) && LittleFS.exists(CONFIG_FILE_TMP)) {
        logCapture("[%s] Recovering config from .tmp\n", TAG);
        LittleFS.rename(CONFIG_FILE_TMP, CONFIG_FILE);
    } else if (LittleFS.exists(CONFIG_FILE_TMP)) {
        LittleFS.remove(CONFIG_FILE_TMP);
    }

    if (!LittleFS.exists(CONFIG_FILE) && !LittleFS.exists(CONFIG_FILE_BAK)) {
        logCapture("[%s] No config file, using defaults (secrets already loaded from NVS)\n", TAG);
        return false;
    }

    JsonDocument doc;
    if (!readConfigFile(CONFIG_FILE, doc)) {
        doc.clear();
        // Truncated/corrupt (e.g. full filesystem or power cut): fall back to last good copy.
        if (!readConfigFile(CONFIG_FILE_BAK, doc)) {
            logCapture("[%s] No usable config (main and .bak), using defaults\n", TAG);
            return false;
        }
        logCapture("[%s] WARNING: config.json unusable, loaded config.json.bak\n", TAG);
    }

    int ver = doc["version"] | 0;
    if (ver < CONFIG_VERSION) {
        logCapture("[%s] Config migration from v%d to v%d\n", TAG, ver, CONFIG_VERSION);
    }

    // Camera. Fallbacks mirror the CameraSettings struct defaults — they used to
    // differ (vflip false vs true, contrast 0 vs 1, sharpness 0 vs 2), so a config
    // written by an older build flipped the image upside down on the next boot.
    JsonObject cam = doc["camera"];
    if (cam) {
        CameraSettings d;  // struct defaults
        appConfig.camera.frame_size   = cam["frame_size"]   | d.frame_size;
        appConfig.camera.jpeg_quality = cam["jpeg_quality"]  | d.jpeg_quality;
        appConfig.camera.vflip        = cam["vflip"]         | d.vflip;
        appConfig.camera.hmirror      = cam["hmirror"]       | d.hmirror;
        appConfig.camera.brightness   = cam["brightness"]    | d.brightness;
        appConfig.camera.contrast     = cam["contrast"]      | d.contrast;
        appConfig.camera.saturation   = cam["saturation"]    | d.saturation;
        appConfig.camera.sharpness    = cam["sharpness"]     | d.sharpness;
        appConfig.camera.denoise      = cam["denoise"]       | d.denoise;
        appConfig.camera.ae_level     = cam["ae_level"]      | d.ae_level;
        appConfig.camera.aec_value    = cam["aec_value"]     | d.aec_value;
        appConfig.camera.agc_gain     = cam["agc_gain"]      | d.agc_gain;
        appConfig.camera.gainceiling  = cam["gainceiling"]   | d.gainceiling;
        appConfig.camera.wb_mode      = cam["wb_mode"]       | d.wb_mode;
        appConfig.camera.aec          = cam["aec"]           | d.aec;
        appConfig.camera.aec2         = cam["aec2"]          | d.aec2;
        appConfig.camera.agc          = cam["agc"]           | d.agc;
        appConfig.camera.awb          = cam["awb"]           | d.awb;
        appConfig.camera.bpc          = cam["bpc"]           | d.bpc;
        appConfig.camera.wpc          = cam["wpc"]           | d.wpc;
        appConfig.camera.raw_gma      = cam["raw_gma"]       | d.raw_gma;
        appConfig.camera.lenc         = cam["lenc"]          | d.lenc;
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
        // Clamp to the same 10..100 range the API accepts; the old code forced the
        // value back down to the 50 % default, so a raised limit never survived a boot.
        appConfig.motion.max_area_pct     = mot["max_area_pct"]     | MOTION_MAX_AREA_DEFAULT;
        if (appConfig.motion.max_area_pct < 10)  appConfig.motion.max_area_pct = 10;
        if (appConfig.motion.max_area_pct > 100) appConfig.motion.max_area_pct = 100;
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
        appConfig.person_detect.confident_threshold   = pd["confident_threshold"]   | PD_CONFIDENT_THRESHOLD_DEFAULT;
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
        appConfig.mqtt.tls_enabled  = mq["tls_enabled"]  | false;
    }

    // Tracker
    JsonObject tr = doc["tracker"];
    if (tr) {
        appConfig.tracker.confirm_hits = tr["confirm_hits"] | TRACKER_CONFIRM_HITS_DEFAULT;
        appConfig.tracker.max_misses   = tr["max_misses"]   | TRACKER_MAX_MISSES_DEFAULT;
        appConfig.tracker.match_dist   = tr["match_dist"]   | TRACKER_MATCH_DIST_DEFAULT;
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

    // Auth (passwords from NVS for security).
    // csrf_required defaults to TRUE to match AuthSettings — the previous `| false`
    // silently disabled CSRF for every device that had ever saved a config.
    appConfig.auth.http_user     = doc["auth"]["user"]           | String(DEFAULT_HTTP_USER);
    appConfig.auth.csrf_required = doc["auth"]["csrf_required"]  | true;

    // Misc
    appConfig.led_enabled = doc["led_enabled"] | true;
    appConfig.idle_fps    = doc["idle_fps"]    | 1;
    appConfig.active_fps  = doc["active_fps"]  | 15;

    // NVS secrets already loaded above — don't duplicate.

    logCapture("[%s] Config loaded (v%d)\n", TAG, appConfig.version);
    return true;
}

bool saveConfig() {
    CfgLock lock;
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
    cam["aec2"]          = appConfig.camera.aec2;
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
    pd["confident_threshold"]  = appConfig.person_detect.confident_threshold;
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
    mq["tls_enabled"]  = appConfig.mqtt.tls_enabled;

    // Tracker
    JsonObject trk = doc["tracker"].to<JsonObject>();
    trk["confirm_hits"] = appConfig.tracker.confirm_hits;
    trk["max_misses"]   = appConfig.tracker.max_misses;
    trk["match_dist"]   = appConfig.tracker.match_dist;

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
    auth["user"]          = appConfig.auth.http_user;
    auth["csrf_required"] = appConfig.auth.csrf_required;

    // Misc
    doc["led_enabled"] = appConfig.led_enabled;
    doc["idle_fps"]    = appConfig.idle_fps;
    doc["active_fps"]  = appConfig.active_fps;

    // Atomic save: write to .tmp, then rename over the original.
    // If power fails mid-write the original remains intact; loader will recover .tmp if rename failed.
    if (LittleFS.exists(CONFIG_FILE_TMP)) {
        LittleFS.remove(CONFIG_FILE_TMP);
    }
    File f = LittleFS.open(CONFIG_FILE_TMP, "w");
    if (!f) {
        logCapture("[%s] Failed to create config tmp file\n", TAG);
        return false;
    }

    size_t expected = measureJsonPretty(doc);
    size_t written = serializeJsonPretty(doc, f);
    f.close();
    // A full LittleFS yields a short write; never rename a truncated file over the good config.
    size_t onDisk = 0;
    { File chk = LittleFS.open(CONFIG_FILE_TMP, "r"); if (chk) { onDisk = chk.size(); chk.close(); } }
    if (written == 0 || written != expected || onDisk != expected) {
        logCapture("[%s] Config tmp write incomplete (wrote %u, expected %u, on disk %u) -- keeping old config\n",
                   TAG, (unsigned)written, (unsigned)expected, (unsigned)onDisk);
        LittleFS.remove(CONFIG_FILE_TMP);
        return false;
    }

    // Keep the last good file as .bak, then move tmp into place. If power dies between the
    // two renames, loadConfig recovers from .tmp (complete, verified above) or .bak.
    if (LittleFS.exists(CONFIG_FILE)) {
        LittleFS.remove(CONFIG_FILE_BAK);
        if (!LittleFS.rename(CONFIG_FILE, CONFIG_FILE_BAK)) {
            logCapture("[%s] Could not create config.json.bak\n", TAG);
            LittleFS.remove(CONFIG_FILE);  // tmp is verified, proceed
        }
    }
    if (!LittleFS.rename(CONFIG_FILE_TMP, CONFIG_FILE)) {
        logCapture("[%s] Atomic rename failed\n", TAG);
        return false;
    }

    logCapture("[%s] Config saved (atomic, %u bytes)\n", TAG, (unsigned)written);
    return true;
}

// Is `key` already stored as an ENC2 blob that decrypts to `want`? Then no write is needed.
static bool secretUnchanged(Preferences& p, const char* key, const String& want) {
    if (want.length() == 0) return !p.isKey(key);
    size_t len = p.getBytesLength(key);
    if (len != 4 + 16 + want.length() + ENC_TAG_LEN) return false;
    uint8_t magic[4];
    if (p.getBytes(key, magic, 4) != 4 || memcmp(magic, ENC_MAGIC2, 4) != 0) return false;
    bool saved = secretsNeedMigration;
    String cur = encGetSecret(p, key, "");
    secretsNeedMigration = saved;
    return cur == want;
}

void saveSecretsToNVS() {
    CfgLock lock;
    if (!nvs.begin("cams3", false)) {
        logCapture("[%s] ERROR: cannot open NVS for secrets, nothing saved\n", TAG);
        return;
    }
    struct { const char* key; const String* val; } items[] = {
        {"wifi_ssid", &appConfig.wifi.ssid}, {"wifi_pass", &appConfig.wifi.password},
        {"http_pass", &appConfig.auth.http_pass},
        {"mqtt_user", &appConfig.mqtt.user}, {"mqtt_pass", &appConfig.mqtt.password},
        {"tg_token", &appConfig.telegram.bot_token}, {"tg_chat_id", &appConfig.telegram.chat_id},
    };
    for (auto& it : items) {
        // Write per key, only if changed: no remove-all window where a power cut loses wifi creds.
        if (secretUnchanged(nvs, it.key, *it.val)) continue;
        // A legacy plaintext (string-typed) entry can't be overwritten by a blob: drop just that one.
        if (it.val->length() > 0 && nvs.isKey(it.key) && nvs.getBytesLength(it.key) == 0) nvs.remove(it.key);
        if (!encPutSecret(nvs, it.key, *it.val)) {
            logCapture("[%s] ERROR: failed to store secret '%s' in NVS\n", TAG, it.key);
        }
    }
    nvs.end();
}

void resetConfig() {
    CfgLock lock;
    appConfig = AppConfig();
    LittleFS.remove(CONFIG_FILE);
    LittleFS.remove(CONFIG_FILE_BAK);  // otherwise loadConfig would resurrect the old config
    LittleFS.remove(CONFIG_FILE_TMP);
    nvs.begin("cams3", false);
    nvs.clear();
    nvs.end();
    logCapture("[%s] Config reset to defaults\n", TAG);
}

void applyConfigToCamera(bool applyFrameSize) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        logCapture("[%s] Camera sensor not available\n", TAG);
        return;
    }

    if (applyFrameSize) {
        s->set_framesize(s, (framesize_t)appConfig.camera.frame_size);
    }
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
    // "AEC" in the UI means auto exposure — that is set_exposure_ctrl(). The old
    // code only called set_aec2() (the DSP refinement stage), so turning AEC off
    // left auto exposure running and the manual aec_value had no effect.
    s->set_exposure_ctrl(s, appConfig.camera.aec ? 1 : 0);
    s->set_aec2(s, appConfig.camera.aec2 ? 1 : 0);
    s->set_gain_ctrl(s, appConfig.camera.agc ? 1 : 0);
    s->set_whitebal(s, appConfig.camera.awb ? 1 : 0);
    s->set_bpc(s, appConfig.camera.bpc ? 1 : 0);
    s->set_wpc(s, appConfig.camera.wpc ? 1 : 0);
    s->set_raw_gma(s, appConfig.camera.raw_gma ? 1 : 0);
    s->set_lenc(s, appConfig.camera.lenc ? 1 : 0);

    logCapture("[%s] Camera settings applied\n", TAG);
}
