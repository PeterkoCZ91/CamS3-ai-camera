#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Config version for migration
#define CONFIG_VERSION 3

// HTTP ports
#define HTTP_PORT       80
#define STREAM_PORT     81

// Default AP credentials
#define DEFAULT_AP_SSID    "CamS3-Setup"
#define DEFAULT_AP_PASS    "cams3admin"

// Default HTTP auth
#define DEFAULT_HTTP_USER  "admin"
#define DEFAULT_HTTP_PASS  "admin"

// Max stream clients
#define MAX_STREAM_CLIENTS 3

// Capture task
#define CAPTURE_TASK_STACK   4096
#define CAPTURE_TASK_PRIO    6
#define STREAM_TASK_PRIO     5

// Frame buffer config
#define FB_COUNT             2
#define JPEG_QUALITY_DEFAULT 12
#define FRAME_SIZE_DEFAULT   13  // FRAMESIZE_UXGA (1600x1200)

// Motion detection defaults
#define MOTION_THRESHOLD_DEFAULT    15
#define MOTION_COOLDOWN_DEFAULT     10  // seconds
#define MOTION_MIN_AREA_DEFAULT     5   // percent
#define MOTION_MAX_AREA_DEFAULT     70  // percent - upper reject (lighting change)
#define MOTION_EMA_ALPHA_DAY        0.92f
#define MOTION_EMA_ALPHA_NIGHT      0.97f
#define MOTION_TRAINING_FRAMES      15
#define MOTION_AGC_GAIN_FACTOR      0.5f
#define MOTION_BRIGHTNESS_MIN       10

// Face detection defaults
#define FACE_SCORE_THRESHOLD_DEFAULT  0.5f
#define FACE_NMS_THRESHOLD_DEFAULT    0.3f
#define FACE_COOLDOWN_DEFAULT         5  // seconds

// Person detection (FOMO) defaults
#define PD_CONFIDENCE_THRESHOLD_DEFAULT  0.6f
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
    bool vflip        = false;
    bool hmirror      = false;
    int brightness    = 0;     // -2 to 2
    int contrast      = 0;     // -2 to 2
    int saturation    = 0;     // -2 to 2
    int sharpness     = 0;     // -3 to 3
    int denoise       = 0;     // 0 to 8
    int ae_level      = 0;     // -2 to 2
    int aec_value     = 300;   // 0 to 1200
    int agc_gain      = 0;     // 0 to 30
    int gainceiling   = 0;     // 0 to 6
    int wb_mode       = 0;     // 0 to 4
    bool aec          = true;
    bool agc          = true;
    bool awb          = true;
    bool bpc          = true;
    bool wpc          = true;
    bool raw_gma      = true;
    bool lenc         = true;
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
};

// Telegram defaults
#define TELEGRAM_COOLDOWN_DEFAULT   30
#define TELEGRAM_ACTIVE_START_DEFAULT 0
#define TELEGRAM_ACTIVE_END_DEFAULT   23
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
    AuthSettings auth;
    bool led_enabled = true;
    int idle_fps     = 1;     // FPS when no client streaming
    int active_fps   = 15;    // FPS when client streaming
};

// Global config instance
extern AppConfig appConfig;

// Config persistence functions
bool loadConfig();
bool saveConfig();
void resetConfig();
void applyConfigToCamera();

#endif // CONFIG_H
