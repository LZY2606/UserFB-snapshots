#pragma once
#include <stdint.h>
#include "mpg_bitwriter.h"
#include "mpg_fdct.h"
#include "mpg_macroblock_encode.h"
#include "../decoder/mpg_idct.h"
#include "../common/mpg_frame.h"
#include "../common/mpg_tables.h"
#include "../common/mpg_vlc_tables.h"

/*
 * Header-only (template parts) plus a matching mpg_inter_encode.cpp
 * (non-template parts). Zero-motion P-picture encoding: the write-side
 * mirror of decoder/mpg_macroblock.h's decodePMacroblock()/
 * decodeInterBlock() P-picture path, for Encoder::setInterMode(true)'s
 * optional inter mode (see encoder/mpg_encoder.h).
 *
 * Scope is deliberately narrow (see docs/scope.md): no motion search -
 * every non-skipped inter macroblock predicts from the *same position* in
 * the most recently reconstructed picture (a zero displacement forward
 * vector), and no macroblock in a P-picture is ever intra-coded - a P
 * picture header is never put on intra macroblock syntax. A macroblock
 * whose six residual blocks all quantize to zero is either skipped via
 * macroblock_address_increment (the middle of a slice) or written as an
 * explicit zero-motion-vector, no-CBP macroblock (a slice's first/last
 * position, which mpg_encoder.h's slice loop keeps explicitly coded - see
 * that file's P-slice comment).
 *
 * Reconstruction discipline: the next picture's reference is always this
 * encoder's own *reconstructed* picture (dequantize -> IDCT -> clip, the
 * exact decoder path), never the caller's raw source - see
 * reconstructInterBlock()/encodeIntraBlockRecon() below, both of which
 * reuse decoder/mpg_idct.h's idct8x8()/clip255() unchanged.
 */

