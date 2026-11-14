/*
 * Numerical contract tests for HEVC inter prediction (motion compensation +
 * weighted prediction) in libde265.
 *
 * The tests drive the production entry point generate_inter_prediction_samples()
 * with hand-built SPS/PPS/slice_segment_header/de265_image/PBMotion objects and
 * compare every output sample against an independent coordinate model.  The
 * model is written from the HEVC specification text (8.5.3.3.3 fractional sample
 * interpolation, 8.5.3.3.4 weighted sample prediction) with explicit filter
 * coefficient tables; it does not call any production interpolation or
 * weighting function.
 *
 * Reference pictures are reached exactly like in real decoding: the slice
 * header's RefPicList maps refIdx to a frame id, and a small base_context
 * subclass resolves the frame id to a de265_image.  Expected predictions are
 * never injected into the production path.
 *
 * Registered groups (see task_support/check.h):
 *  - precision: intermediate precision of the separable filters (truncation
 *    between the horizontal and vertical pass, no premature clipping)
 *  - lists:     prediction-list wiring (single/bi, unweighted/explicit weights,
 *    same reference reachable from both lists)
 *  - planes:    all three 4:2:0 planes, sample-stride vs byte-stride discipline,
 *    out-of-block and row-padding sentinels, reference immutability
 */

#include "task_support/check.h"

#include "libde265/decctx.h"
#include "libde265/motion.h"
#include "libde265/image.h"
#include "libde265/slice.h"
#include "libde265/sps.h"
#include "libde265/pps.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Picture geometry shared by all cases.  The luma width (40) is deliberately
// not a multiple of the allocation alignment (16 samples), so every row is
// followed by padding samples that can be guarded with sentinels.
// ---------------------------------------------------------------------------

constexpr int kPicW = 40;
constexpr int kPicH = 24;
constexpr int kPbW  = 8;
constexpr int kPbH  = 8;
constexpr int kNCS  = 16;   // predSamples stride used by the production code

// ---------------------------------------------------------------------------
// Fixed-point helpers for the independent model.  All divisions by powers of
// two are defined as floor() for negative values as well (this matches the
// arithmetic right shift the production code relies on).
// ---------------------------------------------------------------------------

int floor_div(int a, int b)  // b > 0
{
  int q = a / b;
  int r = a % b;
  return (r != 0 && r < 0) ? q - 1 : q;
}

int floor_mod(int a, int b) { return a - floor_div(a, b) * b; }

int64_t floor_shr(int64_t v, int s)  // floor(v / 2^s), exact for v < 0
{
  if (s <= 0) return v;
  if (v >= 0) return v >> s;
  return -(((-v) + ((int64_t(1) << s) - 1)) >> s);
}

int clip_pixel(int64_t v, int bitDepth)
{
  const int maxval = (1 << bitDepth) - 1;
  if (v < 0) return 0;
  if (v > maxval) return maxval;
  return (int) v;
}

// ---------------------------------------------------------------------------
// Interpolation filter coefficients, written out as tables (HEVC 8.5.3.3.3).
// Luma: 8-tap (7 taps used for phases 1 and 3).  Chroma: 4-tap.
// ---------------------------------------------------------------------------

const int kLumaCoeff[4][8] = {
  {  0, 0,   0, 64,  0,  0,  0,  0 },   // phase 0: not used by the filter path
  { -1, 4, -10, 58, 17, -5,  1,  0 },
  { -1, 4, -11, 40, 40, -11, 4, -1 },
  {  1, -5, 17, 58, -10, 4, -1,  0 }
};

const int kLumaExtraBefore[4] = { 0, 3, 3, 2 };
const int kLumaExtraAfter [4] = { 0, 3, 4, 4 };

const int kChromaCoeff[8][4] = {
  {  0, 64,  0,  0 },                    // phase 0: not used by the filter path
  { -2, 58, 10, -2 },
  { -4, 54, 16, -2 },
  { -6, 46, 28, -4 },
  { -4, 36, 36, -4 },
  { -4, 28, 46, -6 },
  { -2, 16, 54, -4 },
  { -2, 10, 58, -2 }
};

// ---------------------------------------------------------------------------
// Deterministic, non-flat reference texture.  Each reference frame id and
// each plane gets its own seed, so two references are always distinguishable.
// ---------------------------------------------------------------------------

uint32_t mix32(uint32_t h)
{
  h ^= h >> 16;  h *= 0x7feb352dU;
  h ^= h >> 15;  h *= 0x846ca68bU;
  h ^= h >> 16;
  return h;
}

int texture_value(int seed, int cIdx, int x, int y, int bitDepth)
{
  uint32_t h = (uint32_t) seed * 2654435761U;
  h ^= (uint32_t) (cIdx + 1) * 2246822519U;
  h ^= (uint32_t) (x + 1) * 3266489917U;
  h ^= (uint32_t) (y + 1) * 668265263U;
  h = mix32(h);
  const int maxv = (1 << bitDepth) - 1;
  int v = (int) (h >> 16) & maxv;
  // Drive half of the samples to the extremes of the range: the resulting
  // high-frequency content makes the negative filter lobes produce negative
  // intermediates and makes the final stage saturate.
  if (h & 0x8000) v = (v < maxv / 2) ? 0 : maxv;
  return v;
}

