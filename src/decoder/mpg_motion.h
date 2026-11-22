#pragma once
#include <stdint.h>
#include "mpg_bitreader.h"
#include "../common/mpg_frame.h"
#include "../common/mpg_vlc_tables.h"

/*
 * Header-only. Motion vector decode (motion_code/motion_residual, ISO/IEC
 * 11172-2 clause 2.4.3.7/Annex B, Table B.10 - shared by forward and
 * backward vectors alike, just with a different f_code/predictor per
 * direction) and half-pel luma/chroma motion compensation (clause 7.6.4),
 * for Phase B (P-pictures, forward-only) and Phase C (B-pictures,
 * forward/backward/bidirectional - see docs/scope.md). MPEG-1 always uses
 * one 16x16 motion vector per direction per macroblock - no 8x8
 * partitions (that's an H.263/MPEG-4 feature) - so this is considerably
 * simpler than a general MPEG-2/H.263 motion compensator.
 *
 * decodeMotionComponent()'s VLC-plus-raw-bits-plus-predictor shape and
 * the final wraparound (signExtend()) are a direct port of FFmpeg's
 * mpeg_decode_motion() (libavcodec/mpeg12dec.c, itself commented "as
 * H.263, but only 17 codes"). The half-pel averaging formulas
 * (predictBlockHalfPel()'s 2-tap/4-tap cases) were independently derived
 * from the standard "round half up" convention (clause 7.6.4) and then
 * cross-checked against FFmpeg's libavcodec/rnd_avg.h::rnd_avg32() bit
 * trick and libavcodec/hpeldsp.c's pixels8_xy2_8_c() - both reduce to
 * (a+b+1)>>1 (2-tap) and (a+b+c+d+2)>>2 (4-tap) once expanded out of
 * their packed-SIMD form. The chroma motion vector derivation
 * (predictMacroblock()'s `mvX / 2` - C truncating division, deliberately
 * *not* the arithmetic-shift floor division used everywhere else in this
 * file) matches FFmpeg's mpeg_motion_internal() FMT_MPEG1 case exactly;
 * getting this specific rounding direction wrong only shows up as a
 * one-chroma-sample misplacement near odd motion vectors, easy to miss
 * without a real bitstream to check against (see docs/testing.md).
 */

