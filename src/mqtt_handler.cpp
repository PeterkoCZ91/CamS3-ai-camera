#include "mqtt_handler.h"
#include "ws_log.h"

#ifdef INCLUDE_MQTT

#include "config.h"
#include "camera_manager.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <stdlib.h>

#ifdef INCLUDE_MOTION_DETECT
#include "motion_detect.h"
#endif

#ifdef INCLUDE_PERSON_DETECT
#include "person_detection.h"
#endif

#if defined(INCLUDE_FACE_DETECT) && defined(INCLUDE_MOTION_DETECT)
#include "face_detect.h"
#endif

static const char* TAG = "MQTT";

static WiFiClient        mqttNet;
static WiFiClientSecure  mqttNetSecure;
static bool              mqttSecureInUse = false;
// Buffer must hold topic + payload + fixed header for the largest publish we do.
// HA discovery payloads are serialized into a 512-byte scratch buffer and their
// topics run ~60 chars, so a 512-byte client buffer silently dropped them.
static MQTTClient        mqttClient(1024);
static bool              haDiscoverySent = false;
static char              pendingConfigKey[48] = {};
static char              pendingConfigPayload[64] = {};
static volatile bool     pendingConfigMessage = false;
static char*             mqttCaCert = NULL;  // lazily loaded from /ca.pem

// Offline publish queue — holds last N publications while disconnected,
// replayed in order on reconnect. Payload/topic kept in PSRAM.
#define MQTT_QUEUE_CAP 32
#define MQTT_QUEUE_TOPIC_MAX 96
#define MQTT_QUEUE_PAYLOAD_MAX 256
struct MqttQueuedMsg {
    char topic[MQTT_QUEUE_TOPIC_MAX];
    char payload[MQTT_QUEUE_PAYLOAD_MAX];
    size_t payload_len;
    bool retained;
    int qos;
};
static MqttQueuedMsg* mqttQueue = NULL;
static int mqttQueueHead = 0;  // next write slot
static int mqttQueueCount = 0;

static void mqttQueuePush(const char* topic, const char* payload, size_t len, bool retained, int qos) {
    if (!mqttQueue) return;
    MqttQueuedMsg& slot = mqttQueue[mqttQueueHead];
    strlcpy(slot.topic, topic, sizeof(slot.topic));
    size_t cp = len < sizeof(slot.payload) - 1 ? len : sizeof(slot.payload) - 1;
    memcpy(slot.payload, payload, cp);
    slot.payload[cp] = 0;
    slot.payload_len = cp;
    slot.retained = retained;
    slot.qos = qos;
    mqttQueueHead = (mqttQueueHead + 1) % MQTT_QUEUE_CAP;
    if (mqttQueueCount < MQTT_QUEUE_CAP) mqttQueueCount++;
}

static void mqttQueueFlush() {
    if (!mqttQueue || mqttQueueCount == 0 || !mqttClient.connected()) return;
    int idx = (mqttQueueHead - mqttQueueCount + MQTT_QUEUE_CAP) % MQTT_QUEUE_CAP;
    int replayed = 0;
    while (mqttQueueCount > 0 && mqttClient.connected()) {
        MqttQueuedMsg& slot = mqttQueue[idx];
        mqttClient.publish(slot.topic, slot.payload, slot.retained, slot.qos);
        idx = (idx + 1) % MQTT_QUEUE_CAP;
        mqttQueueCount--;
        replayed++;
    }
    if (replayed) logCapture("[%s] Replayed %d queued messages\n", TAG, replayed);
}

// Publish wrapper: send if connected, otherwise enqueue.
static void mqttPublishOrQueue(const char* topic, const char* payload, bool retained, int qos) {
    size_t len = strlen(payload);
    if (mqttClient.connected()) {
        mqttClient.publish(topic, payload, retained, qos);
    } else if (retained) {
        // Only retained messages are worth queueing — ephemeral state becomes
        // stale fast. Keeps the ring useful after long outages.
        mqttQueuePush(topic, payload, len, retained, qos);
    }
}