// ---------------------------------------------------------------------------
// Sample access.  get_image_stride() counts *samples*, not bytes; for more
// than 8 bits the plane is an array of uint16_t.  Never mix the two units.
// ---------------------------------------------------------------------------

int get_sample(const de265_image* img, int cIdx, int x, int y)
{
  const ptrdiff_t stride = img->get_image_stride(cIdx);
  if (img->high_bit_depth(cIdx)) {
    return reinterpret_cast<const uint16_t*>(img->get_image_plane(cIdx))[y * stride + x];
  }
  return img->get_image_plane(cIdx)[y * stride + x];
}

void set_sample(de265_image* img, int cIdx, int x, int y, int v)
{
  const ptrdiff_t stride = img->get_image_stride(cIdx);
  if (img->high_bit_depth(cIdx)) {
    reinterpret_cast<uint16_t*>(img->get_image_plane(cIdx))[y * stride + x] = (uint16_t) v;
  }
  else {
    img->get_image_plane(cIdx)[y * stride + x] = (uint8_t) v;
  }
}

void fill_texture(de265_image* img, int bitDepth, int seed)
{
  for (int c = 0; c < 3; c++) {
    const int w = (c == 0) ? kPicW : kPicW / 2;
    const int h = (c == 0) ? kPicH : kPicH / 2;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        set_sample(img, c, x, y, texture_value(seed, c, x, y, bitDepth));
  }
}

void fill_const(de265_image* img, int value)
{
  for (int c = 0; c < 3; c++) {
    const int h = (c == 0) ? kPicH : kPicH / 2;
    const ptrdiff_t stride = img->get_image_stride(c);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < stride; x++)   // includes the row padding
        set_sample(img, c, x, y, value);
  }
}

std::vector<int> snapshot_plane(const de265_image* img, int c)
{
  const int h = (c == 0) ? kPicH : kPicH / 2;
  const ptrdiff_t stride = img->get_image_stride(c);
  std::vector<int> out((size_t) h * stride);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < stride; x++)
      out[(size_t) y * stride + x] = get_sample(img, c, x, y);
  return out;
}

// ---------------------------------------------------------------------------
// Minimal decoding context: resolves the frame ids found in RefPicList to
// fixed reference images, exactly like the DPB lookup in real decoding.
// ---------------------------------------------------------------------------

class RefContext : public base_context
{
public:
  std::vector<const de265_image*> frames;   // indexed by frame id

  const de265_image* get_image(uint16_t frame_id) const override
  {
    if (frame_id < frames.size()) return frames[frame_id];
    return nullptr;
  }

  bool has_image(uint16_t frame_id) const override
  {
    return get_image(frame_id) != nullptr;
  }
};

// ---------------------------------------------------------------------------
// Coverage counters: the tests assert that the interesting numerical
// situations actually occurred inside the evaluated blocks.
// ---------------------------------------------------------------------------

struct Coverage {
  long clipped_coords = 0;     // reference positions outside the picture (edge replication)
  long neg_intermediate = 0;   // negative horizontal intermediates (negative filter lobes)
  long h_nondivisible = 0;     // horizontal sums not divisible by 2^shift1
  long saturated = 0;          // final samples clipped to [0,max]
  long interior_direct = 0;    // full-pel blocks copied without any clipping

  void add(const Coverage& o)
  {
    clipped_coords   += o.clipped_coords;
    neg_intermediate += o.neg_intermediate;
    h_nondivisible   += o.h_nondivisible;
    saturated        += o.saturated;
    interior_direct  += o.interior_direct;
  }
};

// Reference plane view with per-coordinate clamping to the real picture
// size.  The model never relies on any padding the library may provide.
struct RefPlane {
  const de265_image* img;
  int cIdx;
  int w, h;

  int at_clipped(int x, int y, Coverage& cov) const
  {
    const int cx = std::min(std::max(x, 0), w - 1);
    const int cy = std::min(std::max(y, 0), h - 1);
    if (cx != x || cy != y) cov.clipped_coords++;
    return get_sample(img, cIdx, cx, cy);
  }
};

