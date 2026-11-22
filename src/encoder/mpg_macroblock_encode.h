#pragma once
#include <stdint.h>
#include <cmath>
#include <cstdlib>
#include "mpg_bitwriter.h"
#include "mpg_fdct.h"
#include "../common/mpg_tables.h"
#include "../common/mpg_vlc_tables.h"

/*
 * Header-only. Intra macroblock encode: the write-side mirror of
 * decoder/mpg_macroblock.h's decodeIntraMacroblock()/decodeIntraBlock().
 * Forward DCT (mpg_fdct.h) -> forward quantization -> differential DC /
 * run-length AC VLC write, all six blocks unconditionally (MPEG-1 intra
 * macroblocks have no coded_block_pattern gating - see
 * decodeIntraMacroblock()'s own comment: every block is always coded).
 *
 * Forward quantization is *not* a plain algebraic inverse of
 * decodeIntraBlock()'s dequantization formulas - an earlier version of
 * this file tried that (round-to-nearest of `coef * 8 / (qscale *
 * matrixVal)`) and it measurably under-performed: real-decode PSNR came
 * out far worse than the quantizer_scale used should produce, caught by
 * this project's own correctness oracle (docs/testing.md) diffing
 * encoded-then-decoded output against the original source, the same way
 * every decode-side dequant shift was calibrated. The reason: decode's AC
 * dequant is `dq = ((level*qscale*matrixVal) >> 3 - 1) | 1` - an
 * *arithmetic right shift* (which floors, not rounds, and floors toward
 * -infinity for a negative product) *and* a forced-odd "oddification"
 * step (clause 7.4.4's mismatch control) applied on top - neither of
 * which a simple algebraic inverse accounts for, so naively-rounded
 * levels were systematically reconstructing to the *wrong* dequantized
 * value, not just an imprecise one. quantizeAc() below instead searches a
 * small neighborhood of candidate magnitudes and picks whichever one,
 * run through the *exact* same dequant formula decodeIntraBlock() uses,
 * lands closest to the true coefficient - simulating the decoder rather
 * than inverting its formula, a standard technique real encoders use
 * precisely because vector quantization steps like this one aren't
 * always cleanly invertible. DC has no such quirk (`coef[0] = dcLevel *
 * matrix[0]` is a plain linear scale, no shift or oddification), so
 * quantizeDc() stays a plain round-to-nearest.
 */

