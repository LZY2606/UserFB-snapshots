/*
 * Independent numerical contract tests for HEVC inter prediction.
 *
 * Every case drives the production generate_inter_prediction_samples()
 * (libde265/motion.cc) through real seq_parameter_set / pic_parameter_set /
 * slice_segment_header / de265_image / PBMotion objects and compares every
 * output sample of all three planes against an independent coordinate model
 * of HEVC 8.5.3.3.3 (fractional sample interpolation) and 8.5.3.3.4
 * (weighted sample prediction).
 *
 * The model is written from the specification, not from the library:
 *  - explicit 8-tap luma (table 8-10) and 4-tap chroma (table 8-11)
 *    coefficient tables,
 *  - reference coordinates clamped per coordinate to the real plane size
 *    (the library's padding is never consulted),
 *  - the mandated intermediate precision: the horizontal pass keeps the
 *    truncated (>> shift1) result with full range, including negative
 *    values; nothing is clipped to the pixel range before the final stage,
 *  - the final stage applies the exact weight / log2 denominator / offset
 *    arithmetic and only then saturates to [0, (1<<BitDepth)-1].
 *
 * No production interpolation or weighting function is called from the
 * model, no production output is cached as the answer, and no expected
 * sample array is handed to any library entry point: reference pictures
 * are reached exclusively through slice_segment_header::RefPicList,
 * PBMotion::refIdx and base_context::get_image().
 *
 * Signed division by a power of two is defined as floor() for all signs
 * (shr_floor / fdiv), matching two's-complement arithmetic shifts.
 */

#include "task_support/check.h"

#include "libde265/motion.h"
#include "libde265/decctx.h"
#include "libde265/image.h"
#include "libde265/slice.h"
#include "libde265/sps.h"
#include "libde265/pps.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Signed fixed-point helpers: division by 2^s is floor() for every sign.
// ---------------------------------------------------------------------------

int32_t shr_floor(int32_t v, int s)
{
  if (s <= 0) return v;
  if (v >= 0) return v >> s;
  return -static_cast<int32_t>((static_cast<uint32_t>(-v) + ((1u << s) - 1)) >> s);
}

int fdiv(int v, int s) // floor(v / 2^s)
{
  if (v >= 0) return v >> s;
  return -static_cast<int>((static_cast<uint32_t>(-v) + ((1u << s) - 1)) >> s);
}

int fmod_pow2(int v, int s) // v mod 2^s with floored division, result in [0, 2^s)
{
  return v - (fdiv(v, s) << s);
}

// ---------------------------------------------------------------------------
// Interpolation coefficient tables (HEVC 8.5.3.3.3).
// ---------------------------------------------------------------------------

// Luma 8-tap filters (table 8-10), taps at integer offsets -3..+4.
const int8_t LUMA_COEFF[4][8] = {
  {  0, 0,   0, 64,  0,   0,  0,  0 }, // phase 0: full-sample position
  { -1, 4, -10, 58, 17,  -5,  1,  0 }, // phase 1: quarter
  { -1, 4, -11, 40, 40, -11,  4, -1 }, // phase 2: half
  {  0, 1,  -5, 17, 58, -10,  4, -1 }  // phase 3: three-quarter
};

// Chroma 4-tap filters (table 8-11), taps at integer offsets -1..+2.
const int8_t CHROMA_COEFF[8][4] = {
  {  0, 64,  0,  0 },
  { -2, 58, 10, -2 },
  { -4, 54, 16, -2 },
  { -6, 46, 28, -4 },
  { -4, 36, 36, -4 },
  { -4, 28, 46, -6 },
  { -2, 16, 54, -4 },
  { -2, 10, 58, -2 }
};

// ---------------------------------------------------------------------------
// Fixture: real SPS/PPS/slice header plus images reachable through a
// minimal base_context. The picture is 56x40 luma (4:2:0), deliberately
// not a multiple of the 16-sample allocation alignment so that row-end
// padding exists and can be sentinel-checked.
// ---------------------------------------------------------------------------

const int PIC_W = 56;
const int PIC_H = 40;
const uint16_t FIRST_FRAME_ID = 100;

struct Fixture {
  std::shared_ptr<seq_parameter_set> sps;
  std::shared_ptr<pic_parameter_set> pps;
  slice_segment_header shdr;
  std::unique_ptr<de265_image> cur;
  std::vector<std::unique_ptr<de265_image>> refs; // frame id = FIRST_FRAME_ID + index
};

class TestContext : public base_context {
public:
  explicit TestContext(const Fixture& f) : fx(f) { }

  const de265_image* get_image(uint16_t frame_id) const override
  {
    if (frame_id >= FIRST_FRAME_ID && frame_id < FIRST_FRAME_ID + fx.refs.size())
      return fx.refs[frame_id - FIRST_FRAME_ID].get();
    return nullptr;
  }

  bool has_image(uint16_t frame_id) const override { return get_image(frame_id) != nullptr; }

private:
  const Fixture& fx;
};