static void loadCaIfNeeded() {
    if (mqttCaCert || !appConfig.mqtt.tls_enabled) return;
    if (!LittleFS.exists("/ca.pem")) return;
    File f = LittleFS.open("/ca.pem", "r");
    if (!f) return;
    size_t sz = f.size();
    if (sz == 0 || sz > 8192) { f.close(); return; }
    mqttCaCert = (char*)ps_malloc(sz + 1);
    if (!mqttCaCert) { f.close(); return; }
    f.readBytes(mqttCaCert, sz);
    mqttCaCert[sz] = 0;
    f.close();
    logCapture("[%s] Loaded CA certificate (%d bytes) from /ca.pem\n", TAG, (int)sz);
}

// Previous states for change detection
static bool prevMotion = false;
static bool prevPerson = false;
static bool prevFace = false;

static String topicBase() {
    return appConfig.mqtt.topic_prefix;
}

static String availabilityTopic() {
    return topicBase() + "/availability";
}

static void mqttMessageReceived(String& topic, String& payload) {
    String prefixConfig = topicBase() + "/config/set/";
    String legacyConfig = "camera/config/set/";
    String key;
    if (topic.startsWith(prefixConfig)) {
        key = topic.substring(prefixConfig.length());
    } else if (topic.startsWith(legacyConfig)) {
        key = topic.substring(legacyConfig.length());
    } else {
        return;
    }

    strlcpy(pendingConfigKey, key.c_str(), sizeof(pendingConfigKey));
    strlcpy(pendingConfigPayload, payload.c_str(), sizeof(pendingConfigPayload));
    pendingConfigMessage = true;
}

static void mqttHandlePendingConfig() {
    if (!pendingConfigMessage) return;

    char key[sizeof(pendingConfigKey)];
    char payload[sizeof(pendingConfigPayload)];
    strlcpy(key, pendingConfigKey, sizeof(key));
    strlcpy(payload, pendingConfigPayload, sizeof(payload));
    pendingConfigMessage = false;

    bool needSave = false;
    if (strcmp(key, "motion/enabled") == 0 || strcmp(key, "motion_enabled") == 0) {
        appConfig.motion.enabled = (strcasecmp(payload, "ON") == 0 ||
                                    strcasecmp(payload, "true") == 0 ||
                                    strcmp(payload, "1") == 0);
        needSave = true;
    } else if (strcmp(key, "motion/threshold") == 0 || strcmp(key, "motion_threshold") == 0) {
        int value = atoi(payload);
        if (value >= 5 && value <= 80) {
            appConfig.motion.threshold = value;
            needSave = true;
        }
    } else if (strcmp(key, "person/enabled") == 0 || strcmp(key, "person_detect_enabled") == 0) {
        appConfig.person_detect.enabled = (strcasecmp(payload, "ON") == 0 ||
                                           strcasecmp(payload, "true") == 0 ||
                                           strcmp(payload, "1") == 0);
        needSave = true;
    }

    if (needSave) {
        saveConfig();
        logCapture("[%s] Applied MQTT config %s=%s\n", TAG, key, payload);
    } else {
        logCapture("[%s] Ignored MQTT config %s=%s\n", TAG, key, payload);
    }
}

// --- Home Assistant MQTT Discovery ---