namespace tinympg {

/**
 * Reconstructs what decoder/mpg_macroblock.h's decodeInterBlock() would
 * dequantize a given (positive) inter level magnitude to, at this
 * qscale/matrixVal - the exact same formula, kInterAcShift (>> 4) and
 * oddification both included: dq = ((2*mag+1)*qscale*matrixVal) >> 4,
 * then (dq - 1) | 1. This also covers decodeInterBlock()'s special
 * first-coefficient (run=0, level=1) shortcut, whose (3*qscale*w) >> 4 is
 * just this formula at mag=1 (2*1+1 == 3) - one function serves both.
 */
int32_t dequantInterMagnitude(int32_t mag, int qscale, int32_t matrixVal);

/**
 * Forward-quantizes one non-intra residual coefficient into a signed
 * level, *simulating* decodeInterBlock()'s dequantization rather than
 * algebraically inverting it - the same neighborhood-search technique
 * mpg_macroblock_encode.h's quantizeAc() uses for intra AC coefficients
 * (see that file's top comment for why), just with the inter
 * (2*mag+1)-weighted, >>4 dequant shape above. Magnitude 0 ("don't code
 * this coefficient") is a candidate, and the search is clamped to
 * kMaxAcEscapeMagnitude so writeAcCoefficient() never has to clamp a
 * level the search believed was representable.
 */
int32_t quantizeInterCoef(int32_t coef, int qscale, int32_t matrixVal);

/**
 * Writes macroblock_address_increment (Table B.1) for a real increment of
 * `value` (>= 1): values 1..33 map straight to their table row, anything
 * larger is emitted as one or more macroblock_escape codes (each +33)
 * followed by the remaining 1..33 - the write-side inverse of
 * decodeMbAddrIncrement()'s escape-absorbing loop.
 */
void writeMbAddrIncrement(BitWriter& bw, int value);

/**
 * Writes one coded non-intra block's coefficients (the block must have at
 * least one nonzero level - all-zero blocks aren't coded at all, they're
 * absent from the macroblock's coded_block_pattern). `levels` is raster
 * order, indexed by kZigzag for the scan. The very first coefficient gets
 * decodeInterBlock()'s '1'+sign shortcut exactly when it sits at scan
 * position 0 with magnitude 1; everything else goes through the shared
 * writeAcCoefficient() run/level path, and the block always ends with
 * end-of-block (a coded inter block always carries one - only an
 * un-coded, CBP-absent block has no syntax at all).
 */
void writeInterBlock(BitWriter& bw, const int32_t levels[64]);

/**
 * Writes the "explicit forward motion vector, no coded_block_pattern"
 * P-picture macroblock_type (Table B.2 row 2, '001') followed by a (0,0)
 * motion vector: with forward_f_code = 1 (the picture header this encoder
 * writes), a zero motion_code for each component ('1', Table B.10 row 0)
 * decodes straight to the reset (0,0) predictor - a legal, explicitly
 * zero-motion, residual-free macroblock. Used wherever a slice position
 * must be coded explicitly but has no residual (see mpg_encoder.h's
 * first/last-macroblock rule), since MPEG-1's P table has no "zero MV,
 * no CBP" row of its own.
 */
void writeInterMotionOnlyMacroblock(BitWriter& bw);

/// Placement offsets shared with the decoder's kBlockOffX/kBlockOffY.
static const int kInterBlockOffX[6] = {0, 8, 0, 8, 0, 0};
static const int kInterBlockOffY[6] = {0, 0, 8, 8, 0, 0};

/**
 * Copies one macroblock's six blocks from `ref` into `dst` - the
 * zero-displacement prediction (predictMacroblock() with mv (0,0) in the
 * decoder is a plain copy, no half-pel interpolation path reachable).
 * Used for skipped macroblocks, no-CBP motion-only macroblocks, and as
 * the prediction base every residual is added on top of.
 */
template <typename Allocator>
inline void copyReferenceMacroblock(const Frame<Allocator>& ref, Frame<Allocator>& dst, int mbX,
                                     int mbY) {
  for (int b = 0; b < 6; b++) {
    const uint8_t* srcPlane;
    int srcStride, baseX, baseY;
    if (b < 4) {
      srcPlane = ref.y();
      srcStride = ref.strideY;
      baseX = mbX * 16 + kInterBlockOffX[b];
      baseY = mbY * 16 + kInterBlockOffY[b];
    } else {
      srcPlane = (b == 4) ? ref.u() : ref.v();
      srcStride = ref.strideC;
      baseX = mbX * 8;
      baseY = mbY * 8;
    }
    uint8_t* outPlane = (b < 4) ? dst.y() : ((b == 4) ? dst.u() : dst.v());
    int outStride = (b < 4) ? dst.strideY : dst.strideC;
    for (int y = 0; y < 8; y++) {
      const uint8_t* srcRow = srcPlane + (size_t)(baseY + y) * srcStride + baseX;
      uint8_t* dstRow = outPlane + (size_t)(baseY + y) * outStride + baseX;
      for (int x = 0; x < 8; x++) dstRow[x] = srcRow[x];
    }
  }
}

/**
 * Gathers one block's source samples and forward-transforms/quantizes the
 * signed residual against the reference block at the same position,
 * filling `levels` (raster order, all zeros when this block has no
 * coded residual). Non-template (plain pointers/strides) - defined in
 * mpg_inter_encode.cpp. Returns true if at least one level is nonzero.
 */
bool encodeInterBlockLevels(const uint8_t* srcPlane, int srcStride, const uint8_t* refPlane,
                             int refStride, int baseX, int baseY, int qscale, int32_t levels[64]);

/**
 * Adds one coded inter block's reconstructed residual on top of the
 * prediction already copied into `recon` - the exact mirror of
 * decoder/mpg_macroblock.h's addInterResidual() for a single block:
 * dequantize with dequantInterMagnitude(), idct8x8(), then clip255()
 * around prediction + residual, sample by sample.
 */
template <typename Allocator>
inline void reconstructInterBlock(Frame<Allocator>& recon, int mbX, int mbY, int block,
                                   const int32_t levels[64], int qscale) {
  int32_t coef[64] = {0};
  for (int zz = 0; zz < 64; zz++) {
    int32_t level = levels[kZigzag[zz]];  // levels is raster order, same as writeInterBlock()
    if (level == 0) continue;
    int j = kZigzag[zz];
    int32_t mag = level < 0 ? -level : level;
    int32_t dq = dequantInterMagnitude(mag, qscale, (int32_t)kDefaultNonIntraMatrix[j]);
    coef[j] = level < 0 ? -dq : dq;
  }
  int32_t residual[64];
  idct8x8(coef, residual);

  uint8_t* plane;
  int stride, baseX, baseY;
  if (block < 4) {
    plane = recon.y();
    stride = recon.strideY;
    baseX = mbX * 16 + kInterBlockOffX[block];
    baseY = mbY * 16 + kInterBlockOffY[block];
  } else {
    plane = (block == 4) ? recon.u() : recon.v();
    stride = recon.strideC;
    baseX = mbX * 8;
    baseY = mbY * 8;
  }
  for (int y = 0; y < 8; y++) {
    uint8_t* row = plane + (size_t)(baseY + y) * stride + baseX;
    const int32_t* resRow = residual + y * 8;
    for (int x = 0; x < 8; x++) row[x] = clip255((int32_t)row[x] + resRow[x]);
  }
}

/**
 * Computes one zero-motion inter macroblock at (mbX, mbY) without writing
 * any bitstream syntax: fills the six blocks' residual levels and the
 * resulting CBP, copies the zero-MV prediction into `recon`, and - for
 * every CBP-selected block - adds its decoder-identical reconstructed
 * residual on top (so `recon` is complete even for positions the slice
 * loop later skips via address increment, since this picture itself
 * becomes the next one's reference). Returns the 6-bit CBP (0 = no
 * residual). `levelsOut` (caller-owned, 6x64, raster order per block)
 * keeps the levels for writeInterMacroblockSyntax() once the caller has
 * written the macroblock_address_increment that must precede them.
 */
template <typename Allocator>
inline int computeInterMacroblock(const uint8_t* srcY, int srcStrideY, const uint8_t* srcU,
                                   const uint8_t* srcV, int srcStrideC, const Frame<Allocator>& ref,
                                   Frame<Allocator>& recon, int mbX, int mbY, int qscale,
                                   int32_t levelsOut[6][64]) {
  int cbp = 0;
  for (int b = 0; b < 6; b++) {
    const uint8_t* srcPlane;
    const uint8_t* refPlane;
    int srcStride, refStride, baseX, baseY;
    if (b < 4) {
      srcPlane = srcY;
      srcStride = srcStrideY;
      refPlane = ref.y();
      refStride = ref.strideY;
      baseX = mbX * 16 + kInterBlockOffX[b];
      baseY = mbY * 16 + kInterBlockOffY[b];
    } else {
      srcPlane = (b == 4) ? srcU : srcV;
      srcStride = srcStrideC;
      refPlane = (b == 4) ? ref.u() : ref.v();
      refStride = ref.strideC;
      baseX = mbX * 8;
      baseY = mbY * 8;
    }
    if (encodeInterBlockLevels(srcPlane, srcStride, refPlane, refStride, baseX, baseY, qscale,
                                levelsOut[b])) {
      cbp |= 1 << (5 - b);
    }
  }

  // Prediction first, for every block - the same ordering decodePMacroblock()
  // uses (predictMacroblock(), then addInterResidual() per CBP bit).
  copyReferenceMacroblock(ref, recon, mbX, mbY);
  for (int b = 0; b < 6; b++) {
    if (cbp & (1 << (5 - b))) reconstructInterBlock(recon, mbX, mbY, b, levelsOut[b], qscale);
  }
  return cbp;
}

/**
 * Writes one explicit, residual-carrying inter macroblock's syntax -
 * macroblock_type (Table B.2 row 1: zero motion vector, CBP present),
 * coded_block_pattern, and the CBP-selected blocks - after the caller's
 * macroblock_address_increment, the order clause 2.4.3.7 requires (this is
 * why computeInterMacroblock() and the syntax write are separate
 * functions). Only call for a nonzero `cbp`; a zero-CBP explicit position
 * uses writeInterMotionOnlyMacroblock() instead.
 */
inline void writeInterMacroblockSyntax(BitWriter& bw, int cbp,
                                        const int32_t levelsOut[6][64]) {
  bw.writeVlc(kMbTypePCode[1], kMbTypePBits[1]);  // zero MV, CBP present ('01')
  bw.writeVlc(kCbpCode[cbp], kCbpBits[cbp]);
  for (int b = 0; b < 6; b++) {
    if (cbp & (1 << (5 - b))) writeInterBlock(bw, levelsOut[b]);
  }
}

/**
 * Forward-transforms, quantizes, and VLC-writes one intra 8x8 block (same
 * bitstream syntax as mpg_macroblock_encode.h's encodeIntraBlock()), while
 * also producing the block's decoder-identical reconstruction into
 * `recon` - dequantizing the *selected* levels through
 * decodeIntraBlock()'s exact formulas (dcLevel*matrix[0] for DC,
 * dequantAcMagnitude() for AC), idct8x8(), clip255(). Used for the
 * inter-mode I-picture, whose reconstruction becomes the first P
 * picture's reference.
 */
template <typename Allocator>
inline void encodeIntraBlockRecon(BitWriter& bw, int blockIndex, int32_t lastDc[3],
                                   const uint16_t* quantMatrix, int qscale,
                                   const int32_t pixels[64], Frame<Allocator>& recon, int mbX,
                                   int mbY) {
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

  int32_t deq[64] = {0};
  deq[0] = dcLevel * (int32_t)quantMatrix[0];

  int lastCoded = 0;
  for (int zz = 1; zz < 64; zz++) {
    int j = kZigzag[zz];
    int32_t level = quantizeAc(coef[j], qscale, (int32_t)quantMatrix[j]);
    if (level == 0) continue;
    int run = zz - lastCoded - 1;
    writeAcCoefficient(bw, run, level);
    lastCoded = zz;
    int32_t mag = level < 0 ? -level : level;
    int32_t dq = dequantAcMagnitude(mag, qscale, (int32_t)quantMatrix[j]);
    deq[j] = level < 0 ? -dq : dq;
  }
  bw.writeVlc(kAcCode[kAcEobIndex], kAcBits[kAcEobIndex]);

  int32_t spatial[64];
  idct8x8(deq, spatial);

  uint8_t* plane;
  int stride, baseX, baseY;
  if (blockIndex < 4) {
    plane = recon.y();
    stride = recon.strideY;
    baseX = mbX * 16 + kInterBlockOffX[blockIndex];
    baseY = mbY * 16 + kInterBlockOffY[blockIndex];
  } else {
    plane = (blockIndex == 4) ? recon.u() : recon.v();
    stride = recon.strideC;
    baseX = mbX * 8;
    baseY = mbY * 8;
  }
  for (int y = 0; y < 8; y++) {
    uint8_t* dstRow = plane + (size_t)(baseY + y) * stride + baseX;
    const int32_t* srcRow = spatial + y * 8;
    for (int x = 0; x < 8; x++) dstRow[x] = clip255(srcRow[x]);
  }
}

/**
 * Encodes all 6 blocks of one intra macroblock at (mbX, mbY) from the
 * caller's source planes, writing the same block syntax as
 * encodeIntraMacroblock() but reconstructing into `recon` as it goes (see
 * encodeIntraBlockRecon()). Like the original, does not write
 * macroblock_type/macroblock_address_increment - the caller's slice loop
 * owns those.
 */
template <typename Allocator>
inline void encodeIntraMacroblockRecon(BitWriter& bw, const uint8_t* srcY, int strideY,
                                        const uint8_t* srcU, const uint8_t* srcV, int strideC,
                                        int mbX, int mbY, int32_t lastDc[3],
                                        const uint16_t* quantMatrix, int qscale,
                                        Frame<Allocator>& recon) {
  int32_t pixels[64];
  for (int b = 0; b < 6; b++) {
    const uint8_t* plane;
    int stride, baseX, baseY;
    if (b < 4) {
      plane = srcY;
      stride = strideY;
      baseX = mbX * 16 + kInterBlockOffX[b];
      baseY = mbY * 16 + kInterBlockOffY[b];
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
    encodeIntraBlockRecon(bw, b, lastDc, quantMatrix, qscale, pixels, recon, mbX, mbY);
  }
}

}  // namespace tinympg
