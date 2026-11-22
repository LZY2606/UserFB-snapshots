#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Header-only. Converts packed RGB888/RGB666/RGB565 or packed YUYV 4:2:2
 * source data into planar YUV 4:2:0 - the encode-side mirror of
 * decoder/mpg_rgb.h's YUV-to-RGB converters, using the algebraic inverse
 * of that file's yuvToRgb8() (same ITU-R BT.601 limited-range
 * coefficients, cross-checked against ffmpeg's own RGB->YUV matrix) so a
 * convert-then-encode-then-decode-then-convert-back round trip stays
 * close to identity aside from the deliberate chroma subsampling and DCT
 * quantization loss described in encoder/mpg_encoder.h.
 *
 * Chroma (U/V) is derived once per 2x2 luma block from the *averaged*
 * R/G/B of all four source pixels (rather than four independent U/V
 * values, one of which is then thrown away) - the same "average first,
 * convert once" approach real 4:2:0 chroma subsampling uses, producing
 * less chroma noise than naively picking one corner pixel's chroma per
 * block.
 */

namespace tinympg {

/// BT.601 limited-range RGB->YUV, one pixel - the algebraic inverse of decoder/mpg_rgb.h's yuvToRgb8().
inline void rgbToYuv8(int r, int g, int b, uint8_t* y, uint8_t* u, uint8_t* v) {
  int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
  int uu = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
  int vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
  *y = (uint8_t)(yy < 0 ? 0 : (yy > 255 ? 255 : yy));
  *u = (uint8_t)(uu < 0 ? 0 : (uu > 255 ? 255 : uu));
  *v = (uint8_t)(vv < 0 ? 0 : (vv > 255 ? 255 : vv));
}

/**
 * Converts a `width` x `height` RGB888 image (3 bytes/pixel, R/G/B order,
 * `srcStride` bytes/row) into planar YUV 4:2:0 (`dstY`/`dstU`/`dstV`,
 * `strideY`/`strideC` bytes/row respectively - typically the encoder's
 * own width_/width_/2). `width`/`height` must be even (2x2 chroma
 * blocks).
 */
inline void convertRgb888ToYuv420(const uint8_t* src, int srcStride, int width, int height,
                                   uint8_t* dstY, int strideY, uint8_t* dstU, uint8_t* dstV,
                                   int strideC) {
  for (int by = 0; by < height; by += 2) {
    for (int bx = 0; bx < width; bx += 2) {
      int sumR = 0, sumG = 0, sumB = 0;
      for (int dy = 0; dy < 2; dy++) {
        const uint8_t* row = src + (size_t)(by + dy) * srcStride + bx * 3;
        for (int dx = 0; dx < 2; dx++) {
          const uint8_t* p = row + dx * 3;
          uint8_t yv, uv, vv;
          rgbToYuv8(p[0], p[1], p[2], &yv, &uv, &vv);
          dstY[(size_t)(by + dy) * strideY + (bx + dx)] = yv;
          sumR += p[0];
          sumG += p[1];
          sumB += p[2];
        }
      }
      uint8_t yv, uv, vv;
      rgbToYuv8(sumR / 4, sumG / 4, sumB / 4, &yv, &uv, &vv);
      dstU[(size_t)(by / 2) * strideC + (bx / 2)] = uv;
      dstV[(size_t)(by / 2) * strideC + (bx / 2)] = vv;
    }
  }
}

/**
 * Same as convertRgb888ToYuv420(), for RGB666 source data (3 bytes/pixel,
 * each byte's 6 significant bits left-justified in bits 7:2, matching
 * decoder/mpg_rgb.h's convertYuv420ToRgb666() output convention) - each
 * byte is widened to a full 8-bit value (replicating the top 2 bits into
 * the low 2) before the same RGB->YUV math.
 */
inline void convertRgb666ToYuv420(const uint8_t* src, int srcStride, int width, int height,
                                   uint8_t* dstY, int strideY, uint8_t* dstU, uint8_t* dstV,
                                   int strideC) {
  auto widen = [](uint8_t v) -> int { return v | (v >> 6); };
  for (int by = 0; by < height; by += 2) {
    for (int bx = 0; bx < width; bx += 2) {
      int sumR = 0, sumG = 0, sumB = 0;
      for (int dy = 0; dy < 2; dy++) {
        const uint8_t* row = src + (size_t)(by + dy) * srcStride + bx * 3;
        for (int dx = 0; dx < 2; dx++) {
          const uint8_t* p = row + dx * 3;
          int r = widen(p[0]), g = widen(p[1]), b = widen(p[2]);
          uint8_t yv, uv, vv;
          rgbToYuv8(r, g, b, &yv, &uv, &vv);
          dstY[(size_t)(by + dy) * strideY + (bx + dx)] = yv;
          sumR += r;
          sumG += g;
          sumB += b;
        }
      }
      uint8_t yv, uv, vv;
      rgbToYuv8(sumR / 4, sumG / 4, sumB / 4, &yv, &uv, &vv);
      dstU[(size_t)(by / 2) * strideC + (bx / 2)] = uv;
      dstV[(size_t)(by / 2) * strideC + (bx / 2)] = vv;
    }
  }
}

/**
 * Same as convertRgb888ToYuv420(), for RGB565 source data (uint16_t/pixel,
 * 5-6-5 packed, matching decoder/mpg_rgb.h's convertYuv420ToRgb565()
 * output convention). `srcStride` is in uint16_t entries, not bytes.
 */
inline void convertRgb565ToYuv420(const uint16_t* src, int srcStride, int width, int height,
                                   uint8_t* dstY, int strideY, uint8_t* dstU, uint8_t* dstV,
                                   int strideC) {
  for (int by = 0; by < height; by += 2) {
    for (int bx = 0; bx < width; bx += 2) {
      int sumR = 0, sumG = 0, sumB = 0;
      for (int dy = 0; dy < 2; dy++) {
        const uint16_t* row = src + (size_t)(by + dy) * srcStride + bx;
        for (int dx = 0; dx < 2; dx++) {
          uint16_t px = row[dx];
          int r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
          int r = (r5 << 3) | (r5 >> 2);
          int g = (g6 << 2) | (g6 >> 4);
          int b = (b5 << 3) | (b5 >> 2);
          uint8_t yv, uv, vv;
          rgbToYuv8(r, g, b, &yv, &uv, &vv);
          dstY[(size_t)(by + dy) * strideY + (bx + dx)] = yv;
          sumR += r;
          sumG += g;
          sumB += b;
        }
      }
      uint8_t yv, uv, vv;
      rgbToYuv8(sumR / 4, sumG / 4, sumB / 4, &yv, &uv, &vv);
      dstU[(size_t)(by / 2) * strideC + (bx / 2)] = uv;
      dstV[(size_t)(by / 2) * strideC + (bx / 2)] = vv;
    }
  }
}

/**
 * Converts a `width` x `height` YUYV-order packed YUV 4:2:2 image (Y0 U0
 * Y1 V0 per horizontal pixel pair, the common camera-module convention -
 * `srcStride` bytes/row) into planar YUV 4:2:0. Luma is copied straight
 * through (4:2:2 and 4:2:0 share the same full-resolution Y plane);
 * chroma is already horizontally subsampled by the source format, so
 * only *vertical* subsampling is needed here - done by averaging each
 * pair of vertically-adjacent chroma samples (rather than dropping every
 * other row outright), the same "average, don't discard" approach the
 * RGB converters above use for both axes. `width` must be even (YUYV's
 * own pairing); `height` must be even (4:2:0's vertical chroma pairing).
 */
inline void convertYuyv422ToYuv420(const uint8_t* src, int srcStride, int width, int height,
                                    uint8_t* dstY, int strideY, uint8_t* dstU, uint8_t* dstV,
                                    int strideC) {
  for (int y = 0; y < height; y++) {
    const uint8_t* row = src + (size_t)y * srcStride;
    uint8_t* yRow = dstY + (size_t)y * strideY;
    for (int x = 0; x < width; x += 2) {
      const uint8_t* p = row + x * 2;
      yRow[x] = p[0];
      yRow[x + 1] = p[2];
    }
  }
  for (int by = 0; by < height; by += 2) {
    const uint8_t* row0 = src + (size_t)by * srcStride;
    const uint8_t* row1 = src + (size_t)(by + 1) * srcStride;
    uint8_t* uRow = dstU + (size_t)(by / 2) * strideC;
    uint8_t* vRow = dstV + (size_t)(by / 2) * strideC;
    for (int x = 0; x < width; x += 2) {
      const uint8_t* p0 = row0 + x * 2;
      const uint8_t* p1 = row1 + x * 2;
      uRow[x / 2] = (uint8_t)(((int)p0[1] + (int)p1[1] + 1) / 2);
      vRow[x / 2] = (uint8_t)(((int)p0[3] + (int)p1[3] + 1) / 2);
    }
  }
}

}  // namespace tinympg