static void publishDiscovery() {
    String deviceId = appConfig.wifi.hostname;
    String deviceName = DEVICE_NAME;

    // Device block (shared across all entities)
    // We'll build it inline for each discovery payload

    // Motion binary sensor
    {
        String topic = "homeassistant/binary_sensor/" + deviceId + "/motion/config";
        JsonDocument doc;
        doc["name"] = deviceName + " Motion";
        doc["unique_id"] = deviceId + "_motion";
        doc["state_topic"] = topicBase() + "/motion/state";
        doc["availability_topic"] = availabilityTopic();
        doc["payload_available"]     = "online";
        doc["payload_not_available"] = "offline";
        doc["device_class"] = "motion";
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = deviceId;
        dev["name"] = deviceName;
        dev["manufacturer"] = "M5Stack";
        dev["model"] = "Unit CamS3 5MP";
        dev["sw_version"] = FIRMWARE_VERSION;
        dev["connections"][0][0] = "mac";
        dev["connections"][0][1] = WiFi.macAddress();

        char payload[512];
        serializeJson(doc, payload, sizeof(payload));
        mqttClient.publish(topic.c_str(), payload, true, 1);
    }

    // Person binary sensor
    {
        String topic = "homeassistant/binary_sensor/" + deviceId + "/person/config";
        JsonDocument doc;
        doc["name"] = deviceName + " Person";
        doc["unique_id"] = deviceId + "_person";
        doc["state_topic"] = topicBase() + "/person/state";
        doc["availability_topic"] = availabilityTopic();
        doc["payload_available"]     = "online";
        doc["payload_not_available"] = "offline";
        doc["device_class"] = "occupancy";
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        doc["json_attributes_topic"] = topicBase() + "/person/attributes";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = deviceId;
        dev["name"] = deviceName;

        char payload[512];
        serializeJson(doc, payload, sizeof(payload));
        mqttClient.publish(topic.c_str(), payload, true, 1);
    }

    // Face binary sensor
    {
        String topic = "homeassistant/binary_sensor/" + deviceId + "/face/config";
        JsonDocument doc;
        doc["name"] = deviceName + " Face";
        doc["unique_id"] = deviceId + "_face";
        doc["state_topic"] = topicBase() + "/face/state";
        doc["availability_topic"] = availabilityTopic();
        doc["payload_available"]     = "online";
        doc["payload_not_available"] = "offline";
        doc["device_class"] = "occupancy";
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = deviceId;
        dev["name"] = deviceName;

        char payload[512];
        serializeJson(doc, payload, sizeof(payload));
        mqttClient.publish(topic.c_str(), payload, true, 1);
    }

    // Status sensor (uptime as primary value, full JSON available as attributes)
    {
        String topic = "homeassistant/sensor/" + deviceId + "/status/config";
        JsonDocument doc;
        doc["name"] = deviceName + " Uptime";
        doc["unique_id"] = deviceId + "_status";
        doc["state_topic"] = topicBase() + "/status";
        doc["availability_topic"] = availabilityTopic();
        doc["payload_available"]     = "online";
        doc["payload_not_available"] = "offline";
        doc["value_template"] = "{{ value_json.uptime }}";
        doc["json_attributes_topic"] = topicBase() + "/status";
        doc["unit_of_measurement"] = "s";
        doc["state_class"] = "measurement";
        doc["icon"] = "mdi:timer-outline";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = deviceId;
        dev["name"] = deviceName;

        char payload[512];
        serializeJson(doc, payload, sizeof(payload));
        mqttClient.publish(topic.c_str(), payload, true, 1);
    }

    // Diagnostic sensors extracted from the /status JSON via value_template.
    // entity_category=diagnostic tucks them under the device's Diagnostic section.
    struct DiagEntry {
        const char* suffix;
        const char* name;
        const char* icon;
        const char* unit;
        const char* value_tpl;
        const char* device_class;
    };
    const DiagEntry diag[] = {
        {"fps",         " FPS",        "mdi:camera-timer", "fps", "{{ value_json.fps }}",      NULL},
        {"heap",        " Free Heap",  "mdi:memory",       "kB",  "{{ value_json.heap }}",     NULL},
        {"heap_min",    " Min Heap",   "mdi:memory",       "kB",  "{{ value_json.heap_min }}", NULL},
        {"psram",       " Free PSRAM", "mdi:memory",       "MB",  "{{ value_json.psram }}",    NULL},
        {"rssi",        " RSSI",       "mdi:wifi",         "dBm", "{{ value_json.rssi }}",     "signal_strength"},
        {"clients",     " Clients",    "mdi:account-group",NULL,  "{{ value_json.clients }}",  NULL},
    };
    for (const auto& d : diag) {
        String topic = "homeassistant/sensor/" + deviceId + "/" + d.suffix + "/config";
        JsonDocument doc;
        doc["name"] = deviceName + d.name;
        doc["unique_id"] = deviceId + "_" + d.suffix;
        doc["state_topic"] = topicBase() + "/status";
        doc["availability_topic"] = availabilityTopic();
        doc["payload_available"]     = "online";
        doc["payload_not_available"] = "offline";
        doc["value_template"] = d.value_tpl;
        if (d.unit)         doc["unit_of_measurement"] = d.unit;
        if (d.icon)         doc["icon"] = d.icon;
        if (d.device_class) doc["device_class"] = d.device_class;
        doc["state_class"] = "measurement";
        doc["entity_category"] = "diagnostic";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = deviceId;
        dev["name"] = deviceName;

        char payload[512];
        serializeJson(doc, payload, sizeof(payload));
        mqttClient.publish(topic.c_str(), payload, true, 1);
    }

    logCapture("[%s] HA discovery payloads published\n", TAG);
    haDiscoverySent = true;
}

