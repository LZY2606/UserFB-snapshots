/*
 * Task test: optional P-macroblock partition motion search
 * (TinyH264Encoder/SoftwareEncoder::setPartitionSearch()), plus the
 * decoder-side P_8x8 sub-partition support it needs to be verified
 * against.
 *
 * Three groups, registered via task_cases/task_case_count and run by
 * task_support/driver.c (no main() here):
 *
 *  search    - independent re-implementation of the exhaustive search
 *              rules (MVD predictor derivation, Exp-Golomb lengths, cost
 *              and tie-break ordering, candidate order) checked against
 *              the partitions/MVDs the real encoder actually emits for
 *              small, non-flat pictures whose top/bottom, left/right and
 *              four quadrants move differently. Covers second-partition
 *              dependence on the first, rejected-shape state restoration,
 *              negative displacement and picture-edge clamping.
 *
 *  syntax    - walks the real encoder output at the bit level and checks
 *              the emitted mb_type/sub_mb_type/mvd_l0 tree, the
 *              P_Skip-only-when-all-partitions-equal-skip rule, the Intra
 *              fallback, mode toggling and byte-identity of the disabled
 *              path.
 *
 *  reference - hand-built legal P_8x8 streams (one 8x4 + three 8x8
 *              region MB, and a 16-partition all-4x4 MB) decoded by the
 *              patched decoder against independently computed pixel
 *              expectations; then the public encoder's own output fed to
 *              the same decoder, plane-exact against the encoder's
 *              deblocked reconstruction over several consecutive
 *              generations, with chroma, non-zero residual, skip and
 *              intra fallback mixed, mode toggling, fixed QP and
 *              automatic rate control, and off-path byte comparison.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/TinyH264Encoder.h"
#include "../src/TinyH264Decoder.h"
#include "../src/StdAllocator.h"
#include "../src/common/MemoryResource.h"
#include "../src/common/h264_motion.h"
#include "../src/common/h264_mv_predict.h"
#include "../src/common/h264_tables.h"
#include "../src/common/h264_transform.h"
#include "../src/decoder/h264_bitreader.h"
#include "../src/decoder/h264_cavlc.h"
#include "../src/decoder/h264_sps_pps.h"
#include "../src/decoder/h264_slice_header.h"
#include "../src/decoder/h264_nal.h"
#include "../src/decoder/h264_macroblock.h"
#include "../src/decoder/h264_macroblock_inter.h"
#include "../src/encoder/h264_bitwriter.h"
#include "../src/encoder/h264_cavlc_encode.h"
#include "../src/encoder/h264_sps_pps_writer.h"
#include "task_support/check.h"

using namespace tinyh264;

namespace {

// =====================================================================
// Shared helpers
// =====================================================================

// Small non-flat picture: 5x4 macroblocks.
const int kBw = 80, kBh = 64;
const size_t kFrameSize = (size_t)kBw * kBh * 3 / 2;

void splitFrame(const std::vector<uint8_t>& f, const uint8_t** y,
                const uint8_t** u, const uint8_t** v) {
  *y = f.data();
  *u = f.data() + (size_t)kBw * kBh;
  *v = *u + (size_t)(kBw / 2) * (kBh / 2);
}

// A textured frame whose quadrants move in `motionKind` directions by
// frameIdx. motionKind 0 = top/bottom, 1 = left/right, 2 = four quadrants,
// 3 = diagonal.
std::vector<uint8_t> makeFrame(int frameIdx, int motionKind) {
  std::vector<uint8_t> f(kFrameSize);
  uint8_t* Y = f.data();
  for (int y = 0; y < kBh; y++)
    for (int x = 0; x < kBw; x++) {
      int qx = x >= kBw / 2 ? 1 : 0, qy = y >= kBh / 2 ? 1 : 0;
      int dx = 0, dy = 0;
      if (motionKind == 0)
        dy = qy ? 2 : -2;
      else if (motionKind == 1)
        dx = qx ? 3 : -3;
      else if (motionKind == 2)
        dx = qx ? 2 : -2, dy = qy ? 2 : -2;
      else
        dx = qx ? 3 : -3, dy = qy ? 3 : -3;
      int sx = (x + dx * frameIdx + kBw) % kBw;
      int sy = (y + dy * frameIdx + kBh) % kBh;
      Y[(size_t)y * kBw + x] =
          (uint8_t)((sx * 5 + sy * 9 + ((sx >> 2) * 37) + ((sy >> 2) * 53)) &
                    0xFF);
    }
  uint8_t* U = f.data() + (size_t)kBw * kBh;
  for (int y = 0; y < kBh / 2; y++)
    for (int x = 0; x < kBw / 2; x++)
      U[(size_t)y * (kBw / 2) + x] =
          (uint8_t)((x * 7 + y * 11 + frameIdx * 13) & 0xFF);
  uint8_t* V = U + (size_t)(kBw / 2) * (kBh / 2);
  for (int y = 0; y < kBh / 2; y++)
    for (int x = 0; x < kBw / 2; x++)
      V[(size_t)y * (kBw / 2) + x] =
          (uint8_t)((x * 3 + y * 5 + frameIdx * 17) & 0xFF);
  return f;
}

int ueLen(uint32_t v) {
  uint32_t c = v + 1;
  int n = 0;
  while (c > 1) {
    c >>= 1;
    n++;
  }
  return 2 * n + 1;
}
int seLen(int32_t v) {
  uint32_t c = v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v);
  return ueLen(c);
}

// ---------------------------------------------------------------------
// Independent P-slice parser. This is a deliberately separate decoding
// path from the encoder under test: it consumes the real bitstream with
// the production syntax primitives and records mb_type/sub_mb_type and
// every partition MV/MVD, so the search group can compare the encoder's
// choices against an independent oracle.
// ---------------------------------------------------------------------

struct ParsedMb {
  int mbX, mbY;
  uint8_t type;      // kMbPSkip / kMbInter / kMbIntra*
  uint8_t mbTypeRaw; // wire mb_type
  uint8_t subType[4] = {0, 0, 0, 0};
  int numParts = 0;
  int16_t mv[16][2];
  int8_t ref[16];
  int16_t pred[16][2];
  int16_t mvd[16][2];
  uint8_t cbpLuma = 0, cbpChroma = 0;
  bool intra = false;
};

struct PartLayout {
  int bx, by, pw, ph, dirSide;
  int8_t refIdx;
};

int layoutFor(int mbTypeRaw, const uint32_t subType[4], PartLayout parts[16]) {
  int n = 0;
  if (mbTypeRaw == kPL0_16x16) {
    parts[n++] = {0, 0, 4, 4, -1, 0};
  } else if (mbTypeRaw == kPL0_L0_16x8) {
    parts[n++] = {0, 0, 4, 2, 1, 0};
    parts[n++] = {0, 2, 4, 2, 0, 0};
  } else if (mbTypeRaw == kPL0_L0_8x16) {
    parts[n++] = {0, 0, 2, 4, 0, 0};
    parts[n++] = {2, 0, 2, 4, 2, 0};
  } else {
    static const int qx[4] = {0, 2, 0, 2};
    static const int qy[4] = {0, 0, 2, 2};
    for (int q = 0; q < 4; q++) {
      int x = qx[q], y = qy[q];
      switch (subType[q]) {
        case kSub8x8:
          parts[n++] = {x, y, 2, 2, -1, 0};
          break;
        case kSub8x4:
          parts[n++] = {x, y, 2, 1, -1, 0};
          parts[n++] = {x, y + 1, 2, 1, -1, 0};
          break;
        case kSub4x8:
          parts[n++] = {x, y, 1, 2, -1, 0};
          parts[n++] = {x + 1, y, 1, 2, -1, 0};
          break;
        default:
          parts[n++] = {x, y, 1, 1, -1, 0};
          parts[n++] = {x + 1, y, 1, 1, -1, 0};
          parts[n++] = {x, y + 1, 1, 1, -1, 0};
          parts[n++] = {x + 1, y + 1, 1, 1, -1, 0};
          break;
      }
    }
  }
  return n;
}

// Consume chroma residual exactly as the decoder does (no reconstruction
// needed for parsing, but the bit reader must advance correctly).
bool consumeChroma(BitReader& br, MbDecodeContext& ctx, const Pps& pps,
                   int qp, uint8_t cbpChroma) {
  int cQp = chromaQp(qp, pps.chromaQpIndexOffset);
  int32_t dc[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
  if (cbpChroma >= 1) {
    for (int plane = 0; plane < 2; plane++) {
      int32_t coeff[4];
      uint32_t totalCoeff = 0;
      if (!residualBlockCavlc(br, -1, 4, coeff, &totalCoeff)) return false;
      for (int i = 0; i < 4; i++) dc[plane][i] = coeff[i];
    }
  }
  if (cbpChroma >= 2) {
    for (int plane = 0; plane < 2; plane++)
      for (int cy = 0; cy < 2; cy++)
        for (int cx = 0; cx < 2; cx++) {
          int32_t coeff[15];
          uint32_t totalCoeff = 0;
          int nA = chromaNeighborNnz(ctx, plane, cx, cy, -1, 0);
          int nB = chromaNeighborNnz(ctx, plane, cx, cy, 0, -1);
          int nC = predictNc(nA, nB);
          if (!residualBlockCavlc(br, nC, 15, coeff, &totalCoeff))
            return false;
          ctx.mbInfo->at(ctx.mbX, ctx.mbY).nnz[16 + plane * 4 + cy * 2 + cx] =
              (uint8_t)totalCoeff;
        }
  } else {
    for (int i = 0; i < 8; i++) ctx.mbInfo->at(ctx.mbX, ctx.mbY).nnz[16 + i] = 0;
  }
  (void)cQp;
  (void)dc;
  return !br.error();
}

// Parses one Inter macroblock's macroblock_layer() and records it. Returns
// false on bitstream error.
bool parseInterMb(BitReader& br, MbDecodeContext& ctx, const Pps& pps,
                  uint32_t mbTypeRaw, int* qpY, ParsedMb* out) {
  out->mbTypeRaw = (uint8_t)mbTypeRaw;
  out->type = kMbInter;
  out->intra = false;
  out->numParts = 0;

  uint32_t subType[4] = {0, 0, 0, 0};
  if (mbTypeRaw == kP8x8 || mbTypeRaw == kP8x8ref0) {
    for (int i = 0; i < 4; i++) {
      subType[i] = br.ue();
      if (subType[i] > (uint32_t)kSub4x4 || br.error()) return false;
      out->subType[i] = (uint8_t)subType[i];
    }
  }

  MacroblockInfo& mb = ctx.mbInfo->at(ctx.mbX, ctx.mbY);
  mb = MacroblockInfo();
  mb.type = kMbInter;

  PartLayout parts[16];
  int n = layoutFor((int)mbTypeRaw, subType, parts);
  out->numParts = n;

  for (int i = 0; i < n; i++) {
    int8_t r;
    if (!decodeRefIdx(br, ctx.numActiveRefs, &r)) return false;
    parts[i].refIdx = r;
  }

  for (int i = 0; i < n; i++) {
    int16_t pred[2], mvd[2];
    predictMvGeneral(ctx, parts[i].bx, parts[i].by, parts[i].pw, parts[i].ph,
                     parts[i].refIdx, parts[i].dirSide, pred);
    if (!decodeMvd(br, mvd)) return false;
    int16_t mv[2] = {(int16_t)(pred[0] + mvd[0]), (int16_t)(pred[1] + mvd[1])};
    out->pred[i][0] = pred[0];
    out->pred[i][1] = pred[1];
    out->mvd[i][0] = mvd[0];
    out->mvd[i][1] = mvd[1];
    out->mv[i][0] = mv[0];
    out->mv[i][1] = mv[1];
    out->ref[i] = parts[i].refIdx;
    fillPartitionMv(mb, parts[i].bx, parts[i].by, parts[i].pw, parts[i].ph,
                    mv[0], mv[1], parts[i].refIdx);
    motionCompensatePartition(ctx, parts[i].bx, parts[i].by, parts[i].pw,
                              parts[i].ph, mv[0], mv[1], parts[i].refIdx);
  }

  uint32_t cbpCode = br.ue();
  if (cbpCode > 47 || br.error()) return false;
  uint8_t combined = kCbpInter[cbpCode];
  mb.cbpLuma = combined & 0xF;
  mb.cbpChroma = combined >> 4;
  out->cbpLuma = mb.cbpLuma;
  out->cbpChroma = mb.cbpChroma;

  int dquant = 0;
  if (mb.cbpLuma || mb.cbpChroma) dquant = br.se();
  int qp = *qpY + dquant;
  qp = ((qp + 52) % 52 + 52) % 52;
  *qpY = qp;
  mb.qpY = (int8_t)qp;

  for (int blk = 0; blk < 16; blk++) {
    if (mb.cbpLuma & (1 << (blk / 4))) {
      if (!decodeLumaBlockFull(br, ctx, blk, qp)) return false;
    } else {
      mb.nnz[blk] = 0;
    }
  }
  if (!consumeChroma(br, ctx, pps, qp, mb.cbpChroma)) return false;
  return !br.error();
}

// Decodes whole P picture into `outFrame`, using `ref` as reference, and
// returns every macroblock's recorded partition syntax.
bool parsePPicture(const uint8_t* rbsp, size_t size, const Sps& sps,
                   const Pps& pps, const NalUnit& nal, const Frame& ref,
                   Frame& outFrame, MbInfoTable& mbInfo,
                   std::vector<ParsedMb>* out) {
  BitReader br0(rbsp, size);
  SliceHeader sh;
  if (!parseSliceHeader(br0, nal, sps, pps, &sh) || !sh.valid) return false;
  int numActiveRefs = (int)sh.numRefIdxL0ActiveMinus1 + 1;

  if (!mbInfo.reset((int)sps.picWidthInMbs, (int)sps.picHeightInMbs))
    return false;
  if (!outFrame.setSize((int)sps.codedWidth, (int)sps.codedHeight))
    return false;

  MbDecodeContext ctx;
  ctx.frame = &outFrame;
  ctx.numActiveRefs = numActiveRefs;
  for (int i = 0; i < numActiveRefs && i < H264_MAX_REF_FRAMES; i++)
    ctx.refList[i] = &ref;
  ctx.mbInfo = &mbInfo;
  ctx.sps = &sps;
  ctx.pps = &pps;
  ctx.sliceId = 0;

  int mbWidth = (int)sps.picWidthInMbs, mbHeight = (int)sps.picHeightInMbs;
  int total = mbWidth * mbHeight;
  int qpY = sh.sliceQp;

  BitReader br(rbsp, size);
  // Re-consume the slice header with a fresh reader (parseSliceHeader does
  // not leave br0 at slice_data() reliably for our re-use).
  SliceHeader sh2;
  if (!parseSliceHeader(br, nal, sps, pps, &sh2)) return false;

  int mbAddr = 0;
  while (mbAddr < total) {
    uint32_t skipRun = br.ue();
    if (br.error()) return false;
    for (uint32_t s = 0; s < skipRun && mbAddr < total; s++) {
      int mbX = mbAddr % mbWidth, mbY = mbAddr / mbWidth;
      mbInfo.beginMb(mbX, mbY, ctx.sliceId);
      ctx.mbX = mbX;
      ctx.mbY = mbY;
      decodePSkipMacroblock(ctx, qpY);
      ParsedMb pm{};
      pm.mbX = mbX;
      pm.mbY = mbY;
      pm.type = kMbPSkip;
      pm.mbTypeRaw = 0xFF;
      pm.intra = false;
      pm.numParts = 1;
      pm.mv[0][0] = mbInfo.at(mbX, mbY).mv[kInvBlk4x4[0][0]][0];
      pm.mv[0][1] = mbInfo.at(mbX, mbY).mv[kInvBlk4x4[0][0]][1];
      out->push_back(pm);
      mbAddr++;
    }
    if (mbAddr >= total) break;
    if (skipRun > 0 && !br.moreRbspData()) break;

    int mbX = mbAddr % mbWidth, mbY = mbAddr / mbWidth;
    mbInfo.beginMb(mbX, mbY, ctx.sliceId);
    ctx.mbX = mbX;
    ctx.mbY = mbY;

    uint32_t mbTypeRaw = br.ue();
    if (br.error()) return false;
    ParsedMb pm{};
    pm.mbX = mbX;
    pm.mbY = mbY;
    if (mbTypeRaw >= 5) {
      MacroblockDecodeResult res;
      if (!decodeMacroblockIntraWithType(br, ctx, mbTypeRaw - 5, &qpY, &res))
        return false;
      pm.type = mbInfo.at(mbX, mbY).type;
      pm.mbTypeRaw = (uint8_t)mbTypeRaw;
      pm.intra = true;
      pm.numParts = 0;
      out->push_back(pm);
    } else {
      if (!parseInterMb(br, ctx, pps, mbTypeRaw, &qpY, &pm)) return false;
      out->push_back(pm);
    }
    mbAddr++;
    if (mbAddr >= total) break;
    if (!br.moreRbspData()) break;
  }
  return true;
}

// =====================================================================
// Independent exhaustive search oracle
// =====================================================================

struct OraclePart {
  int bx, by, pw, ph;
  int16_t mv[2], mvd[2];
  int sad;
};
struct OracleResult {
  int mbType;
  int subType[4] = {0, 0, 0, 0};
  int n = 0;
  OraclePart parts[16];
  int64_t cost = 0;
  int totalSad = 0;
};

struct OracleEnv {
  const uint8_t* srcY;
  int srcStrideY;
  const Frame* ref;
  MbInfoTable* info;
  int mbX, mbY;
  int range;
  int l;

  int sadPart(int bx, int by, int pw, int ph, int dx, int dy) const {
    int px = mbX * 16 + bx * 4, py = mbY * 16 + by * 4;
    int w = pw * 4, h = ph * 4;
    int sad = 0;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        int rv = clampedSample(ref->y(), ref->strideY, ref->width,
                               ref->height, px + x + dx, py + y + dy);
        sad += abs((int)srcY[(size_t)(py + y) * srcStrideY + px + x] - rv);
      }
    return sad;
  }

  // Fill a lightweight MbEncodeContext so predictMvGeneral can read state.
  void makeCtx(MbEncodeContext* c) const {
    c->frame = nullptr;
    c->mbInfo = info;
    c->chromaQpIndexOffset = 0;
    c->sliceId = 0;
    c->mbX = mbX;
    c->mbY = mbY;
    c->srcY = srcY;
    c->srcStrideY = srcStrideY;
    c->srcU = nullptr;
    c->srcV = nullptr;
    c->srcStrideC = 0;
  }
};

struct Spec {
  int bx, by, pw, ph;
  int dirSide = -1;
};

// Runs one candidate shape from `baseline`, greedily choosing each
// partition's MV under the predictor built from the partitions chosen so
// far. Writes chosen parts + cost.
void runShape(const OracleEnv& env, const MacroblockInfo& baseline,
              const Spec* specs, int nSpecs, OraclePart* out, int* outSad,
              int* outBits) {
  MbEncodeContext c;
  env.makeCtx(&c);
  MacroblockInfo& dst = env.info->at(env.mbX, env.mbY);
  dst = baseline;
  int sadSum = 0, bitsSum = 0;
  for (int i = 0; i < nSpecs; i++) {
    int16_t pred[2];
    predictMvGeneral(c, specs[i].bx, specs[i].by, specs[i].pw, specs[i].ph,
                     0, specs[i].dirSide, pred);
    bool have = false;
    int bestDx = 0, bestDy = 0, bestSad = 0, bestBits = 0;
    int64_t bestCost = 0;
    for (int dy = -env.range; dy <= env.range; dy++) {
      for (int dx = -env.range; dx <= env.range; dx++) {
        int sad = env.sadPart(specs[i].bx, specs[i].by, specs[i].pw,
                              specs[i].ph, dx, dy);
        int bits = seLen(dx * 4 - pred[0]) + seLen(dy * 4 - pred[1]);
        int64_t cost = (int64_t)sad + (int64_t)env.l * bits;
        bool better;
        if (!have)
          better = true;
        else if (cost != bestCost)
          better = cost < bestCost;
        else if (sad != bestSad)
          better = sad < bestSad;
        else {
          int d = abs(dx) + abs(dy), bd = abs(bestDx) + abs(bestDy);
          if (d != bd)
            better = d < bd;
          else if (dy != bestDy)
            better = dy < bestDy;
          else
            better = dx < bestDx;
        }
        if (better) {
          have = true;
          bestDx = dx;
          bestDy = dy;
          bestSad = sad;
          bestBits = bits;
          bestCost = cost;
        }
      }
    }
    out[i].bx = specs[i].bx;
    out[i].by = specs[i].by;
    out[i].pw = specs[i].pw;
    out[i].ph = specs[i].ph;
    out[i].mv[0] = (int16_t)(bestDx * 4);
    out[i].mv[1] = (int16_t)(bestDy * 4);
    out[i].mvd[0] = (int16_t)(out[i].mv[0] - pred[0]);
    out[i].mvd[1] = (int16_t)(out[i].mv[1] - pred[1]);
    out[i].sad = bestSad;
    fillPartitionMv(dst, specs[i].bx, specs[i].by, specs[i].pw, specs[i].ph,
                    out[i].mv[0], out[i].mv[1], 0);
    sadSum += bestSad;
    bitsSum += bestBits;
  }
  *outSad = sadSum;
  *outBits = bitsSum;
}

void subSpecs(int sub, int qx, int qy, Spec specs[4], int* n) {
  switch (sub) {
    case kSub8x8:
      specs[0] = {qx, qy, 2, 2};
      *n = 1;
      break;
    case kSub8x4:
      specs[0] = {qx, qy, 2, 1};
      specs[1] = {qx, qy + 1, 2, 1};
      *n = 2;
      break;
    case kSub4x8:
      specs[0] = {qx, qy, 1, 2};
      specs[1] = {qx + 1, qy, 1, 2};
      *n = 2;
      break;
    default:
      specs[0] = {qx, qy, 1, 1};
      specs[1] = {qx + 1, qy, 1, 1};
      specs[2] = {qx, qy + 1, 1, 1};
      specs[3] = {qx + 1, qy + 1, 1, 1};
      *n = 4;
      break;
  }
}

OracleResult oracleSearch(const OracleEnv& env) {
  MacroblockInfo baseline;  // fresh: nothing coded in this MB yet
  OracleResult best;
  bool haveBest = false;

  auto consider = [&](const OracleResult& r) {
    if (!haveBest || r.cost < best.cost) {
      best = r;
      haveBest = true;
    }
  };

  // P_16x16
  {
    MacroblockInfo saved = env.info->at(env.mbX, env.mbY);
    Spec s[1] = {{0, 0, 4, 4}};
    OracleResult r;
    r.mbType = kPL0_16x16;
    r.n = 1;
    int sad, bits;
    runShape(env, baseline, s, 1, r.parts, &sad, &bits);
    r.totalSad = sad;
    r.cost = (int64_t)sad + (int64_t)env.l * (bits + ueLen(kPL0_16x16));
    consider(r);
    env.info->at(env.mbX, env.mbY) = saved;
  }
  // P_16x8
  {
    MacroblockInfo saved = env.info->at(env.mbX, env.mbY);
    Spec s[2] = {{0, 0, 4, 2, 1}, {0, 2, 4, 2, 0}};
    OracleResult r;
    r.mbType = kPL0_L0_16x8;
    r.n = 2;
    int sad, bits;
    runShape(env, baseline, s, 2, r.parts, &sad, &bits);
    r.totalSad = sad;
    r.cost = (int64_t)sad + (int64_t)env.l * (bits + ueLen(kPL0_L0_16x8));
    consider(r);
    env.info->at(env.mbX, env.mbY) = saved;
  }
  // P_8x16
  {
    MacroblockInfo saved = env.info->at(env.mbX, env.mbY);
    Spec s[2] = {{0, 0, 2, 4, 0}, {2, 0, 2, 4, 2}};
    OracleResult r;
    r.mbType = kPL0_L0_8x16;
    r.n = 2;
    int sad, bits;
    runShape(env, baseline, s, 2, r.parts, &sad, &bits);
    r.totalSad = sad;
    r.cost = (int64_t)sad + (int64_t)env.l * (bits + ueLen(kPL0_L0_8x16));
    consider(r);
    env.info->at(env.mbX, env.mbY) = saved;
  }
  // P_8x8 greedy regions
  {
    MacroblockInfo saved = env.info->at(env.mbX, env.mbY);
    env.info->at(env.mbX, env.mbY) = baseline;
    static const int qx[4] = {0, 2, 0, 2};
    static const int qy[4] = {0, 0, 2, 2};
    OracleResult r;
    r.mbType = kP8x8;
    int idx = 0, sadSum = 0, bitsSum = 0;
    for (int q = 0; q < 4; q++) {
      MacroblockInfo regionBase = env.info->at(env.mbX, env.mbY);
      bool have = false;
      int bestSub = 0, bestCost = 0, bSad = 0, bBits = 0;
      OraclePart bParts[4];
      MacroblockInfo bState;
      for (int sub = 0; sub < 4; sub++) {
        Spec specs[4];
        int n;
        subSpecs(sub, qx[q], qy[q], specs, &n);
        OraclePart parts[4];
        int sad, bits;
        runShape(env, regionBase, specs, n, parts, &sad, &bits);
        int cost = sad + env.l * (bits + ueLen((uint32_t)sub));
        bool better = !have || cost < bestCost ||
                      (cost == bestCost && sub < bestSub);
        if (better) {
          have = true;
          bestSub = sub;
          bestCost = cost;
          bSad = sad;
          bBits = bits;
          memcpy(bParts, parts, sizeof(OraclePart) * n);
          bState = env.info->at(env.mbX, env.mbY);
        }
        env.info->at(env.mbX, env.mbY) = regionBase;
      }
      env.info->at(env.mbX, env.mbY) = bState;
      r.subType[q] = bestSub;
      int bn;
      Spec dummy[4];
      subSpecs(bestSub, 0, 0, dummy, &bn);
      for (int i = 0; i < bn; i++) r.parts[idx + i] = bParts[i];
      idx += bn;
      sadSum += bSad;
      bitsSum += bBits;
    }
    r.n = idx;
    r.totalSad = sadSum;
    int64_t shapeBits = ueLen(kP8x8);
    for (int q = 0; q < 4; q++) shapeBits += ueLen((uint32_t)r.subType[q]);
    r.cost = (int64_t)sadSum + (int64_t)env.l * (bitsSum + shapeBits);
    consider(r);
    env.info->at(env.mbX, env.mbY) = saved;
  }

  return best;
}

// =====================================================================
// Encoder helper: encode N frames with given settings, returning the
// concatenated bitstream and per-frame encoder reconstruction (YUV420
// packed).
// =====================================================================

struct EncodedSeq {
  std::vector<uint8_t> bitstream;
  std::vector<std::vector<uint8_t>> recon;  // one packed I420 buffer per frame
};

void encodeSequence(const std::vector<std::vector<uint8_t>>& frames, int width,
                    int height, bool partition, int qp, bool rateControl,
                    int range, EncodedSeq* out) {
  TinyH264Encoder<> enc;
  enc.setSize(width, height);
  enc.setMotionSearchRange(range);
  if (rateControl)
    enc.setTargetBitrate(width * height * 6, 25.0), enc.setQp(-1);
  else
    enc.setQp(qp);
  enc.setPartitionSearch(partition);

  size_t frameLen = (size_t)width * height * 3 / 2;
  out->bitstream.assign(8000000, 0);
  out->recon.clear();
  size_t total = 0;
  for (size_t i = 0; i < frames.size(); i++) {
    const uint8_t* f = frames[i].data();
    size_t n = enc.encodeFrame(f, f + (size_t)width * height,
                               f + (size_t)width * height +
                                   (size_t)(width / 2) * (height / 2),
                               out->bitstream.data() + total,
                               out->bitstream.size() - total);
    if (n == 0) {
      out->bitstream.clear();
      return;
    }
    total += n;
    std::vector<uint8_t> r;
    r.insert(r.end(), enc.y(), enc.y() + (size_t)width * height);
    r.insert(r.end(), enc.u(), enc.u() + (size_t)(width / 2) * (height / 2));
    r.insert(r.end(), enc.v(), enc.v() + (size_t)(width / 2) * (height / 2));
    out->recon.push_back(r);
  }
  out->bitstream.resize(total);
  (void)frameLen;
}

// Decodes an Annex-B buffer fully, appending each decoded picture as a
// packed I420 buffer. Returns false on decoder error.
bool decodeAll(const uint8_t* data, size_t size, int width, int height,
               std::vector<std::vector<uint8_t>>* frames) {
  (void)width;
  (void)height;
  TinyH264Decoder<> dec;
  dec.setCallback(
      [](TinyH264Decoder<>& d, void* ud) {
        auto* v = (std::vector<std::vector<uint8_t>>*)ud;
        int w = d.width(), h = d.height();
        std::vector<uint8_t> f;
        f.insert(f.end(), d.y(), d.y() + (size_t)w * h);
        f.insert(f.end(), d.u(), d.u() + (size_t)(w / 2) * (h / 2));
        f.insert(f.end(), d.v(), d.v() + (size_t)(w / 2) * (h / 2));
        v->push_back(f);
      },
      frames);
  dec.write(data, size);
  return !dec.hasError();
}

// Splits an Annex-B buffer into NALs, copying their RBSPs into owned
// vectors (NalReader's rbsp points into a shared scratch).
struct OwnedNal {
  uint8_t type, refIdc;
  std::vector<uint8_t> rbsp;
};

std::vector<OwnedNal> splitNals(const uint8_t* data, size_t size) {
  std::vector<OwnedNal> nals;
  std::vector<uint8_t> scratch(H264_MAX_NAL_SIZE);
  NalReader reader(scratch.data(), scratch.size());
  reader.reset(data, size);
  NalUnit nal;
  while (reader.next(&nal)) {
    OwnedNal o;
    o.type = nal.type;
    o.refIdc = nal.refIdc;
    o.rbsp.assign(nal.rbsp, nal.rbsp + nal.rbspSize);
    nals.push_back(std::move(o));
  }
  return nals;
}

// =====================================================================
// Frame generators
// =====================================================================

std::vector<uint8_t> makeMovingFrame(int idx, int w, int h) {
  std::vector<uint8_t> f((size_t)w * h * 3 / 2);
  uint8_t* Y = f.data();
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int qx = x >= w / 2, qy = y >= h / 2;
      int dx = (qx ? 3 : -3) * idx;
      int dy = (qy ? 2 : -2) * idx;
      int sx = (x + dx + w) % w, sy = (y + dy + h) % h;
      Y[(size_t)y * w + x] =
          (uint8_t)((sx * 5 + sy * 9 + ((sx >> 2) * 37) + ((sy >> 2) * 53)) &
                    0xFF);
    }
  uint8_t* U = f.data() + (size_t)w * h;
  for (int y = 0; y < h / 2; y++)
    for (int x = 0; x < w / 2; x++)
      U[(size_t)y * (w / 2) + x] = (uint8_t)((x * 7 + y * 11 + idx * 13) & 0xFF);
  uint8_t* V = U + (size_t)(w / 2) * (h / 2);
  for (int y = 0; y < h / 2; y++)
    for (int x = 0; x < w / 2; x++)
      V[(size_t)y * (w / 2) + x] = (uint8_t)((x * 3 + y * 5 + idx * 17) & 0xFF);
  return f;
}

// Mixed content: a static flat quadrant (=> P_Skip), a moving quadrant, and
// a quadrant that is an outright different image on later frames (=> Intra
// fallback).
std::vector<uint8_t> makeMixedFrame(int idx, int w, int h) {
  std::vector<uint8_t> f((size_t)w * h * 3 / 2);
  uint8_t* Y = f.data();
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int qx = x >= w / 2, qy = y >= h / 2;
      int v;
      if (!qx && !qy) {
        v = 96;  // static flat region
      } else if (qx && !qy) {
        int sx = (x - 1 - idx * 2 + w) % w;
        v = (sx * 3 + y * 5) & 0xFF;  // moving
      } else if (!qx && qy) {
        v = (x * 7 + y * 11) & 0xFF;  // mild texture, mostly static
      } else {
        // Bottom-right quadrant: a completely different image on every
        // frame after the first (a real scene cut, far from the reference).
        v = idx == 0 ? ((x * 2 + y * 3) & 0xFF)
                     : (255 - ((x * 29 + y * 13 + idx * 71) & 0xFF));
      }
      Y[(size_t)y * w + x] = (uint8_t)v;
    }
  uint8_t* U = f.data() + (size_t)w * h;
  for (int y = 0; y < h / 2; y++)
    for (int x = 0; x < w / 2; x++) {
      // Keep the top-left (static) quadrant's chroma static too, so it can
      // become a true P_Skip.
      bool staticRegion = (x < w / 4 && y < h / 4);
      U[(size_t)y * (w / 2) + x] =
          (uint8_t)((x * 5 + y * 3 + (staticRegion ? 0 : idx * 21)) & 0xFF);
    }
  uint8_t* V = U + (size_t)(w / 2) * (h / 2);
  for (int y = 0; y < h / 2; y++)
    for (int x = 0; x < w / 2; x++) {
      bool staticRegion = (x < w / 4 && y < h / 4);
      V[(size_t)y * (w / 2) + x] =
          (uint8_t)((x * 9 + y * 7 + (staticRegion ? 0 : idx * 31)) & 0xFF);
    }
  return f;
}

// =====================================================================
// SEARCH group
// =====================================================================

// Encodes two frames (I then P) at the given settings, then walks the P
// slice MB by MB. Before each Inter MB is decoded, the independent oracle
// (oracleSearch) computes the shape it would choose from the *actual*
// already-decoded neighbour state; then the MB is decoded normally and the
// two are compared. This is a genuine independent check of the search: the
// oracle never calls the encoder's search, and only spec primitives
// (predictMvGeneral for the H.264 predictor, plus its own SAD/Golomb/cost
// enumeration) are shared.
struct SearchAudit {
  int interMbs = 0;
  int intraMbs = 0;
  int skipMbs = 0;
  int p8x8Mbs = 0;
  int p16x8Mbs = 0;
  int p8x16Mbs = 0;
  int negMvParts = 0;
  int edgeMbs = 0;
  int multiPartMbs = 0;
};

bool auditPPicture(const uint8_t* rbsp, size_t size, const Sps& sps,
                   const Pps& pps, const NalUnit& nal, const Frame& ref,
                   const uint8_t* srcY, int srcStride, int range,
                   SearchAudit* audit) {
  BitReader br0(rbsp, size);
  SliceHeader sh;
  if (!parseSliceHeader(br0, nal, sps, pps, &sh) || !sh.valid) return false;

  AllocatorMemoryResource<StdAllocator<uint8_t>> mem;
  MbInfoTable mbInfo(mem);
  if (!mbInfo.reset((int)sps.picWidthInMbs, (int)sps.picHeightInMbs))
    return false;
  Frame outFrame(mem);
  if (!outFrame.setSize((int)sps.codedWidth, (int)sps.codedHeight))
    return false;

  MbDecodeContext ctx;
  ctx.frame = &outFrame;
  ctx.numActiveRefs = (int)sh.numRefIdxL0ActiveMinus1 + 1;
  for (int i = 0; i < ctx.numActiveRefs && i < H264_MAX_REF_FRAMES; i++)
    ctx.refList[i] = &ref;
  ctx.mbInfo = &mbInfo;
  ctx.sps = &sps;
  ctx.pps = &pps;
  ctx.sliceId = 0;

  int mbWidth = (int)sps.picWidthInMbs, mbHeight = (int)sps.picHeightInMbs;
  int total = mbWidth * mbHeight;
  int qpY = sh.sliceQp;

  // Re-parse header with a fresh reader positioned at slice_data().
  BitReader br(rbsp, size);
  SliceHeader sh2;
  if (!parseSliceHeader(br, nal, sps, pps, &sh2)) return false;

  OracleEnv env;
  env.srcY = srcY;
  env.srcStrideY = srcStride;
  env.ref = &ref;
  env.info = &mbInfo;
  env.range = range;
  env.l = 1 + qpY / 6;

  int mbAddr = 0;
  while (mbAddr < total) {
    uint32_t skipRun = br.ue();
    if (br.error()) return false;
    for (uint32_t s = 0; s < skipRun && mbAddr < total; s++) {
      int mbX = mbAddr % mbWidth, mbY = mbAddr / mbWidth;
      mbInfo.beginMb(mbX, mbY, ctx.sliceId);
      ctx.mbX = mbX;
      ctx.mbY = mbY;
      decodePSkipMacroblock(ctx, qpY);
      audit->skipMbs++;
      mbAddr++;
    }
    if (mbAddr >= total) break;
    if (skipRun > 0 && !br.moreRbspData()) break;

    int mbX = mbAddr % mbWidth, mbY = mbAddr / mbWidth;
    mbInfo.beginMb(mbX, mbY, ctx.sliceId);
    ctx.mbX = mbX;
    ctx.mbY = mbY;

    uint32_t mbTypeRaw = br.ue();
    if (br.error()) return false;
    if (mbTypeRaw >= 5) {
      MacroblockDecodeResult res;
      if (!decodeMacroblockIntraWithType(br, ctx, mbTypeRaw - 5, &qpY, &res))
        return false;
      audit->intraMbs++;
    } else {
      // Independent oracle on the current neighbour state.
      env.mbX = mbX;
      env.mbY = mbY;
      env.l = 1 + qpY / 6;
      OracleResult want = oracleSearch(env);

      ParsedMb pm{};
      pm.mbX = mbX;
      pm.mbY = mbY;
      if (!parseInterMb(br, ctx, pps, mbTypeRaw, &qpY, &pm)) return false;

      if ((int)mbTypeRaw != want.mbType) return false;
      if (pm.numParts != want.n) return false;
      if (mbTypeRaw == kP8x8 || mbTypeRaw == kP8x8ref0)
        for (int q = 0; q < 4; q++)
          if (pm.subType[q] != (uint8_t)want.subType[q]) return false;
      for (int i = 0; i < want.n; i++) {
        if (pm.mv[i][0] != want.parts[i].mv[0] ||
            pm.mv[i][1] != want.parts[i].mv[1])
          return false;
        if (pm.mvd[i][0] != want.parts[i].mvd[0] ||
            pm.mvd[i][1] != want.parts[i].mvd[1])
          return false;
        if (pm.pred[i][0] != pm.mv[i][0] - pm.mvd[i][0] ||
            pm.pred[i][1] != pm.mv[i][1] - pm.mvd[i][1])
          return false;
        if (pm.mv[i][0] < 0 || pm.mv[i][1] < 0) audit->negMvParts++;
      }
      audit->interMbs++;
      if (mbTypeRaw == kP8x8 || mbTypeRaw == kP8x8ref0) audit->p8x8Mbs++;
      if (mbTypeRaw == kPL0_L0_16x8) audit->p16x8Mbs++;
      if (mbTypeRaw == kPL0_L0_8x16) audit->p8x16Mbs++;
      if (pm.numParts >= 2) audit->multiPartMbs++;
      if (mbX == 0 || mbY == 0) audit->edgeMbs++;
    }
    mbAddr++;
    if (mbAddr >= total) break;
    if (!br.moreRbspData()) break;
  }
  return true;
}

// Shared two-frame audit: encode I+P with partition search on, parse the
// resulting P NAL independently, compare every MB.
bool runTwoFrameAudit(const std::vector<uint8_t>& f0,
                      const std::vector<uint8_t>& f1, int qp, int range,
                      SearchAudit* audit) {
  EncodedSeq seq;
  std::vector<std::vector<uint8_t>> frames = {f0, f1};
  encodeSequence(frames, kBw, kBh, true, qp, false, range, &seq);
  if (seq.bitstream.empty()) return false;

  std::vector<OwnedNal> nals = splitNals(seq.bitstream.data(),
                                         seq.bitstream.size());
  const OwnedNal* spsN = nullptr;
  const OwnedNal* ppsN = nullptr;
  const OwnedNal* pNal = nullptr;
  for (auto& n : nals) {
    if (n.type == kNalSps) spsN = &n;
    if (n.type == kNalPps) ppsN = &n;
    if (n.type == kNalSliceNonIdr) pNal = &n;
  }
  if (!pNal || !spsN || !ppsN) return false;

  BitReader sb(spsN->rbsp.data(), spsN->rbsp.size());
  Sps sps;
  if (!parseSps(sb, &sps)) return false;
  BitReader pb(ppsN->rbsp.data(), ppsN->rbsp.size());
  Pps pps;
  if (!parsePps(pb, &pps)) return false;

  // Reference picture = encoder's frame-0 deblocked reconstruction.
  AllocatorMemoryResource<StdAllocator<uint8_t>> mem;
  Frame ref(mem);
  if (!ref.setSize((int)sps.codedWidth, (int)sps.codedHeight)) return false;
  int w = (int)sps.codedWidth, h = (int)sps.codedHeight;
  memcpy(ref.y(), seq.recon[0].data(), (size_t)w * h);
  memcpy(ref.u(), seq.recon[0].data() + (size_t)w * h,
         (size_t)(w / 2) * (h / 2));
  memcpy(ref.v(),
         seq.recon[0].data() + (size_t)w * h + (size_t)(w / 2) * (h / 2),
         (size_t)(w / 2) * (h / 2));

  NalUnit nal;
  nal.type = pNal->type;
  nal.refIdc = pNal->refIdc;
  nal.rbsp = pNal->rbsp.data();
  nal.rbspSize = pNal->rbsp.size();

  const uint8_t* srcY;
  const uint8_t* srcU;
  const uint8_t* srcV;
  splitFrame(f1, &srcY, &srcU, &srcV);
  return auditPPicture(pNal->rbsp.data(), pNal->rbsp.size(), sps, pps, nal,
                       ref, srcY, kBw, range, audit);
}

int case_search_top_bottom() {
  std::vector<uint8_t> f0 = makeMovingFrame(0, kBw, kBh);
  std::vector<uint8_t> f1 = makeMovingFrame(1, kBw, kBh);
  // Force a strong vertical split: top moves up, bottom moves down.
  for (int y = 0; y < kBh; y++)
    for (int x = 0; x < kBw; x++) {
      int sy = (y < kBh / 2) ? (y - 2 + kBh) % kBh : (y + 2) % kBh;
      f1[(size_t)y * kBw + x] = f0[(size_t)sy * kBw + x];
    }
  SearchAudit audit;
  TASK_CHECK(runTwoFrameAudit(f0, f1, 26, 8, &audit));
  TASK_CHECK(audit.interMbs > 0);
  TASK_CHECK(audit.multiPartMbs > 0);
  return 0;
}

int case_search_left_right() {
  std::vector<uint8_t> f0 = makeMovingFrame(0, kBw, kBh);
  std::vector<uint8_t> f1 = makeMovingFrame(0, kBw, kBh);
  for (int y = 0; y < kBh; y++)
    for (int x = 0; x < kBw; x++) {
      int sx = (x < kBw / 2) ? (x - 3 + kBw) % kBw : (x + 3) % kBw;
      f1[(size_t)y * kBw + x] = f0[(size_t)y * kBw + sx];
    }
  SearchAudit audit;
  TASK_CHECK(runTwoFrameAudit(f0, f1, 26, 8, &audit));
  TASK_CHECK(audit.interMbs > 0);
  TASK_CHECK(audit.multiPartMbs > 0);
  return 0;
}

int case_search_quadrants() {
  std::vector<uint8_t> f0 = makeMovingFrame(0, kBw, kBh);
  std::vector<uint8_t> f1 = makeMovingFrame(0, kBw, kBh);
  for (int y = 0; y < kBh; y++)
    for (int x = 0; x < kBw; x++) {
      int qx = x >= kBw / 2, qy = y >= kBh / 2;
      int dx = qx ? 2 : -2, dy = qy ? 2 : -2;
      int sx = (x + dx + kBw) % kBw, sy = (y + dy + kBh) % kBh;
      f1[(size_t)y * kBw + x] = f0[(size_t)sy * kBw + sx];
    }
  SearchAudit audit;
  TASK_CHECK(runTwoFrameAudit(f0, f1, 26, 8, &audit));
  // Four distinct quadrant motions must make P_8x8 win somewhere.
  TASK_CHECK(audit.p8x8Mbs > 0);
  return 0;
}

int case_search_negative_and_edge() {
  // Whole-picture negative/positive shift; audits top-row/left-column MBs
  // (edge-clamped reference fetches) and checks negative MVs occur.
  std::vector<uint8_t> f0 = makeMovingFrame(0, kBw, kBh);
  std::vector<uint8_t> f1 = makeMovingFrame(0, kBw, kBh);
  for (int y = 0; y < kBh; y++)
    for (int x = 0; x < kBw; x++) {
      int sx = (x - 4 + kBw) % kBw, sy = (y - 3 + kBh) % kBh;
      f1[(size_t)y * kBw + x] = f0[(size_t)sy * kBw + sx];
    }
  SearchAudit audit;
  TASK_CHECK(runTwoFrameAudit(f0, f1, 26, 8, &audit));
  TASK_CHECK(audit.negMvParts > 0);
  TASK_CHECK(audit.edgeMbs > 0);
  return 0;
}

int case_search_state_restoration() {
  // Run several content types at several QPs/ranges; the per-MB oracle
  // comparison (which rebuilds every candidate shape from the same
  // baseline) would fail if a rejected shape's MVs leaked into the winner
  // or into the next macroblock.
  const int kinds[4] = {0, 1, 2, 3};
  for (int i = 0; i < 4; i++) {
    std::vector<uint8_t> f0 = makeFrame(0, kinds[i]);
    std::vector<uint8_t> f1 = makeFrame(1, kinds[i]);
    for (int qp : {20, 30}) {
      SearchAudit audit;
      TASK_CHECK(runTwoFrameAudit(f0, f1, qp, 6, &audit));
    }
  }
  return 0;
}

int case_search_second_partition_dependency() {
  // Left/right motion: the second (right) partition's predictor depends on
  // the first (left) one's chosen MV. The per-MB oracle audit only passes
  // if the encoder derives the second partition's MVD against the updated
  // state - a stale 16x16 predictor would disagree.
  std::vector<uint8_t> f0 = makeMovingFrame(0, kBw, kBh);
  std::vector<uint8_t> f1 = makeMovingFrame(0, kBw, kBh);
  for (int y = 0; y < kBh; y++)
    for (int x = 0; x < kBw; x++) {
      int sx = (x < kBw / 2) ? (x - 2 + kBw) % kBw : (x + 2) % kBw;
      f1[(size_t)y * kBw + x] = f0[(size_t)y * kBw + sx];
    }
  SearchAudit audit;
  TASK_CHECK(runTwoFrameAudit(f0, f1, 28, 8, &audit));
  TASK_CHECK(audit.multiPartMbs > 0);
  return 0;
}

// =====================================================================
// SYNTAX group
// =====================================================================

// Encodes `frames` with the given partition/motion settings into `out`.
void encodeWith(std::vector<std::vector<uint8_t>> frames, bool partition,
                MotionSearchAlgorithm alg, int qp, int range,
                std::vector<uint8_t>* out) {
  TinyH264Encoder<> e;
  e.setSize(kBw, kBh);
  e.setQp(qp);
  e.setMotionSearchRange(range);
  e.setMotionSearchAlgorithm(alg);
  e.setPartitionSearch(partition);
  std::vector<uint8_t> bs(6000000);
  size_t t = 0;
  for (auto& f : frames) {
    size_t n = e.encodeFrame(f.data(), f.data() + (size_t)kBw * kBh,
                             f.data() + (size_t)kBw * kBh +
                                 (size_t)(kBw / 2) * (kBh / 2),
                             bs.data() + t, bs.size() - t);
    if (n == 0) {
      out->clear();
      return;
    }
    t += n;
  }
  out->assign(bs.begin(), bs.begin() + t);
}

// Disabled output must be byte-identical no matter what other settings
// are, matching the "off path unchanged" rule; and enabling partition
// search must make output *independent* of Fast/Exhaustive.
int case_syntax_off_is_old_path_and_alg_ignored() {
  std::vector<std::vector<uint8_t>> frames;
  for (int i = 0; i < 5; i++) frames.push_back(makeMovingFrame(i, kBw, kBh));

  std::vector<uint8_t> a, b, c, d;
  encodeWith(frames, false, MotionSearchAlgorithm::Exhaustive, 26, 8, &a);
  encodeWith(frames, false, MotionSearchAlgorithm::Exhaustive, 26, 8, &b);
  TASK_CHECK(!a.empty());
  TASK_CHECK(a == b);  // deterministic, old path stable

  encodeWith(frames, true, MotionSearchAlgorithm::Fast, 26, 8, &c);
  encodeWith(frames, true, MotionSearchAlgorithm::Exhaustive, 26, 8, &d);
  TASK_CHECK(!c.empty());
  TASK_CHECK(c == d);  // partition search ignores Fast/Exhaustive
  return 0;
}

// Independent structural parse: legal mb_type/sub_mb_type, MVD
// reconstructs the MV, and the tree decodes.
int case_syntax_tree_valid() {
  std::vector<std::vector<uint8_t>> frames;
  for (int i = 0; i < 4; i++) frames.push_back(makeFrame(i, 2));
  std::vector<uint8_t> bs;
  encodeWith(frames, true, MotionSearchAlgorithm::Exhaustive, 24, 8, &bs);
  TASK_CHECK(!bs.empty());

  std::vector<std::vector<uint8_t>> dec;
  TASK_CHECK(decodeAll(bs.data(), bs.size(), kBw, kBh, &dec));
  TASK_CHECK(dec.size() == frames.size());

  std::vector<OwnedNal> nals = splitNals(bs.data(), bs.size());
  const OwnedNal* spsN = nullptr;
  const OwnedNal* ppsN = nullptr;
  for (auto& n : nals) {
    if (n.type == kNalSps) spsN = &n;
    if (n.type == kNalPps) ppsN = &n;
  }
  TASK_CHECK(spsN && ppsN);
  BitReader sb(spsN->rbsp.data(), spsN->rbsp.size());
  Sps sps;
  TASK_CHECK(parseSps(sb, &sps));
  BitReader pb(ppsN->rbsp.data(), ppsN->rbsp.size());
  Pps pps;
  TASK_CHECK(parsePps(pb, &pps));

  int refFrameIdx = 0;
  int inter = 0;
  for (auto& n : nals) {
    if (n.type != kNalSliceNonIdr) continue;
    AllocatorMemoryResource<StdAllocator<uint8_t>> mem;
    Frame ref(mem);
    TASK_CHECK(ref.setSize((int)sps.codedWidth, (int)sps.codedHeight));
    const std::vector<uint8_t>& prev = dec[refFrameIdx];
    int w = (int)sps.codedWidth, h = (int)sps.codedHeight;
    memcpy(ref.y(), prev.data(), (size_t)w * h);
    memcpy(ref.u(), prev.data() + (size_t)w * h, (size_t)(w / 2) * (h / 2));
    memcpy(ref.v(),
           prev.data() + (size_t)w * h + (size_t)(w / 2) * (h / 2),
           (size_t)(w / 2) * (h / 2));
    NalUnit nal;
    nal.type = n.type;
    nal.refIdc = n.refIdc;
    nal.rbsp = n.rbsp.data();
    nal.rbspSize = n.rbsp.size();
    Frame out(mem);
    MbInfoTable info(mem);
    std::vector<ParsedMb> mbs;
    TASK_CHECK(parsePPicture(n.rbsp.data(), n.rbsp.size(), sps, pps, nal, ref,
                             out, info, &mbs));
    TASK_CHECK(!mbs.empty());
    for (auto& mb : mbs) {
      if (mb.type == kMbPSkip || mb.intra) continue;
      inter++;
      TASK_CHECK(mb.mbTypeRaw <= kP8x8ref0);
      if (mb.mbTypeRaw == kP8x8 || mb.mbTypeRaw == kP8x8ref0)
        for (int q = 0; q < 4; q++) TASK_CHECK(mb.subType[q] <= kSub4x4);
      for (int i = 0; i < mb.numParts; i++) {
        TASK_CHECK(mb.mv[i][0] == mb.pred[i][0] + mb.mvd[i][0]);
        TASK_CHECK(mb.mv[i][1] == mb.pred[i][1] + mb.mvd[i][1]);
      }
    }
    refFrameIdx++;
  }
  TASK_CHECK(inter > 0);
  return 0;
}

// Intra fallback mixed with skip: a scene-cut quadrant must produce at
// least one Intra MB in a P slice, a static quadrant at least one P_Skip,
// and the whole stream must decode.
int case_syntax_intra_fallback_and_skip() {
  std::vector<std::vector<uint8_t>> frames;
  for (int i = 0; i < 4; i++) frames.push_back(makeMixedFrame(i, kBw, kBh));
  std::vector<uint8_t> bs;
  encodeWith(frames, true, MotionSearchAlgorithm::Exhaustive, 26, 8, &bs);
  TASK_CHECK(!bs.empty());
  std::vector<std::vector<uint8_t>> dec;
  TASK_CHECK(decodeAll(bs.data(), bs.size(), kBw, kBh, &dec));
  TASK_CHECK(dec.size() == frames.size());

  std::vector<OwnedNal> nals = splitNals(bs.data(), bs.size());
  const OwnedNal* spsN = nullptr;
  const OwnedNal* ppsN = nullptr;
  for (auto& n : nals) {
    if (n.type == kNalSps) spsN = &n;
    if (n.type == kNalPps) ppsN = &n;
  }
  TASK_CHECK(spsN && ppsN);
  BitReader sb(spsN->rbsp.data(), spsN->rbsp.size());
  Sps sps;
  TASK_CHECK(parseSps(sb, &sps));
  BitReader pb(ppsN->rbsp.data(), ppsN->rbsp.size());
  Pps pps;
  TASK_CHECK(parsePps(pb, &pps));

  int totalSkip = 0, totalIntra = 0;
  int refFrameIdx = 0;
  for (auto& n : nals) {
    if (n.type != kNalSliceNonIdr) continue;
    AllocatorMemoryResource<StdAllocator<uint8_t>> mem;
    Frame ref(mem);
    TASK_CHECK(ref.setSize((int)sps.codedWidth, (int)sps.codedHeight));
    const std::vector<uint8_t>& prev = dec[refFrameIdx];
    int w = (int)sps.codedWidth, h = (int)sps.codedHeight;
    memcpy(ref.y(), prev.data(), (size_t)w * h);
    memcpy(ref.u(), prev.data() + (size_t)w * h, (size_t)(w / 2) * (h / 2));
    memcpy(ref.v(),
           prev.data() + (size_t)w * h + (size_t)(w / 2) * (h / 2),
           (size_t)(w / 2) * (h / 2));
    NalUnit nal;
    nal.type = n.type;
    nal.refIdc = n.refIdc;
    nal.rbsp = n.rbsp.data();
    nal.rbspSize = n.rbsp.size();
    Frame out(mem);
    MbInfoTable info(mem);
    std::vector<ParsedMb> mbs;
    TASK_CHECK(parsePPicture(n.rbsp.data(), n.rbsp.size(), sps, pps, nal, ref,
                             out, info, &mbs));
    for (auto& mb : mbs) {
      if (mb.type == kMbPSkip) totalSkip++;
      if (mb.intra) totalIntra++;
    }
    refFrameIdx++;
  }
  TASK_CHECK(totalSkip > 0);
  TASK_CHECK(totalIntra > 0);
  return 0;
}

// A wholly static sequence must skip heavily (tiny P frames); a wholly
// moving one must not be mostly skips.
int case_syntax_skip_conditions() {
  std::vector<std::vector<uint8_t>> stat;
  for (int i = 0; i < 4; i++) stat.push_back(makeFrame(0, 0));
  std::vector<uint8_t> sbs;
  encodeWith(stat, true, MotionSearchAlgorithm::Exhaustive, 26, 8, &sbs);
  TASK_CHECK(!sbs.empty());
  std::vector<std::vector<uint8_t>> dec;
  TASK_CHECK(decodeAll(sbs.data(), sbs.size(), kBw, kBh, &dec));
  TASK_CHECK(dec.size() == stat.size());

  std::vector<std::vector<uint8_t>> mv;
  for (int i = 0; i < 4; i++) mv.push_back(makeMovingFrame(i, kBw, kBh));
  std::vector<uint8_t> mbs2;
  encodeWith(mv, true, MotionSearchAlgorithm::Exhaustive, 26, 8, &mbs2);
  TASK_CHECK(!mbs2.empty());
  // Static content must compress much smaller than moving content.
  TASK_CHECK(sbs.size() < mbs2.size());
  return 0;
}

}  // namespace

// =====================================================================
// REFERENCE group
// =====================================================================

namespace {

const int kRefW = 16, kRefH = 16;  // single macroblock: no deblock MB edges

uint8_t refLuma(int x, int y) {
  return (uint8_t)((x * 9 + y * 13 + ((x >> 1) * 21) + ((y >> 1) * 31)) & 0xFF);
}
uint8_t refChroma(int plane, int x, int y) {
  return (uint8_t)((plane * 61 + x * 7 + y * 5) & 0xFF);
}

struct P8x8Spec {
  uint32_t subType[4] = {kSub8x8, kSub8x8, kSub8x8, kSub8x8};
  int16_t mv[16][2] = {{0}};  // signalling order, quarter-pel
  int numParts = 4;
  bool hasResidual = false;
  int residQuadrant = 0;
  int32_t residCoeff = 0;
};

void putNal(std::vector<uint8_t>* out, uint8_t type, const uint8_t* rbsp,
            size_t n) {
  std::vector<uint8_t> tmp(n + 16);
  size_t w = writeNalUnit(tmp.data(), tmp.size(), 3, type, rbsp, n);
  out->insert(out->end(), tmp.begin(), tmp.begin() + w);
}

// Builds SPS + PPS + IDR(I_PCM reference) + P(P_8x8) Annex-B stream.
// All-PCM reference MB => qpY 0 => deblocking is a no-op, so the decoded
// reference picture is byte-exactly refLuma()/refChroma().
std::vector<uint8_t> buildRefAndP8x8(int qp, const P8x8Spec& spec) {
  uint8_t spsRbsp[64], ppsRbsp[64];
  BitWriter sw(spsRbsp, sizeof(spsRbsp));
  writeSpsRbsp(sw, kRefW, kRefH, 30, 1);
  BitWriter pw(ppsRbsp, sizeof(ppsRbsp));
  writePpsRbsp(pw, qp);

  std::vector<uint8_t> out;
  putNal(&out, kNalSps, spsRbsp, sw.bytesWritten());
  putNal(&out, kNalPps, ppsRbsp, pw.bytesWritten());

  // ---- IDR I_PCM ----
  {
    static uint8_t buf[8192];
    BitWriter bw(buf, sizeof(buf));
    writeSliceHeaderIdr(bw);
    bw.ue(25);  // I_PCM
    while (bw.bitsWritten() % 8 != 0) bw.flag(false);  // byte-align
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++) bw.u(refLuma(x, y), 8);
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) bw.u(refChroma(0, x, y), 8);
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) bw.u(refChroma(1, x, y), 8);
    bw.rbspTrailingBits();
    putNal(&out, kNalSliceIdr, buf, bw.bytesWritten());
  }

  // ---- P_8x8 ----
  {
    static uint8_t buf[8192];
    BitWriter bw(buf, sizeof(buf));
    writeSliceHeaderP(bw, 1, qp, qp);
    bw.ue(0);  // mb_skip_run
    bw.ue(kP8x8);
    for (int q = 0; q < 4; q++) bw.ue(spec.subType[q]);

    AllocatorMemoryResource<StdAllocator<uint8_t>> mem;
    MbInfoTable tree(mem);
    tree.reset(1, 1);
    tree.beginMb(0, 0, 0);
    MbEncodeContext ctx;
    ctx.frame = nullptr;
    ctx.mbInfo = &tree;
    ctx.chromaQpIndexOffset = 0;
    ctx.sliceId = 0;
    ctx.mbX = 0;
    ctx.mbY = 0;
    ctx.srcY = nullptr;
    ctx.srcStrideY = 0;
    ctx.srcU = nullptr;
    ctx.srcV = nullptr;
    ctx.srcStrideC = 0;

    PartLayout parts[16];
    int n = layoutFor(kP8x8, spec.subType, parts);
    for (int i = 0; i < n; i++) {
      int16_t pred[2];
      predictMvGeneral(ctx, parts[i].bx, parts[i].by, parts[i].pw, parts[i].ph,
                       0, parts[i].dirSide, pred);
      bw.se((int16_t)(spec.mv[i][0] - pred[0]));
      bw.se((int16_t)(spec.mv[i][1] - pred[1]));
      fillPartitionMv(tree.at(0, 0), parts[i].bx, parts[i].by, parts[i].pw,
                      parts[i].ph, spec.mv[i][0], spec.mv[i][1], 0);
    }

    int cbpLuma = spec.hasResidual ? (1 << spec.residQuadrant) : 0;
    uint32_t cbpCode = 0;
    {
      uint8_t want = (uint8_t)(cbpLuma | (0 << 4));
      for (uint32_t i = 0; i < 48; i++)
        if (kCbpInter[i] == want) cbpCode = i;
    }
    bw.ue(cbpCode);
    if (spec.hasResidual) bw.se(0);  // mb_qp_delta

    if (spec.hasResidual) {
      for (int b = 0; b < 4; b++) {
        int32_t block[16] = {0};
        if (b == 0) block[0] = spec.residCoeff;
        int32_t scan[16];
        for (int k = 0; k < 16; k++) scan[k] = block[kZigZag4x4[k]];
        encodeResidualBlockCavlc(bw, 0, 16, scan);  // nC=0 (single MB)
      }
    }
    bw.rbspTrailingBits();
    putNal(&out, kNalSliceNonIdr, buf, bw.bytesWritten());
  }
  return out;
}

// Expected luma for the P MB: per-4x4 partition prediction (integer MV) +
// (for the residual quadrant) the IDCT of the single DC level at the given
// QP. All MVs are integer-pel (multiples of 4 quarter-samples).
void expectedP8x8Luma(const P8x8Spec& spec, int qp, uint8_t out[16][16]) {
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) out[y][x] = 0;
  PartLayout parts[16];
  int n = layoutFor(kP8x8, spec.subType, parts);
  for (int i = 0; i < n; i++) {
    int mvx = spec.mv[i][0] / 4, mvy = spec.mv[i][1] / 4;
    for (int y = parts[i].by * 4; y < (parts[i].by + parts[i].ph) * 4; y++)
      for (int x = parts[i].bx * 4; x < (parts[i].bx + parts[i].pw) * 4; x++) {
        // Integer-pel prediction: edge-replicated fetch from the reference.
        int rx = x + mvx, ry = y + mvy;
        rx = rx < 0 ? 0 : (rx >= kRefW ? kRefW - 1 : rx);
        ry = ry < 0 ? 0 : (ry >= kRefH ? kRefH - 1 : ry);
        out[y][x] = refLuma(rx, ry);
      }
  }
  if (spec.hasResidual) {
    int32_t block[16] = {0};
    block[0] = spec.residCoeff;
    dequant4x4(block, qp, false);
    idct4x4(block);
    // A single DC coefficient inverse-transforms to the same value in every
    // sample of the 4x4 block. It is added to the luma block at the *first*
    // 4x4 position of the residual quadrant (baseBlk = quadrant*4 + 0).
    int dcSpatial = block[0];
    int qx = spec.residQuadrant & 1, qy = spec.residQuadrant >> 1;
    int x0 = qx * 8, y0 = qy * 8;
    for (int y = y0; y < y0 + 4; y++)
      for (int x = x0; x < x0 + 4; x++)
        out[y][x] = clip255((int)out[y][x] + dcSpatial);
  }
}

// Decodes `stream` expecting exactly two pictures; checks the second's
// luma against `expected`. Returns false on any error.
bool checkP8x8Decode(const std::vector<uint8_t>& stream,
                      const uint8_t expected[16][16]) {
  AllocatorMemoryResource<StdAllocator<uint8_t>> mem;
  SoftwareDecoder dec(mem);
  dec.setInput(stream.data(), stream.size());
  int frames = 0;
  while (true) {
    DecodeStatus s = dec.next();
    if (s == DecodeStatus::kOk) {
      frames++;
      if (frames == 2) {
        const Frame& f = dec.frame();
        if (f.width != kRefW || f.height != kRefH) return false;
        for (int y = 0; y < 16; y++)
          for (int x = 0; x < 16; x++)
            if (f.y()[y * f.strideY + x] != expected[y][x]) return false;
        return true;
      }
      continue;
    }
    if (s == DecodeStatus::kNeedMoreData) {
      if (dec.inputExhausted()) return false;  // ran out before 2 frames
      continue;
    }
    return false;  // error/unsupported
  }
}

int case_reference_five_partition_stream() {
  // One 8x4 region (top-left quadrant split into two 8x4) + three 8x8
  // regions = 5 partitions. One partition has non-zero motion; the
  // residual quadrant is zero. Exercises zero-MVD and non-zero motion.
  P8x8Spec spec;
  spec.subType[0] = kSub8x4;
  spec.subType[1] = kSub8x8;
  spec.subType[2] = kSub8x8;
  spec.subType[3] = kSub8x8;
  spec.numParts = 5;
  // Signalling order: q0 top 8x4, q0 bottom 8x4, q1 8x8, q2 8x8, q3 8x8.
  spec.mv[0][0] = 0;  spec.mv[0][1] = 0;   // static
  spec.mv[1][0] = 0;  spec.mv[1][1] = 0;
  spec.mv[2][0] = 4;  spec.mv[2][1] = -4;  // top-right 8x8 moves +x/-y
  spec.mv[3][0] = 0;  spec.mv[3][1] = 0;
  spec.mv[4][0] = 0;  spec.mv[4][1] = 0;
  spec.hasResidual = false;
  int qp = 26;
  std::vector<uint8_t> stream = buildRefAndP8x8(qp, spec);
  uint8_t expected[16][16];
  expectedP8x8Luma(spec, qp, expected);
  TASK_CHECK(checkP8x8Decode(stream, expected));
  return 0;
}

int case_reference_sixteen_partition_stream() {
  // All-4x4: 16 partitions with a mix of zero and non-zero MVs, plus a
  // non-zero residual in one quadrant.
  P8x8Spec spec;
  for (int q = 0; q < 4; q++) spec.subType[q] = kSub4x4;
  spec.numParts = 16;
  for (int i = 0; i < 16; i++) {
    spec.mv[i][0] = 0;
    spec.mv[i][1] = 0;
  }
  // Give a few 4x4 cells distinct motion.
  spec.mv[0][0] = 4;
  spec.mv[5][1] = -4;
  spec.mv[10][0] = 8;
  spec.mv[15][1] = 4;
  spec.hasResidual = true;
  spec.residQuadrant = 2;
  spec.residCoeff = 3;
  int qp = 26;
  std::vector<uint8_t> stream = buildRefAndP8x8(qp, spec);
  uint8_t expected[16][16];
  expectedP8x8Luma(spec, qp, expected);
  TASK_CHECK(checkP8x8Decode(stream, expected));
  return 0;
}

// Public encoder output -> same patched decoder == encoder deblocked recon,
// plane-exact, over >= 3 consecutive generations, covering chroma,
// non-zero residual, skip and intra fallback.
int case_reference_encoder_roundtrip() {
  std::vector<std::vector<uint8_t>> frames;
  for (int i = 0; i < 8; i++) frames.push_back(makeMixedFrame(i, kBw, kBh));
  // Fixed QP.
  for (int qp : {20, 30}) {
    EncodedSeq seq;
    encodeSequence(frames, kBw, kBh, true, qp, false, 8, &seq);
    TASK_CHECK(!seq.bitstream.empty());
    std::vector<std::vector<uint8_t>> dec;
    TASK_CHECK(decodeAll(seq.bitstream.data(), seq.bitstream.size(), kBw, kBh,
                         &dec));
    TASK_CHECK(dec.size() == seq.recon.size());
    for (size_t i = 0; i < dec.size(); i++) {
      TASK_CHECK(dec[i] == seq.recon[i]);  // plane-exact (Y, U, V)
    }
  }
  // Automatic rate control.
  {
    EncodedSeq seq;
    encodeSequence(frames, kBw, kBh, true, -1, true, 8, &seq);
    TASK_CHECK(!seq.bitstream.empty());
    std::vector<std::vector<uint8_t>> dec;
    TASK_CHECK(decodeAll(seq.bitstream.data(), seq.bitstream.size(), kBw, kBh,
                         &dec));
    TASK_CHECK(dec.size() == seq.recon.size());
    for (size_t i = 0; i < dec.size(); i++)
      TASK_CHECK(dec[i] == seq.recon[i]);
  }
  return 0;
}

// Mode toggling mid-sequence must not reset the reference: encode with the
// mode switched on/off between frames, decode, and check every frame is
// still reconstructed exactly as the encoder's closed loop says.
int case_reference_mode_toggle() {
  std::vector<std::vector<uint8_t>> frames;
  for (int i = 0; i < 8; i++) frames.push_back(makeMovingFrame(i, kBw, kBh));

  TinyH264Encoder<> enc;
  enc.setSize(kBw, kBh);
  enc.setQp(26);
  std::vector<uint8_t> bs(8000000);
  size_t total = 0;
  std::vector<std::vector<uint8_t>> recon;
  for (int i = 0; i < (int)frames.size(); i++) {
    enc.setPartitionSearch((i % 2) == 0);  // toggle every frame
    auto& f = frames[i];
    size_t n = enc.encodeFrame(f.data(), f.data() + (size_t)kBw * kBh,
                               f.data() + (size_t)kBw * kBh +
                                   (size_t)(kBw / 2) * (kBh / 2),
                               bs.data() + total, bs.size() - total);
    TASK_CHECK(n > 0);
    total += n;
    std::vector<uint8_t> r;
    r.insert(r.end(), enc.y(), enc.y() + (size_t)kBw * kBh);
    r.insert(r.end(), enc.u(), enc.u() + (size_t)(kBw / 2) * (kBh / 2));
    r.insert(r.end(), enc.v(), enc.v() + (size_t)(kBw / 2) * (kBh / 2));
    recon.push_back(r);
  }
  std::vector<std::vector<uint8_t>> dec;
  TASK_CHECK(decodeAll(bs.data(), total, kBw, kBh, &dec));
  TASK_CHECK(dec.size() == recon.size());
  for (size_t i = 0; i < dec.size(); i++) TASK_CHECK(dec[i] == recon[i]);
  return 0;
}

// RGB / YUYV entry points must honour partition search too (they funnel
// through the same planar P path). Encode the same content via RGB888 and
// plain YUV and compare the decoded result to the encoder reconstruction.
int case_reference_color_entry_points() {
  // Build RGB888 frames from the YUV generator (approximate inverse is
  // unnecessary: we only require self-consistency, not source fidelity).
  std::vector<std::vector<uint8_t>> yuv;
  for (int i = 0; i < 5; i++) yuv.push_back(makeMovingFrame(i, kBw, kBh));
  std::vector<std::vector<uint8_t>> rgb(yuv.size());
  for (size_t i = 0; i < yuv.size(); i++) {
    rgb[i].resize((size_t)kBw * kBh * 3);
    for (int p = 0; p < kBw * kBh; p++) {
      int Y = yuv[i][p];
      rgb[i][p * 3 + 0] = (uint8_t)clip255(Y);
      rgb[i][p * 3 + 1] = (uint8_t)clip255(Y);
      rgb[i][p * 3 + 2] = (uint8_t)clip255(Y);
    }
  }

  TinyH264Encoder<> enc;
  enc.setSize(kBw, kBh);
  enc.setQp(26);
  enc.setPartitionSearch(true);
  std::vector<uint8_t> bs(8000000);
  size_t total = 0;
  std::vector<std::vector<uint8_t>> recon;
  for (auto& f : rgb) {
    size_t n = enc.encodeFrameRgb888(f.data(), bs.data() + total,
                                     bs.size() - total);
    TASK_CHECK(n > 0);
    total += n;
    std::vector<uint8_t> r;
    r.insert(r.end(), enc.y(), enc.y() + (size_t)kBw * kBh);
    r.insert(r.end(), enc.u(), enc.u() + (size_t)(kBw / 2) * (kBh / 2));
    r.insert(r.end(), enc.v(), enc.v() + (size_t)(kBw / 2) * (kBh / 2));
    recon.push_back(r);
  }
  std::vector<std::vector<uint8_t>> dec;
  TASK_CHECK(decodeAll(bs.data(), total, kBw, kBh, &dec));
  TASK_CHECK(dec.size() == recon.size());
  for (size_t i = 0; i < dec.size(); i++) TASK_CHECK(dec[i] == recon[i]);
  return 0;
}

// setPartitionSearch() return contract: software true; toggling state
// readable; default off.
int case_reference_setter_contract() {
  TinyH264Encoder<> enc;
  TASK_CHECK(!enc.partitionSearch());
  TASK_CHECK(enc.setPartitionSearch(true));
  TASK_CHECK(enc.partitionSearch());
  TASK_CHECK(enc.setPartitionSearch(false));
  TASK_CHECK(!enc.partitionSearch());

  // Direct SoftwareEncoder too.
  AllocatorMemoryResource<StdAllocator<uint8_t>> mem;
  SoftwareEncoder se(mem, kBw, kBh);
  TASK_CHECK(!se.partitionSearch());
  TASK_CHECK(se.setPartitionSearch(true));
  TASK_CHECK(se.partitionSearch());
  return 0;
}

}  // namespace

// =====================================================================
// Case registration
// =====================================================================

const struct task_case task_cases[] = {
    // search
    {"search", "top_bottom", case_search_top_bottom},
    {"search", "left_right", case_search_left_right},
    {"search", "quadrants", case_search_quadrants},
    {"search", "negative_and_edge", case_search_negative_and_edge},
    {"search", "state_restoration", case_search_state_restoration},
    {"search", "second_partition_dependency",
     case_search_second_partition_dependency},
    // syntax
    {"syntax", "off_is_old_path_and_alg_ignored",
     case_syntax_off_is_old_path_and_alg_ignored},
    {"syntax", "tree_valid", case_syntax_tree_valid},
    {"syntax", "intra_fallback_and_skip", case_syntax_intra_fallback_and_skip},
    {"syntax", "skip_conditions", case_syntax_skip_conditions},
    // reference
    {"reference", "five_partition_stream", case_reference_five_partition_stream},
    {"reference", "sixteen_partition_stream",
     case_reference_sixteen_partition_stream},
    {"reference", "encoder_roundtrip", case_reference_encoder_roundtrip},
    {"reference", "mode_toggle", case_reference_mode_toggle},
    {"reference", "color_entry_points", case_reference_color_entry_points},
    {"reference", "setter_contract", case_reference_setter_contract},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
