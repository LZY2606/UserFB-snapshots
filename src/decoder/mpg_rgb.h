#pragma once
#include <stdint.h>
#include "../common/mpg_frame.h"
#include "mpg_idct.h"  // clip255

/*
 * Header-only. Converts a decoded YUV 4:2:0 planar picture to RGB565,
 * RGB666, or RGB888 - the pixel formats embedded display libraries
 * (TFT_eSPI, Adafruit_GFX, LovyanGFX, ...) commonly expect. Not part of
 * the decode path itself (MPEG-1 is natively YUV; nothing in decoder/
 * mpg_*.h besides this file depends on it) - an optional conversion step
 * for callers that want to hand decoded frames straight to a display.
 * Direct port of TinyH264's decoder/h264_rgb.h - the YUV->RGB math is
 * codec-agnostic (same ITU-R BT.601 limited-range conversion, same
 * cross-checked-against-ffmpeg integer coefficients), only the Frame type
 * it's templated on differs.
 */

namespace tinympg {

/**
 * Computes the 8-bit R/G/B values for one YUV sample (ITU-R BT.601
 * limited range: Y in [16,235], Cb/Cr in [16,240] - the convention real
 * MPEG-1 encoders emit by default). Shared by every pixel-format
 * converter below; only the final packing into the destination buffer
 * differs between RGB565/RGB666/RGB888.
 */
inline void yuvToRgb8(int y, int u, int v, uint8_t* r, uint8_t* g, uint8_t* b) {
  int c = y - 16;
  int d = u - 128;
  int e = v - 128;
  *r = clip255((298 * c + 409 * e + 128) >> 8);
  *g = clip255((298 * c - 100 * d - 208 * e + 128) >> 8);
  *b = clip255((298 * c + 516 * d + 128) >> 8);
}

/**
 * Converts a `width` x `height` rectangle of a YUV 4:2:0 planar frame,
 * starting at (originX, originY) in the source planes, to RGB565 into a
 * caller-provided buffer. `dst` must have room for width*height
 * uint16_t entries, laid out with *no* row padding - this function never
 * allocates. `originX`/`originY` must be even (chroma is subsampled 2x).
 * Not otherwise bounds-checked.
 */
inline void convertYuv420ToRgb565(const uint8_t* yPlane, int strideY,
                                   const uint8_t* uPlane, const uint8_t* vPlane,
                                   int strideC, int width, int height,
                                   uint16_t* dst, int originX = 0,
                                   int originY = 0) {
  for (int row = 0; row < height; row++) {
    int py = originY + row;
    const uint8_t* yRow = yPlane + (size_t)py * strideY;
    const uint8_t* uRow = uPlane + (size_t)(py >> 1) * strideC;
    const uint8_t* vRow = vPlane + (size_t)(py >> 1) * strideC;
    uint16_t* dstRow = dst + (size_t)row * width;
    for (int col = 0; col < width; col++) {
      int px = originX + col;
      uint8_t r, g, b;
      yuvToRgb8(yRow[px], uRow[px >> 1], vRow[px >> 1], &r, &g, &b);
      dstRow[col] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
  }
}

/**
 * Same as convertYuv420ToRgb565(), packing into RGB666 instead: 3 bytes
 * per pixel (R, G, B order), each byte holding its 6-bit component
 * left-justified in bits 7:2 - the wire format real 18-bit-parallel/SPI
 * TFT controllers expect. `dst` must have room for width*height*3
 * uint8_t entries.
 */
inline void convertYuv420ToRgb666(const uint8_t* yPlane, int strideY,
                                   const uint8_t* uPlane, const uint8_t* vPlane,
                                   int strideC, int width, int height,
                                   uint8_t* dst, int originX = 0,
                                   int originY = 0) {
  for (int row = 0; row < height; row++) {
    int py = originY + row;
    const uint8_t* yRow = yPlane + (size_t)py * strideY;
    const uint8_t* uRow = uPlane + (size_t)(py >> 1) * strideC;
    const uint8_t* vRow = vPlane + (size_t)(py >> 1) * strideC;
    uint8_t* dstRow = dst + (size_t)row * width * 3;
    for (int col = 0; col < width; col++) {
      int px = originX + col;
      uint8_t r, g, b;
      yuvToRgb8(yRow[px], uRow[px >> 1], vRow[px >> 1], &r, &g, &b);
      uint8_t* p = dstRow + col * 3;
      p[0] = (uint8_t)(r & 0xFC);
      p[1] = (uint8_t)(g & 0xFC);
      p[2] = (uint8_t)(b & 0xFC);
    }
  }
}

/**
 * Same as convertYuv420ToRgb565(), packing into RGB888 instead: 3 bytes
 * per pixel (R, G, B order), full 8-bit precision per channel. `dst` must
 * have room for width*height*3 uint8_t entries.
 */
inline void convertYuv420ToRgb888(const uint8_t* yPlane, int strideY,
                                   const uint8_t* uPlane, const uint8_t* vPlane,
                                   int strideC, int width, int height,
                                   uint8_t* dst, int originX = 0,
                                   int originY = 0) {
  for (int row = 0; row < height; row++) {
    int py = originY + row;
    const uint8_t* yRow = yPlane + (size_t)py * strideY;
    const uint8_t* uRow = uPlane + (size_t)(py >> 1) * strideC;
    const uint8_t* vRow = vPlane + (size_t)(py >> 1) * strideC;
    uint8_t* dstRow = dst + (size_t)row * width * 3;
    for (int col = 0; col < width; col++) {
      int px = originX + col;
      uint8_t* p = dstRow + col * 3;
      yuvToRgb8(yRow[px], uRow[px >> 1], vRow[px >> 1], &p[0], &p[1], &p[2]);
    }
  }
}

/// Convenience overload taking a decoded Frame directly (see TinyMPGDecoder::toRGB565()) - whole frame.
template <typename Allocator>
inline void convertFrameToRgb565(const Frame<Allocator>& frame, uint16_t* dst) {
  convertYuv420ToRgb565(frame.y(), frame.strideY, frame.u(), frame.v(), frame.strideC,
                         frame.width, frame.height, dst);
}

/// Convenience overload taking a decoded Frame directly - a sub-rectangle.
template <typename Allocator>
inline void convertFrameToRgb565(const Frame<Allocator>& frame, int originX, int originY,
                                  int width, int height, uint16_t* dst) {
  convertYuv420ToRgb565(frame.y(), frame.strideY, frame.u(), frame.v(), frame.strideC,
                         width, height, dst, originX, originY);
}

/// Convenience overload taking a decoded Frame directly (see TinyMPGDecoder::toRGB666()) - whole frame.
template <typename Allocator>
inline void convertFrameToRgb666(const Frame<Allocator>& frame, uint8_t* dst) {
  convertYuv420ToRgb666(frame.y(), frame.strideY, frame.u(), frame.v(), frame.strideC,
                         frame.width, frame.height, dst);
}

/// Convenience overload taking a decoded Frame directly - a sub-rectangle.
template <typename Allocator>
inline void convertFrameToRgb666(const Frame<Allocator>& frame, int originX, int originY,
                                  int width, int height, uint8_t* dst) {
  convertYuv420ToRgb666(frame.y(), frame.strideY, frame.u(), frame.v(), frame.strideC,
                         width, height, dst, originX, originY);
}

/// Convenience overload taking a decoded Frame directly (see TinyMPGDecoder::toRGB888()) - whole frame.
template <typename Allocator>
inline void convertFrameToRgb888(const Frame<Allocator>& frame, uint8_t* dst) {
  convertYuv420ToRgb888(frame.y(), frame.strideY, frame.u(), frame.v(), frame.strideC,
                         frame.width, frame.height, dst);
}

/// Convenience overload taking a decoded Frame directly - a sub-rectangle.
template <typename Allocator>
inline void convertFrameToRgb888(const Frame<Allocator>& frame, int originX, int originY,
                                  int width, int height, uint8_t* dst) {
  convertYuv420ToRgb888(frame.y(), frame.strideY, frame.u(), frame.v(), frame.strideC,
                         width, height, dst, originX, originY);
}

}  // namespace tinympg