static bool mqttConnect() {
    if (appConfig.mqtt.server.length() == 0) return false;

    // Choose plaintext or TLS transport; switching requires a fresh begin().
    if (appConfig.mqtt.tls_enabled) {
        loadCaIfNeeded();
        if (!mqttSecureInUse) {
            if (mqttCaCert) {
                mqttNetSecure.setCACert(mqttCaCert);
            } else {
                mqttNetSecure.setInsecure();
            }
            mqttSecureInUse = true;
        }
        mqttClient.begin(appConfig.mqtt.server.c_str(), appConfig.mqtt.port, mqttNetSecure);
    } else {
        mqttSecureInUse = false;
        mqttClient.begin(appConfig.mqtt.server.c_str(), appConfig.mqtt.port, mqttNet);
    }
    // LWT: broker marks us offline if the connection drops unexpectedly.
    mqttClient.setWill(availabilityTopic().c_str(), "offline", true, 1);

    String clientId = appConfig.wifi.hostname;
    bool ok;
    if (appConfig.mqtt.user.length() > 0) {
        ok = mqttClient.connect(clientId.c_str(),
                                appConfig.mqtt.user.c_str(),
                                appConfig.mqtt.password.c_str());
    } else {
        ok = mqttClient.connect(clientId.c_str());
    }

    if (ok) {
        logCapture("[%s] Connected to %s:%d\n", TAG,
                      appConfig.mqtt.server.c_str(), appConfig.mqtt.port);

        // Announce online (retained, overrides LWT while we're alive)
        mqttClient.publish(availabilityTopic().c_str(), "online", true, 1);

        // Publish HA discovery on first connect
        if (!haDiscoverySent) {
            publishDiscovery();
        }

        String configSetTopic = topicBase() + "/config/set/#";
        mqttClient.subscribe(configSetTopic.c_str(), 1);
        mqttClient.subscribe("camera/config/set/#", 1);

        // Drain any publications that were queued while we were offline.
        mqttQueueFlush();
    } else {
        logCapture("[%s] Connection failed\n", TAG);
    }

    return ok;
}

void mqttInit() {
    // Offline queue lives in PSRAM so it can be sized generously without
    // eating into the internal heap that the async web server needs.
    if (!mqttQueue && psramFound()) {
        mqttQueue = (MqttQueuedMsg*)ps_calloc(MQTT_QUEUE_CAP, sizeof(MqttQueuedMsg));
        if (mqttQueue) {
            logCapture("[%s] Offline queue allocated (%d slots, %d bytes PSRAM)\n",
                          TAG, MQTT_QUEUE_CAP, (int)(MQTT_QUEUE_CAP * sizeof(MqttQueuedMsg)));
        }
    }
    mqttClient.onMessage(mqttMessageReceived);
    logCapture("[%s] MQTT handler initialized\n", TAG);
}

void mqttPublishMotion(bool detected) {
    const char* payload = detected ? "ON" : "OFF";

    // Home Assistant's discovery config points at <prefix>/motion/state.
    String topic = topicBase() + "/motion/state";
    mqttPublishOrQueue(topic.c_str(), payload, true, 0);

    // The A12 companion subscribes to <prefix>/motion — without the /state suffix —
    // and uses it to open a YOLO window and force a detection pass. Publishing only
    // the HA spelling meant that gate never fired: A12 fell back to its own frame
    // differencing and the camera's motion detector contributed nothing to it.
    // Two publishes of a 2-3 byte payload on a state change is not worth optimizing.
    String flat = topicBase() + "/motion";
    mqttPublishOrQueue(flat.c_str(), payload, true, 0);
}

void mqttPublishPerson(bool detected, int count) {
    String stateTopic = topicBase() + "/person/state";
    mqttPublishOrQueue(stateTopic.c_str(), detected ? "ON" : "OFF", true, 0);

    String attrTopic = topicBase() + "/person/attributes";
    char attrBuf[64];
    snprintf(attrBuf, sizeof(attrBuf), "{\"count\":%d}", count);
    mqttPublishOrQueue(attrTopic.c_str(), attrBuf, true, 0);
}

void mqttPublishPersonUncertain(float confidence, int tracks) {
    String topic = topicBase() + "/person_uncertain";
    char payload[80];
    snprintf(payload, sizeof(payload), "{\"confidence\":%.2f,\"tracks\":%d}", confidence, tracks);
    mqttPublishOrQueue(topic.c_str(), payload, false, 0);
}

