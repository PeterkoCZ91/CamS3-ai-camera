#ifndef PERSON_DETECTION_H
#define PERSON_DETECTION_H

#include <Arduino.h>
#include <freertos/semphr.h>

struct PersonDetectResult {
    bool detected;
    int person_count;         // Number of confirmed tracked persons
    int raw_detections;       // Raw FOMO detections before tracking
    float top_score;          // Highest confidence
    uint32_t inference_ms;    // Last FOMO inference time
    int track_count;          // Active tracks (all states)
};

// Initialize person detection (allocate buffers, load model)
void personDetectInit();

// FreeRTOS task entry point — waits on motion semaphore
void personDetectTask(void* param);

// Set the motion semaphore (called from main.cpp after creating it)
void personDetectSetSemaphore(SemaphoreHandle_t sem);

// Query state
bool isPersonDetected();
unsigned long getLastPersonTime();
uint32_t getPersonEventCount();
PersonDetectResult getPersonDetectResult();

#endif // PERSON_DETECTION_H
