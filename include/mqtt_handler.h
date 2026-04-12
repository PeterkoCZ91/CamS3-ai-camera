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
void mqttPublishFace(bool detected, int count);
void mqttPublishStatus();

// Check connection
bool isMqttConnected();

#endif // MQTT_HANDLER_H