bool make_headers(Fixture& F, int bitDepth, int sliceType, bool wPred, bool wBiPred)
{
  auto sps = std::make_shared<seq_parameter_set>();
  sps->set_defaults();
  sps->chroma_format_idc = 1; // 4:2:0
  sps->pic_width_in_luma_samples  = PIC_W;
  sps->pic_height_in_luma_samples = PIC_H;
  sps->bit_depth_luma   = bitDepth;
  sps->bit_depth_chroma = bitDepth;
  sps->set_CB_log2size_range(3, 3); // MinCb = 8 so 56x40 is an exact multiple
  sps->set_TB_log2size_range(2, 3);
  sps->log2_min_pcm_luma_coding_block_size      = 3;
  sps->log2_diff_max_min_pcm_luma_coding_block_size = 0;
  sps->range_extension.high_precision_offsets_enabled_flag = 0;
  if (sps->compute_derived_values(false) != DE265_OK)
    return false;

  auto pps = std::make_shared<pic_parameter_set>();
  pps->set_defaults();
  pps->sps = sps;
  pps->weighted_pred_flag  = wPred;
  pps->weighted_bipred_flag = wBiPred;

  F.sps = sps;
  F.pps = pps;
  F.shdr.set_defaults();
  F.shdr.pps = pps;
  F.shdr.slice_type = sliceType;
  F.shdr.luma_log2_weight_denom = 0;
  F.shdr.ChromaLog2WeightDenom = 0;

  // Identity weights everywhere; cases override the entries they use.
  for (int l = 0; l < 2; l++)
    for (int i = 0; i < MAX_NUM_REF_PICS; i++) {
      F.shdr.LumaWeight[l][i] = 1;
      F.shdr.luma_offset[l][i] = 0;
      F.shdr.ChromaWeight[l][i][0] = 1;
      F.shdr.ChromaWeight[l][i][1] = 1;
      F.shdr.ChromaOffset[l][i][0] = 0;
      F.shdr.ChromaOffset[l][i][1] = 0;
      F.shdr.RefPicList[l][i] = 0;
    }
  return true;
}

std::unique_ptr<de265_image> make_image(const Fixture& F, bool reference)
{
  auto img = std::make_unique<de265_image>();
  if (img->alloc_image(PIC_W, PIC_H, de265_chroma_420, F.sps,
                       false /* no metadata */, nullptr /* decoder_context */,
                       0 /* pts */, nullptr /* user_data */,
                       false /* default allocation */) != DE265_OK)
    return nullptr;
  if (reference)
    img->PicState = UsedForShortTermReference;
  return img;
}

// ---------------------------------------------------------------------------
// Plane sample access. get_image_stride() counts samples (not bytes); for
// bit depth > 8 the plane is an array of uint16_t.
// ---------------------------------------------------------------------------

bool high_depth(const de265_image* img, int cIdx) { return img->get_bit_depth(cIdx) > 8; }

int get_sample(const de265_image* img, int cIdx, int x, int y)
{
  const ptrdiff_t stride = img->get_image_stride(cIdx);
  if (high_depth(img, cIdx))
    return reinterpret_cast<const uint16_t*>(img->get_image_plane(cIdx))[y * stride + x];
  return img->get_image_plane(cIdx)[y * stride + x];
}

void set_sample(de265_image* img, int cIdx, int x, int y, int v)
{
  const ptrdiff_t stride = img->get_image_stride(cIdx);
  if (high_depth(img, cIdx))
    reinterpret_cast<uint16_t*>(img->get_image_plane(cIdx))[y * stride + x] = (uint16_t) v;
  else
    img->get_image_plane(cIdx)[y * stride + x] = (uint8_t) v;
}

// Plane geometry: samples really used (from the SPS), and the allocated
// stride. The full stride x height region is filled and verified so that
// writes past the row width (e.g. byte-stride/sample-stride confusion in
// 16-bit planes) are caught.
void plane_dims(const Fixture& F, int cIdx, int& w, int& h)
{
  w = (cIdx == 0) ? F.sps->pic_width_in_luma_samples
                  : F.sps->pic_width_in_luma_samples / F.sps->SubWidthC;
  h = (cIdx == 0) ? F.sps->pic_height_in_luma_samples
                  : F.sps->pic_height_in_luma_samples / F.sps->SubHeightC;
}

// ---------------------------------------------------------------------------
// Deterministic reference textures.
// ---------------------------------------------------------------------------

uint32_t mix32(uint32_t v)
{
  v ^= v >> 15; v *= 0x2C1B3C6Du;
  v ^= v >> 12; v *= 0x297A2D39u;
  v ^= v >> 15;
  return v;
}

// Full-range pseudo-random texture with a smooth ramp mixed in, plus
// deterministic period-5 bright spikes on a dark background. The spike
// phase is chosen so that every luma filter phase yields a negative
// horizontal sum at x ≡ 3 (mod 5): the negative taps (e.g. -1, -10, -5
// for phase 1) then land on spikes while the dominant +58 centre tap
// lands on the dark background (sum = -16*maxval + 80*base < 0 since
// base <= maxval/8). This makes negative intermediates deterministic
// instead of accidental. Different salts give distinguishable references.
int texel_noise(int x, int y, int salt, int maxval)
{
  const uint32_t h = mix32((uint32_t)(x) * 0x9E3779B1u ^
                           (uint32_t)(y) * 0x85EBCA77u ^
                           (uint32_t)(salt) * 0x27D4EB2Fu);
  const int noise = (int)(h % (uint32_t)(maxval + 1));
  const int ramp  = (x * 5 + y * 3 + salt * 11) % (maxval + 1);
  if (x % 5 == 0 || x % 5 == 2)
    return maxval;
  return (noise + ramp) / 16;
}

