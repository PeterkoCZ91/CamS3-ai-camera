#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>

// Initialize MQTT client and connect
void mqttInit();

// FreeRTOS task entry point
void mqttTask(void* param);

// Publish state updates
void mqttPublishMotion(bool detected);
void mqttPublishPerson(bool detected, int count);
void mqttPublishPersonUncertain(float confidence, int tracks);
void mqttPublishFace(bool detected, int count);
void mqttPublishStatus();

// Check connection
bool isMqttConnected();

// Link health, same meaning as the DFR1154 firmware's /status fields.
// state follows PubSubClient: 0 = connected, -1 = disconnected.
struct MqttLinkStats {
    int      state;
    uint32_t connects;       // successful connects since boot
    uint32_t failures;       // failed connect attempts since boot
    uint32_t link_uptime_s;  // seconds the current connection has been up (0 when down)
};
MqttLinkStats mqttGetLinkStats();

#endif // MQTT_HANDLER_H
