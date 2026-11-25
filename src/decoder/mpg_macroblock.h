#pragma once
#include <stdint.h>
#include <string.h>
#include "mpg_bitreader.h"
#include "mpg_idct.h"
#include "mpg_motion.h"
#include "../common/mpg_frame.h"
#include "../common/mpg_tables.h"
#include "../common/mpg_vlc_tables.h"

/*
 * Header-only. macroblock_address_increment, macroblock_type (the
 * two-code I-picture case, Table B.2's 7-row P-picture case, and its
 * 11-row B-picture case - see docs/scope.md), coded_block_pattern, and
 * block-layer (DCT coefficient) parsing and reconstruction for intra and
 * non-intra (inter, Phase B/C) macroblocks, ISO/IEC 11172-2 clause
 * 2.4.3.7/Annex B. decodeInterBlock() is shared unchanged by P- and
 * B-pictures alike - a non-intra block's residual coding doesn't
 * distinguish which reference(s) produced its prediction.
 *
 * The dequantization arithmetic in decodeIntraBlock() and
 * decodeInterBlock() follows the shape of FFmpeg's
 * ff_mpeg1_decode_block_intra()/mpeg1_decode_block_inter()
 * (libavcodec/mpeg12.c, mpeg12dec.c) - the run/level table walk, the
 * "(level-1)|1" oddification/mismatch-control step, and the two
 * escape-level special cases for a raw byte of 0x00/0x80 - but the AC
 * dequant shifts were determined *empirically* against this project's
 * actual correctness oracle (see docs/testing.md) rather than trusted as
 * literally read from those functions' current source: decodeIntraBlock()
 * needs `>> 3`, not the `>> 4` that function's current source reads,
 * confirmed by checking a real `ffmpeg`-encoded stream's macroblock_type/
 * quantizer_scale bits and table lookups against `ffmpeg -debug 64`'s own
 * internal per-block coefficient dump across ten independent nonzero AC
 * coefficients (test/native/test_decode_iframe.cpp). kInterAcShift below
 * was calibrated the same way against a real P-picture stream
 * (test/native/test_decode_pframe.cpp), then separately re-confirmed
 * (unchanged) against a real bidirectionally-predicted B-picture
 * macroblock's coefficients (test/native/test_decode_bframe.cpp) - the
 * same shift holds for both, as expected given decodeInterBlock() itself
 * doesn't change between them.
 */