// Simple linear ramps, fully hand-computable, used by the worked examples.
// All values stay far below 2^bitDepth on a 56x40 / 28x20 plane.
int texel_ramp(int cIdx, int x, int y)
{
  switch (cIdx) {
  case 0:  return 2 * x + 5 * y;      // <= 305
  case 1:  return 3 * x + 7 * y + 1;  // <= 215
  default: return 5 * x + 3 * y + 2;  // <= 194
  }
}

// ---------------------------------------------------------------------------
// Independent coordinate model.
// ---------------------------------------------------------------------------

struct Counters {
  long samples = 0;            // modelled output samples
  long neg_intermediate = 0;   // negative horizontal-pass results
  long nondiv_horizontal = 0;  // horizontal sums not divisible by 2^shift1
  long sat_low = 0;            // final-stage saturations to 0
  long sat_high = 0;           // final-stage saturations to maxval
};

// Reference picture accessor with per-coordinate clamping to the real
// plane size (8.5.3.3.3.1); never reads outside [0,w) x [0,h).
struct RefPlane {
  const de265_image* img;
  int cIdx;
  int w, h;

  int get(int x, int y) const
  {
    x = std::min(std::max(x, 0), w - 1);
    y = std::min(std::max(y, 0), h - 1);
    return get_sample(img, cIdx, x, y);
  }
};

// Fractional interpolation for one sample (8.5.3.3.3.2 luma,
// 8.5.3.3.3.3 chroma). Returns the intermediate-precision prediction
// (predSamplesLX), not clipped to the sample range.
int32_t interp_sample(const RefPlane& ref, bool luma, int bitDepth,
                      int mvx, int mvy, int xP, int yP, int x, int y,
                      int subW, int subH, Counters& ctr)
{
  const int shift1 = std::min(4, bitDepth - 8);
  const int shift3 = std::max(2, 14 - bitDepth);

  if (luma) {
    const int xFrac = fmod_pow2(mvx, 2);
    const int yFrac = fmod_pow2(mvy, 2);
    const int xInt = xP + fdiv(mvx, 2);
    const int yInt = yP + fdiv(mvy, 2);

    if (xFrac == 0 && yFrac == 0)
      return ref.get(xInt + x, yInt + y) << shift3;

    // Horizontal pass: truncated by shift1, full range kept (no clipping).
    auto H = [&](int col, int row) -> int32_t {
      if (xFrac == 0) return ref.get(col, row);
      int32_t s = 0;
      for (int k = 0; k < 8; k++)
        s += LUMA_COEFF[xFrac][k] * ref.get(col + k - 3, row);
      if (s < 0) ctr.neg_intermediate++;
      if (shift1 > 0 && (s & ((1 << shift1) - 1)) != 0) ctr.nondiv_horizontal++;
      return shr_floor(s, shift1);
    };

    if (yFrac == 0)
      return H(xInt + x, yInt + y);

    int32_t s = 0;
    for (int k = 0; k < 8; k++)
      s += LUMA_COEFF[yFrac][k] * H(xInt + x, yInt + y + k - 3);
    return shr_floor(s, xFrac == 0 ? shift1 : 6);
  }
  else {
    const int cmvx = mvx * (2 / subW);
    const int cmvy = mvy * (2 / subH);
    const int xFrac = fmod_pow2(cmvx, 3);
    const int yFrac = fmod_pow2(cmvy, 3);
    const int xInt = xP / subW + fdiv(cmvx, 3);
    const int yInt = yP / subH + fdiv(cmvy, 3);

    if (xFrac == 0 && yFrac == 0)
      return ref.get(xInt + x, yInt + y) << shift3;

    auto H = [&](int col, int row) -> int32_t {
      if (xFrac == 0) return ref.get(col, row);
      int32_t s = 0;
      for (int k = 0; k < 4; k++)
        s += CHROMA_COEFF[xFrac][k] * ref.get(col + k - 1, row);
      if (s < 0) ctr.neg_intermediate++;
      if (shift1 > 0 && (s & ((1 << shift1) - 1)) != 0) ctr.nondiv_horizontal++;
      return shr_floor(s, shift1);
    };

    if (yFrac == 0)
      return H(xInt + x, yInt + y);

    int32_t s = 0;
    for (int k = 0; k < 4; k++)
      s += CHROMA_COEFF[yFrac][k] * H(xInt + x, yInt + y + k - 1);
    return shr_floor(s, xFrac == 0 ? shift1 : 6);
  }
}

