#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include <Arduino.h>
#include "esp_camera.h"

// Camera lifecycle
bool cameraInit();
void cameraDeinit();
bool cameraReinit();

// Frame capture (thread-safe via ring buffer)
camera_fb_t* captureFrame();
void releaseFrame(camera_fb_t* fb);

// Capture task management
void startCaptureTask();
void stopCaptureTask();

// Ring buffer access for streaming
// Returns pointer to latest JPEG and its length. Caller must call ringBufferRelease() when done.
bool ringBufferGetLatest(const uint8_t** buf, size_t* len);
void ringBufferRelease();

// Stats
uint32_t getCaptureCount();
float getCaptureFps();
uint32_t getLastCaptureMs();
uint32_t getCaptureErrors();

// Stream client tracking
void streamClientConnected();
void streamClientDisconnected();
int getStreamClientCount();

#endif // CAMERA_MANAGER_H
