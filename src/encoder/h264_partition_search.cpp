#include "h264_partition_search.h"

#include <stdint.h>
#include <stdlib.h>  // abs()

#include "h264_bitwriter.h"
#include "h264_cavlc_encode.h"
#include "h264_forward_transform.h"
#include "h264_macroblock_encode.h"  // quantizeChromaInter/writeChromaResidual
#include "h264_macroblock_encode_inter.h"  // quantizeChromaInter()
#include "../common/h264_motion.h"
#include "../common/h264_mv_predict.h"
#include "../common/h264_tables.h"
#include "../common/h264_transform.h"

/*
 * Implementation of the optional exhaustive P-macroblock partition search
 * declared in h264_partition_search.h, plus the matching partition-aware
 * macroblock bitstream writer. Kept in a .cpp (rather than inline in a
 * header) because both halves are sizeable and would otherwise be
 * duplicated into every translation unit of this header-only library.
 *
 * All search-time state lives either on the stack or in a local copy of
 * the current macroblock's MacroblockInfo. Trial shapes write into the
 * real macroblock-info entry only while being evaluated and are restored
 * from a saved baseline afterwards, so a rejected candidate can never
 * leak an MV into a later candidate or a neighbouring macroblock. The
 * winning shape's state is left in place for the final bitstream writer
 * and for the macroblocks that follow this one in raster order.
 */