// Weighted sample prediction (8.5.3.3.4) for one sample, including the
// final saturation to the sample range.
int32_t combine_sample(const Fixture& F, int cIdx,
                       int32_t p0, int32_t p1, bool use0, bool use1,
                       const PBMotion& vi, Counters& ctr)
{
  const int bitDepth = F.sps->get_bit_depth(cIdx);
  const int maxval = (1 << bitDepth) - 1;
  const slice_segment_header& shdr = F.shdr;

  const bool weighted = (shdr.slice_type == SLICE_TYPE_P) ? F.pps->weighted_pred_flag
                                                          : F.pps->weighted_bipred_flag;
  const int shift1 = std::max(2, 14 - bitDepth);
  const int log2WD = (cIdx == 0 ? shdr.luma_log2_weight_denom
                                : shdr.ChromaLog2WeightDenom) + shift1;
  const int offsetShift = (cIdx == 0 ? F.sps->WpOffsetBdShiftY
                                     : F.sps->WpOffsetBdShiftC);

  auto weight = [&](int l) -> int {
    const int r = vi.refIdx[l];
    return cIdx == 0 ? shdr.LumaWeight[l][r] : shdr.ChromaWeight[l][r][cIdx - 1];
  };
  auto offset = [&](int l) -> int {
    const int r = vi.refIdx[l];
    const int o = cIdx == 0 ? shdr.luma_offset[l][r] : shdr.ChromaOffset[l][r][cIdx - 1];
    return o << offsetShift;
  };

  int32_t out;
  if (use0 && use1) {
    if (!weighted) {
      const int shift2 = std::max(3, 15 - bitDepth);
      out = shr_floor(p0 + p1 + (1 << (shift2 - 1)), shift2);
    }
    else {
      const int rnd = (offset(0) + offset(1) + 1) << log2WD;
      out = shr_floor(p0 * weight(0) + p1 * weight(1) + rnd, log2WD + 1);
    }
  }
  else {
    const int l = use0 ? 0 : 1;
    const int32_t p = use0 ? p0 : p1;
    if (!weighted)
      out = shr_floor(p + (1 << (shift1 - 1)), shift1);
    else
      out = shr_floor(p * weight(l) + (1 << (log2WD - 1)), log2WD) + offset(l);
  }

  if (out < 0)      { ctr.sat_low++;  out = 0; }
  if (out > maxval) { ctr.sat_high++; out = maxval; }
  return out;
}

// ---------------------------------------------------------------------------
// Block execution and full-plane verification.
// ---------------------------------------------------------------------------

struct BlockSpec {
  int xP, yP, n;            // square prediction block, nCS = nPbW = nPbH = n
  int mv0x, mv0y, mv1x, mv1y;
  uint8_t predFlag0, predFlag1;
  uint8_t refIdx0, refIdx1;
};

uint32_t sentinel_value(int cIdx, int x, int y, bool hbd)
{
  const uint32_t v = 0x40u + (uint32_t)(cIdx * 977 + x * 131 + y * 57);
  return hbd ? (0x5A00u | (v & 0x1FFu)) : (v & 0xFFu);
}

