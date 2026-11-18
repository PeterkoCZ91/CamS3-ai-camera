#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Config version for migration
#define CONFIG_VERSION 3

// HTTP ports
#define HTTP_PORT       80
#define STREAM_PORT     81   // analytics (/detection-stream), /snapshot, legacy /stream
#define VIEWER_STREAM_PORT 82  // /stream for browsers, so it never waits behind A12

// Default AP credentials
#define DEFAULT_AP_SSID    "CamS3-Setup"
#define DEFAULT_AP_PASS    "cams3admin"

// Default HTTP auth
#define DEFAULT_HTTP_USER  "admin"
#define DEFAULT_HTTP_PASS  "admin"

// Max stream clients
#define MAX_STREAM_CLIENTS 3

// Capture task
#define CAPTURE_TASK_STACK   6144
#define CAPTURE_TASK_PRIO    6
#define STREAM_TASK_PRIO     5

// Frame buffer config
// FRAMESIZE_UXGA (1600x1200). QSXGA (framesize_t 21, 2560x1920) is the sensor
// maximum, but the PY260 driver in arduino-esp32 2.0.17 cannot sustain capture at
// that size (0 fps, errors) — which is why the UI only offers up to UXGA.
// (The old comment said "QSXGA (14)"; 14 is FHD. 21 is QSXGA.)
// frame_size in config.json, /api/settings, the UI and A12 uses the framesize_t
// numbering of esp32-camera as shipped with arduino-esp32 2.0.x (13 = UXGA). The
// esp32-camera in core 3.x inserted FRAMESIZE_128X128 and FRAMESIZE_320X320 into
// the enum, shifting every size from QVGA up by two (13 became HD). The stored and
// published numbers stay as they were; cameraFrameSizeFromApi() converts at the
// driver boundary. Never cast appConfig.camera.frame_size to framesize_t directly.
#define API_FRAMESIZE_QVGA   5
#define API_FRAMESIZE_UXGA   13
#define FB_COUNT             2
#define JPEG_QUALITY_DEFAULT 8
#define FRAME_SIZE_DEFAULT   13

// Motion detection defaults (tuned 2026-04-20 against PY260 low-light noise)
#define MOTION_THRESHOLD_DEFAULT    20
#define MOTION_COOLDOWN_DEFAULT     10  // seconds
#define MOTION_MIN_AREA_DEFAULT     5   // percent
#define MOTION_MAX_AREA_DEFAULT     50  // percent - upper reject (lighting change)
#define MOTION_EMA_ALPHA_DAY        0.92f
#define MOTION_EMA_ALPHA_NIGHT      0.98f
#define MOTION_TRAINING_FRAMES      15
#define MOTION_AGC_GAIN_FACTOR      1.0f
#define MOTION_BRIGHTNESS_MIN       10

// Face detection defaults
#define FACE_SCORE_THRESHOLD_DEFAULT  0.5f
#define FACE_NMS_THRESHOLD_DEFAULT    0.3f
#define FACE_COOLDOWN_DEFAULT         5  // seconds

// Person detection (FOMO) defaults
#define PD_CONFIDENCE_THRESHOLD_DEFAULT  0.60f
#define PD_CONFIDENT_THRESHOLD_DEFAULT   0.75f
#define PD_TEMPORAL_FRAMES_DEFAULT       2
#define PD_COOLDOWN_DEFAULT              10  // seconds
#define PD_INPUT_SIZE                    64  // FOMO model input 64x64

// Timelapse defaults
#define TIMELAPSE_INTERVAL_DEFAULT  60  // seconds

// SD card
#define SD_MAX_USAGE_PERCENT  90

// MQTT defaults
#define MQTT_PORT_DEFAULT     1883

struct CameraSettings {
    int frame_size    = FRAME_SIZE_DEFAULT;
    int jpeg_quality  = JPEG_QUALITY_DEFAULT;
    bool vflip        = true;   // M5Stack Unit CamS3 is typically mounted upside-down
    bool hmirror      = false;
    int brightness    = 0;     // -2 to 2
    int contrast      = 1;     // -2 to 2
    int saturation    = 0;     // -2 to 2
    int sharpness     = 2;     // -3 to 3
    int denoise       = 0;     // 0 to 8
    int ae_level      = 0;     // -2 to 2
    int aec_value     = 300;   // 0 to 1200
    int agc_gain      = 0;     // 0 to 30
    int gainceiling   = 0;     // 0 to 6
    int wb_mode       = 0;     // 0 to 4
    bool aec          = true;   // exposure control (set_exposure_ctrl)
    bool aec2         = true;   // AEC DSP refinement (set_aec2) — separate knob
    bool agc          = true;
    bool awb          = true;
    bool bpc          = true;
    bool wpc          = true;
    bool raw_gma      = true;
    bool lenc         = true;
    // Opt-in night overlay (see cameraAutoProfileTick). Off by default: untested on PY260.
    bool auto_profile = false;
};

struct WiFiSettings {
    String ssid;
    String password;
    String hostname = "cams3";
};

struct MotionSettings {
    bool enabled           = false;
    int threshold          = MOTION_THRESHOLD_DEFAULT;
    int cooldown_sec       = MOTION_COOLDOWN_DEFAULT;
    int min_area_pct       = MOTION_MIN_AREA_DEFAULT;
    int max_area_pct       = MOTION_MAX_AREA_DEFAULT;
    float ema_alpha_day    = MOTION_EMA_ALPHA_DAY;
    float ema_alpha_night  = MOTION_EMA_ALPHA_NIGHT;
    int training_frames    = MOTION_TRAINING_FRAMES;
    float agc_gain_factor  = MOTION_AGC_GAIN_FACTOR;
    int brightness_min     = MOTION_BRIGHTNESS_MIN;
    bool temporal_filter   = true;
    bool spatial_filter    = true;
    bool night_suppress    = true;
    bool save_to_sd        = false;
};