namespace tinyh264 {
namespace {

/// One partition to search, in signalling order (4x4 units).
struct PartSpec {
  int bx, by, pw, ph;
  int dirSide;  ///< predictMvGeneral() directional shortcut, -1 = none
};

/// Exp-Golomb (ue(v)) code length for value `v`.
inline int ueBits(uint32_t v) {
  uint32_t codeNum = v + 1;
  int lz = 0;
  while (codeNum > 1) {
    codeNum >>= 1;
    lz++;
  }
  return 2 * lz + 1;
}

/// Signed Exp-Golomb (se(v)) code length for value `v`.
inline int seBits(int32_t v) {
  uint32_t codeNum = (v > 0) ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v);
  return ueBits(codeNum);  // se(v) is ue(v) over the mapped codeNum
}

/**
 * Raw luma SAD between the source region of one partition and the
 * reference at integer offset (dx,dy) from that partition's own position.
 * Out-of-picture reference positions are edge-replicated via
 * clampedSample(), exactly like real motion compensation, so search cost
 * and reconstruction agree at picture edges.
 */
int sadPartition(const MbEncodeContext& ctx, const Frame& ref, int bx, int by,
                 int pw, int ph, int dx, int dy) {
  int px = ctx.mbX * 16 + bx * 4;
  int py = ctx.mbY * 16 + by * 4;
  int w = pw * 4, h = ph * 4;
  int rx = px + dx, ry = py + dy;
  int sad = 0;
  if (rx >= 0 && ry >= 0 && rx + w <= ref.width && ry + h <= ref.height) {
    for (int y = 0; y < h; y++) {
      const uint8_t* src = ctx.srcY + (size_t)(py + y) * ctx.srcStrideY + px;
      const uint8_t* refRow = ref.y() + (size_t)(ry + y) * ref.strideY + rx;
      for (int x = 0; x < w; x++) sad += abs((int)src[x] - (int)refRow[x]);
    }
    return sad;
  }
  for (int y = 0; y < h; y++) {
    const uint8_t* src = ctx.srcY + (size_t)(py + y) * ctx.srcStrideY + px;
    for (int x = 0; x < w; x++) {
      int rv = clampedSample(ref.y(), ref.strideY, ref.width, ref.height,
                             px + x + dx, py + y + dy);
      sad += abs((int)src[x] - rv);
    }
  }
  return sad;
}

struct MvChoice {
  int dx = 0, dy = 0;
  int sad = 0;
  int mvdBits = 0;
  int64_t cost = 0;
};

/**
 * Exhaustive integer-pel search for one partition under a *fixed*
 * predictor: cost = raw SAD + L * signed-Exp-Golomb MVD bits. Ties are
 * broken by raw SAD, then |dx|+|dy|, then mvY, then mvX, per the task's
 * rule - deterministic, with no magnitude bias folded into the cost.
 */
MvChoice choosePartitionMv(const MbEncodeContext& ctx, const Frame& ref,
                           const PartSpec& s, int range, int l,
                           const int16_t* pred) {
  MvChoice best;
  bool have = false;
  for (int dy = -range; dy <= range; dy++) {
    for (int dx = -range; dx <= range; dx++) {
      int sad = sadPartition(ctx, ref, s.bx, s.by, s.pw, s.ph, dx, dy);
      int32_t mvdX = dx * 4 - pred[0];
      int32_t mvdY = dy * 4 - pred[1];
      int bits = seBits(mvdX) + seBits(mvdY);
      int64_t cost = (int64_t)sad + (int64_t)l * bits;
      bool better;
      if (!have) {
        better = true;
      } else if (cost != best.cost) {
        better = cost < best.cost;
      } else if (sad != best.sad) {
        better = sad < best.sad;
      } else {
        int disp = abs(dx) + abs(dy);
        int bestDisp = abs(best.dx) + abs(best.dy);
        if (disp != bestDisp) {
          better = disp < bestDisp;
        } else if (dy != best.dy) {
          better = dy < best.dy;
        } else {
          better = dx < best.dx;
        }
      }
      if (better) {
        best.dx = dx;
        best.dy = dy;
        best.sad = sad;
        best.mvdBits = bits;
        best.cost = cost;
        have = true;
      }
    }
  }
  return best;
}

/// Builds the partition list for one P_8x8 quadrant/sub_mb_type.
/// Sub-partitions use the general median predictor (dirSide -1): Table
/// 8-9's one-sided shortcut applies only to the top-level P_16x8/P_8x16
/// partitions, not to P_8x8 sub-shapes - verified by decoding real x264
/// partitions=all streams pixel-exact with this project's decoder.
int buildSubSpecs(int subType, int qx, int qy, PartSpec specs[4]) {
  switch (subType) {
    case kSub8x8:
      specs[0] = {qx, qy, 2, 2, -1};
      return 1;
    case kSub8x4:
      specs[0] = {qx, qy, 2, 1, -1};
      specs[1] = {qx, qy + 1, 2, 1, -1};
      return 2;
    case kSub4x8:
      specs[0] = {qx, qy, 1, 2, -1};
      specs[1] = {qx + 1, qy, 1, 2, -1};
      return 2;
    case kSub4x4:
    default:
      specs[0] = {qx, qy, 1, 1, -1};
      specs[1] = {qx + 1, qy, 1, 1, -1};
      specs[2] = {qx, qy + 1, 1, 1, -1};
      specs[3] = {qx + 1, qy + 1, 1, 1, -1};
      return 4;
  }
}

/**
 * Runs one partition list in signalling order against the current
 * macroblock-info entry (which must already hold the state this trial
 * starts from), selecting each partition's MV under the predictor built
 * from the partitions selected so far. Selected MVs are written into the
 * macroblock-info entry immediately, so later partitions' directional and
 * median predictors see them; the caller has saved/restores the entry
 * around trials. `parts` receives this shape's partitions starting at
 * index `partOffset`.
 */
void runSpecs(const MbEncodeContext& ctx, const Frame& ref, int range, int l,
              const PartSpec* specs, int nSpecs, int partOffset,
              PartitionSearchPart* parts, int* outMvdBits, int* outSad) {
  int mvdBits = 0;
  int sad = 0;
  for (int i = 0; i < nSpecs; i++) {
    const PartSpec& s = specs[i];
    int16_t pred[2];
    predictMvGeneral(ctx, s.bx, s.by, s.pw, s.ph, /*curRefIdx=*/0, s.dirSide,
                     pred);
    MvChoice c = choosePartitionMv(ctx, ref, s, range, l, pred);

    PartitionSearchPart& p = parts[partOffset + i];
    p.bx = (uint8_t)s.bx;
    p.by = (uint8_t)s.by;
    p.pw = (uint8_t)s.pw;
    p.ph = (uint8_t)s.ph;
    p.mv[0] = (int16_t)(c.dx * 4);
    p.mv[1] = (int16_t)(c.dy * 4);
    p.mvd[0] = (int16_t)(p.mv[0] - pred[0]);
    p.mvd[1] = (int16_t)(p.mv[1] - pred[1]);
    p.sad = c.sad;

    fillPartitionMv(ctx.mbInfo->at(ctx.mbX, ctx.mbY), s.bx, s.by, s.pw, s.ph,
                    p.mv[0], p.mv[1], /*refIdx=*/0);
    mvdBits += c.mvdBits;
    sad += c.sad;
  }
  *outMvdBits = mvdBits;
  *outSad = sad;
}

}  // namespace