// Runs the given blocks through the production implementation and verifies
// every sample of every plane against the independent model. Everything
// outside the blocks (including row-end padding) must keep the sentinel
// pattern; the reference pictures must be byte-identical afterwards.
int run_and_verify(Fixture& F, const BlockSpec* blocks, int nBlocks, Counters& ctr)
{
  TestContext ctx(F);
  const int bitDepth = F.sps->BitDepth_Y;

  // --- sentinel-fill the current image (full allocated region) ---
  for (int cIdx = 0; cIdx < 3; cIdx++) {
    const ptrdiff_t stride = F.cur->get_image_stride(cIdx);
    int w, h; plane_dims(F, cIdx, w, h);
    const bool hbd = high_depth(F.cur.get(), cIdx);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < stride; x++)
        set_sample(F.cur.get(), cIdx, x, y, (int) sentinel_value(cIdx, x, y, hbd));
  }

  // --- snapshot the reference pictures ---
  std::vector<std::vector<uint8_t>> refSnapshot;
  for (const auto& r : F.refs) {
    for (int cIdx = 0; cIdx < 3; cIdx++) {
      const ptrdiff_t stride = r->get_image_stride(cIdx);
      int w, h; plane_dims(F, cIdx, w, h);
      const size_t bytes = (size_t) stride * h * (high_depth(r.get(), cIdx) ? 2 : 1);
      const uint8_t* p = r->get_image_plane(cIdx);
      refSnapshot.emplace_back(p, p + bytes);
    }
  }

  // --- expected values: sentinel everywhere, model inside the blocks ---
  std::vector<int32_t> expected[3];
  for (int cIdx = 0; cIdx < 3; cIdx++) {
    const ptrdiff_t stride = F.cur->get_image_stride(cIdx);
    int w, h; plane_dims(F, cIdx, w, h);
    const bool hbd = high_depth(F.cur.get(), cIdx);
    expected[cIdx].assign((size_t) stride * h, 0);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < stride; x++)
        expected[cIdx][(size_t) y * stride + x] = (int32_t) sentinel_value(cIdx, x, y, hbd);
  }

  for (int b = 0; b < nBlocks; b++) {
    const BlockSpec& bs = blocks[b];

    PBMotion vi;
    vi.predFlag[0] = bs.predFlag0;
    vi.predFlag[1] = bs.predFlag1;
    vi.refIdx[0] = bs.refIdx0;
    vi.refIdx[1] = bs.refIdx1;
    vi.mv[0].x = (int16_t) bs.mv0x;
    vi.mv[0].y = (int16_t) bs.mv0y;
    vi.mv[1].x = (int16_t) bs.mv1x;
    vi.mv[1].y = (int16_t) bs.mv1y;

    // --- production call ---
    generate_inter_prediction_samples(&ctx, &F.shdr, F.cur.get(),
                                      bs.xP, bs.yP, 0, 0, bs.n, bs.n, bs.n, &vi);
    TASK_CHECK(F.cur->integrity != INTEGRITY_DECODING_ERRORS);

    // --- model for the same block ---
    const int predFlag[2] = { bs.predFlag0, bs.predFlag1 };
    const de265_image* refPic[2] = { nullptr, nullptr };
    for (int l = 0; l < 2; l++) {
      if (!predFlag[l]) continue;
      // Reference identity is resolved exactly like the production code:
      // through RefPicList and refIdx, never by passing samples directly.
      refPic[l] = ctx.get_image((uint16_t) F.shdr.RefPicList[l][vi.refIdx[l]]);
      TASK_CHECK(refPic[l] != nullptr);
    }

    for (int cIdx = 0; cIdx < 3; cIdx++) {
      const int subW = (cIdx == 0) ? 1 : F.sps->SubWidthC;
      const int subH = (cIdx == 0) ? 1 : F.sps->SubHeightC;
      const int bw = bs.n / subW, bh = bs.n / subH;
      const ptrdiff_t stride = F.cur->get_image_stride(cIdx);

      for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
          int32_t p[2] = { 0, 0 };
          for (int l = 0; l < 2; l++) {
            if (!predFlag[l]) continue;
            RefPlane ref { refPic[l], cIdx, 0, 0 };
            plane_dims(F, cIdx, ref.w, ref.h);
            p[l] = interp_sample(ref, cIdx == 0, bitDepth,
                                 vi.mv[l].x, vi.mv[l].y,
                                 bs.xP, bs.yP, x, y, subW, subH, ctr);
          }
          const int32_t out = combine_sample(F, cIdx, p[0], p[1],
                                             predFlag[0] != 0, predFlag[1] != 0,
                                             vi, ctr);
          ctr.samples++;
          expected[cIdx][(size_t)(bs.yP / subH + y) * stride + (bs.xP / subW + x)] = out;
        }
    }
  }

  // --- compare every sample of every plane ---
  for (int cIdx = 0; cIdx < 3; cIdx++) {
    const ptrdiff_t stride = F.cur->get_image_stride(cIdx);
    int w, h; plane_dims(F, cIdx, w, h);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < stride; x++) {
#ifdef CONTRACT_DEBUG_FIRST_DIFF
        if (get_sample(F.cur.get(), cIdx, x, y) != expected[cIdx][(size_t) y * stride + x]) {
          fprintf(stderr, "FIRST_DIFF cIdx=%d x=%d y=%d got=%d expected=%d\n",
                  cIdx, x, y, get_sample(F.cur.get(), cIdx, x, y),
                  (int) expected[cIdx][(size_t) y * stride + x]);
          return 1;
        }
#endif
        TASK_CHECK(get_sample(F.cur.get(), cIdx, x, y) ==
                   expected[cIdx][(size_t) y * stride + x]);
      }
  }

  // --- references must be untouched ---
  size_t si = 0;
  for (const auto& r : F.refs)
    for (int cIdx = 0; cIdx < 3; cIdx++) {
      const ptrdiff_t stride = r->get_image_stride(cIdx);
      int w, h; plane_dims(F, cIdx, w, h);
      const size_t bytes = (size_t) stride * h * (high_depth(r.get(), cIdx) ? 2 : 1);
      TASK_CHECK(std::memcmp(r->get_image_plane(cIdx), refSnapshot[si].data(), bytes) == 0);
      si++;
    }

  return 0;
}

// Fills F.refs with `nRefs` pictures using the noise texture and registers
// them in both reference lists (cases overwrite the lists as needed).
int make_noise_refs(Fixture& F, int nRefs)
{
  const int maxval = (1 << F.sps->BitDepth_Y) - 1;
  for (int i = 0; i < nRefs; i++) {
    auto img = make_image(F, true);
    TASK_CHECK(img != nullptr);
    for (int cIdx = 0; cIdx < 3; cIdx++) {
      int w, h; plane_dims(F, cIdx, w, h);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          set_sample(img.get(), cIdx, x, y, texel_noise(x, y, 17 + i * 31 + cIdx * 7, maxval));
    }
    F.refs.push_back(std::move(img));
  }
  return 0;
}

int make_ramp_refs(Fixture& F, int nRefs)
{
  for (int i = 0; i < nRefs; i++) {
    auto img = make_image(F, true);
    TASK_CHECK(img != nullptr);
    for (int cIdx = 0; cIdx < 3; cIdx++) {
      int w, h; plane_dims(F, cIdx, w, h);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          set_sample(img.get(), cIdx, x, y, texel_ramp(cIdx, x, y));
    }
    F.refs.push_back(std::move(img));
  }
  return 0;
}

