// RIFF/AVI 1.0 muxer — MJPEG video only (no audio stream).
//
// Container layout:
//   RIFF 'AVI '
//     LIST 'hdrl'
//       'avih'  — MainAVIHeader
//       LIST 'strl' (video)
//         'strh' — AVIStreamHeader (vids/MJPG)
//         'strf' — BITMAPINFOHEADER
//     LIST 'movi'
//       '00dc' — video chunks (JPEG)
//
// begin() writes a placeholder header (sizes zeroed).
// end() seeks back and patches RIFF size, movi size, and frame counts.

#include "avi_writer.h"

// Gated by the flag that advertises it. The muxer is complete but not wired to any
// recorder task yet (POST /record returns 501) — see docs/known_issues.md.
#ifdef INCLUDE_AVI_WRITER

#include "ws_log.h"
#include <string.h>

static void put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void put16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static void putCC(uint8_t* p, const char* cc) { memcpy(p, cc, 4); }

bool AviWriter::begin(File& file, uint16_t width, uint16_t height, uint8_t fps) {
    _file = &file;
    _width = width; _height = height;
    _fps = fps > 0 ? fps : 10;
    _videoFrames = 0; _totalDataBytes = 0;

    // RIFF(12) + LIST hdrl(12) + avih(64) +
    // LIST strl_v(12) + strh_v(64) + strf_v(48) +
    // LIST movi(12) = 224 bytes
    const size_t HDR_SIZE = 224;
    uint8_t hdr[HDR_SIZE];
    memset(hdr, 0, HDR_SIZE);

    uint32_t usPerFrame = 1000000 / _fps;
    size_t p = 0;

    putCC(hdr + p, "RIFF"); p += 4;
    put32(hdr + p, 0);      p += 4;  // patched in end()
    putCC(hdr + p, "AVI "); p += 4;

    putCC(hdr + p, "LIST"); p += 4;
    put32(hdr + p, 4 + 64 + (12 + 64 + 48)); p += 4;  // hdrl payload = 192
    putCC(hdr + p, "hdrl"); p += 4;

    // avih (MainAVIHeader, 56 bytes)
    putCC(hdr + p, "avih"); p += 4;
    put32(hdr + p, 56);     p += 4;
    put32(hdr + p, usPerFrame); p += 4;
    put32(hdr + p, 0);      p += 4;  // dwMaxBytesPerSec
    put32(hdr + p, 0);      p += 4;  // dwPaddingGranularity
    put32(hdr + p, 0x10);   p += 4;  // dwFlags: AVIF_HASINDEX
    put32(hdr + p, 0);      p += 4;  // dwTotalFrames — patched in end()
    put32(hdr + p, 0);      p += 4;  // dwInitialFrames
    put32(hdr + p, 1);      p += 4;  // dwStreams
    put32(hdr + p, 0);      p += 4;  // dwSuggestedBufferSize
    put32(hdr + p, _width); p += 4;
    put32(hdr + p, _height);p += 4;
    p += 16;  // dwReserved[4]

    // LIST strl (video)
    putCC(hdr + p, "LIST"); p += 4;
    put32(hdr + p, 4 + 64 + 48); p += 4;
    putCC(hdr + p, "strl"); p += 4;

    // strh (AVIStreamHeader — video, 56 bytes)
    putCC(hdr + p, "strh"); p += 4;
    put32(hdr + p, 56);     p += 4;
    putCC(hdr + p, "vids"); p += 4;
    putCC(hdr + p, "MJPG"); p += 4;
    put32(hdr + p, 0);      p += 4;  // dwFlags
    put16(hdr + p, 0);      p += 2;  // wPriority
    put16(hdr + p, 0);      p += 2;  // wLanguage
    put32(hdr + p, 0);      p += 4;  // dwInitialFrames
    put32(hdr + p, 1);      p += 4;  // dwScale
    put32(hdr + p, _fps);   p += 4;  // dwRate
    put32(hdr + p, 0);      p += 4;  // dwStart
    put32(hdr + p, 0);      p += 4;  // dwLength — patched in end()
    put32(hdr + p, 0);      p += 4;  // dwSuggestedBufferSize
    put32(hdr + p, 0);      p += 4;  // dwQuality
    put32(hdr + p, 0);      p += 4;  // dwSampleSize
    put16(hdr + p, 0); p += 2;       // rcFrame.left
    put16(hdr + p, 0); p += 2;       // rcFrame.top
    put16(hdr + p, _width); p += 2;  // rcFrame.right
    put16(hdr + p, _height);p += 2;  // rcFrame.bottom

    // strf (BITMAPINFOHEADER, 40 bytes)
    putCC(hdr + p, "strf"); p += 4;
    put32(hdr + p, 40);     p += 4;
    put32(hdr + p, 40);     p += 4;  // biSize
    put32(hdr + p, _width); p += 4;
    put32(hdr + p, _height);p += 4;
    put16(hdr + p, 1);      p += 2;  // biPlanes
    put16(hdr + p, 24);     p += 2;  // biBitCount
    putCC(hdr + p, "MJPG"); p += 4;  // biCompression
    put32(hdr + p, _width * _height * 3); p += 4;
    p += 16;  // biXPels, biYPels, biClrUsed, biClrImportant

    // LIST movi
    putCC(hdr + p, "LIST"); p += 4;
    _moviStart = p;
    put32(hdr + p, 0);      p += 4;  // patched in end()
    putCC(hdr + p, "movi"); p += 4;

    if (p != HDR_SIZE) {
        logCapture("[AVI] Header size mismatch: %u != %u\n", (unsigned)p, (unsigned)HDR_SIZE);
        return false;
    }

    return _file->write(hdr, HDR_SIZE) == HDR_SIZE;
}

bool AviWriter::writeVideoFrame(const uint8_t* jpegData, size_t jpegLen) {
    if (!writeChunk("00dc", jpegData, jpegLen)) return false;
    _videoFrames++;
    return true;
}

bool AviWriter::writeChunk(const char fourcc[4], const uint8_t* data, size_t len) {
    if (!_file) return false;
    uint8_t chunkHdr[8];
    memcpy(chunkHdr, fourcc, 4);
    put32(chunkHdr + 4, (uint32_t)len);
    if (_file->write(chunkHdr, 8) != 8) return false;
    if (len > 0 && _file->write(data, len) != len) return false;
    _totalDataBytes += 8 + len;
    if (len & 1) { writePadByte(len); _totalDataBytes++; }
    return true;
}

void AviWriter::writePadByte(size_t len) {
    if (len & 1) { uint8_t zero = 0; _file->write(&zero, 1); }
}

bool AviWriter::end() {
    if (!_file) return false;
    uint8_t buf[4];

    uint32_t riffSizeVal = (uint32_t)(_file->position() - 8);
    _file->seek(4);
    put32(buf, riffSizeVal); _file->write(buf, 4);

    // avih.dwTotalFrames at offset 48
    _file->seek(48);
    put32(buf, _videoFrames); _file->write(buf, 4);

    // video strh.dwLength at offset 140
    _file->seek(140);
    put32(buf, _videoFrames); _file->write(buf, 4);

    // movi LIST size
    uint32_t moviSize = 4 + _totalDataBytes;
    _file->seek(_moviStart);
    put32(buf, moviSize); _file->write(buf, 4);

    _file->flush();
    _file = nullptr;
    return true;
}

#endif // INCLUDE_AVI_WRITER