namespace tinympg {

/**
 * Forward-quantizes a DC coefficient (raw fdct8x8() output) into a DC
 * level - the inverse of decodeIntraBlock()'s `coef[0] = dcLevel *
 * matrix0`. Round-to-nearest; `matrix0` is `quantMatrix[0]` (the intra
 * matrix's DC entry, 8 by default - clause 2.4.4.4).
 */
inline int32_t quantizeDc(int32_t coef, int32_t matrix0) {
  if (matrix0 <= 0) matrix0 = 8;
  return (int32_t)std::lround((float)coef / (float)matrix0);
}

/**
 * The largest AC coefficient *magnitude* MPEG-1's escape coding
 * (writeAcEscapeLevel() below) can actually represent - a real format
 * limit, not a tuning knob: the escape mechanism's two-tier 8-bit/16-bit
 * scheme (mirroring decodeIntraBlock()'s escape decode exactly - see
 * writeAcEscapeLevel()'s own comment) tops out at 255 in either
 * direction. quantizeAc() must never search past this, or it can pick a
 * magnitude that *looks* optimal in isolation but silently gets clamped
 * down to something else entirely at write time - exactly the bug this
 * constant's introduction fixed: an early version of quantizeAc()
 * clamped nothing, occasionally chose a magnitude in the high hundreds
 * for a large real coefficient (a sharp edge in real video content
 * easily produces one), and writeAcEscapeLevel()'s own clamp then wrote
 * something dequantizing to a *wildly* different value than the one
 * quantizeAc() had actually optimized for and believed it was encoding -
 * caught by this file's companion test (test/native/test_encode_iframe.cpp)
 * diffing real decoded output against the source, not by inspection.
 */
static const int32_t kMaxAcEscapeMagnitude = 255;

/**
 * Reconstructs what decodeIntraBlock() would dequantize a given
 * (positive) magnitude to, at this `qscale`/`matrixVal` - the *exact*
 * same formula, floor-shift and oddification both included: `dq =
 * ((mag*qscale*matrixVal) >> 3 - 1) | 1`. `mag` must be >= 1 (magnitude
 * 0 means "no coefficient", handled separately by quantizeAc() - this
 * formula is only ever exercised for a real, coded coefficient on the
 * decode side).
 */
inline int32_t dequantAcMagnitude(int32_t mag, int qscale, int32_t matrixVal) {
  int32_t dq = (mag * qscale * matrixVal) >> 3;
  return (dq - 1) | 1;
}

/**
 * Forward-quantizes an AC coefficient (raw fdct8x8() output) into a
 * signed level, by *simulating* decodeIntraBlock()'s dequantization
 * (dequantAcMagnitude() above) rather than algebraically inverting it -
 * see this file's top comment for why that distinction matters here.
 * Starts from the algebraic estimate (round-to-nearest of `coef * 8 /
 * (qscale*matrixVal)`, ignoring the floor/oddify quirks), clamped to
 * [1, kMaxAcEscapeMagnitude] *before* searching (see that constant's own
 * comment for why this has to happen before, not after, picking a
 * candidate), then checks every magnitude within +/-2 of that clamped
 * center (also kept within the same legal range) - including magnitude 0
 * itself, i.e. "don't code this coefficient at all" - and returns
 * whichever candidate's *simulated* dequantized value lands closest to
 * the true `coef` (ties broken toward the smaller magnitude, favoring
 * the more-compressible choice when it's just as accurate). A radius of
 * 2 is enough to cover the local search space dequantAcMagnitude()'s
 * floor+oddify nonlinearity can shift the true optimum by - verified
 * empirically (not just assumed) by this file's companion test
 * (test/native/test_encode_iframe.cpp) diffing real decoded output
 * against the source, the same oracle every dequant shift in this
 * codebase has been calibrated against.
 */
inline int32_t quantizeAc(int32_t coef, int qscale, int32_t matrixVal) {
  if (qscale <= 0) qscale = 1;
  if (matrixVal <= 0) matrixVal = 1;
  int32_t estimate =
      (int32_t)std::lround((float)std::abs(coef) * 8.0f / ((float)qscale * (float)matrixVal));
  if (estimate > kMaxAcEscapeMagnitude) estimate = kMaxAcEscapeMagnitude;

  int32_t bestMag = 0;
  int32_t bestErr = std::abs(coef);  // magnitude 0 -> dequantizes to 0
  for (int32_t mag = estimate - 2; mag <= estimate + 2; mag++) {
    if (mag <= 0 || mag > kMaxAcEscapeMagnitude) continue;
    int32_t dq = dequantAcMagnitude(mag, qscale, matrixVal);
    int32_t err = std::abs(std::abs(coef) - dq);
    if (err < bestErr || (err == bestErr && mag < bestMag)) {
      bestErr = err;
      bestMag = mag;
    }
  }
  return coef < 0 ? -bestMag : bestMag;
}

/**
 * dct_dc_size (0..11): the number of bits needed to represent |diff| in
 * the "half-range" convention decodeIntraBlock()'s readDcDifferential()
 * decodes - the smallest size such that 2^(size-1) <= |diff| <= 2^size-1,
 * or 0 for diff == 0.
 */
inline int dcDiffSize(int32_t diff) {
  if (diff == 0) return 0;
  uint32_t a = diff < 0 ? (uint32_t)(-diff) : (uint32_t)diff;
  int size = 0;
  while (a > 0) {
    a >>= 1;
    size++;
  }
  return size;
}

/**
 * Writes dct_dc_differential's `size` info bits for `diff` - the exact
 * inverse of decoder/mpg_macroblock.h's readDcDifferential(): a positive
 * diff is written as-is; a negative diff is written as `diff + 2^size -
 * 1` (which readDcDifferential() maps back to `v - (2^size-1)` = diff).
 * No bits are written for size == 0 (diff == 0), matching decode.
 */
inline void writeDcDifferential(BitWriter& bw, int32_t diff, int size) {
  if (size == 0) return;
  uint32_t v = diff >= 0 ? (uint32_t)diff
                         : (uint32_t)(diff + (int32_t)(1u << size) - 1);
  bw.u(v, size);
}

/**
 * Linear search for a (run, level) pair in the AC run/level VLC table
 * (kAcRun/kAcLevel, common/mpg_vlc_tables.h) - the write-side counterpart
 * of decode's table walk, but in reverse: decode matches bits to a table
 * row; encode has to find which row (if any) matches a known symbol.
 * `level` here is always the *positive* magnitude, matching kAcLevel[]'s
 * own convention (sign is a separate bit). Returns -1 if no table entry
 * matches (encodeAcCoefficient() falls back to the escape code in that
 * case) - not the fastest possible approach (up to kAcEscapeIndex
 * comparisons per coefficient), but this table is 111 entries and this
 * runs once per *coded* (nonzero) coefficient, not once per sample - left
 * as a future optimization the same way decode's own readVlc() linear
 * scan was (see mpg_vlc_tables.h's file comment).
 */
inline int findAcVlcIndex(int run, int level) {
  for (int i = 0; i < kAcEscapeIndex; i++) {
    if (kAcRun[i] == run && kAcLevel[i] == level) return i;
  }
  return -1;
}

/**
 * Writes one AC coefficient's escape-coded level (clause 2.4.3.7's escape
 * path within Table B.14) - the exact inverse of decodeIntraBlock()'s
 * escape-level decode: a signed byte directly represents levels in
 * [-127,-1] U [1,127]; level 128..255 is written as a 0x00 trigger byte
 * followed by the level as a plain unsigned byte; level -128..-255 is
 * written as a 0x80 trigger byte followed by (level + 256) - matching
 * decode's `lvl == 0` / `lvl == -128` extension triggers exactly. Levels
 * outside +/-kMaxAcEscapeMagnitude are clamped defensively rather than
 * producing a non-round-trippable bitstream - quantizeAc() is the real
 * line of defense (it never *selects* a magnitude outside that range in
 * the first place, see that constant's own comment for why the clamp has
 * to live there, not just here), so reaching this clamp at all would
 * mean a caller fed writeAcCoefficient() a level from somewhere else.
 */
inline void writeAcEscapeLevel(BitWriter& bw, int32_t level) {
  if (level > kMaxAcEscapeMagnitude) level = kMaxAcEscapeMagnitude;
  if (level < -kMaxAcEscapeMagnitude) level = -kMaxAcEscapeMagnitude;
  if (level > 0) {
    if (level < 128) {
      bw.u((uint32_t)level, 8);
    } else {
      bw.u(0, 8);
      bw.u((uint32_t)level, 8);
    }
  } else {  // level < 0 - never 0, only genuinely nonzero coefficients reach here
    if (level > -128) {
      bw.u((uint32_t)(256 + level), 8);
    } else {
      bw.u(0x80, 8);
      bw.u((uint32_t)(level + 256), 8);
    }
  }
}

/**
 * Writes one nonzero AC coefficient: `run` zero coefficients preceded it
 * since the last coded one (0 if immediately adjacent - the same
 * "zero-gap count" convention kAcRun[]'s own entries use, *not* an
 * already-incremented "advance amount" - see decodeIntraBlock()'s `i +=
 * run` for where that +1 gets applied on the decode side), `level` is
 * signed (magnitude + sign folded together). Prefers the table's own
 * (code, bits) pair when the (run, level) pair has one; falls back to the
 * escape code otherwise. The escape path's raw 6-bit run field is `run`
 * itself, not `run - 1` - decodeIntraBlock()'s escape decode does `run =
 * br.u(6) + 1` and then `i += run`, so encode has to write exactly the
 * value that, plus one, gives the intended advance - getting this
 * off-by-one wrong silently shifts every subsequent coefficient in the
 * block.
 */
inline void writeAcCoefficient(BitWriter& bw, int run, int32_t level) {
  int mag = level < 0 ? -level : level;
  int idx = findAcVlcIndex(run, mag);
  if (idx >= 0) {
    bw.writeVlc(kAcCode[idx], kAcBits[idx]);
    bw.flag(level < 0);
    return;
  }
  bw.writeVlc(kAcCode[kAcEscapeIndex], kAcBits[kAcEscapeIndex]);
  bw.u((uint32_t)run, 6);
  writeAcEscapeLevel(bw, level);
}

/**
 * Forward-transforms, quantizes, and VLC-writes one intra 8x8 block -
 * DC term (differential against `lastDc[component]`, updated in place)
 * plus AC run/level terms in zigzag order, terminated by end-of-block.
 * `blockIndex` (0..5) selects the DC predictor component the same way
 * decodeIntraBlock() does (0..3 luma share component 0, 4 is Cb, 5 is
 * Cr). `pixels` is the raw 8x8 sample block (raster order, absolute
 * 0..255 sample values - see mpg_fdct.h's file comment for why no -128
 * centering). `quantMatrix` is the active intra quantizer matrix (raster
 * order, SequenceInfo::intraMatrix on the decode side).
 */
inline void encodeIntraBlock(BitWriter& bw, int blockIndex, int32_t lastDc[3],
                              const uint16_t* quantMatrix, int qscale,
                              const int32_t pixels[64]) {
  int32_t coef[64];
  fdct8x8(pixels, coef);

  int component = blockIndex < 4 ? 0 : blockIndex - 3;  // 0=Y, 1=Cb, 2=Cr
  const uint16_t* dcCode = component == 0 ? kDcLumCode : kDcChromaCode;
  const uint8_t* dcBits = component == 0 ? kDcLumBits : kDcChromaBits;

  int32_t dcLevel = quantizeDc(coef[0], (int32_t)quantMatrix[0]);
  int32_t diff = dcLevel - lastDc[component];
  lastDc[component] = dcLevel;
  int size = dcDiffSize(diff);
  bw.writeVlc(dcCode[size], dcBits[size]);
  writeDcDifferential(bw, diff, size);

  int lastCoded = 0;  // zigzag index of the last coefficient actually written (0 = DC)
  for (int zz = 1; zz < 64; zz++) {
    int j = kZigzag[zz];
    int32_t level = quantizeAc(coef[j], qscale, (int32_t)quantMatrix[j]);
    if (level == 0) continue;
    int run = zz - lastCoded - 1;
    writeAcCoefficient(bw, run, level);
    lastCoded = zz;
  }
  bw.writeVlc(kAcCode[kAcEobIndex], kAcBits[kAcEobIndex]);
}

/**
 * Placement offsets shared with the decoder's own kBlockOffX/kBlockOffY
 * (decoder/mpg_macroblock.h) - block order 0..3 are the luma quadrants
 * (top-left, top-right, bottom-left, bottom-right), 4 is Cb, 5 is Cr.
 */
static const int kEncBlockOffX[6] = {0, 8, 0, 8, 0, 0};
static const int kEncBlockOffY[6] = {0, 0, 8, 8, 0, 0};

/**
 * Encodes all 6 blocks of one intra macroblock at (mbX, mbY), reading
 * source samples directly from the caller's own Y/U/V planes (no
 * intermediate Frame/picture buffer - see docs/scope.md's Encoder
 * section: an intra-only encoder has no reconstruction to keep, since
 * MPEG-1 intra has no spatial prediction for a later macroblock to depend
 * on, only the differential DC predictor this function threads through
 * `lastDc`). Does *not* write macroblock_type/macroblock_address_increment
 * - that's the caller's job (encoder/mpg_encoder.h's slice loop), mirroring
 * decodeIntraMacroblock()'s own scope (block-layer only).
 */
inline void encodeIntraMacroblock(BitWriter& bw, const uint8_t* srcY, int strideY,
                                   const uint8_t* srcU, const uint8_t* srcV, int strideC,
                                   int mbX, int mbY, int32_t lastDc[3],
                                   const uint16_t* quantMatrix, int qscale) {
  int32_t pixels[64];
  for (int b = 0; b < 6; b++) {
    const uint8_t* plane;
    int stride, baseX, baseY;
    if (b < 4) {
      plane = srcY;
      stride = strideY;
      baseX = mbX * 16 + kEncBlockOffX[b];
      baseY = mbY * 16 + kEncBlockOffY[b];
    } else {
      plane = (b == 4) ? srcU : srcV;
      stride = strideC;
      baseX = mbX * 8;
      baseY = mbY * 8;
    }
    for (int y = 0; y < 8; y++) {
      const uint8_t* srcRow = plane + (size_t)(baseY + y) * stride + baseX;
      int32_t* dstRow = pixels + y * 8;
      for (int x = 0; x < 8; x++) dstRow[x] = srcRow[x];
    }
    encodeIntraBlock(bw, b, lastDc, quantMatrix, qscale, pixels);
  }
}

}  // namespace tinympg