int make_current(Fixture& F)
{
  F.cur = make_image(F, false);
  TASK_CHECK(F.cur != nullptr);
  return 0;
}

void set_ref_lists(Fixture& F, int l0a, int l0b, int l1a, int l1b)
{
  F.shdr.RefPicList[0][0] = (int) FIRST_FRAME_ID + l0a;
  F.shdr.RefPicList[0][1] = (int) FIRST_FRAME_ID + l0b;
  F.shdr.RefPicList[1][0] = (int) FIRST_FRAME_ID + l1a;
  F.shdr.RefPicList[1][1] = (int) FIRST_FRAME_ID + l1b;
}

void set_luma_weight(Fixture& F, int l, int idx, int w, int o)
{
  F.shdr.LumaWeight[l][idx] = (int16_t) w;
  F.shdr.luma_offset[l][idx] = (int16_t) o;
}

void set_chroma_weight(Fixture& F, int l, int idx, int cbw, int cbo, int crw, int cro)
{
  F.shdr.ChromaWeight[l][idx][0] = (int16_t) cbw;
  F.shdr.ChromaWeight[l][idx][1] = (int16_t) crw;
  F.shdr.ChromaOffset[l][idx][0] = (int16_t) cbo;
  F.shdr.ChromaOffset[l][idx][1] = (int16_t) cro;
}

// Counts sample positions where two references differ (the references of a
// bi-predictive case must be distinguishable, otherwise swapping lists
// could not be detected).
long count_diff(const Fixture& F, int a, int b, int cIdx)
{
  int w, h; plane_dims(F, cIdx, w, h);
  long n = 0;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (get_sample(F.refs[a].get(), cIdx, x, y) != get_sample(F.refs[b].get(), cIdx, x, y))
        n++;
  return n;
}

// ---------------------------------------------------------------------------
// Group "precision": intermediate precision, truncation vs rounding,
// negative intermediates, final saturation.
// ---------------------------------------------------------------------------

// 10 bit, P-slice, unweighted. Luma horizontal phase 1 with shift1 = 2.
// Worked example (see tests/inter_weight_contract.md): reference luma is
// f(x,y) = 2x+5y, block 8x8 at (16,16), MV (1,0). The horizontal sum at
// output (0,0) is 7198 = 4*1799+2: truncation gives 1799, round-half-up
// would give 1800. Final sample: (1799+8)>>4 = 112 (rounding would give
// 113). Every luma sample of the block sits on the same boundary, so the
// truncation rule is checked 64 times here.
int case_precision_luma_hphase_10bit()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 10, SLICE_TYPE_P, false, false));
  TASK_CHECK(make_ramp_refs(F, 1) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 0, 0, 0);

  const BlockSpec blocks[] = {
    { 16, 16, 8,  1, 0, 0, 0,  1, 0, 0, 0 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 1, ctr) == 0);

  // The worked example: hand-computed value at luma (16,16).
  TASK_CHECK(get_sample(F.cur.get(), 0, 16, 16) == 112);
  // Chroma worked example: Cb at (8,8) is (1302+8)>>4 = 81.
  TASK_CHECK(get_sample(F.cur.get(), 1, 8, 8) == 81);
  // The linear ramp makes every horizontal luma sum leave remainder 2.
  TASK_CHECK(ctr.nondiv_horizontal > 0);
  return 0;
}

// 12 bit, B-slice, plain average of two lists. Negative and positive MVs,
// both fractional directions, edge clamping on both lists, noise texture
// producing negative and non-divisible horizontal intermediates
// (shift1 = 4 at 12 bit).
int case_precision_luma_hv_12bit()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 12, SLICE_TYPE_B, false, false));
  TASK_CHECK(make_noise_refs(F, 2) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 0, 1, 1);
  TASK_CHECK(count_diff(F, 0, 1, 0) > 1000);

  const BlockSpec blocks[] = {
    { 8, 8, 16,  -29, -27, 180, 140,  1, 1, 0, 0 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 1, ctr) == 0);
  TASK_CHECK(ctr.neg_intermediate > 0);
  TASK_CHECK(ctr.nondiv_horizontal > 0);
  return 0;
}

// 10 bit, P-slice, explicit weighted prediction with an amplifying weight
// and a large negative offset: the final stage saturates at both ends of
// the sample range. Chroma uses different weights per channel, including a
// negative Cr weight.
int case_precision_weighted_saturation_10bit()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 10, SLICE_TYPE_P, true, false));
  TASK_CHECK(make_noise_refs(F, 1) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 0, 0, 0);

  F.shdr.luma_log2_weight_denom = 0;
  F.shdr.ChromaLog2WeightDenom = 0;
  set_luma_weight(F, 0, 0, 127, -100);
  set_chroma_weight(F, 0, 0, 64, 10, -64, 0);

  const BlockSpec blocks[] = {
    { 8, 8, 16,  8, -4, 0, 0,  1, 0, 0, 0 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 1, ctr) == 0);
  TASK_CHECK(ctr.sat_low > 0);
  TASK_CHECK(ctr.sat_high > 0);
  return 0;
}

