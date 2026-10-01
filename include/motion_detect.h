#ifndef MOTION_DETECT_H
#define MOTION_DETECT_H

#include <Arduino.h>
#include <freertos/semphr.h>

// Motion analysis size (grid): 80x60 grayscale -> 20x15 block grid (4x4 averaging).
// The JPEG decoder writes at (sensor_W/8 x sensor_H/8); at UXGA that's 200x150
// RGB565 = 60 KB, so the decode buffer is sized for the largest frame the sensor
// can produce (see IMG_DECODE_MAX_* in image_utils.h), not for the analysis size.
// We subsample from the frame's actual decoded dimensions to MOTION_DECODE_W x H.
#define MOTION_DECODE_W     80
#define MOTION_DECODE_H     60
#define MOTION_GRID_W       20
#define MOTION_GRID_H       15
#define MOTION_GRID_SIZE    (MOTION_GRID_W * MOTION_GRID_H)
#define MOTION_REL_THRESHOLD        0.12f
#define MOTION_EMA_ALPHA_MOTION     0.998f
#define MOTION_NIGHT_THRESHOLD_MULT 2.5f
// At night the per-block threshold alone is not enough: sensor noise still trips
// individual blocks, so require a proportionally larger changed area as well.
#define MOTION_NIGHT_AREA_MULT      2.5f

// How long motion waits for the person detector to reach a verdict before sending
// its own notification. With person detection enabled, an immediate motion photo is
// mostly noise (a car, a branch) and a duplicate whenever a person IS found. But if
// the detector returns UNCERTAIN — which is the common case in poor light — nothing
// was sent at all and the event was lost. So: defer, then send only if person
// detection stayed quiet.
#define MOTION_AI_FALLBACK_MS       5000
#define MOTION_BRIGHTNESS_JUMP      80

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

bool motionDetectInit();  // false = buffers not allocated, do NOT start the task
void motionDetectSetSemaphore(SemaphoreHandle_t sem);

// Region of interest: one character per grid block, row-major, MOTION_GRID_SIZE
// characters long. '0' masks the block out, anything else keeps it. Blocks outside
// the ROI never contribute to motion, and the trigger percentage is computed over
// the ROI area rather than the whole grid.
// Passing NULL (or a wrong-length string) clears the mask = whole frame active.
// Until this existed the mask saved through /api/roi was stored and then ignored.
bool motionDetectSetRoiMask(const char* mask, int len);
void motionDetectClearRoiMask();
int  motionDetectRoiActiveBlocks();
void motionDetectTask(void* param);
bool isMotionDetected();
unsigned long getLastMotionTime();
uint32_t getMotionEventCount();
MotionDebugInfo getMotionDebugInfo();
void motionDetectResetBackground();

#endif // MOTION_DETECT_H