namespace tinympg {

/**
 * Shift used by decodeInterBlock()'s dequantization formula, shared by
 * P- and B-picture inter blocks alike - see this file's top comment for
 * how this was determined (empirically, against real P- and B-picture
 * bitstreams decoded by `ffmpeg -debug 64`), the same way
 * decodeIntraBlock()'s `>> 3` was.
 */
static const int kInterAcShift = 4;

/// Result of decodeMbAddrIncrement() - see kMbAddrIncCode's file comment (mpg_vlc_tables.h) for what each case means.
struct MbAddrIncrement {
  int value = 0;       ///< macroblock_address_increment - 1 (0 = "no gap, next macroblock is immediately coded")
  bool endOfSlice = false;  ///< matched the all-zero "end of slice" guard code
  bool error = false;
};

/**
 * Decodes one macroblock_address_increment, transparently absorbing any
 * macroblock_escape codes (each worth +33) and macroblock_stuffing codes
 * (no-op) that precede the real value, per clause 2.4.3.7's
 * `while (macroblock_escape) ...` / `while (macroblock_stuffing) ...`
 * loops. Used both for the first macroblock of a slice (where a value > 0
 * means the slice doesn't start at column 0) and, in an I-picture, for
 * every subsequent macroblock (where the value must always be exactly 0 -
 * any skip is illegal in an I-picture, clause 7.2.1 - the caller is
 * responsible for rejecting a nonzero value there).
 */
inline MbAddrIncrement decodeMbAddrIncrement(BitReader& br) {
  MbAddrIncrement r;
  int total = 0;
  for (;;) {
    int idx = br.readVlc(kMbAddrIncCode, kMbAddrIncBits, kMbAddrIncTableSize, kMbAddrIncMaxBits);
    if (idx < 0) {
      r.error = true;
      return r;
    }
    if (idx == kMbAddrIncEndIndex) {
      r.endOfSlice = true;
      return r;
    }
    if (idx == kMbAddrIncStuffingIndex) continue;
    if (idx == kMbAddrIncEscapeIndex) {
      total += 33;
      continue;
    }
    total += idx;  // idx == real value - 1
    break;
  }
  r.value = total;
  return r;
}

/// Result of decodeMbTypeIntra() - Table B.2, I-picture case ('1' or '01'; '00' is illegal).
struct MbTypeIntra {
  bool quant = false;  ///< true if a new quantizer_scale follows (macroblock_type == '01')
  bool error = false;
};

inline MbTypeIntra decodeMbTypeIntra(BitReader& br) {
  MbTypeIntra r;
  if (br.flag()) return r;       // '1': intra, no quant change
  if (br.flag()) {               // '01': intra, quant change follows
    r.quant = true;
    return r;
  }
  r.error = true;                // '00': not a valid I-picture macroblock_type
  return r;
}

/**
 * dct_dc_differential decode (clause 2.4.3.7's differential DC coding):
 * given a dct_dc_size (0..11, already VLC-decoded), reads that many raw
 * bits and maps them to a signed differential per the standard
 * (JPEG-style) "half-range" convention: values in the lower half of the
 * `size`-bit range are negative. size == 0 always means a zero
 * differential (no bits follow).
 */
inline int32_t readDcDifferential(BitReader& br, int size) {
  if (size == 0) return 0;
  uint32_t v = br.u((int)size);
  uint32_t half = 1u << (size - 1);
  return (v < half) ? (int32_t)v - (int32_t)((1u << size) - 1) : (int32_t)v;
}

/**
 * Decodes and dequantizes one intra 8x8 block's DCT coefficients (DC term
 * plus AC run/level-coded terms up to end-of-block), clause 2.4.3.7 /
 * Annex B. `blockIndex` (0..5, the standard 4:2:0 macroblock block order -
 * 0..3 are the four luma quadrants, 4 is Cb, 5 is Cr) selects which of
 * `lastDc`'s three per-component DC predictors to use and update -
 * MPEG-1's DC prediction is carried differentially across intra
 * macroblocks in coding order (clause 2.4.3.7), reset to 128 at the start
 * of each slice by the caller (see mpg_decoder.h::decodeSlice()).
 * `quantMatrix` is the active (custom or default) intra quantizer matrix
 * in raster order (SequenceInfo::intraMatrix). `coef` (raster order,
 * zero-initialized here) receives the dequantized coefficients, ready for
 * idct8x8(). Returns false (bitstream error, or a run that would overflow
 * the 8x8 block) without producing a usable `coef`.
 */
inline bool decodeIntraBlock(BitReader& br, int blockIndex, int32_t lastDc[3],
                              const uint16_t* quantMatrix, int qscale,
                              int32_t coef[64]) {
  memset(coef, 0, 64 * sizeof(int32_t));

  int component = blockIndex < 4 ? 0 : blockIndex - 3;  // 0=Y, 1=Cb, 2=Cr
  const uint16_t* dcCode = component == 0 ? kDcLumCode : kDcChromaCode;
  const uint8_t* dcBits = component == 0 ? kDcLumBits : kDcChromaBits;
  int dcMaxBits = component == 0 ? kDcLumMaxBits : kDcChromaMaxBits;

  int size = br.readVlc(dcCode, dcBits, 12, dcMaxBits);
  if (size < 0) return false;
  int32_t diff = readDcDifferential(br, size);
  lastDc[component] += diff;
  coef[0] = lastDc[component] * (int32_t)quantMatrix[0];

  int i = 0;  // raster-scan position accumulator; DC already occupies position 0
  for (;;) {
    int idx = br.readVlc(kAcCode, kAcBits, kAcTableSize, kAcMaxBits);
    if (idx < 0) return false;
    if (idx == kAcEobIndex) break;

    int run, level;
    bool negative;
    if (idx == kAcEscapeIndex) {
      run = (int)br.u(6) + 1;
      int32_t lvl = (int32_t)br.u(8);
      if (lvl >= 128) lvl -= 256;  // interpret as signed 8-bit
      if (lvl == -128) {
        lvl = (int32_t)br.u(8) - 256;
      } else if (lvl == 0) {
        lvl = (int32_t)br.u(8);
      }
      negative = lvl < 0;
      level = negative ? -lvl : lvl;
    } else {
      run = kAcRun[idx] + 1;
      level = kAcLevel[idx];
      negative = br.flag();  // sign bit follows the matched code
    }

    i += run;
    if (i > 63) return false;
    int j = kZigzag[i];
    int32_t dq = (level * qscale * (int32_t)quantMatrix[j]) >> 3;
    dq = (dq - 1) | 1;  // oddification / mismatch control, clause 7.4.4
    coef[j] = negative ? -dq : dq;
  }
  return true;
}

/**
 * Decodes all 6 blocks of one intra macroblock and writes the
 * reconstructed pixels directly into `frame` at macroblock position
 * (mbX, mbY) - MPEG-1 intra has no spatial prediction to accumulate onto
 * (docs/scope.md), so each block's clipped IDCT output *is* the final
 * pixel value. Block order/placement (clause 2.4.3.7, Figure 2-16): 0/1
 * are the top-left/top-right 8x8 luma quadrants, 2/3 are bottom-left/
 * bottom-right, 4 is Cb, 5 is Cr (4:2:0 chroma is one 8x8 block covering
 * the whole 16x16 luma macroblock area). Returns false if any block's
 * decodeIntraBlock() fails.
 */
template <typename Allocator>
inline bool decodeIntraMacroblock(BitReader& br, Frame<Allocator>& frame, int mbX,
                                   int mbY, int32_t lastDc[3],
                                   const uint16_t* quantMatrix, int qscale) {
  static const int kOffX[6] = {0, 8, 0, 8, 0, 0};
  static const int kOffY[6] = {0, 0, 8, 8, 0, 0};

  int32_t coef[64];
  int32_t pixels[64];
  for (int b = 0; b < 6; b++) {
    if (!decodeIntraBlock(br, b, lastDc, quantMatrix, qscale, coef)) return false;
    idct8x8(coef, pixels);

    uint8_t* plane;
    int stride, baseX, baseY;
    if (b < 4) {
      plane = frame.y();
      stride = frame.strideY;
      baseX = mbX * 16 + kOffX[b];
      baseY = mbY * 16 + kOffY[b];
    } else {
      plane = (b == 4) ? frame.u() : frame.v();
      stride = frame.strideC;
      baseX = mbX * 8;
      baseY = mbY * 8;
    }
    for (int y = 0; y < 8; y++) {
      uint8_t* dstRow = plane + (size_t)(baseY + y) * stride + baseX;
      const int32_t* srcRow = pixels + y * 8;
      for (int x = 0; x < 8; x++) dstRow[x] = clip255(srcRow[x]);
    }
  }
  return true;
}

/**
 * Placement offsets shared by decodeIntraMacroblock() and
 * decodePMacroblock() - block order 0..3 are the luma quadrants
 * (top-left, top-right, bottom-left, bottom-right), 4 is Cb, 5 is Cr.
 */
static const int kBlockOffX[6] = {0, 8, 0, 8, 0, 0};
static const int kBlockOffY[6] = {0, 0, 8, 8, 0, 0};

/// Result of decodeMbTypeP() - Table B.2, P-picture rows (see kMbTypePCode's file comment, mpg_vlc_tables.h).
struct MbTypeP {
  bool intra = false;   ///< this macroblock is intra-coded (P-pictures may freely mix intra and inter macroblocks)
  bool quant = false;   ///< a new quantizer_scale follows
  bool zeroMv = false;  ///< no motion vector is coded - use (0,0) and reset the motion vector predictor
  bool hasCbp = false;  ///< coded_block_pattern (and possibly residual data) follows
  bool error = false;
};

/**
 * Decodes one P-picture macroblock_type (Table B.2's P-picture rows).
 * Unlike I-pictures' two-code table, this needs a real VLC walk (7
 * codes) - see kMbTypePCode/kMbTypePBits's file comment (mpg_vlc_tables.h)
 * for the row->meaning mapping this mirrors from FFmpeg's ptype2mb_type[].
 */
inline MbTypeP decodeMbTypeP(BitReader& br) {
  MbTypeP r;
  int idx = br.readVlc(kMbTypePCode, kMbTypePBits, kMbTypePTableSize, kMbTypePMaxBits);
  switch (idx) {
    case 0: r.intra = true; break;
    case 1: r.zeroMv = true; r.hasCbp = true; break;
    case 2: break;  // explicit forward MV, no residual ("motion-compensated copy only")
    case 3: r.hasCbp = true; break;  // explicit forward MV + residual
    case 4: r.intra = true; r.quant = true; break;
    case 5: r.quant = true; r.zeroMv = true; r.hasCbp = true; break;
    case 6: r.quant = true; r.hasCbp = true; break;
    default: r.error = true; break;  // includes idx < 0 (no VLC match)
  }
  return r;
}

/// Result of decodeCodedBlockPattern().
struct CbpResult {
  int pattern = 0;  ///< 6-bit coded_block_pattern; bit 5 = block 0 (Y0) ... bit 0 = block 5 (Cr)
  bool error = false;
};

/**
 * Decodes coded_block_pattern (Table B.9) - see kCbpCode's file comment
 * (mpg_vlc_tables.h) for why the decoded VLC row index is used directly
 * as the pattern value, no further remapping. A decoded pattern of 0
 * ("no block coded" - nonsensical when coded_block_pattern is present at
 * all, since a macroblock with nothing coded wouldn't set
 * macroblock_pattern in the first place) is rejected as a bitstream
 * error, matching FFmpeg's own `cbp <= 0` check.
 */
inline CbpResult decodeCodedBlockPattern(BitReader& br) {
  CbpResult r;
  int idx = br.readVlc(kCbpCode, kCbpBits, kCbpTableSize, kCbpMaxBits);
  if (idx <= 0) {
    r.error = true;
    return r;
  }
  r.pattern = idx;
  return r;
}

/**
 * Decodes and dequantizes one non-intra (inter) 8x8 block's DCT
 * coefficients, clause 2.4.3.7 / Annex B. Structurally different from
 * decodeIntraBlock(): there's no separate differentially-coded DC term -
 * position 0 is just another run/level-coded AC-style coefficient - but
 * the *very first* coefficient of the block has its own especially short
 * ('1' bit + sign, 2 bits total) code reserved for the single most common
 * case (run=0, level=1), distinct from - and taking priority over - the
 * same (run, level) pair's normal (longer) code elsewhere in the block;
 * decodeIntraBlock() has no equivalent shortcut since its DC term is
 * never AC-coded at all. `quantMatrix` is the active non-intra quantizer
 * matrix (SequenceInfo::nonIntraMatrix); unlike intra dequant, every
 * coefficient here (including escape-coded ones) is scaled by
 * `level * 2 + 1`, not `level` directly - ported from FFmpeg's
 * mpeg1_decode_block_inter() (libavcodec/mpeg12dec.c), with the same kind
 * of empirically-recalibrated shift decodeIntraBlock() needed (see this
 * file's top comment and docs/testing.md) rather than trusted as literally
 * read from that function's current source.
 */
inline bool decodeInterBlock(BitReader& br, const uint16_t* quantMatrix, int qscale,
                              int32_t coef[64]) {
  memset(coef, 0, 64 * sizeof(int32_t));
  int i = -1;  // no DC term occupies position 0 here, unlike decodeIntraBlock()

  if (br.peekBits(1) == 1) {  // first-coefficient shortcut: '1' + sign, means (run=0, level=1)
    br.skipBits(1);
    bool negative = br.flag();
    int32_t dq = (3 * qscale * (int32_t)quantMatrix[0]) >> kInterAcShift;
    dq = (dq - 1) | 1;
    i = 0;
    coef[0] = negative ? -dq : dq;
  }

  for (;;) {
    int idx = br.readVlc(kAcCode, kAcBits, kAcTableSize, kAcMaxBits);
    if (idx < 0) return false;
    if (idx == kAcEobIndex) break;

    int run, level;
    bool negative;
    if (idx == kAcEscapeIndex) {
      run = (int)br.u(6) + 1;
      int32_t lvl = (int32_t)br.u(8);
      if (lvl >= 128) lvl -= 256;
      if (lvl == -128) {
        lvl = (int32_t)br.u(8) - 256;
      } else if (lvl == 0) {
        lvl = (int32_t)br.u(8);
      }
      negative = lvl < 0;
      level = negative ? -lvl : lvl;
    } else {
      run = kAcRun[idx] + 1;
      level = kAcLevel[idx];
      negative = br.flag();
    }

    i += run;
    if (i > 63) return false;
    int j = kZigzag[i];
    int32_t dq = ((level * 2 + 1) * qscale * (int32_t)quantMatrix[j]) >> kInterAcShift;
    dq = (dq - 1) | 1;
    coef[j] = negative ? -dq : dq;
  }
  return true;
}

/**
 * Adds a coded_block_pattern-selected residual on top of an already
 * motion-compensated macroblock at (mbX, mbY) - the shared last step of
 * decodePMacroblock() and decodeBMacroblock() once prediction has been
 * written. `nonIntraMatrix` is always the non-intra quantizer matrix
 * (Phase B/C never add residual onto an intra-predicted macroblock -
 * intra macroblocks have no prediction to add onto in the first place,
 * see decodeIntraMacroblock()).
 */
template <typename Allocator>
inline bool addInterResidual(BitReader& br, Frame<Allocator>& frame, int mbX, int mbY,
                              const uint16_t* nonIntraMatrix, int qscale, int cbpPattern) {
  int32_t coef[64];
  int32_t residual[64];
  for (int b = 0; b < 6; b++) {
    if (!(cbpPattern & (1 << (5 - b)))) continue;  // this block has no residual - prediction alone stands
    if (!decodeInterBlock(br, nonIntraMatrix, qscale, coef)) return false;
    idct8x8(coef, residual);

    uint8_t* plane;
    int stride, baseX, baseY;
    if (b < 4) {
      plane = frame.y();
      stride = frame.strideY;
      baseX = mbX * 16 + kBlockOffX[b];
      baseY = mbY * 16 + kBlockOffY[b];
    } else {
      plane = (b == 4) ? frame.u() : frame.v();
      stride = frame.strideC;
      baseX = mbX * 8;
      baseY = mbY * 8;
    }
    for (int y = 0; y < 8; y++) {
      uint8_t* dstRow = plane + (size_t)(baseY + y) * stride + baseX;
      const int32_t* srcRow = residual + y * 8;
      for (int x = 0; x < 8; x++) {
        dstRow[x] = clip255((int32_t)dstRow[x] + srcRow[x]);
      }
    }
  }
  return true;
}

/**
 * Decodes one P-picture macroblock at (mbX, mbY): macroblock_type,
 * optional new quantizer_scale, motion vector (explicit, zero, or none
 * for an intra macroblock), motion-compensated prediction from `ref`,
 * and optional coded_block_pattern-selected residual added on top -
 * clause 2.4.3.7. `lastMv` (2 elements: horizontal, vertical) is the
 * forward motion vector predictor, carried across macroblocks within a
 * slice; reset to (0,0) here for both intra and zero-MV macroblocks
 * (clause 7.6.3.4 - concealment_motion_vectors, the only case where an
 * intra macroblock in H.262/MPEG-2 keeps the predictor alive instead, is
 * a picture_coding_extension-only field that doesn't exist in MPEG-1).
 * `curMv` is separate: the motion vector a *skipped* macroblock right
 * after this one would inherit (see mpg_decoder.h::decodeSliceP()) - it
 * tracks `lastMv` exactly except for an intra macroblock, which leaves it
 * untouched (a skip can only ever mean "same motion as the last *inter*
 * macroblock", so intra macroblocks - having no motion vector of their
 * own - don't get to redefine it). `lastDc` is reset to 128 for any
 * *non-intra* macroblock (DC prediction only continues across
 * consecutive intra macroblocks, same as Phase A, but a non-intra
 * macroblock in between breaks that chain) - notably *not* reset by a
 * skipped macroblock, which never reaches this function at all. Returns
 * false on any bitstream error.
 */
template <typename Allocator>
inline bool decodePMacroblock(BitReader& br, Frame<Allocator>& frame,
                               const Frame<Allocator>& ref, int mbX, int mbY,
                               int32_t lastDc[3], int32_t lastMv[2], int32_t curMv[2],
                               const uint16_t* intraMatrix, const uint16_t* nonIntraMatrix,
                               int* qscale, int fCode, bool fullPelForward) {
  MbTypeP mt = decodeMbTypeP(br);
  if (mt.error) return false;
  if (mt.quant) {
    int q = (int)br.u(5);
    if (q == 0) return false;
    *qscale = q;
  }

  if (mt.intra) {
    lastMv[0] = 0;
    lastMv[1] = 0;
    return decodeIntraMacroblock(br, frame, mbX, mbY, lastDc, intraMatrix, *qscale);
  }

  int mvX, mvY;
  if (mt.zeroMv) {
    lastMv[0] = 0;
    lastMv[1] = 0;
    mvX = 0;
    mvY = 0;
  } else {
    MotionResult rx = decodeMotionComponent(br, fCode, lastMv[0]);
    if (rx.error) return false;
    lastMv[0] = rx.value;
    MotionResult ry = decodeMotionComponent(br, fCode, lastMv[1]);
    if (ry.error) return false;
    lastMv[1] = ry.value;
    mvX = rx.value;
    mvY = ry.value;
    if (fullPelForward) {
      mvX *= 2;
      mvY *= 2;
    }
  }
  curMv[0] = mvX;
  curMv[1] = mvY;
  lastDc[0] = lastDc[1] = lastDc[2] = 128;

  predictMacroblock(ref, frame, mbX, mbY, mvX, mvY);

  if (mt.hasCbp) {
    CbpResult cbp = decodeCodedBlockPattern(br);
    if (cbp.error) return false;
    if (!addInterResidual(br, frame, mbX, mbY, nonIntraMatrix, *qscale, cbp.pattern)) return false;
  }
  return true;
}

/// Result of decodeMbTypeB() - Table B.2, B-picture rows (see kMbTypeBCode's file comment, mpg_vlc_tables.h).
struct MbTypeB {
  bool intra = false;      ///< this macroblock is intra-coded
  bool quant = false;      ///< a new quantizer_scale follows
  bool hasForward = false;  ///< a forward (temporally past) motion vector is coded
  bool hasBackward = false; ///< a backward (temporally future) motion vector is coded
  bool hasCbp = false;      ///< coded_block_pattern (and possibly residual data) follows
  bool error = false;
};

/**
 * Decodes one B-picture macroblock_type (Table B.2's B-picture rows).
 * Unlike P-pictures, there's no "zero motion vector" macroblock_type row -
 * every non-intra B-picture macroblock explicitly decodes a motion_code
 * for each direction macroblock_type says is active (decodeMotionComponent()
 * itself handles "motion_code == 0" meaning "same as predictor", which
 * plays a similar role). See kMbTypeBCode/kMbTypeBBits's file comment
 * (mpg_vlc_tables.h) for the row->meaning mapping this mirrors from
 * FFmpeg's btype2mb_type[].
 */
inline MbTypeB decodeMbTypeB(BitReader& br) {
  MbTypeB r;
  int idx = br.readVlc(kMbTypeBCode, kMbTypeBBits, kMbTypeBTableSize, kMbTypeBMaxBits);
  switch (idx) {
    case 0: r.intra = true; break;
    case 1: r.hasBackward = true; break;
    case 2: r.hasBackward = true; r.hasCbp = true; break;
    case 3: r.hasForward = true; break;
    case 4: r.hasForward = true; r.hasCbp = true; break;
    case 5: r.hasForward = true; r.hasBackward = true; break;
    case 6: r.hasForward = true; r.hasBackward = true; r.hasCbp = true; break;
    case 7: r.intra = true; r.quant = true; break;
    case 8: r.quant = true; r.hasBackward = true; r.hasCbp = true; break;
    case 9: r.quant = true; r.hasForward = true; r.hasCbp = true; break;
    case 10: r.quant = true; r.hasForward = true; r.hasBackward = true; r.hasCbp = true; break;
    default: r.error = true; break;  // includes idx < 0 (no VLC match)
  }
  return r;
}

/**
 * Decodes one B-picture macroblock at (mbX, mbY): macroblock_type,
 * optional new quantizer_scale, forward and/or backward motion vector(s),
 * bidirectional-aware motion-compensated prediction
 * (predictMacroblockBi(), mpg_motion.h), and optional coded_block_
 * pattern-selected residual added on top - clause 2.4.3.7/7.6.5.
 *
 * `lastMvFwd`/`lastMvBack` are the two *independent* differential motion
 * vector predictors (clause 7.6.3.4) - each is only read/updated when
 * its own direction is actually coded for this macroblock, exactly
 * mirroring FFmpeg's per-direction `HAS_MV(mb_type, dir)` gating; a
 * macroblock using only one direction leaves the other direction's
 * predictor completely untouched, persisting from whenever it was last
 * actually used. `curMvFwd`/`curMvBack`/`curHasForward`/`curHasBackward`
 * are separate: what a *skipped* macroblock right after this one would
 * inherit (see mpg_decoder.h::decodeSliceB()) - since B-picture skip
 * reuses not just the previous macroblock's motion vector(s) but its
 * entire prediction *mode* (clause 7.6.6), unlike P-pictures where skip
 * is always unconditionally forward-only. As with decodePMacroblock(),
 * an intra macroblock resets both motion vector predictors to (0,0) but
 * leaves the skip-inheritance state alone (the caller separately rejects
 * a skip immediately after an intra macroblock as a bitstream error,
 * matching FFmpeg's "skip with previntra" check - there's nothing
 * meaningful for such a skip to inherit - see `outWasIntra`, which
 * reports this macroblock's intra/non-intra status back to the caller
 * for exactly that check). `lastDc` is reset to 128 for any non-intra
 * macroblock, same rule as Phase A/B.
 */
template <typename Allocator>
inline bool decodeBMacroblock(BitReader& br, Frame<Allocator>& frame,
                               const Frame<Allocator>& refPast, const Frame<Allocator>& refFuture,
                               int mbX, int mbY, int32_t lastDc[3], int32_t lastMvFwd[2],
                               int32_t lastMvBack[2], int32_t curMvFwd[2], int32_t curMvBack[2],
                               bool* curHasForward, bool* curHasBackward, bool* outWasIntra,
                               const uint16_t* intraMatrix, const uint16_t* nonIntraMatrix,
                               int* qscale, int fCodeFwd, int fCodeBack, bool fullPelFwd,
                               bool fullPelBack) {
  MbTypeB mt = decodeMbTypeB(br);
  if (mt.error) return false;
  *outWasIntra = mt.intra;
  if (mt.quant) {
    int q = (int)br.u(5);
    if (q == 0) return false;
    *qscale = q;
  }

  if (mt.intra) {
    lastMvFwd[0] = lastMvFwd[1] = 0;
    lastMvBack[0] = lastMvBack[1] = 0;
    return decodeIntraMacroblock(br, frame, mbX, mbY, lastDc, intraMatrix, *qscale);
  }

  if (mt.hasForward) {
    MotionResult rx = decodeMotionComponent(br, fCodeFwd, lastMvFwd[0]);
    if (rx.error) return false;
    lastMvFwd[0] = rx.value;
    MotionResult ry = decodeMotionComponent(br, fCodeFwd, lastMvFwd[1]);
    if (ry.error) return false;
    lastMvFwd[1] = ry.value;
    curMvFwd[0] = fullPelFwd ? rx.value * 2 : rx.value;
    curMvFwd[1] = fullPelFwd ? ry.value * 2 : ry.value;
  }
  if (mt.hasBackward) {
    MotionResult rx = decodeMotionComponent(br, fCodeBack, lastMvBack[0]);
    if (rx.error) return false;
    lastMvBack[0] = rx.value;
    MotionResult ry = decodeMotionComponent(br, fCodeBack, lastMvBack[1]);
    if (ry.error) return false;
    lastMvBack[1] = ry.value;
    curMvBack[0] = fullPelBack ? rx.value * 2 : rx.value;
    curMvBack[1] = fullPelBack ? ry.value * 2 : ry.value;
  }
  *curHasForward = mt.hasForward;
  *curHasBackward = mt.hasBackward;
  lastDc[0] = lastDc[1] = lastDc[2] = 128;

  predictMacroblockBi(mt.hasForward ? &refPast : nullptr, mt.hasBackward ? &refFuture : nullptr,
                       frame, mbX, mbY, curMvFwd[0], curMvFwd[1], curMvBack[0], curMvBack[1]);

  if (mt.hasCbp) {
    CbpResult cbp = decodeCodedBlockPattern(br);
    if (cbp.error) return false;
    if (!addInterResidual(br, frame, mbX, mbY, nonIntraMatrix, *qscale, cbp.pattern)) return false;
  }
  return true;
}

}  // namespace tinympg