// ---------------------------------------------------------------------------
// Group "lists": list combination and weight selection.
// ---------------------------------------------------------------------------

// 8 bit, P-slice, single-list unweighted prediction from two different
// references selected through refIdx 0 and 1.
int case_lists_single_l0_unweighted()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 8, SLICE_TYPE_P, false, false));
  TASK_CHECK(make_noise_refs(F, 2) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 1, 0, 0);
  TASK_CHECK(count_diff(F, 0, 1, 0) > 1000);

  const BlockSpec blocks[] = {
    {  8,  8, 16,   2,  1, 0, 0,  1, 0, 0, 0 },
    { 32, 16, 16,  -3, -2, 0, 0,  1, 0, 1, 0 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 2, ctr) == 0);
  return 0;
}

// 8 bit, B-slice, only L1 predicted, explicit weights: negative luma
// weight with a positive offset, distinct chroma weights per channel.
int case_lists_single_l1_explicit_negative_weight()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 8, SLICE_TYPE_B, false, true));
  TASK_CHECK(make_noise_refs(F, 1) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 0, 0, 0);

  F.shdr.luma_log2_weight_denom = 1;
  F.shdr.ChromaLog2WeightDenom = 1;
  set_luma_weight(F, 1, 0, -3, 40);
  set_chroma_weight(F, 1, 0, 2, -5, -1, 8);

  const BlockSpec blocks[] = {
    { 16, 8, 16,  0, 0, 3, -2,  0, 1, 0, 0 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 1, ctr) == 0);
  TASK_CHECK(ctr.sat_low > 0); // negative weight pulls large samples below 0
  return 0;
}

// 8 bit, B-slice, unweighted. Block 1 uses the same reference identity and
// the same MV in both lists (the production merge shortcut applies and is
// numerically exact for the plain average); block 2 averages two genuinely
// different references.
int case_lists_bi_plain_average()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 8, SLICE_TYPE_B, false, false));
  TASK_CHECK(make_noise_refs(F, 2) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 0, 0, 1);
  TASK_CHECK(count_diff(F, 0, 1, 0) > 1000);

  const BlockSpec blocks[] = {
    {  0, 0, 16,  4, 4,  4, 4,  1, 1, 0, 0 }, // same ref, same MV
    { 24, 8, 16,  2, 6, -4, 2,  1, 1, 0, 1 }  // different refs
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 2, ctr) == 0);
  return 0;
}

// 8 bit, B-slice, explicit weights. Worked example: the SAME reference
// identity and the SAME motion vector in both lists, but different weights
// (L0: w=1, L1: w=3, denom 0). Merging the two lists (as the unweighted
// shortcut would) yields p; the specified bi-weighted result is 2p.
// At luma (8,8) the ramp reference holds 2*8+5*8 = 56, so the answer is
// 112, not 56.
int case_lists_bi_explicit_same_ref()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 8, SLICE_TYPE_B, false, true));
  TASK_CHECK(make_ramp_refs(F, 1) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 0, 0, 0); // both lists reference the same picture

  F.shdr.luma_log2_weight_denom = 0;
  F.shdr.ChromaLog2WeightDenom = 0;
  set_luma_weight(F, 0, 0, 1, 0);
  set_luma_weight(F, 1, 0, 3, 0);
  set_chroma_weight(F, 0, 0, 1, 0, 2, 0);
  set_chroma_weight(F, 1, 0, 3, 0, 1, 0);

  const BlockSpec blocks[] = {
    { 8, 8, 16,  0, 0, 0, 0,  1, 1, 0, 0 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 1, ctr) == 0);

  // Hand-computed: pred = 56<<6 = 3584, out = (1*3584 + 3*3584 + 64) >> 7 = 112.
  TASK_CHECK(get_sample(F.cur.get(), 0, 8, 8) == 112);
  // Cb at (4,4): ramp 3*4+7*4+1 = 41, out = (1*2624 + 3*2624 + 64) >> 7 = 82.
  TASK_CHECK(get_sample(F.cur.get(), 1, 4, 4) == 82);
  // Cr at (4,4): ramp 5*4+3*4+2 = 34, out = (2*2176 + 1*2176 + 64) >> 7 = 51.
  TASK_CHECK(get_sample(F.cur.get(), 2, 4, 4) == 51);
  return 0;
}

