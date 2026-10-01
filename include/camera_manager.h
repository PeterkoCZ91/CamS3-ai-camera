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

// Read-only live sensor register view (GET /api/sensor). The capture task samples a
// fixed register list every ~10 frames (and on a one-shot request) into a seq-lock
// protected cache; callers only copy it and never touch SCCB. Register semantics
// are UNVERIFIED for the PY260 (list is the OV5640 map). 0xFFFF = read failed.
#define SENSOR_VIEW_REGS 20
struct SensorView {
    bool     valid;
    uint16_t pid, ver, midh, midl;
    uint32_t frame;                   // capture counter when sampled
    uint16_t regs[SENSOR_VIEW_REGS];
    bool     custom_set;              // a one-shot result exists
    uint16_t custom_reg, custom_val;
    uint32_t custom_frame;
};
bool cameraSensorViewGet(SensorView* out);          // false until first sample
uint16_t cameraSensorViewRegAddr(int i);
void cameraSensorViewRequestReg(uint16_t reg);      // one-shot, serviced by capture task
bool cameraSensorViewOneShotPending();

// Stats
uint32_t getCaptureCount();
float getCaptureFps();
uint32_t getLastCaptureMs();
uint32_t getCaptureErrors();
// millis() of the last frame actually copied into the ring (0 = none yet). Unlike
// getLastCaptureMs() it does not advance for dropped/oversize frames — use it to
// judge whether consumers are being fed.
uint32_t getLastPublishedMs();
// Frames discarded for exceeding the ring slot size (also counted in ring dropped).
uint32_t getOversizeDroppedFrames();
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