namespace tinympg {

/**
 * Sign-extends the low `bits` bits of `val` (two's complement), used for
 * the motion vector "modulo decoding" wraparound in clause 7.6.3.1 - a
 * differentially-coded motion vector component is always taken modulo
 * the range implied by f_code, so a value that overflows past one edge
 * of the picture wraps to the other side rather than growing unbounded.
 */
inline int32_t signExtend(int32_t val, int bits) {
  int32_t shift = 32 - bits;
  return (val << shift) >> shift;
}

/// Result of decodeMotionComponent().
struct MotionResult {
  int32_t value = 0;
  bool error = false;
};

/**
 * Decodes one motion vector component (horizontal or vertical - called
 * twice per explicitly-coded forward motion vector), clause 2.4.3.7's
 * motion_code/motion_residual. `predictor` is the same component of the
 * previous coded macroblock's motion vector (0 at the start of a slice,
 * or after an intra/zero-MV macroblock resets it - see
 * decodePMacroblock(), mpg_macroblock.h); the result becomes the new
 * predictor for the next macroblock in turn. `fCode` (1..7, from
 * picture_header's forward_f_code) controls how many raw
 * motion_residual bits extend the coarse VLC-coded magnitude for larger
 * search ranges.
 */
inline MotionResult decodeMotionComponent(BitReader& br, int fCode, int32_t predictor) {
  MotionResult r;
  int idx = br.readVlc(kMotionCode, kMotionCodeBits, kMotionCodeTableSize, kMotionCodeMaxBits);
  if (idx < 0) {
    r.error = true;
    return r;
  }
  if (idx == 0) {  // no motion_code: this component equals its predictor, unchanged
    r.value = predictor;
    return r;
  }

  bool negative = br.flag();
  int shift = fCode - 1;
  int32_t val = idx;
  if (shift > 0) {
    val = ((val - 1) << shift) | (int32_t)br.u(shift);
    val++;
  }
  if (negative) val = -val;
  val += predictor;
  r.value = signExtend(val, 5 + shift);
  return r;
}

/**
 * Samples one pixel from a plane, clamping out-of-range coordinates to
 * the nearest edge pixel (border replication) - real motion vectors
 * legally point a few pixels outside the picture near its edges (clause
 * 7.6.4 allows this), and this is the standard, simple way to handle it
 * (mathematically equivalent to FFmpeg's own edge-emulation buffer for a
 * plain clamp, just computed per-sample instead of into a temporary).
 */
inline uint8_t clampSample(const uint8_t* plane, int stride, int w, int h, int x, int y) {
  if (x < 0) x = 0;
  else if (x >= w) x = w - 1;
  if (y < 0) y = 0;
  else if (y >= h) y = h - 1;
  return plane[(size_t)y * stride + x];
}

/**
 * Computes one motion-compensated sample from `srcPlane` at half-pel
 * position (baseX + x, baseY + y) with fractional offsets `fracX`/`fracY`
 * (each 0 or 1) - direct copy, horizontal, vertical, or bilinear half-pel
 * interpolation depending on which fractional bits are set. Shared by
 * both predictBlockHalfPel() (a straight write, "put") and
 * predictBlockHalfPelAvg() (blended into the existing destination
 * sample, "avg" - see that function's comment for when Phase C's
 * bidirectional prediction needs it instead).
 */
inline int predictedSample(const uint8_t* srcPlane, int srcStride, int srcW, int srcH, int sx,
                            int sy, int fracX, int fracY) {
  if (!fracX && !fracY) {
    return clampSample(srcPlane, srcStride, srcW, srcH, sx, sy);
  } else if (fracX && !fracY) {
    int p0 = clampSample(srcPlane, srcStride, srcW, srcH, sx, sy);
    int p1 = clampSample(srcPlane, srcStride, srcW, srcH, sx + 1, sy);
    return (p0 + p1 + 1) >> 1;
  } else if (!fracX && fracY) {
    int p0 = clampSample(srcPlane, srcStride, srcW, srcH, sx, sy);
    int p1 = clampSample(srcPlane, srcStride, srcW, srcH, sx, sy + 1);
    return (p0 + p1 + 1) >> 1;
  } else {
    int p00 = clampSample(srcPlane, srcStride, srcW, srcH, sx, sy);
    int p10 = clampSample(srcPlane, srcStride, srcW, srcH, sx + 1, sy);
    int p01 = clampSample(srcPlane, srcStride, srcW, srcH, sx, sy + 1);
    int p11 = clampSample(srcPlane, srcStride, srcW, srcH, sx + 1, sy + 1);
    return (p00 + p10 + p01 + p11 + 2) >> 2;
  }
}

/**
 * Motion-compensates one `blockW` x `blockH` rectangle from `srcPlane`
 * (a reference picture's plane) into `dstPlane` at (dstX, dstY), using
 * `mvX`/`mvY` (half-pel units - the integer part is `mv >> 1`, the
 * fractional part `mv & 1` selects among direct copy / horizontal /
 * vertical / bilinear half-pel interpolation). This *overwrites*
 * dstPlane with the prediction; adding any coded residual on top is the
 * caller's job (decodePMacroblock()/decodeBMacroblock(), mpg_macroblock.h)
 * once this returns. Used directly for single-direction prediction (a
 * P-picture's only direction, or a B-picture macroblock using just
 * forward or just backward prediction) - see predictBlockHalfPelAvg()
 * for the second half of Phase C's bidirectional case.
 */
inline void predictBlockHalfPel(const uint8_t* srcPlane, int srcStride, int srcW, int srcH,
                                 uint8_t* dstPlane, int dstStride, int dstX, int dstY,
                                 int blockW, int blockH, int mvX, int mvY) {
  int baseX = dstX + (mvX >> 1);
  int baseY = dstY + (mvY >> 1);
  int fracX = mvX & 1;
  int fracY = mvY & 1;

  for (int y = 0; y < blockH; y++) {
    uint8_t* dstRow = dstPlane + (size_t)(dstY + y) * dstStride + dstX;
    int sy = baseY + y;
    for (int x = 0; x < blockW; x++) {
      dstRow[x] = (uint8_t)predictedSample(srcPlane, srcStride, srcW, srcH, baseX + x, sy, fracX,
                                            fracY);
    }
  }
}

/**
 * The "avg" counterpart of predictBlockHalfPel(): computes the same
 * motion-compensated sample, but *blends* it into whatever is already at
 * `dstPlane` (round-half-up 2-tap average, clause 7.6.4) instead of
 * overwriting it - used for the second direction of a bidirectionally-
 * predicted B-picture macroblock, after the first direction has already
 * been written via predictBlockHalfPel(). This exact "put the first
 * prediction, then avg the second one on top" sequencing - rather than
 * computing both predictions independently and averaging them in one
 * step - is deliberate: it's what FFmpeg's own mpegvideo_dec.c does for
 * MPEG-1/2 (`op_pix` switches from `put_pixels_tab` to `avg_pixels_tab`
 * between the forward and backward ff_mpv_motion() calls), and the two
 * are only guaranteed to agree because both stages use the *same*
 * round-half-up convention (verified against a real bidirectionally-
 * predicted macroblock - see docs/testing.md - MPEG-1 always takes that
 * path; the alternate `no_rnd` averaging FFmpeg's hpeldsp.c also defines
 * is for other codecs' P-frames, never reached for `FMT_MPEG1`).
 */
inline void predictBlockHalfPelAvg(const uint8_t* srcPlane, int srcStride, int srcW, int srcH,
                                    uint8_t* dstPlane, int dstStride, int dstX, int dstY,
                                    int blockW, int blockH, int mvX, int mvY) {
  int baseX = dstX + (mvX >> 1);
  int baseY = dstY + (mvY >> 1);
  int fracX = mvX & 1;
  int fracY = mvY & 1;

  for (int y = 0; y < blockH; y++) {
    uint8_t* dstRow = dstPlane + (size_t)(dstY + y) * dstStride + dstX;
    int sy = baseY + y;
    for (int x = 0; x < blockW; x++) {
      int val = predictedSample(srcPlane, srcStride, srcW, srcH, baseX + x, sy, fracX, fracY);
      dstRow[x] = (uint8_t)((dstRow[x] + val + 1) >> 1);
    }
  }
}

/**
 * Motion-compensates a whole macroblock (16x16 luma + two 8x8 chroma
 * blocks) at (mbX, mbY) from `ref` into `frame`, given a luma motion
 * vector in half-pel units. Chroma's motion vector is derived from the
 * luma one by clause 7.6.4's rule for 4:2:0: divide by 2 using C's
 * truncate-toward-zero `/` (*not* `>>1`, which floors - the two only
 * differ for odd negative values, but getting this wrong silently
 * misplaces chroma prediction by one sample near motion vectors that
 * trigger it). Called once per non-intra macroblock before any residual
 * is added (see decodePMacroblock(), mpg_macroblock.h) - for a
 * macroblock with no coded_block_pattern at all, this prediction *is*
 * the final reconstructed macroblock, unmodified. Single-direction only
 * (a P-picture's one forward reference); see predictMacroblockBi() for
 * Phase C's forward/backward/bidirectional B-picture case.
 */
template <typename Allocator>
inline void predictMacroblock(const Frame<Allocator>& ref, Frame<Allocator>& frame, int mbX,
                               int mbY, int mvX, int mvY) {
  predictBlockHalfPel(ref.y(), ref.strideY, ref.codedWidth, ref.codedHeight, frame.y(),
                       frame.strideY, mbX * 16, mbY * 16, 16, 16, mvX, mvY);

  int cmvX = mvX / 2;  // C truncating division, deliberately not `>>1` - see file comment
  int cmvY = mvY / 2;
  int cw = ref.codedWidth / 2;
  int ch = ref.codedHeight / 2;
  predictBlockHalfPel(ref.u(), ref.strideC, cw, ch, frame.u(), frame.strideC, mbX * 8, mbY * 8,
                       8, 8, cmvX, cmvY);
  predictBlockHalfPel(ref.v(), ref.strideC, cw, ch, frame.v(), frame.strideC, mbX * 8, mbY * 8,
                       8, 8, cmvX, cmvY);
}

/**
 * Motion-compensates a whole macroblock (Phase B, B-pictures) using
 * either or both of a forward (`refForward`, temporally past) and
 * backward (`refBackward`, temporally future) reference - clause 7.6.5's
 * three prediction cases. Exactly one of `refForward`/`refBackward` may
 * be null (forward-only or backward-only prediction, decodeMbTypeB()'s
 * "MB_BACK"/"MB_FOR"-only rows - see mpg_macroblock.h), but not both.
 * When both are given (bidirectional prediction), the forward direction
 * is written first (`predictBlockHalfPel()`, a direct "put") and the
 * backward direction is *averaged* on top (`predictBlockHalfPelAvg()`) -
 * see that function's comment for why this order/composition, not one
 * combined average, is what MPEG-1 actually specifies.
 */
template <typename Allocator>
inline void predictMacroblockBi(const Frame<Allocator>* refForward,
                                 const Frame<Allocator>* refBackward, Frame<Allocator>& frame,
                                 int mbX, int mbY, int mvFwdX, int mvFwdY, int mvBackX,
                                 int mvBackY) {
  bool wroteFirst = false;
  if (refForward) {
    predictBlockHalfPel(refForward->y(), refForward->strideY, refForward->codedWidth,
                         refForward->codedHeight, frame.y(), frame.strideY, mbX * 16, mbY * 16,
                         16, 16, mvFwdX, mvFwdY);
    int cmvX = mvFwdX / 2, cmvY = mvFwdY / 2;
    int cw = refForward->codedWidth / 2, ch = refForward->codedHeight / 2;
    predictBlockHalfPel(refForward->u(), refForward->strideC, cw, ch, frame.u(), frame.strideC,
                         mbX * 8, mbY * 8, 8, 8, cmvX, cmvY);
    predictBlockHalfPel(refForward->v(), refForward->strideC, cw, ch, frame.v(), frame.strideC,
                         mbX * 8, mbY * 8, 8, 8, cmvX, cmvY);
    wroteFirst = true;
  }
  if (refBackward) {
    int cmvX = mvBackX / 2, cmvY = mvBackY / 2;
    int cw = refBackward->codedWidth / 2, ch = refBackward->codedHeight / 2;
    if (wroteFirst) {
      predictBlockHalfPelAvg(refBackward->y(), refBackward->strideY, refBackward->codedWidth,
                              refBackward->codedHeight, frame.y(), frame.strideY, mbX * 16,
                              mbY * 16, 16, 16, mvBackX, mvBackY);
      predictBlockHalfPelAvg(refBackward->u(), refBackward->strideC, cw, ch, frame.u(),
                              frame.strideC, mbX * 8, mbY * 8, 8, 8, cmvX, cmvY);
      predictBlockHalfPelAvg(refBackward->v(), refBackward->strideC, cw, ch, frame.v(),
                              frame.strideC, mbX * 8, mbY * 8, 8, 8, cmvX, cmvY);
    } else {
      predictBlockHalfPel(refBackward->y(), refBackward->strideY, refBackward->codedWidth,
                           refBackward->codedHeight, frame.y(), frame.strideY, mbX * 16,
                           mbY * 16, 16, 16, mvBackX, mvBackY);
      predictBlockHalfPel(refBackward->u(), refBackward->strideC, cw, ch, frame.u(),
                           frame.strideC, mbX * 8, mbY * 8, 8, 8, cmvX, cmvY);
      predictBlockHalfPel(refBackward->v(), refBackward->strideC, cw, ch, frame.v(),
                           frame.strideC, mbX * 8, mbY * 8, 8, 8, cmvX, cmvY);
    }
  }
}

}  // namespace tinympg