// 10 bit, B-slice, explicit weights selected per list and per refIdx:
// RefPicList[0] = {A, B}, RefPicList[1] = {B, A}, all four (list, refIdx)
// pairs carry different weights/offsets, including a negative weight and
// offsets of both signs. The two blocks exercise both refIdx values.
int case_lists_bi_explicit_two_refs()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 10, SLICE_TYPE_B, false, true));
  TASK_CHECK(make_noise_refs(F, 2) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 1, 1, 0);
  TASK_CHECK(count_diff(F, 0, 1, 0) > 1000);

  F.shdr.luma_log2_weight_denom = 2;
  F.shdr.ChromaLog2WeightDenom = 3;
  set_luma_weight(F, 0, 0, 5, -7);
  set_luma_weight(F, 0, 1, 9, 3);
  set_luma_weight(F, 1, 0, -2, 11);
  set_luma_weight(F, 1, 1, 4, -1);
  set_chroma_weight(F, 0, 0, 6, -3, -3, 5);
  set_chroma_weight(F, 0, 1, 10, 2, 7, -8);
  set_chroma_weight(F, 1, 0, -1, 4, 3, -2);
  set_chroma_weight(F, 1, 1, 2, 6, -4, 1);

  const BlockSpec blocks[] = {
    {  8, 8, 16,  6, 2,  -2, 8,  1, 1, 0, 0 },
    { 32, 8, 16,  0, -6, 10, 0,  1, 1, 1, 1 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 2, ctr) == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// Group "planes": all three planes, chroma phases, strides, edges.
// ---------------------------------------------------------------------------

// 8 bit 4:2:0, explicit bi-prediction. Luma quarter-pel phases map to
// chroma eighth-pel phases: block 1 uses hv, block 2 uses hv with edge
// clamping at the top, block 3 uses horizontal-only and vertical-only
// chroma filtering. Cb and Cr carry different weights.
int case_planes_420_8bit_chroma_phases()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 8, SLICE_TYPE_B, false, true));
  TASK_CHECK(make_noise_refs(F, 2) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 0, 1, 1);
  TASK_CHECK(count_diff(F, 0, 1, 1) > 200);

  F.shdr.luma_log2_weight_denom = 1;
  F.shdr.ChromaLog2WeightDenom = 2;
  set_luma_weight(F, 0, 0, 3, 2);
  set_luma_weight(F, 1, 0, -1, 6);
  set_chroma_weight(F, 0, 0, 2, -1, 1, 4);
  set_chroma_weight(F, 1, 0, -2, 3, 4, -2);

  const BlockSpec blocks[] = {
    {  0,  0, 16,  1,  1,  3, -1,  1, 1, 0, 0 },
    { 40,  0, 16,  5, -3, -7,  3,  1, 1, 0, 0 },
    {  0, 24, 16,  2,  0,  0,  6,  1, 1, 0, 0 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 3, ctr) == 0);
  return 0;
}

// 10 bit 4:2:0, plain bi-prediction average at the right/bottom picture
// edge: both lists clamp luma and chroma coordinates. 16-bit planes are
// checked over the full stride so that a byte-stride/sample-stride mix-up
// would land in sentinel territory.
int case_planes_420_10bit_edge_strides()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 10, SLICE_TYPE_B, false, false));
  TASK_CHECK(make_noise_refs(F, 2) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 0, 1, 1);
  TASK_CHECK(count_diff(F, 0, 1, 2) > 200);

  const BlockSpec blocks[] = {
    { 40, 24, 16,  61,  62, -80, -72,  1, 1, 0, 0 },
    {  0,  0,  8, -29, -27,  29,  27,  1, 1, 0, 0 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 2, ctr) == 0);
  TASK_CHECK(ctr.neg_intermediate > 0);
  TASK_CHECK(ctr.nondiv_horizontal > 0);
  return 0;
}

// 12 bit 4:2:0, single-list unweighted, full-sample motion vectors:
// interior direct copy, clamped copy at the top-left corner, and heavy
// clamping at the bottom-right where the vector points fully outside.
int case_planes_420_12bit_fullpel()
{
  Fixture F;
  TASK_CHECK(make_headers(F, 12, SLICE_TYPE_P, false, false));
  TASK_CHECK(make_noise_refs(F, 1) == 0);
  TASK_CHECK(make_current(F) == 0);
  set_ref_lists(F, 0, 0, 0, 0);

  const BlockSpec blocks[] = {
    {  8,  8, 16,  16,   8, 0, 0,  1, 0, 0, 0 },
    {  4,  4,  8, -32, -16, 0, 0,  1, 0, 0, 0 },
    { 32, 16, 16,  96,  88, 0, 0,  1, 0, 0, 0 }
  };

  Counters ctr;
  TASK_CHECK(run_and_verify(F, blocks, 3, ctr) == 0);
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration (consumed by task_support/driver.c).
// ---------------------------------------------------------------------------

const task_case task_cases[] = {
  { "precision", "luma_hphase_10bit_truncation",  case_precision_luma_hphase_10bit },
  { "precision", "luma_hv_12bit_intermediates",   case_precision_luma_hv_12bit },
  { "precision", "weighted_saturation_10bit",     case_precision_weighted_saturation_10bit },
  { "lists",     "single_l0_unweighted",          case_lists_single_l0_unweighted },
  { "lists",     "single_l1_explicit_negative_weight", case_lists_single_l1_explicit_negative_weight },
  { "lists",     "bi_plain_average",              case_lists_bi_plain_average },
  { "lists",     "bi_explicit_same_ref",          case_lists_bi_explicit_same_ref },
  { "lists",     "bi_explicit_two_refs",          case_lists_bi_explicit_two_refs },
  { "planes",    "yuv420_8bit_chroma_phases",     case_planes_420_8bit_chroma_phases },
  { "planes",    "yuv420_10bit_edge_strides",     case_planes_420_10bit_edge_strides },
  { "planes",    "yuv420_12bit_fullpel",          case_planes_420_12bit_fullpel }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
