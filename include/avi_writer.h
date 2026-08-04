#ifndef AVI_WRITER_H
#define AVI_WRITER_H

#include <Arduino.h>
#include "FS.h"

// RIFF/AVI 1.0 muxer — MJPEG video only (CamS3 has no microphone).
//
// Usage:
//   AviWriter avi;
//   avi.begin(file, width, height, fps);
//   // loop:
//     avi.writeVideoFrame(jpegData, jpegLen);
//   avi.end();
class AviWriter {
public:
    bool begin(File& file, uint16_t width, uint16_t height, uint8_t fps);
    bool writeVideoFrame(const uint8_t* jpegData, size_t jpegLen);
    bool end();

    uint32_t getVideoFrames() const { return _videoFrames; }
    uint32_t getTotalBytes()  const { return _totalDataBytes; }

private:
    File*    _file = nullptr;
    uint16_t _width = 0;
    uint16_t _height = 0;
    uint8_t  _fps = 10;
    uint32_t _videoFrames = 0;
    uint32_t _totalDataBytes = 0;
    uint32_t _moviStart = 0;

    bool writeChunk(const char fourcc[4], const uint8_t* data, size_t len);
    void writePadByte(size_t len);
};

#endif // AVI_WRITER_H