// Luma sample interpolation (8.5.3.3.3.2).  Returns the intermediate
// prediction samples predSamplesLX (not yet clipped to the pixel range).
std::vector<int> model_luma(const de265_image* ref,
                            int mvx, int mvy,
                            int xP, int yP, int nW, int nH,
                            int bitDepth, Coverage& cov)
{
  const RefPlane plane { ref, 0, kPicW, kPicH };

  const int xFrac = floor_mod(mvx, 4);
  const int yFrac = floor_mod(mvy, 4);
  const int xInt  = xP + floor_div(mvx, 4);
  const int yInt  = yP + floor_div(mvy, 4);

  const int shift1 = std::min(4, bitDepth - 8);
  const int shift2 = 6;
  const int shift3 = std::max(2, 14 - bitDepth);

  std::vector<int> out((size_t) nW * nH);

  if (xFrac == 0 && yFrac == 0) {
    const long clipped_before = cov.clipped_coords;
    for (int y = 0; y < nH; y++)
      for (int x = 0; x < nW; x++)
        out[(size_t) y * nW + x] = plane.at_clipped(xInt + x, yInt + y, cov) << shift3;
    if (cov.clipped_coords == clipped_before) cov.interior_direct++;
    return out;
  }

  const int extT = kLumaExtraBefore[yFrac];
  const int extB = kLumaExtraAfter [yFrac];
  const int extL = kLumaExtraBefore[xFrac];
  const int rows = nH + extT + extB;

  // Horizontal pass: intermediate values keep the mandated precision
  // (>> shift1, truncation towards -inf); they are NOT clipped to the
  // pixel range before the vertical pass.
  std::vector<int> tmp((size_t) rows * nW);
  const int hTaps = (xFrac == 2) ? 8 : 7;
  for (int r = 0; r < rows; r++) {
    const int ry = yInt + r - extT;
    for (int x = 0; x < nW; x++) {
      if (xFrac == 0) {
        tmp[(size_t) r * nW + x] = plane.at_clipped(xInt + x, ry, cov);
      }
      else {
        int s = 0;
        for (int k = 0; k < hTaps; k++)
          s += kLumaCoeff[xFrac][k] * plane.at_clipped(xInt + x - extL + k, ry, cov);
        if (s < 0) cov.neg_intermediate++;
        if (shift1 > 0 && (s & ((1 << shift1) - 1)) != 0) cov.h_nondivisible++;
        tmp[(size_t) r * nW + x] = (int) floor_shr(s, shift1);
      }
    }
  }

  // Vertical pass.
  const int vshift = (xFrac == 0) ? shift1 : shift2;
  const int vTaps  = (yFrac == 2) ? 8 : 7;
  for (int y = 0; y < nH; y++) {
    for (int x = 0; x < nW; x++) {
      if (yFrac == 0) {
        out[(size_t) y * nW + x] = tmp[(size_t) y * nW + x];
      }
      else {
        int s = 0;
        for (int k = 0; k < vTaps; k++)
          s += kLumaCoeff[yFrac][k] * tmp[(size_t) (y + k) * nW + x];
        out[(size_t) y * nW + x] = (int) floor_shr(s, vshift);
      }
    }
  }

  return out;
}

// Chroma sample interpolation (8.5.3.3.3.3) for 4:2:0.
std::vector<int> model_chroma(const de265_image* ref, int cIdx,
                              int mvx, int mvy,
                              int xP, int yP, int nW, int nH,
                              int bitDepth, Coverage& cov)
{
  const RefPlane plane { ref, cIdx, kPicW / 2, kPicH / 2 };

  // 4:2:0: the luma motion vector is reused in 1/8-pel chroma units.
  const int xFrac = floor_mod(mvx, 8);
  const int yFrac = floor_mod(mvy, 8);
  const int xInt  = xP / 2 + floor_div(mvx, 8);
  const int yInt  = yP / 2 + floor_div(mvy, 8);

  const int w = nW / 2;
  const int h = nH / 2;

  const int shift1 = std::min(4, bitDepth - 8);
  const int shift2 = 6;
  const int shift3 = std::max(2, 14 - bitDepth);

  std::vector<int> out((size_t) w * h);

  if (xFrac == 0 && yFrac == 0) {
    const long clipped_before = cov.clipped_coords;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        out[(size_t) y * w + x] = plane.at_clipped(xInt + x, yInt + y, cov) << shift3;
    if (cov.clipped_coords == clipped_before) cov.interior_direct++;
    return out;
  }

  // The filter always reads one row above and two rows below.
  const int rows = h + 3;
  std::vector<int> tmp((size_t) rows * w);
  for (int r = 0; r < rows; r++) {
    const int ry = yInt + r - 1;
    for (int x = 0; x < w; x++) {
      if (xFrac == 0) {
        tmp[(size_t) r * w + x] = plane.at_clipped(xInt + x, ry, cov);
      }
      else {
        int s = 0;
        for (int k = 0; k < 4; k++)
          s += kChromaCoeff[xFrac][k] * plane.at_clipped(xInt + x - 1 + k, ry, cov);
        if (s < 0) cov.neg_intermediate++;
        if (shift1 > 0 && (s & ((1 << shift1) - 1)) != 0) cov.h_nondivisible++;
        tmp[(size_t) r * w + x] = (int) floor_shr(s, shift1);
      }
    }
  }

  const int vshift = (xFrac == 0) ? shift1 : shift2;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (yFrac == 0) {
        // row y+1 of the intermediate buffer is the reference row yInt+y
        out[(size_t) y * w + x] = tmp[(size_t) (y + 1) * w + x];
      }
      else {
        int s = 0;
        for (int k = 0; k < 4; k++)
          s += kChromaCoeff[yFrac][k] * tmp[(size_t) (y + k) * w + x];
        out[(size_t) y * w + x] = (int) floor_shr(s, vshift);
      }
    }
  }

  return out;
}

// ---------------------------------------------------------------------------
// Case description and shared driver.
// ---------------------------------------------------------------------------

struct CaseDef {
  int bitDepth;
  int sliceType;      // SLICE_TYPE_P or SLICE_TYPE_B
  bool weighted;      // explicit weighted prediction (pred_weight_table present)

