#include "mqtt_handler.h"

#ifdef INCLUDE_MQTT

#include "config.h"
#include "camera_manager.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

static WiFiClient mqttNet;
static MQTTClient mqttClient(512);
static bool haDiscoverySent = false;

// Previous states for change detection
static bool prevMotion = false;
static bool prevPerson = false;
static bool prevFace = false;

static String topicBase() {
    return appConfig.mqtt.topic_prefix;
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
        doc["device_class"] = "motion";
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = deviceId;
        dev["name"] = deviceName;
        dev["manufacturer"] = "M5Stack";
        dev["model"] = "Unit CamS3 5MP";
        dev["sw_version"] = FIRMWARE_VERSION;

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

    // Status sensor
    {
        String topic = "homeassistant/sensor/" + deviceId + "/status/config";
        JsonDocument doc;
        doc["name"] = deviceName + " Status";
        doc["unique_id"] = deviceId + "_status";
        doc["state_topic"] = topicBase() + "/status";
        doc["value_template"] = "{{ value_json.uptime }}";
        doc["json_attributes_topic"] = topicBase() + "/status";
        doc["icon"] = "mdi:camera";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = deviceId;
        dev["name"] = deviceName;

        char payload[512];
        serializeJson(doc, payload, sizeof(payload));
        mqttClient.publish(topic.c_str(), payload, true, 1);
    }

    Serial.printf("[%s] HA discovery payloads published\n", TAG);
    haDiscoverySent = true;
}

static bool mqttConnect() {
    if (appConfig.mqtt.server.length() == 0) return false;

    mqttClient.begin(appConfig.mqtt.server.c_str(), appConfig.mqtt.port, mqttNet);

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
        Serial.printf("[%s] Connected to %s:%d\n", TAG,
                      appConfig.mqtt.server.c_str(), appConfig.mqtt.port);

        // Publish HA discovery on first connect
        if (!haDiscoverySent) {
            publishDiscovery();
        }
    } else {
        Serial.printf("[%s] Connection failed\n", TAG);
    }

    return ok;
}

void mqttInit() {
    Serial.printf("[%s] MQTT handler initialized\n", TAG);
}

void mqttPublishMotion(bool detected) {
    if (!mqttClient.connected()) return;
    String topic = topicBase() + "/motion/state";
    mqttClient.publish(topic.c_str(), detected ? "ON" : "OFF", true, 0);
}

void mqttPublishPerson(bool detected, int count) {
    if (!mqttClient.connected()) return;
    String stateTopic = topicBase() + "/person/state";
    mqttClient.publish(stateTopic.c_str(), detected ? "ON" : "OFF", true, 0);

    // Attributes with count
    String attrTopic = topicBase() + "/person/attributes";
    char attrBuf[64];
    snprintf(attrBuf, sizeof(attrBuf), "{\"count\":%d}", count);
    mqttClient.publish(attrTopic.c_str(), attrBuf, true, 0);
}

void mqttPublishFace(bool detected, int count) {
    if (!mqttClient.connected()) return;
    String topic = topicBase() + "/face/state";
    mqttClient.publish(topic.c_str(), detected ? "ON" : "OFF", true, 0);
}

void mqttPublishStatus() {
    if (!mqttClient.connected()) return;
    String topic = topicBase() + "/status";
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"uptime\":%lu,\"heap\":%d,\"psram\":%d,\"fps\":%.1f,\"rssi\":%d,\"clients\":%d}",
        millis() / 1000,
        ESP.getFreeHeap() / 1024,
        ESP.getFreePsram() / (1024 * 1024),
        getCaptureFps(),
        getRSSI(),
        getStreamClientCount()
    );
    mqttClient.publish(topic.c_str(), buf, false, 0);
}

bool isMqttConnected() {
    return mqttClient.connected();
}

void mqttTask(void* param) {
    Serial.printf("[%s] MQTT task started\n", TAG);

    // Wait for WiFi
    while (!isWiFiConnected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    unsigned long lastStatusPublish = 0;
    unsigned long lastReconnectAttempt = 0;

    while (true) {
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

        // Periodic status publish (every 30s)
        if (millis() - lastStatusPublish > 30000) {
            mqttPublishStatus();
            lastStatusPublish = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

#endif // INCLUDE_MQTT
