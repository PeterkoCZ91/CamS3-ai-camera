#ifndef MOTION_DETECT_H
#define MOTION_DETECT_H

#include <Arduino.h>
#include <freertos/semphr.h>

// Grid dimensions: 80x60 decoded -> 20x15 block grid (4x4 averaging)
#define MOTION_DECODE_W   80
#define MOTION_DECODE_H   60
#define MOTION_GRID_W     20
#define MOTION_GRID_H     15
#define MOTION_GRID_SIZE  (MOTION_GRID_W * MOTION_GRID_H)

struct MotionDebugInfo {
    float changed_pct;           // Percentage of changed blocks
    float avg_brightness;        // Average frame brightness (0-255)
    int sensor_gain;             // Current sensor AGC gain
    float adaptive_threshold;    // Current adaptive threshold
    int active_blocks;           // Blocks exceeding threshold
    int clustered_blocks;        // Blocks after spatial filtering
    int consecutive_frames;      // Consecutive motion frames
    bool night_mode;             // Night suppression active
    bool training;               // Background model still training
    uint32_t decode_ms;          // JPEG decode time
    uint32_t analysis_ms;        // Motion analysis time
};

void motionDetectInit();
void motionDetectSetSemaphore(SemaphoreHandle_t sem);
void motionDetectTask(void* param);
bool isMotionDetected();
unsigned long getLastMotionTime();
uint32_t getMotionEventCount();
MotionDebugInfo getMotionDebugInfo();
void motionDetectResetBackground();

#endif // MOTION_DETECT_H