  PBMotion vi;
  int xP, yP;         // prediction block position (luma samples)

  int listFrame[2][2];  // RefPicList[list][entry] = frame id (0 terminates)

  int lumaDenom, chromaDenom;
  int lumaW[2], lumaO[2];         // per list, at the used refIdx
  int chromaW[2][2], chromaO[2][2];  // [list][component]

  // coverage that this case must exhibit
  bool needClipping, needNegIntermediate, needHNondivisible;
  bool needSaturation, needInteriorDirect;
};

struct Anchor {
  int c, x, y, expected;   // hand-computed sample value (see tests/inter_weight_contract.md)
};

int plane_w(int c) { return c == 0 ? kPicW : kPicW / 2; }
int plane_h(int c) { return c == 0 ? kPicH : kPicH / 2; }

int run_def(const CaseDef& def, const Anchor* anchors, size_t nAnchors)
{
  const int bd = def.bitDepth;

  // --- parameter sets ---

  auto sps = std::make_shared<seq_parameter_set>();
  sps->set_defaults();
  sps->chroma_format_idc = 1;   // 4:2:0
  sps->ChromaArrayType = 1;
  sps->pic_width_in_luma_samples  = kPicW;
  sps->pic_height_in_luma_samples = kPicH;
  sps->BitDepth_Y = bd;
  sps->BitDepth_C = bd;
  sps->SubWidthC  = 2;
  sps->SubHeightC = 2;
  sps->WpOffsetBdShiftY = (uint8_t) (bd - 8);
  sps->WpOffsetBdShiftC = (uint8_t) (bd - 8);

  auto pps = std::make_shared<pic_parameter_set>();
  pps->set_defaults();
  pps->sps = sps;
  pps->weighted_pred_flag   = def.weighted;
  pps->weighted_bipred_flag = def.weighted;

  // --- slice header ---

  slice_segment_header shdr;
  shdr.pps = pps;
  shdr.slice_type = (uint8_t) def.sliceType;
  shdr.luma_log2_weight_denom = (uint8_t) def.lumaDenom;
  shdr.ChromaLog2WeightDenom  = (uint8_t) def.chromaDenom;
  for (int l = 0; l < 2; l++) {
    for (int i = 0; i < 2; i++)
      shdr.RefPicList[l][i] = def.listFrame[l][i];
    const int r = def.vi.refIdx[l];
    shdr.luma_weight_flag[l][r]   = 1;
    shdr.chroma_weight_flag[l][r] = 1;
    shdr.LumaWeight[l][r]  = (int16_t) def.lumaW[l];
    shdr.luma_offset[l][r] = (int16_t) def.lumaO[l];
    for (int c = 0; c < 2; c++) {
      shdr.ChromaWeight[l][r][c] = (int16_t) def.chromaW[l][c];
      shdr.ChromaOffset[l][r][c] = (int16_t) def.chromaO[l][c];
    }
  }

  // --- reference images (frame ids 1 and 2) ---

  RefContext ctx;
  ctx.frames.assign(3, nullptr);
  std::vector<std::unique_ptr<de265_image>> store;
  const de265_image* frameImg[3] = { nullptr, nullptr, nullptr };

  for (int id = 1; id <= 2; id++) {
    bool needed = false;
    for (int l = 0; l < 2; l++)
      for (int i = 0; i < 2; i++)
        if (def.listFrame[l][i] == id) needed = true;
    if (!needed) continue;

    auto img = std::make_unique<de265_image>();
    TASK_CHECK(img->alloc_image(kPicW, kPicH, de265_chroma_420, sps,
                                /*allocMetadata=*/false, nullptr,
                                /*pts=*/0, nullptr,
                                /*useCustomAllocFunctions=*/false) == DE265_OK);
    img->PicState = UsedForShortTermReference;
    img->integrity = INTEGRITY_CORRECT;
    fill_texture(img.get(), bd, /*seed=*/id * 1000 + 7);
    frameImg[id] = img.get();
    ctx.frames[id] = img.get();
    store.push_back(std::move(img));
  }

  // --- current image, filled with a sentinel ---

  auto cur = std::make_unique<de265_image>();
  TASK_CHECK(cur->alloc_image(kPicW, kPicH, de265_chroma_420, sps,
                              false, nullptr, 0, nullptr, false) == DE265_OK);
  cur->integrity = INTEGRITY_CORRECT;

  const int sentinel = (bd <= 8) ? 0xA5 : (0x5A5A & ((1 << bd) - 1));
  fill_const(cur.get(), sentinel);

  // --- snapshot the references to prove they are never modified ---

  std::vector<int> snap[3];
  for (int id = 1; id <= 2; id++)
    if (frameImg[id])
      for (int c = 0; c < 3; c++) {
        std::vector<int> p = snapshot_plane(frameImg[id], c);
        snap[id].insert(snap[id].end(), p.begin(), p.end());
      }
  if (frameImg[1] && frameImg[2])
    TASK_CHECK(snap[1] != snap[2]);   // the two references are distinguishable

  // --- run the production inter prediction ---

  PBMotion vi = def.vi;
  generate_inter_prediction_samples(&ctx, &shdr, cur.get(),
                                    def.xP, def.yP, 0, 0,
                                    kNCS, kPbW, kPbH, &vi);

  // --- independent model ---

  Coverage cov;

  int activeLists[2];
  int nActive = 0;
  for (int l = 0; l < 2; l++)
    if (vi.predFlag[l]) activeLists[nActive++] = l;
  TASK_CHECK(nActive >= 1);

  // The same-picture/same-MV merge the production applies when explicit
  // weighted prediction is off.  (Numerically identical to the average;
  // replicated here so the model exercises the same decision.)
  bool merged = false;
  if (!def.weighted && nActive == 2 &&
      vi.mv[0].x == vi.mv[1].x && vi.mv[0].y == vi.mv[1].y &&
      def.listFrame[0][vi.refIdx[0]] == def.listFrame[1][vi.refIdx[1]]) {
    merged = true;
  }
  const bool bipred = (nActive == 2) && !merged;

  std::vector<int> inter[2][3];
  const int nLists = bipred ? 2 : 1;
  for (int i = 0; i < nLists; i++) {
    const int l = activeLists[i];
    const de265_image* ref = frameImg[def.listFrame[l][vi.refIdx[l]]];
    TASK_CHECK(ref != nullptr);
    inter[l][0] = model_luma(ref, vi.mv[l].x, vi.mv[l].y,
                             def.xP, def.yP, kPbW, kPbH, bd, cov);
    for (int c = 1; c < 3; c++)
      inter[l][c] = model_chroma(ref, c, vi.mv[l].x, vi.mv[l].y,
                                 def.xP, def.yP, kPbW, kPbH, bd, cov);
  }

  const int shift1w = std::max(2, 14 - bd);
  const int shift2w = std::max(3, 15 - bd);

  // --- compare every sample of every plane, including row padding ---

  for (int c = 0; c < 3; c++) {
    const int bw = (c == 0) ? kPbW : kPbW / 2;
    const int bh = (c == 0) ? kPbH : kPbH / 2;
    const int bx = (c == 0) ? def.xP : def.xP / 2;
    const int by = (c == 0) ? def.yP : def.yP / 2;

    const int log2WD = (c == 0 ? def.lumaDenom : def.chromaDenom) + shift1w;
    int w[2], o[2];
    for (int i = 0; i < nLists; i++) {
      const int l = activeLists[i];
      w[l] = (c == 0) ? def.lumaW[l] : def.chromaW[l][c - 1];
      o[l] = ((c == 0) ? def.lumaO[l] : def.chromaO[l][c - 1]) * (1 << (bd - 8));
    }

    const ptrdiff_t stride = cur->get_image_stride(c);
    int mismatches = 0;
    int firstX = -1, firstY = -1, firstExp = 0, firstAct = 0;

    for (int y = 0; y < plane_h(c); y++) {
      for (int x = 0; x < stride; x++) {   // past the picture width: padding
        const bool inside = (x >= bx && x < bx + bw && y >= by && y < by + bh);
        int expected;
        if (!inside) {
          expected = sentinel;
        }
        else {
          const int l0 = activeLists[0];
          const int in0 = inter[l0][c][(size_t) (y - by) * bw + (x - bx)];
          int64_t v;
          if (!bipred) {
            if (def.weighted) {
              v = floor_shr((int64_t) in0 * w[l0] + (int64_t(1) << (log2WD - 1)), log2WD) + o[l0];
            }
            else {
              v = floor_shr((int64_t) in0 + (int64_t(1) << (shift1w - 1)), shift1w);
            }
          }
          else {
            const int l1 = activeLists[1];
            const int in1 = inter[l1][c][(size_t) (y - by) * bw + (x - bx)];
            if (def.weighted) {
              const int64_t rnd = (int64_t) (o[l0] + o[l1] + 1) << log2WD;
              v = floor_shr((int64_t) in0 * w[l0] + (int64_t) in1 * w[l1] + rnd, log2WD + 1);
            }
            else {
              v = floor_shr((int64_t) in0 + in1 + (int64_t(1) << (shift2w - 1)), shift2w);
            }
          }
          if (v < 0 || v > ((1 << bd) - 1)) cov.saturated++;
          expected = clip_pixel(v, bd);
        }

        const int actual = get_sample(cur.get(), c, x, y);
        if (actual != expected) {
          if (mismatches == 0) {
            firstX = x; firstY = y; firstExp = expected; firstAct = actual;
          }
          mismatches++;
        }
      }
    }

    if (mismatches != 0)
      fprintf(stderr, "plane %d: %d mismatches, first at (%d,%d): expected %d, got %d\n",
              c, mismatches, firstX, firstY, firstExp, firstAct);
    TASK_CHECK(mismatches == 0);
  }

  // --- references must be untouched ---

  for (int id = 1; id <= 2; id++) {
    if (!frameImg[id]) continue;
    std::vector<int> now;
    for (int c = 0; c < 3; c++) {
      std::vector<int> p = snapshot_plane(frameImg[id], c);
      now.insert(now.end(), p.begin(), p.end());
    }
    TASK_CHECK(now == snap[id]);
  }

  // --- no decoder warnings were raised ---

  TASK_CHECK(ctx.get_warning() == DE265_OK);
  TASK_CHECK(cur->integrity == INTEGRITY_CORRECT);

  // --- the case really exercised the situations it claims to ---

  if (def.needClipping)        TASK_CHECK(cov.clipped_coords > 0);
  if (def.needNegIntermediate) TASK_CHECK(cov.neg_intermediate > 0);
  if (def.needHNondivisible)   TASK_CHECK(cov.h_nondivisible > 0);
  if (def.needSaturation)      TASK_CHECK(cov.saturated > 0);
  if (def.needInteriorDirect)  TASK_CHECK(cov.interior_direct > 0);

  // --- hand-computed anchor samples ---

  for (size_t a = 0; a < nAnchors; a++) {
    const int actual = get_sample(cur.get(), anchors[a].c, anchors[a].x, anchors[a].y);
    if (actual != anchors[a].expected)
      fprintf(stderr, "anchor plane %d (%d,%d): expected %d, got %d\n",
              anchors[a].c, anchors[a].x, anchors[a].y, anchors[a].expected, actual);
    TASK_CHECK(actual == anchors[a].expected);
  }

  return 0;
}

