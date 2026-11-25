#pragma once
#include <stdint.h>
#include <cmath>
#include "../decoder/mpg_idct.h"

/*
 * Header-only. The 8x8 forward DCT (ISO/IEC 11172-2 clause 2.4.4.3, encode
 * direction) - like decoder/mpg_idct.h's idct8x8(), MPEG-1 doesn't mandate
 * a specific forward-transform algorithm/implementation either (only the
 * *decoder's* accuracy bound is normative), so this is the direct,
 * separable, single-precision (`float`) DCT-II that's the textbook
 * orthogonal adjoint of idct8x8()'s DCT-III - same cosTab/C(u) constants
 * (decoder::detail::idctConstants(), reused via this file's cross-
 * directory include, the same kind of deliberate encoder->decoder
 * dependency TinyH264's own encoder/h264_encoder.h takes on
 * decoder/h264_deblock.h for the same reason: reuse an already-verified
 * piece rather than risk a second, subtly different implementation of the
 * same math), just applied in the adjoint role (each output frequency
 * sums over all spatial samples, instead of each output sample summing
 * over all frequencies). `float`, not `double` - see mpg_idct.h's file
 * comment for why (this project's first real on-device measurement
 * pinned `double` as the dominant encode/decode cost, since most
 * microcontroller FPUs, including the ESP32's, have no hardware
 * `double` support) - every literal below is deliberately `f`-suffixed
 * for the same reason that file explains.
 *
 * Not optimized further than that, for the same reason idct8x8() isn't
 * (see that file's own comment) - a future fixed-point pass is deferred
 * until this project's correctness is settled here too.
 */

namespace tinympg {

/**
 * 8x8 forward DCT, raster order (block[row*8+col] = f(x=col, y=row), the
 * same convention idct8x8() consumes/produces). `out[row*8+col]` receives
 * F(u=col, v=row) - the raw transform coefficient, not yet quantized
 * (encoder/mpg_macroblock_encode.h's job). For an intra block, `block`
 * should be the raw sample values (0..255) directly - MPEG-1 intra DCT
 * operates on absolute sample values, not samples centered around zero
 * (unlike JPEG's baseline convention) - matching decodeIntraBlock()'s own
 * convention of feeding IDCT output straight to clip255() with no added
 * offset (docs/scope.md's Phase A section).
 */
inline void fdct8x8(const int32_t block[64], int32_t out[64]) {
  const detail::IdctConstants& k = detail::idctConstants();
  float tmp[8][8];  // tmp[y][u]: 1D forward DCT along the horizontal (x) axis, per row y
  for (int y = 0; y < 8; y++) {
    for (int u = 0; u < 8; u++) {
      float sum = 0.0f;
      for (int x = 0; x < 8; x++) sum += (float)block[y * 8 + x] * k.cosTab[x][u];
      tmp[y][u] = k.c[u] * sum;
    }
  }
  for (int u = 0; u < 8; u++) {
    for (int v = 0; v < 8; v++) {
      float sum = 0.0f;
      for (int y = 0; y < 8; y++) sum += tmp[y][u] * k.cosTab[y][v];
      out[v * 8 + u] = (int32_t)std::lround(k.c[v] * sum * 0.25f);
    }
  }
}

}  // namespace tinympg