struct FaceDetectSettings {
    bool enabled           = false;
    float score_threshold  = FACE_SCORE_THRESHOLD_DEFAULT;
    float nms_threshold    = FACE_NMS_THRESHOLD_DEFAULT;
    bool two_stage         = true;
    int cooldown_sec       = FACE_COOLDOWN_DEFAULT;
    bool save_to_sd        = false;
};

struct PersonDetectSettings {
    bool enabled                = false;
    float confidence_threshold  = PD_CONFIDENCE_THRESHOLD_DEFAULT;
    // Above this score a detection is CONFIDENT (direct notification); between the
    // two thresholds it is UNCERTAIN and only goes out over MQTT for A12/YOLO
    // verification. Was a compile-time constant, so the split point could not be
    // tuned per install.
    float confident_threshold   = PD_CONFIDENT_THRESHOLD_DEFAULT;
    int temporal_frames         = PD_TEMPORAL_FRAMES_DEFAULT;
    int cooldown_sec            = PD_COOLDOWN_DEFAULT;
    bool save_to_sd             = false;
};

struct TimelapseSettings {
    bool enabled        = false;
    int interval_sec    = TIMELAPSE_INTERVAL_DEFAULT;
    bool save_to_sd     = true;
};

struct MqttSettings {
    bool enabled       = false;
    String server;
    int port           = MQTT_PORT_DEFAULT;
    String user;
    String password;
    String topic_prefix = "cams3";
    bool tls_enabled   = false;  // Use WiFiClientSecure (setInsecure when no CA pinned)
};

// Tracker defaults (person detection tracking)
#define TRACKER_CONFIRM_HITS_DEFAULT   3
#define TRACKER_MAX_MISSES_DEFAULT     5
#define TRACKER_MATCH_DIST_DEFAULT     40

struct TrackerSettings {
    int confirm_hits  = TRACKER_CONFIRM_HITS_DEFAULT;
    int max_misses    = TRACKER_MAX_MISSES_DEFAULT;
    int match_dist    = TRACKER_MATCH_DIST_DEFAULT;
};

// Telegram defaults
#define TELEGRAM_COOLDOWN_DEFAULT   30
#define TELEGRAM_ACTIVE_START_DEFAULT 0
#define TELEGRAM_ACTIVE_END_DEFAULT   23
// Capped at 15 s: the Telegram task's long poll must finish well inside the task WDT
// (a 30 s default combined with a >20 s configured value panicked into a boot loop).
#define TELEGRAM_POLL_INTERVAL_MS_DEFAULT 15000

struct TelegramSettings {
    bool enabled             = false;
    String bot_token;               // stored in NVS, not JSON
    String chat_id;                 // stored in NVS, not JSON
    bool notify_on_motion    = true;
    bool notify_on_face      = true;
    bool photo_on_motion     = true;
    bool photo_on_face       = true;
    int cooldown_sec         = TELEGRAM_COOLDOWN_DEFAULT;
    bool notify_on_person   = true;
    bool photo_on_person    = true;
    int active_start_hour    = TELEGRAM_ACTIVE_START_DEFAULT;  // 0-23
    int active_end_hour      = TELEGRAM_ACTIVE_END_DEFAULT;    // 0-23, supports overnight wrap
    int poll_interval_ms     = TELEGRAM_POLL_INTERVAL_MS_DEFAULT;
};

struct AuthSettings {
    String http_user = DEFAULT_HTTP_USER;
    String http_pass = DEFAULT_HTTP_PASS;
    bool csrf_required = true;  // When true, POST handlers require X-CSRF-Token matching /api/csrf
};

struct AppConfig {
    int version = CONFIG_VERSION;
    CameraSettings camera;
    WiFiSettings wifi;
    MotionSettings motion;
    FaceDetectSettings face_detect;
    PersonDetectSettings person_detect;
    TimelapseSettings timelapse;
    MqttSettings mqtt;
    TelegramSettings telegram;
    TrackerSettings tracker;
    AuthSettings auth;
    bool led_enabled = true;
    int idle_fps     = 1;     // FPS when no client streaming
    int active_fps   = 15;    // FPS when client streaming
    int lens_fov_deg = 0;     // horizontal FOV of the fitted lens, owner-entered (0 = unknown)
    int lens_ref_fov_deg = 0; // FOV the pixel thresholds were tuned for; 0 = FOV scaling off
};

// Global config instance
extern AppConfig appConfig;

// Config persistence functions
bool loadConfig();
bool saveConfig();
void resetConfig();

// Clamp every numeric / name field of appConfig into its valid range. Single source
// of truth shared by the loader (config.json is untrusted after a corrupt write or
// hand edit) and POST /api/settings, so the two can never disagree about limits.
void sanitizeConfig();

// Persist secrets to NVS. Same as saveSecretsToNVS() (kept void for existing callers)
// but reports failure so the API does not claim success for a write that did not land.
bool saveSecretsToNVSChecked();

// Push appConfig.camera to the sensor over SCCB (I2C).
// MUST be called from the capture task (or before it starts) — the sensor bus is
// not safe to touch while esp_camera_fb_get() is mid-frame. HTTP handlers ask for
// this via cameraRequestSettingsApply() instead of calling it directly.
// applyFrameSize is false for runtime applies: changing the frame size under a
// running driver leaves the DMA buffers sized for the old resolution, so that path
// goes through a full cameraReinit() instead.
void applyConfigToCamera(bool applyFrameSize = true);
// True while the auto night profile overlay is active (owned by the capture task).
bool cameraNightProfileActive();

#endif // CONFIG_H