PBMotion make_vi(int pred0, int ref0, int mv0x, int mv0y,
                 int pred1, int ref1, int mv1x, int mv1y)
{
  PBMotion vi;
  vi.predFlag[0] = (uint8_t) pred0;
  vi.predFlag[1] = (uint8_t) pred1;
  vi.refIdx[0]   = (uint8_t) ref0;
  vi.refIdx[1]   = (uint8_t) ref1;
  vi.mv[0].x = (int16_t) mv0x;
  vi.mv[0].y = (int16_t) mv0y;
  vi.mv[1].x = (int16_t) mv1x;
  vi.mv[1].y = (int16_t) mv1y;
  return vi;
}

// ---------------------------------------------------------------------------
// Group "precision": intermediate precision of the separable filters.
// ---------------------------------------------------------------------------

// 10 bit, luma horizontal phase 1 only (vertical phase 0): the horizontal
// intermediate is truncated by shift1=2 and passed on unmodified.  Contains
// a hand-computed anchor that separates floor-truncation from
// add-half-then-shift rounding (see tests/inter_weight_contract.md).
int case_precision_luma_hpass_truncation()
{
  CaseDef def = {};
  def.bitDepth  = 10;
  def.sliceType = SLICE_TYPE_P;
  def.weighted  = false;
  def.vi = make_vi(1, 0, 1, 0,   0, 0, 0, 0);   // mv=(1,0): xFrac=1, yFrac=0
  def.xP = 8;  def.yP = 8;
  def.listFrame[0][0] = 1;
  def.needNegIntermediate = true;
  def.needHNondivisible   = true;

  Anchor anchors[] = {
    { 0, 8, 8 + 1, 495 },   // worked examples, see tests/inter_weight_contract.md
    { 0, 8 + 2, 8 + 7, 0 }
  };
  return run_def(def, anchors, sizeof(anchors) / sizeof(anchors[0]));
}

