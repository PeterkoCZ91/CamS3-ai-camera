#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include <Arduino.h>
#include "esp_camera.h"

// Camera lifecycle. cameraDeinit()/cameraReinit() return false when the capture
// task could not be stopped — the caller must NOT assume the camera was torn down.
bool cameraInit();
bool cameraDeinit();
bool cameraReinit();

// Frame capture (thread-safe via ring buffer)
camera_fb_t* captureFrame();
void releaseFrame(camera_fb_t* fb);

// Capture task management. stopCaptureTask() blocks until the task has exited
// (up to 3s) and returns false if it never did.
void startCaptureTask();
bool stopCaptureTask();

// Ring buffer access for streaming
// Returns slot handle (>= 0) on success, -1 on failure.
// Caller MUST pass the same handle back to ringBufferRelease() when done.
// Optional out params carry the sensor dimensions of THAT frame — decoders must
// use them instead of deriving sizes from appConfig.camera.frame_size, which can
// change under them (and used to overflow their decode buffers when it did).
int  ringBufferGetLatest(const uint8_t** buf, size_t* len,
                         uint16_t* width = nullptr, uint16_t* height = nullptr);
uint32_t ringBufferGetTimestamp(int handle);
void ringBufferRelease(int handle);

// Serialize JPEG decode calls (jpg2rgb565 / fmt2rgb888). The underlying
// tjpgd decoder is NOT reentrant — concurrent decodes from motion, face
// and person tasks produce "JPG Decompression Failed! Data format error".
// Acquire before calling any jpg2rgb565/fmt2rgb888 path; release after.
bool cameraDecodeLock(TickType_t timeout = pdMS_TO_TICKS(2000));
void cameraDecodeUnlock();

// Reinit request (set by capture task on repeated failures or by a frame-size
// change, executed from the main loop where stopping the capture task is safe).
void cameraRequestReinit();
bool cameraReinitRequested();
void cameraClearReinitRequest();

// Ask the capture task to push appConfig.camera to the sensor at the next safe
// point (right after esp_camera_fb_return, when the SCCB bus is idle). HTTP
// handlers must use this rather than calling applyConfigToCamera() themselves:
// writing sensor registers from the async server task races the capture DMA.
void cameraRequestSettingsApply();

// Stats
uint32_t getCaptureCount();
float getCaptureFps();
uint32_t getLastCaptureMs();
uint32_t getCaptureErrors();
// Frames the capture task had to throw away because every ring slot was held by a
// reader — a sign that some consumer (stream client, detector) is falling behind.
uint32_t getRingDroppedFrames();

// Stream client tracking
void streamClientConnected();
void streamClientDisconnected();
void detectionStreamClientConnected();
void detectionStreamClientDisconnected();
int getStreamClientCount();
int getDetectionStreamClientCount();
int getTotalStreamClientCount();

#endif // CAMERA_MANAGER_H