void mqttPublishFace(bool detected, int count) {
    String topic = topicBase() + "/face/state";
    mqttPublishOrQueue(topic.c_str(), detected ? "ON" : "OFF", true, 0);
}

void mqttPublishStatus() {
    if (!mqttClient.connected()) return;  // ephemeral telemetry, don't queue
    String topic = topicBase() + "/status";
    size_t minHeapKb = ESP.getMinFreeHeap() / 1024;
    size_t heapKb = ESP.getFreeHeap() / 1024;
    size_t heapTotalKb = ESP.getHeapSize() / 1024;
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"uptime\":%lu,\"heap\":%u,\"heap_min\":%u,\"heap_total\":%u,"
        "\"psram\":%u,\"fps\":%.1f,\"rssi\":%d,\"clients\":%d}",
        millis() / 1000,
        (unsigned)heapKb,
        (unsigned)minHeapKb,
        (unsigned)heapTotalKb,
        (unsigned)(ESP.getFreePsram() / (1024 * 1024)),
        getCaptureFps(),
        getRSSI(),
        getStreamClientCount()
    );
    mqttClient.publish(topic.c_str(), buf, false, 0);
}

static void mqttPublishHeartbeat() {
    if (!mqttClient.connected()) return;
    String topic = topicBase() + "/camera/status/heartbeat";
    char payload[96];
    snprintf(payload, sizeof(payload), "{\"uptime\":%lu,\"free_heap\":%u}",
             millis() / 1000, (unsigned)ESP.getFreeHeap());
    mqttClient.publish(topic.c_str(), payload, false, 0);
}

static void mqttPublishProfile() {
    if (!mqttClient.connected()) return;
    const char* profile = "DAY";
#ifdef INCLUDE_MOTION_DETECT
    MotionDebugInfo mdi = getMotionDebugInfo();
    if (mdi.avg_brightness < 15.0f) profile = "NIGHT";
    else if (mdi.avg_brightness <= 40.0f) profile = "DUSK";
#endif
    String topic = topicBase() + "/camera/status/profile";
    mqttClient.publish(topic.c_str(), profile, true, 0);
}

bool isMqttConnected() {
    return mqttClient.connected();
}

void mqttTask(void* param) {
    logCapture("[%s] MQTT task started\n", TAG);
    esp_task_wdt_add(NULL);

    // Wait for WiFi
    while (!isWiFiConnected()) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    unsigned long lastStatusPublish = 0;
    unsigned long lastHeartbeatPublish = 0;
    unsigned long lastReconnectAttempt = 0;

    while (true) {
        esp_task_wdt_reset();
        if (!appConfig.mqtt.enabled) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            haDiscoverySent = false;
            continue;
        }

        // Reconnect if needed
        if (!mqttClient.connected()) {
            if (millis() - lastReconnectAttempt > 10000) {
                lastReconnectAttempt = millis();
                mqttConnect();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        mqttClient.loop();
        mqttHandlePendingConfig();

        // Publish state changes
        #ifdef INCLUDE_MOTION_DETECT
        bool curMotion = isMotionDetected();
        if (curMotion != prevMotion) {
            mqttPublishMotion(curMotion);
            prevMotion = curMotion;
        }
        #endif

        #ifdef INCLUDE_PERSON_DETECT
        bool curPerson = isPersonDetected();
        if (curPerson != prevPerson) {
            PersonDetectResult pdr = getPersonDetectResult();
            mqttPublishPerson(curPerson, pdr.person_count);
            prevPerson = curPerson;
        }
        #endif

        #if defined(INCLUDE_FACE_DETECT) && defined(INCLUDE_MOTION_DETECT)
        bool curFace = isFaceDetected();
        if (curFace != prevFace) {
            FaceDetectResult fdr = getFaceDetectResult();
            mqttPublishFace(curFace, fdr.face_count);
            prevFace = curFace;
        }
        #endif

        // Periodic heartbeat/profile publish (every 5s)
        if (millis() - lastHeartbeatPublish > 5000) {
            mqttPublishHeartbeat();
            mqttPublishProfile();
            lastHeartbeatPublish = millis();
        }

        // Periodic status publish (every 30s)
        if (millis() - lastStatusPublish > 30000) {
            mqttPublishStatus();
            lastStatusPublish = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

#endif // INCLUDE_MQTT