// 12 bit, negative motion vector, both fractional phases non-zero (luma
// quarter-pel phases 2 and 3, chroma eighth-pel phases 6 and 3), reference
// coordinates clipped at the top-left corner.
int case_precision_luma_bidir_12bit()
{
  CaseDef def = {};
  def.bitDepth  = 12;
  def.sliceType = SLICE_TYPE_P;
  def.weighted  = false;
  def.vi = make_vi(1, 0, -26, -21,   0, 0, 0, 0);  // xFrac=2, yFrac=3
  def.xP = 8;  def.yP = 8;
  def.listFrame[0][0] = 1;
  def.needClipping        = true;
  def.needNegIntermediate = true;
  def.needHNondivisible   = true;
  return run_def(def, nullptr, 0);
}

// 8 bit, both fractional phases non-zero (luma phases 3 and 1, chroma
// eighth-pel phases 3 and 5).  For 8 bit the horizontal pass keeps the full
// sum (shift1=0); the intermediate must still not be clipped to 8 bit.
int case_precision_chroma_eighthpel_8bit()
{
  CaseDef def = {};
  def.bitDepth  = 8;
  def.sliceType = SLICE_TYPE_P;
  def.weighted  = false;
  def.vi = make_vi(1, 0, 3, 5,   0, 0, 0, 0);   // xFrac=3, yFrac=1
  def.xP = 16; def.yP = 8;
  def.listFrame[0][0] = 1;
  def.needNegIntermediate = true;
  return run_def(def, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Group "lists": prediction-list and weight wiring.
// ---------------------------------------------------------------------------

// Single-list, unweighted, full-pel motion, block completely inside the
// reference: the direct copy path.
int case_lists_p_single_unweighted()
{
  CaseDef def = {};
  def.bitDepth  = 8;
  def.sliceType = SLICE_TYPE_P;
  def.weighted  = false;
  def.vi = make_vi(1, 0, 8, -4,   0, 0, 0, 0);  // integer MV
  def.xP = 16; def.yP = 12;
  def.listFrame[0][0] = 1;
  def.needInteriorDirect = true;
  return run_def(def, nullptr, 0);
}

// Single-list, explicit weights (P slice): negative luma weight, positive
// and negative offsets, luma and chroma parameters all different.
int case_lists_p_single_explicit_weight()
{
  CaseDef def = {};
  def.bitDepth  = 10;
  def.sliceType = SLICE_TYPE_P;
  def.weighted  = true;
  def.vi = make_vi(1, 0, -9, 6,   0, 0, 0, 0);  // xFrac=3, yFrac=2
  def.xP = 8;  def.yP = 8;
  def.listFrame[0][0] = 1;
  def.lumaDenom   = 1;
  def.chromaDenom = 2;
  def.lumaW[0] = 5;   def.lumaO[0] = -7;
  def.chromaW[0][0] = 2;  def.chromaO[0][0] = 40;
  def.chromaW[0][1] = -2; def.chromaO[0][1] = 13;
  def.needNegIntermediate = true;
  def.needHNondivisible   = true;
  def.needSaturation      = true;
  return run_def(def, nullptr, 0);
}

// Bi-prediction, unweighted average, two distinguishable references,
// opposite motion-vector signs, clipping at the right picture edge.
int case_lists_b_bi_average()
{
  CaseDef def = {};
  def.bitDepth  = 8;
  def.sliceType = SLICE_TYPE_B;
  def.weighted  = false;
  def.vi = make_vi(1, 0, 7, -3,   1, 0, -5, 9);
  def.xP = 32; def.yP = 8;
  def.listFrame[0][0] = 1;
  def.listFrame[1][0] = 2;
  def.needClipping = true;
  return run_def(def, nullptr, 0);
}

// Bi-prediction, explicit weights, 12 bit.  L0 reaches its reference through
// refIdx=1 (the list's second entry); weights and offsets differ per list
// and per component, including negative weights.
int case_lists_b_bi_explicit_weight()
{
  CaseDef def = {};
  def.bitDepth  = 12;
  def.sliceType = SLICE_TYPE_B;
  def.weighted  = true;
  def.vi = make_vi(1, 1, 11, 4,   1, 0, -6, -13);
  def.xP = 16; def.yP = 8;
  def.listFrame[0][0] = 2;  def.listFrame[0][1] = 1;   // L0 refIdx 1 -> frame 1
  def.listFrame[1][0] = 2;                              // L1 refIdx 0 -> frame 2
  def.lumaDenom   = 2;
  def.chromaDenom = 1;
  def.lumaW[0] = 9;   def.lumaO[0] = -5;
  def.lumaW[1] = -7;  def.lumaO[1] = 11;
  def.chromaW[0][0] = 4;   def.chromaO[0][0] = -9;
  def.chromaW[0][1] = -3;  def.chromaO[0][1] = 2;
  def.chromaW[1][0] = -1;  def.chromaO[1][0] = 7;
  def.chromaW[1][1] = 6;   def.chromaO[1][1] = -13;
  def.needNegIntermediate = true;
  def.needHNondivisible   = true;
  return run_def(def, nullptr, 0);
}

// The same reference identity (frame 1) and the same motion vector in both
// lists, but different weights per list.  With explicit weighted prediction
// the decoder must NOT merge the two lists; the hand-computed anchor
// separates independent weighting from the merge shortcut.
int case_lists_same_ref_same_mv_distinct_weights()
{
  CaseDef def = {};
  def.bitDepth  = 8;
  def.sliceType = SLICE_TYPE_B;
  def.weighted  = true;
  def.vi = make_vi(1, 0, 4, 0,   1, 1, 4, 0);   // same full-pel MV, same frame
  def.xP = 16; def.yP = 8;
  def.listFrame[0][0] = 1;  def.listFrame[0][1] = 2;
  def.listFrame[1][0] = 2;  def.listFrame[1][1] = 1;   // L1 refIdx 1 -> frame 1
  def.lumaDenom   = 0;
  def.chromaDenom = 0;
  def.lumaW[0] = 1;  def.lumaO[0] = 1;
  def.lumaW[1] = 2;  def.lumaO[1] = -2;
  def.chromaW[0][0] = 2;   def.chromaO[0][0] = 3;
  def.chromaW[0][1] = 7;   def.chromaO[0][1] = -1;
  def.chromaW[1][0] = -4;  def.chromaO[1][0] = -6;
  def.chromaW[1][1] = 1;   def.chromaO[1][1] = 5;
  def.needInteriorDirect = true;

  Anchor anchors[] = {
    { 0, 16, 8, 186 },    // worked example, see tests/inter_weight_contract.md
  };
  return run_def(def, anchors, sizeof(anchors) / sizeof(anchors[0]));
}

// ---------------------------------------------------------------------------
// Group "planes": all three 4:2 0 planes, strides, sentinels.
// ---------------------------------------------------------------------------

// Bi-prediction average at 8 bit with fractional motion in both directions
// on all three planes; block away from the origin.
int case_planes_420_8bit_all_planes()
{
  CaseDef def = {};
  def.bitDepth  = 8;
  def.sliceType = SLICE_TYPE_B;
  def.weighted  = false;
  def.vi = make_vi(1, 0, 3, 5,   1, 0, -11, -2);
  def.xP = 12; def.yP = 4;
  def.listFrame[0][0] = 1;
  def.listFrame[1][0] = 2;
  def.needNegIntermediate = true;
  return run_def(def, nullptr, 0);
}

// 10 bit, strongly negative motion vector with zero horizontal and non-zero
// vertical phase (luma vertical-only filtering, chroma eighth-pel phase 7):
// edge replication on all planes.
int case_planes_edge_clipping_negative_mv()
{
  CaseDef def = {};
  def.bitDepth  = 10;
  def.sliceType = SLICE_TYPE_P;
  def.weighted  = false;
  def.vi = make_vi(1, 0, -32, -17,   0, 0, 0, 0);  // xFrac=0, yFrac=3
  def.xP = 4;  def.yP = 4;
  def.listFrame[0][0] = 1;
  def.needClipping = true;
  return run_def(def, nullptr, 0);
}

// 12 bit, explicit weights chosen to drive the final stage into saturation
// at both ends of the 12-bit range (negative luma weight, large chroma
// weights and offsets).
int case_planes_saturation_12bit()
{
  CaseDef def = {};
  def.bitDepth  = 12;
  def.sliceType = SLICE_TYPE_P;
  def.weighted  = true;
  def.vi = make_vi(1, 0, 6, 2,   0, 0, 0, 0);   // xFrac=2, yFrac=2
  def.xP = 16; def.yP = 8;
  def.listFrame[0][0] = 1;
  def.lumaDenom   = 0;
  def.chromaDenom = 0;
  def.lumaW[0] = -4;  def.lumaO[0] = 200;
  def.chromaW[0][0] = 64;  def.chromaO[0][0] = 50;
  def.chromaW[0][1] = 64;  def.chromaO[0][1] = -250;
  def.needSaturation      = true;
  def.needNegIntermediate = true;
  def.needHNondivisible   = true;
  return run_def(def, nullptr, 0);
}

// 12-bit stride discipline: the sample stride must not be confused with the
// byte stride.  Checked directly against the plane accessor.
int case_planes_highbd_stride_units()
{
  auto sps = std::make_shared<seq_parameter_set>();
  sps->set_defaults();
  sps->chroma_format_idc = 1;
  sps->ChromaArrayType = 1;
  sps->pic_width_in_luma_samples  = kPicW;
  sps->pic_height_in_luma_samples = kPicH;
  sps->BitDepth_Y = 12;
  sps->BitDepth_C = 12;
  sps->SubWidthC  = 2;
  sps->SubHeightC = 2;
  sps->WpOffsetBdShiftY = 4;
  sps->WpOffsetBdShiftC = 4;

  de265_image img;
  TASK_CHECK(img.alloc_image(kPicW, kPicH, de265_chroma_420, sps,
                             false, nullptr, 0, nullptr, false) == DE265_OK);

  for (int c = 0; c < 3; c++) {
    const ptrdiff_t stride = img.get_image_stride(c);   // samples per row
    TASK_CHECK(stride >= plane_w(c));
    TASK_CHECK(stride % 16 == 0);                       // allocation alignment
    TASK_CHECK(img.get_bytes_per_pixel(c) == 2);
    // The byte address of sample (x,y) is plane + (y*stride+x)*bytes.
    for (int y = 0; y < plane_h(c); y += 5) {
      for (int x = 0; x < plane_w(c); x += 7) {
        const uint8_t* viaAccessor =
          static_cast<const uint8_t*>(img.get_image_plane_at_pos_any_depth(c, x, y));
        const uint8_t* viaStride =
          img.get_image_plane(c) + ((ptrdiff_t) y * stride + x) * 2;
        TASK_CHECK(viaAccessor == viaStride);
        // reading through the uint16 sample stride matches the accessor
        set_sample(&img, c, x, y, 0x0ABC);
        uint16_t sampleAt;
        memcpy(&sampleAt, viaAccessor, 2);
        TASK_CHECK(sampleAt == 0x0ABC);
      }
    }
  }
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration (consumed by task_support/driver.c).
// ---------------------------------------------------------------------------

extern const task_case task_cases[] = {
  { "precision", "luma_hpass_truncation_10bit",       case_precision_luma_hpass_truncation },
  { "precision", "luma_bidir_negative_mv_12bit",      case_precision_luma_bidir_12bit },
  { "precision", "chroma_eighthpel_8bit",             case_precision_chroma_eighthpel_8bit },
  { "lists",     "p_single_unweighted_fullpel_8bit",  case_lists_p_single_unweighted },
  { "lists",     "p_single_explicit_weight_10bit",    case_lists_p_single_explicit_weight },
  { "lists",     "b_bi_average_8bit",                 case_lists_b_bi_average },
  { "lists",     "b_bi_explicit_weight_12bit",        case_lists_b_bi_explicit_weight },
  { "lists",     "same_ref_same_mv_distinct_weights", case_lists_same_ref_same_mv_distinct_weights },
  { "planes",    "ycbcr_420_8bit_all_planes",         case_planes_420_8bit_all_planes },
  { "planes",    "edge_clipping_negative_mv_10bit",   case_planes_edge_clipping_negative_mv },
  { "planes",    "saturation_12bit_weighted",         case_planes_saturation_12bit },
  { "planes",    "highbd_stride_units",               case_planes_highbd_stride_units },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
