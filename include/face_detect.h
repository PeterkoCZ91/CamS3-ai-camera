#ifndef FACE_DETECT_H
#define FACE_DETECT_H

#include <Arduino.h>

struct FaceDetectResult {
    bool detected;          // Face(s) currently detected
    int face_count;         // Number of faces in last inference
    int largest_x;          // Largest face bounding box
    int largest_y;
    int largest_w;
    int largest_h;
    float largest_score;    // Confidence of largest face
    uint32_t inference_ms;  // Last inference time
};

void faceDetectInit();
void faceDetectTask(void* param);
bool isFaceDetected();
unsigned long getLastFaceTime();
uint32_t getFaceEventCount();
FaceDetectResult getFaceDetectResult();

#endif // FACE_DETECT_H
