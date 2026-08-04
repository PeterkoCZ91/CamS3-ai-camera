#ifndef PERSON_DETECTION_H
#define PERSON_DETECTION_H

#include <Arduino.h>
#include <freertos/semphr.h>

enum class PersonDecision : uint8_t { NONE = 0, UNCERTAIN = 1, CONFIDENT = 2 };

struct PersonDetectResult {
    bool detected;
    int person_count;         // Number of confirmed tracked persons
    int raw_detections;       // Raw FOMO detections before tracking
    float top_score;          // Highest confidence
    uint32_t inference_ms;    // Last FOMO inference time
    int track_count;          // Active tracks (all states)
    PersonDecision decision;  // Last three-state classification
};

// Initialize person detection (allocate buffers, load model). Returns false on allocation failure.
bool personDetectInit();

// FreeRTOS task entry point — waits on motion semaphore
void personDetectTask(void* param);

// Set the motion semaphore (called from main.cpp after creating it)
void personDetectSetSemaphore(SemaphoreHandle_t sem);

PersonDecision classifyDecision(float confidence);
const char* personDecisionToString(PersonDecision decision);

// millis() of the last notification person detection actually sent out (0 = never).
// Motion detection uses this to decide whether its own fallback notification is
// still needed — see MOTION_AI_FALLBACK_MS in motion_detect.h.
unsigned long getLastPersonNotifyTime();

// Query state
bool isPersonDetected();
unsigned long getLastPersonTime();
uint32_t getPersonEventCount();
PersonDetectResult getPersonDetectResult();

#endif // PERSON_DETECTION_H
