#ifndef IMAGE_UTILS_H
#define IMAGE_UTILS_H

#include <Arduino.h>

// Shared helpers for the three detection paths (motion / person / face).
// Before this header each of them carried its own copy of the RGB565->gray
// conversion plus a hand-written frame_size -> decoded-size lookup table.
// The tables drifted: motion_detect knew about QXGA/QSXGA, person and face
// did not, so selecting those sizes made jpg2rgb565 write past the end of
// their decode buffers. Deriving the size from the frame that was actually
// captured removes that failure mode entirely.

// Largest source frame we let the sensor produce (QSXGA 2560x1920). Decode
// buffers are sized from this so a decode can never overflow them, whatever
// frame_size the user picks.
#define IMG_SRC_MAX_W   2560
#define IMG_SRC_MAX_H   1920

// Decoded (1/8 scale) worst case: 320x240 RGB565 = 153600 B in PSRAM.
#define IMG_DECODE_MAX_W   (IMG_SRC_MAX_W / 8)
#define IMG_DECODE_MAX_H   (IMG_SRC_MAX_H / 8)
#define IMG_DECODE_MAX_SZ  (IMG_DECODE_MAX_W * IMG_DECODE_MAX_H * 2)

// Decoded dimensions for a JPG_SCALE_8X decode of a srcW x srcH frame.
// tjpgd emits whole MCUs, so a source that is not a multiple of 8 rounds up.
static inline void imgDecodedDims8x(uint16_t srcW, uint16_t srcH, int& decW, int& decH) {
    decW = (srcW + 7) / 8;
    decH = (srcH + 7) / 8;
}

// True when a decode of srcW x srcH at 1/8 scale fits in `bufBytes`.
static inline bool imgDecodeFits(uint16_t srcW, uint16_t srcH, size_t bufBytes,
                                 int& decW, int& decH) {
    imgDecodedDims8x(srcW, srcH, decW, decH);
    if (decW <= 0 || decH <= 0) return false;
    return (size_t)decW * (size_t)decH * 2u <= bufBytes;
}

// RGB565 (big-endian, as produced by jpg2rgb565) -> 8-bit luma, BT.601.
// Fixed point: (77R + 150G + 29B) >> 8 with the 5/6-bit channels scaled to 8.
static inline uint8_t imgRgb565ToGray(uint16_t pixel) {
    uint8_t r = (pixel >> 11) & 0x1F;
    uint8_t g = (pixel >> 5)  & 0x3F;
    uint8_t b =  pixel        & 0x1F;
    return (uint8_t)((77 * (r << 3) + 150 * (g << 2) + 29 * (b << 3)) >> 8);
}

// Whole-frame RGB565 -> grayscale, same dimensions.
static inline void imgRgb565ToGrayscale(const uint8_t* rgb565, uint8_t* gray, int w, int h) {
    const uint16_t* pixels = (const uint16_t*)rgb565;
    for (int i = 0; i < w * h; i++) gray[i] = imgRgb565ToGray(pixels[i]);
}

#endif // IMAGE_UTILS_H