bool searchP16Partitions(const MbEncodeContext& ctx, const Frame& ref,
                         int range, int targetQp, PartitionSearchResult* out) {
  if (!out || range < 1) return false;
  if (ctx.mbX < 0 || ctx.mbY < 0) return false;

  MacroblockInfo& dst = ctx.mbInfo->at(ctx.mbX, ctx.mbY);
  /*
   * Start every trial from a clean macroblock-info entry. mbInfo_ is
   * zeroed at picture start, but using an explicit fresh value here makes
   * the "nothing leaks between candidates/neighbours" rule local and
   * unconditional; the winning shape's state is written back before this
   * function returns.
   */
  const MacroblockInfo baseline;
  dst = baseline;
  const int l = 1 + (targetQp / 6);

  // --- Candidate big shapes, each started from the same prior state ------
  struct BigShape {
    bool valid = false;
    PartitionSearchResult res;
    MacroblockInfo state;
  };
  BigShape best;
  BigShape cur;

  // P_16x16 ---------------------------------------------------------------
  dst = baseline;
  {
    int mvdBits = 0, sad = 0;
    PartSpec s = {0, 0, 4, 4, -1};
    cur.res = PartitionSearchResult();
    cur.res.mbType = kPL0_16x16;
    runSpecs(ctx, ref, range, l, &s, 1, 0, cur.res.parts, &mvdBits, &sad);
    cur.res.numParts = 1;
    cur.res.totalSad = sad;
    cur.res.totalCost = (int64_t)sad +
                        (int64_t)l * (mvdBits + ueBits(kPL0_16x16));
    cur.state = dst;
    best = cur;
    best.valid = true;
  }

  // P_16x8: top then bottom ----------------------------------------------
  dst = baseline;
  {
    int mvdBits = 0, sad = 0;
    PartSpec specs[2] = {{0, 0, 4, 2, 1}, {0, 2, 4, 2, 0}};
    cur.res = PartitionSearchResult();
    cur.res.mbType = kPL0_L0_16x8;
    runSpecs(ctx, ref, range, l, specs, 2, 0, cur.res.parts, &mvdBits, &sad);
    cur.res.numParts = 2;
    cur.res.totalSad = sad;
    cur.res.totalCost = (int64_t)sad +
                        (int64_t)l * (mvdBits + ueBits(kPL0_L0_16x8));
    cur.state = dst;
    if (!best.valid || cur.res.totalCost < best.res.totalCost) {
      best = cur;
      best.valid = true;
    }
  }

  // P_8x16: left then right ----------------------------------------------
  dst = baseline;
  {
    int mvdBits = 0, sad = 0;
    PartSpec specs[2] = {{0, 0, 2, 4, 0}, {2, 0, 2, 4, 2}};
    cur.res = PartitionSearchResult();
    cur.res.mbType = kPL0_L0_8x16;
    runSpecs(ctx, ref, range, l, specs, 2, 0, cur.res.parts, &mvdBits, &sad);
    cur.res.numParts = 2;
    cur.res.totalSad = sad;
    cur.res.totalCost = (int64_t)sad +
                        (int64_t)l * (mvdBits + ueBits(kPL0_L0_8x16));
    cur.state = dst;
    if (!best.valid || cur.res.totalCost < best.res.totalCost) {
      best = cur;
      best.valid = true;
    }
  }

  // P_8x8: four quadrants, greedy left-to-right/top-to-bottom; each
  // quadrant's shape trial starts from the state selected by the
  // quadrants before it. ---------------------------------------------------
  dst = baseline;
  {
    static const int kQuadX[4] = {0, 2, 0, 2};
    static const int kQuadY[4] = {0, 0, 2, 2};
    PartitionSearchResult r;
    r.mbType = kP8x8;
    int mvdBits = 0, sad = 0, numParts = 0;

    struct QuadChoice {
      bool valid = false;
      int subType = 0;
      int cost = 0;
      MacroblockInfo state;
      PartitionSearchPart parts[4];
      int n = 0;
      int bits = 0;
      int sad = 0;
    };

    for (int q = 0; q < 4; q++) {
      const MacroblockInfo regionBaseline = dst;
      QuadChoice qc;
      for (int subType = 0; subType < 4; subType++) {
        PartSpec specs[4];
        int n = buildSubSpecs(subType, kQuadX[q], kQuadY[q], specs);
        dst = regionBaseline;
        PartitionSearchPart trialParts[4];
        int bits = 0, qSad = 0;
        runSpecs(ctx, ref, range, l, specs, n, 0, trialParts, &bits, &qSad);
        int cost = qSad + l * (bits + ueBits((uint32_t)subType));
        bool better;
        if (!qc.valid) {
          better = true;
        } else if (cost != qc.cost) {
          better = cost < qc.cost;
        } else {
          better = subType < qc.subType;  // total-cost tie: smaller type
        }
        if (better) {
          qc.valid = true;
          qc.subType = subType;
          qc.cost = cost;
          qc.state = dst;
          qc.n = n;
          qc.bits = bits;
          qc.sad = qSad;
          for (int i = 0; i < n; i++) qc.parts[i] = trialParts[i];
        }
      }
      dst = qc.state;
      r.subMbType[q] = (uint8_t)qc.subType;
      for (int i = 0; i < qc.n; i++) r.parts[numParts + i] = qc.parts[i];
      numParts += qc.n;
      mvdBits += qc.bits;
      sad += qc.sad;
    }
    r.numParts = numParts;
    r.totalSad = sad;
    int shapeBits = ueBits(kP8x8);
    for (int q = 0; q < 4; q++) shapeBits += ueBits(r.subMbType[q]);
    r.totalCost = (int64_t)sad + (int64_t)l * (mvdBits + shapeBits);
    cur.res = r;
    cur.state = dst;
    if (!best.valid || cur.res.totalCost < best.res.totalCost) {
      best = cur;
      best.valid = true;
    }
  }

  // Leave the winning shape's macroblock state in place: the final
  // bitstream writer uses it directly, and later macroblocks' predictors
  // read it. Rejected shapes never reach here.
  dst = best.state;
  *out = best.res;
  return true;
}

