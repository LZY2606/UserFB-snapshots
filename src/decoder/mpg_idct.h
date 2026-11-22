#pragma once
#include <stdint.h>
#include <cmath>

/*
 * Header-only. The 8x8 inverse DCT (ISO/IEC 11172-2 clause 2.4.4.3 - MPEG-1
 * doesn't mandate a specific IDCT algorithm/implementation, only an
 * accuracy bound real decoders are expected to meet, so this is a direct,
 * separable, single-precision (`float`) implementation of the textbook
 * formula rather than a port of any one reference decoder's fixed-point
 * approximation. Dequantization (which produces this function's input,
 * already at the coefficient scale the classical IDCT formula expects) is
 * decoder/mpg_macroblock.h's job, not this file's.
 *
 * Deliberately `float`, not `double`: this project's first real on-device
 * performance measurement (docs/memory-budget.md's Performance section -
 * ~1.5fps encode, ~2.1fps decode at QCIF on a plain ESP32) confirmed
 * `double` was the dominant cost - the ESP32's Xtensa LX6 FPU (like most
 * microcontroller FPUs) only implements *single*-precision arithmetic in
 * hardware, so every `double` operation this transform used fell back to
 * slow software emulation. `float` keeps every multiply-add on this
 * project's hot path in hardware instead. Every literal below is
 * deliberately `f`-suffixed (`0.25f`, not `0.25`) - a bare floating-point
 * literal is `double`, and mixing it into a `float` expression silently
 * promotes the whole expression back to `double`, defeating the point.
 *
 * Not optimized further than that: a per-call separable transform (even
 * in `float`) is still nowhere near what an embedded target wants long-
 * term (real decoders use fixed-point/integer approximations, e.g.
 * FFmpeg's simple_idct) - left as a future optimization, same as
 * mpg_bitreader.h::readVlc()'s linear scan.
 */

namespace tinympg {

/// Clip1 (clause 2.4.4.3): clamps a signed sample value into [0,255].
inline uint8_t clip255(int32_t v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

namespace detail {

/// pi as a float literal - deliberately not <cmath>'s M_PI (a double), see this file's top comment.
static const float kPi = 3.14159265358979323846f;

/// Lazily-built, cached cos((2x+1)*u*pi/16) table plus the C(u) normalization factors.
struct IdctConstants {
  float cosTab[8][8];
  float c[8];
  IdctConstants() {
    for (int x = 0; x < 8; x++) {
      for (int u = 0; u < 8; u++) {
        cosTab[x][u] = std::cos((float)(2 * x + 1) * (float)u * kPi / 16.0f);
      }
    }
    c[0] = 1.0f / std::sqrt(2.0f);
    for (int i = 1; i < 8; i++) c[i] = 1.0f;
  }
};

inline const IdctConstants& idctConstants() {
  static const IdctConstants k;
  return k;
}

}  // namespace detail

/**
 * 8x8 inverse DCT, raster order (block[row*8+col] = F(u=col, v=row), the
 * same raster convention kZigzag/decodeIntraBlock() dequantize into).
 * `out[row*8+col]` receives the reconstructed spatial-domain sample -
 * directly the final pixel value for an intra block (MPEG-1 intra has no
 * separate spatial prediction to add this to, see docs/scope.md), still
 * needing clip255() before writeback since IDCT output can exceed
 * [0,255].
 */
inline void idct8x8(const int32_t block[64], int32_t out[64]) {
  const detail::IdctConstants& k = detail::idctConstants();
  float tmp[8][8];  // tmp[x][v]: 1D IDCT along the horizontal (u) axis
  for (int v = 0; v < 8; v++) {
    for (int x = 0; x < 8; x++) {
      float sum = 0.0f;
      for (int u = 0; u < 8; u++) sum += k.c[u] * (float)block[v * 8 + u] * k.cosTab[x][u];
      tmp[x][v] = sum;
    }
  }
  for (int x = 0; x < 8; x++) {
    for (int y = 0; y < 8; y++) {
      float sum = 0.0f;
      for (int v = 0; v < 8; v++) sum += k.c[v] * tmp[x][v] * k.cosTab[y][v];
      out[y * 8 + x] = (int32_t)std::lround(sum * 0.25f);
    }
  }
}

}  // namespace tinympg