int encodeInterPartitionMacroblock(BitWriter& bw, MbEncodeContext& ctx,
                                    const Frame& ref,
                                    const PartitionSearchResult& r,
                                    int qpPrev, int targetQp) {
  const int mbX = ctx.mbX, mbY = ctx.mbY;
  MacroblockInfo& mb = ctx.mbInfo->at(mbX, mbY);
  mb.type = kMbInter;

  const int px0 = mbX * 16, py0 = mbY * 16;
  const int cpx0 = mbX * 8, cpy0 = mbY * 8;

  // --- Motion compensation for every selected partition ------------------
  // Doing this here (rather than during search) makes sure an Intra trial
  // that overwrote the prediction planes cannot survive into the final
  // reconstruction: the whole tiled prediction is rebuilt first.
  for (int i = 0; i < r.numParts; i++) {
    const PartitionSearchPart& p = r.parts[i];
    int px = px0 + p.bx * 4, py = py0 + p.by * 4;
    int w = p.pw * 4, h = p.ph * 4;
    motionCompLuma(ctx.frame->yRow(py) + px, ctx.frame->strideY, ref, px, py,
                   w, h, p.mv[0], p.mv[1]);
    int cpx = cpx0 + p.bx * 2, cpy = cpy0 + p.by * 2;
    motionCompChroma(ctx.frame->uRow(cpy) + cpx, ctx.frame->strideC, ref.u(),
                     ref.strideC, ref.width / 2, ref.height / 2, cpx, cpy,
                     p.pw * 2, p.ph * 2, p.mv[0], p.mv[1]);
    motionCompChroma(ctx.frame->vRow(cpy) + cpx, ctx.frame->strideC, ref.v(),
                     ref.strideC, ref.width / 2, ref.height / 2, cpx, cpy,
                     p.pw * 2, p.ph * 2, p.mv[0], p.mv[1]);
  }

  // --- Whole-macroblock residual transform/quantize ---------------------
  int32_t lumaBlocks[16][16];
  int cbpLuma = 0;
  for (int blk = 0; blk < 16; blk++) {
    int bx = kBlk4x4X[blk], by = kBlk4x4Y[blk];
    int px = px0 + bx * 4, py = py0 + by * 4;
    subtractBlock4x4(ctx.srcY + (size_t)py * ctx.srcStrideY + px,
                      ctx.srcStrideY, ctx.frame->yRow(py) + px,
                      ctx.frame->strideY, lumaBlocks[blk]);
    forwardDct4x4(lumaBlocks[blk]);
    if (quantizeBlock4x4(lumaBlocks[blk], targetQp, false))
      cbpLuma |= (1 << (blk / 4));
  }
  mb.cbpLuma = (uint8_t)cbpLuma;

  int32_t chromaBlocks[2][4][16];
  int32_t chromaDcGrid[2][4];
  quantizeChromaInter(ctx, mb, targetQp, chromaBlocks, chromaDcGrid);

  // --- Partition syntax: mb_type, sub_mb_type list, mvd_l0 --------------
  bw.ue(r.mbType);
  if (r.mbType == kP8x8) {
    for (int q = 0; q < 4; q++) bw.ue(r.subMbType[q]);
  }
  /*
   * ref_idx_l0: decodeRefIdx() reads zero bits when numActiveRefs <= 1
   * (this encoder's single-reference scope) - nothing to write.
   */
  for (int i = 0; i < r.numParts; i++) {
    bw.se(r.parts[i].mvd[0]);
    bw.se(r.parts[i].mvd[1]);
  }

  // --- coded_block_pattern / mb_qp_delta --------------------------------
  uint32_t cbpCode = 0;
  {
    uint8_t want = (uint8_t)(mb.cbpLuma | (mb.cbpChroma << 4));
    for (uint32_t i = 0; i < 48; i++) {
      if (kCbpInter[i] == want) {
        cbpCode = i;
        break;
      }
    }
  }
  bw.ue(cbpCode);

  bool hasResidual = mb.cbpLuma != 0 || mb.cbpChroma != 0;
  int qpUsed = hasResidual ? targetQp : qpPrev;
  if (hasResidual) bw.se(targetQp - qpPrev);
  mb.qpY = (int8_t)qpUsed;

  // --- Luma residual, per 4x4 block gated on its quadrant's cbp bit -----
  for (int blk = 0; blk < 16; blk++) {
    if (mb.cbpLuma & (1 << (blk / 4))) {
      int bx = kBlk4x4X[blk], by = kBlk4x4Y[blk];
      int px = px0 + bx * 4, py = py0 + by * 4;
      int32_t scan[16];
      for (int k = 0; k < 16; k++) scan[k] = lumaBlocks[blk][kZigZag4x4[k]];
      int nA = lumaNeighborNnz(ctx, bx, by, -1, 0);
      int nB = lumaNeighborNnz(ctx, bx, by, 0, -1);
      int nC = predictNc(nA, nB);
      encodeResidualBlockCavlc(bw, nC, 16, scan);
      uint32_t totalCoeff = 0;
      for (int k = 0; k < 16; k++) if (scan[k] != 0) totalCoeff++;
      mb.nnz[blk] = (uint8_t)totalCoeff;

      int32_t block[16];
      for (int i = 0; i < 16; i++) block[i] = lumaBlocks[blk][i];
      dequant4x4(block, targetQp, false);
      idct4x4(block);
      addResidual4x4(ctx.frame->yRow(py) + px, ctx.frame->strideY, block);
    } else {
      mb.nnz[blk] = 0;
    }
  }

  writeChromaResidual(bw, ctx, mb, targetQp, chromaBlocks, chromaDcGrid);

  return qpUsed;
}

}  // namespace tinyh264
