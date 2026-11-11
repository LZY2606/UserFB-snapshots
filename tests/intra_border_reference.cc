/*
 * Regression test for libde265's intra reference-sample machinery.
 *
 * The test builds small, fully explicit de265_image / SPS / PPS objects:
 *
 *   - every pixel is written by the test itself with a value that identifies
 *     its position (so a sample taken from the wrong place is visible),
 *   - tile boundaries, slice ids, inter/intra block modes and constrained
 *     intra prediction are programmed through the real metadata APIs,
 *   - the scan-order tables come from pps->set_derived_values(), i.e. from
 *     the production code path.
 *
 * The expected values are produced by intra_border_refmodel.{h,cc}, an
 * independent implementation of the relevant HEVC clauses that works only
 * from the geometry description and the pixels the test wrote.  It never calls
 * libde265.
 *
 * For each border case we compare, separately:
 *   - the availability mask,
 *   - the samples that were available before substitution,
 *   - the complete border after reference-sample substitution.
 *
 * For prediction cases we feed the independent border through the independent
 * planar/DC/angular predictors and compare the whole output block against
 * decode_intra_prediction().  When reference sample filtering is active, the
 * border is checked both before and after filtering, and the block is
 * predicted from the filtered reference.  Every image sample outside the
 * predicted block is verified to be unchanged.
 */

#include "intra_border_refmodel.h"

#include "libde265/de265.h"
#include "libde265/decctx.h"
#include "libde265/image.h"
#include "libde265/intrapred.h"
#include "libde265/pps.h"
#include "libde265/sps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

static long g_checks = 0;
static int g_failures = 0;

static void fail(const std::string& name, const char* what, long long index,
                 long long expected, long long actual)
{
  std::printf("FAIL case=%s what=%s index=%lld expected=%lld actual=%lld\n",
              name.c_str(), what, index, expected, actual);
  g_failures++;
}

// Reports only the first difference of a category, so a broken case produces
// one actionable line instead of thousands.
struct diff_reporter
{
  std::string name;
  const char* what;

  long long count = 0;
  long long first_index = 0, first_expected = 0, first_actual = 0;

  diff_reporter(std::string n, const char* w) : name(std::move(n)), what(w) {}

  void hit(long long index, long long expected, long long actual)
  {
    if (count == 0) {
      first_index = index;
      first_expected = expected;
      first_actual = actual;
    }
    count++;
  }

  void finish()
  {
    if (count == 0) return;
    fail(name, what, first_index, first_expected, first_actual);
    if (count > 1)
      std::printf("DIFFCOUNT case=%s what=%s differences=%lld\n",
                  name.c_str(), what, count);
  }
};

struct test_scene
{
  std::string name;
  std::shared_ptr<seq_parameter_set> sps;
  std::shared_ptr<pic_parameter_set> pps;
  std::unique_ptr<decoder_context> decctx;
  std::unique_ptr<de265_image> img;
  ref_scene model;
};

struct border_case
{
  const char* label;
  int xB, yB, nT, cIdx;
};

struct prediction_case
{
  const char* label;
  int xB, yB, nT, cIdx, mode;
};

// ---------------------------------------------------------------------------
// Scene construction
// ---------------------------------------------------------------------------

static void fill_planes(de265_image& img, int bit_depth, bool gradient)
{
  for (int c = 0; c < 3; c++) {
    const int w = img.get_width(c);
    const int h = img.get_height(c);
    const int maxv = (1 << bit_depth) - 1;

    int scale = 1;
    if (gradient) {
      const int denom = w + h - 2;
      if (denom > 0) {
        scale = maxv / denom;
        if (scale < 1) scale = 1;
      }
    }

    if (img.high_bit_depth(c)) {
      uint16_t* p = reinterpret_cast<uint16_t*>(img.get_image_plane(c));
      const ptrdiff_t stride = img.get_image_stride(c);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          p[x + y * stride] = static_cast<uint16_t>(
              gradient ? (x + y) * scale : ((x * 29 + y * 37 + c * 11) & maxv));
    }
    else {
      uint8_t* p = img.get_image_plane(c);
      const ptrdiff_t stride = img.get_image_stride(c);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          p[x + y * stride] = static_cast<uint8_t>(
              gradient ? (x + y) * scale : ((x * 29 + y * 37 + c * 11) & maxv));
    }
  }
}

static std::unique_ptr<test_scene> make_scene(
    const std::string& name,
    int w, int h, int bit_depth, int log2_ctb,
    const std::vector<int>& col_widths,
    const std::vector<int>& row_heights,
    const std::vector<int>& slice_map,
    const std::vector<std::pair<int,int>>& inter_cbs,
    bool constrained, bool strong_smoothing, bool gradient)
{
  std::unique_ptr<test_scene> s(new test_scene);
  s->name = name;

  s->sps = std::make_shared<seq_parameter_set>();
  s->sps->set_defaults();
  s->sps->set_resolution(w, h);
  s->sps->set_CB_log2size_range(3, log2_ctb);
  s->sps->set_TB_log2size_range(2, 5);
  s->sps->bit_depth_luma = static_cast<uint8_t>(bit_depth);
  s->sps->bit_depth_chroma = static_cast<uint8_t>(bit_depth);
  s->sps->chroma_format_idc = 1;  // 4:2:0
  s->sps->strong_intra_smoothing_enable_flag = strong_smoothing;
  if (s->sps->compute_derived_values() != DE265_OK) {
    std::fprintf(stderr, "scene %s: SPS rejected\n", name.c_str());
    std::exit(2);
  }

  s->pps = std::make_shared<pic_parameter_set>();
  s->pps->set_defaults();
  if (!col_widths.empty()) {
    if (col_widths.size() > DE265_MAX_TILE_COLUMNS ||
        row_heights.size() > DE265_MAX_TILE_ROWS) {
      std::fprintf(stderr, "scene %s: too many tiles\n", name.c_str());
      std::exit(2);
    }

    int sum = 0;
    for (size_t i = 0; i < col_widths.size(); i++) sum += col_widths[i];
    if (sum != s->sps->PicWidthInCtbsY) {
      std::fprintf(stderr, "scene %s: tile columns do not cover the picture\n", name.c_str());
      std::exit(2);
    }
    sum = 0;
    for (size_t i = 0; i < row_heights.size(); i++) sum += row_heights[i];
    if (sum != s->sps->PicHeightInCtbsY) {
      std::fprintf(stderr, "scene %s: tile rows do not cover the picture\n", name.c_str());
      std::exit(2);
    }

    s->pps->tiles_enabled_flag = 1;
    s->pps->uniform_spacing_flag = 0;
    s->pps->num_tile_columns = static_cast<uint8_t>(col_widths.size());
    s->pps->num_tile_rows = static_cast<uint8_t>(row_heights.size());
    for (size_t i = 0; i < col_widths.size(); i++)
      s->pps->colWidth[i] = static_cast<uint16_t>(col_widths[i]);
    for (size_t i = 0; i < row_heights.size(); i++)
      s->pps->rowHeight[i] = static_cast<uint16_t>(row_heights[i]);
  }
  s->pps->constrained_intra_pred_flag = constrained;
  s->pps->set_derived_values(s->sps.get());

  s->decctx.reset(new decoder_context);
  s->img.reset(new de265_image);
  if (s->img->alloc_image(w, h, de265_chroma_420, s->sps, true, s->decctx.get(),
                          0, nullptr, false) != DE265_OK) {
    std::fprintf(stderr, "scene %s: image allocation failed\n", name.c_str());
    std::exit(2);
  }
  s->img->set_headers(std::shared_ptr<video_parameter_set>(), s->sps, s->pps);

  // --- description used by the independent reference model ---

  s->model.picW = w;
  s->model.picH = h;
  s->model.log2Ctb = s->sps->Log2CtbSizeY;
  s->model.log2MinTb = s->sps->Log2MinTrafoSize;
  s->model.log2MinCb = s->sps->Log2MinCbSizeY;
  s->model.subW = s->sps->SubWidthC;
  s->model.subH = s->sps->SubHeightC;
  s->model.chromaArrayType = s->sps->ChromaArrayType;
  s->model.bitDepth = bit_depth;
  s->model.strongIntraSmoothing = strong_smoothing;
  s->model.constrainedIntra = constrained;
  s->model.intraSmoothingDisabled = false;

  for (int i = 0; i <= s->pps->num_tile_columns; i++)
    s->model.colBd.push_back(s->pps->colBd[i]);
  for (int i = 0; i <= s->pps->num_tile_rows; i++)
    s->model.rowBd.push_back(s->pps->rowBd[i]);

  const int ctbsW = s->model.picWidthInCtbs();
  const int ctbsH = s->model.picHeightInCtbs();
  s->model.sliceOfCtb.resize(ctbsW * ctbsH, 0);

  for (int y = 0; y < ctbsH; y++)
    for (int x = 0; x < ctbsW; x++) {
      const int value = slice_map.empty() ? 0 : slice_map[x + y * ctbsW];
      s->model.sliceOfCtb[x + y * ctbsW] = value;
      s->img->set_SliceAddrRS(x, y, value);
    }

  s->model.interCb.assign(s->model.picWidthInCbs() * s->model.picHeightInCbs(), 0);
  for (const auto& p : inter_cbs) {
    s->img->set_pred_mode(p.first, p.second, s->sps->Log2MinCbSizeY, MODE_INTER);
    const int cx = p.first >> s->model.log2MinCb;
    const int cy = p.second >> s->model.log2MinCb;
    s->model.interCb[cx + cy * s->model.picWidthInCbs()] = 1;
  }

  fill_planes(*s->img, bit_depth, gradient);

  return s;
}

// ---------------------------------------------------------------------------
// Border checks
// ---------------------------------------------------------------------------

template <class pixel_t>
void check_border(test_scene& sc, const border_case& c)
{
  const int MAXN = MAX_INTRA_PRED_BLOCK_SIZE;
  pixel_t prod_mem[4 * MAXN + 1];
  pixel_t* prod = &prod_mem[2 * MAXN];

  intra_border_computer<pixel_t> comp;
  comp.init(prod, sc.img.get(), c.nT, c.cIdx, c.xB, c.yB);
  comp.preproc();

  const bool gate_left = comp.availableLeft;
  const bool gate_top = comp.availableTop;
  const bool gate_topleft = comp.availableTopLeft;
  const bool gate_topright = comp.availableTopRight;

  comp.fill_from_image();

  pixel_t ref_mem[4 * MAXN + 1];
  uint8_t ref_av_mem[4 * MAXN + 1];
  pixel_t* refb = &ref_mem[2 * MAXN];
  uint8_t* ref_av = &ref_av_mem[2 * MAXN];

  const pixel_t* plane = reinterpret_cast<const pixel_t*>(sc.img->get_image_plane(c.cIdx));
  const ptrdiff_t stride = sc.img->get_image_stride(c.cIdx);

  ref_reference_samples(sc.model, plane, stride, c.cIdx, c.xB, c.yB, c.nT, refb, ref_av);

  int n_avail = 0;
  for (int i = -2 * c.nT; i <= 2 * c.nT; i++) n_avail += ref_av[i] ? 1 : 0;

  const std::string name = sc.name + "/" + c.label;

  std::printf("COVER border case=%s nT=%d cIdx=%d xB=%d yB=%d nAvail=%d/%d"
              " gate-left=%d gate-top=%d gate-topleft=%d gate-topright=%d\n",
              name.c_str(), c.nT, c.cIdx, c.xB, c.yB,
              n_avail, 4 * c.nT + 1,
              gate_left, gate_top, gate_topleft, gate_topright);

  // 1) Availability mask.
  {
    diff_reporter rep(name, "availability");
    for (int i = -2 * c.nT; i <= 2 * c.nT; i++) {
      g_checks++;
      const long long prod_value = comp.available[i] ? 1 : 0;
      const long long ref_value = ref_av[i] ? 1 : 0;
      if (prod_value != ref_value) rep.hit(i, ref_value, prod_value);
    }
    rep.finish();
  }

  // 2) Values of the samples that were available before substitution.
  {
    diff_reporter rep(name, "known-value");
    for (int i = -2 * c.nT; i <= 2 * c.nT; i++) {
      if (!ref_av[i]) continue;
      g_checks++;
      const long long prod_value = static_cast<long long>(prod[i]);
      const long long ref_value = static_cast<long long>(refb[i]);
      if (prod_value != ref_value) rep.hit(i, ref_value, prod_value);
    }
    rep.finish();
  }

  // 3) The whole border after reference-sample substitution.
  comp.reference_sample_substitution();
  {
    diff_reporter rep(name, "substituted-border");
    for (int i = -2 * c.nT; i <= 2 * c.nT; i++) {
      g_checks++;
      const long long prod_value = static_cast<long long>(comp.out_border[i]);
      const long long ref_value = static_cast<long long>(refb[i]);
      if (prod_value != ref_value) rep.hit(i, ref_value, prod_value);
    }
    rep.finish();
  }
}

// ---------------------------------------------------------------------------
// Prediction checks
// ---------------------------------------------------------------------------

template <class pixel_t>
void check_prediction(test_scene& sc, const prediction_case& c)
{
  de265_image* img = sc.img.get();

  const int w = img->get_width(c.cIdx);
  const int h = img->get_height(c.cIdx);
  const ptrdiff_t stride = img->get_image_stride(c.cIdx);
  pixel_t* plane = reinterpret_cast<pixel_t*>(img->get_image_plane(c.cIdx));

  const std::string name = sc.name + "/" + c.label;

  if (c.xB + c.nT > w || c.yB + c.nT > h) {
    std::printf("SKIP prediction case=%s: block does not fit the plane\n", name.c_str());
    return;
  }

  // Snapshot the plane; prediction may only modify the nT*nT output block.
  std::vector<pixel_t> before(static_cast<size_t>(stride) * h);
  for (int y = 0; y < h; y++)
    std::memcpy(&before[static_cast<size_t>(y) * stride],
                plane + static_cast<size_t>(y) * stride,
                static_cast<size_t>(stride) * sizeof(pixel_t));

  // Independent border, substitution and filtering.
  const int MAXN = MAX_INTRA_PRED_BLOCK_SIZE;
  pixel_t ref_raw_mem[4 * MAXN + 1];
  pixel_t ref_filt_mem[4 * MAXN + 1];
  pixel_t* ref_raw = &ref_raw_mem[2 * MAXN];
  pixel_t* ref_filt = &ref_filt_mem[2 * MAXN];
  uint8_t ref_av_mem[4 * MAXN + 1];
  uint8_t* ref_av = &ref_av_mem[2 * MAXN];

  ref_reference_samples(sc.model, plane, stride, c.cIdx, c.xB, c.yB, c.nT, ref_raw, ref_av);
  for (int i = -2 * c.nT; i <= 2 * c.nT; i++) ref_filt[i] = ref_raw[i];
  ref_filter_border(sc.model, ref_filt, c.nT, c.cIdx, c.mode);

  std::vector<pixel_t> ref_block(static_cast<size_t>(c.nT) * c.nT);
  ref_predict(sc.model, ref_block.data(), c.nT, c.mode, c.nT, c.cIdx, false, ref_filt);

  std::printf("PRED case=%s nT=%d cIdx=%d xB=%d yB=%d mode=%d filter=%d\n",
              name.c_str(), c.nT, c.cIdx, c.xB, c.yB, c.mode,
              ref_filter_is_active(sc.model, c.nT, c.cIdx, c.mode) ? 1 : 0);

  // Production border (same pipeline as decode_intra_prediction uses internally).
  pixel_t prod_mem[4 * MAXN + 1];
  pixel_t* prod = &prod_mem[2 * MAXN];

  intra_border_computer<pixel_t> comp;
  comp.init(prod, img, c.nT, c.cIdx, c.xB, c.yB);
  comp.preproc();
  comp.fill_from_image();
  comp.reference_sample_substitution();

  // Border before filtering.
  {
    diff_reporter rep(name, "border-before-filtering");
    for (int i = -2 * c.nT; i <= 2 * c.nT; i++) {
      g_checks++;
      const long long prod_value = static_cast<long long>(comp.out_border[i]);
      const long long ref_value = static_cast<long long>(ref_raw[i]);
      if (prod_value != ref_value) rep.hit(i, ref_value, prod_value);
    }
    rep.finish();
  }

  // Border after filtering.  decode_intra_prediction() filters only when the
  // SPS range extension allows it and the component is luma (or 4:4:4).
  const bool filter_gate =
      (img->get_sps().range_extension.intra_smoothing_disabled_flag == 0 &&
       (c.cIdx == 0 || img->get_sps().ChromaArrayType == CHROMA_444));

  pixel_t prod_filt_mem[4 * MAXN + 1];
  pixel_t* prod_filt = &prod_filt_mem[2 * MAXN];
  for (int i = -2 * c.nT; i <= 2 * c.nT; i++) prod_filt[i] = comp.out_border[i];

  if (filter_gate)
    intra_prediction_sample_filtering<pixel_t>(img->get_sps(), prod_filt, c.nT,
                                               c.cIdx,
                                               static_cast<IntraPredMode>(c.mode));

  {
    diff_reporter rep(name, "border-after-filtering");
    for (int i = -2 * c.nT; i <= 2 * c.nT; i++) {
      g_checks++;
      const long long prod_value = static_cast<long long>(prod_filt[i]);
      const long long ref_value = static_cast<long long>(ref_filt[i]);
      if (prod_value != ref_value) rep.hit(i, ref_value, prod_value);
    }
    rep.finish();
  }

  // Production prediction.
  decode_intra_prediction(img, c.xB, c.yB, static_cast<IntraPredMode>(c.mode),
                          c.nT, c.cIdx);

  {
    diff_reporter rep(name, "prediction");
    for (int y = 0; y < c.nT; y++)
      for (int x = 0; x < c.nT; x++) {
        g_checks++;
        const long long actual = static_cast<long long>(plane[c.xB + x + (c.yB + y) * stride]);
        const long long expected = static_cast<long long>(ref_block[x + y * c.nT]);
        if (actual != expected) rep.hit(y * c.nT + x, expected, actual);
      }
    rep.finish();
  }

  // Nothing outside the predicted block may be written.
  {
    diff_reporter rep(name, "outside-write");
    for (int y = 0; y < h; y++)
      for (int x = 0; x < static_cast<int>(stride); x++) {
        if (x >= c.xB && x < c.xB + c.nT && y >= c.yB && y < c.yB + c.nT) continue;
        g_checks++;
        const long long after = static_cast<long long>(plane[x + y * stride]);
        const long long original = static_cast<long long>(before[static_cast<size_t>(y) * stride + x]);
        if (after != original) rep.hit(x + y * static_cast<int>(stride), original, after);
      }
    rep.finish();
  }

  // Restore the scene for the following cases.
  for (int y = 0; y < h; y++)
    std::memcpy(plane + static_cast<size_t>(y) * stride,
                &before[static_cast<size_t>(y) * stride],
                static_cast<size_t>(stride) * sizeof(pixel_t));
}

static void run_border(test_scene& sc, const border_case& c)
{
  if (sc.img->high_bit_depth(c.cIdx))
    check_border<uint16_t>(sc, c);
  else
    check_border<uint8_t>(sc, c);
}

static void run_prediction(test_scene& sc, const prediction_case& c)
{
  if (sc.img->high_bit_depth(c.cIdx))
    check_prediction<uint16_t>(sc, c);
  else
    check_prediction<uint8_t>(sc, c);
}

} // namespace

int main()
{
  if (de265_init() != DE265_OK) {
    std::fprintf(stderr, "de265_init() failed\n");
    return 2;
  }

  // ---------------------------------------------------------------------
  // Scene A: 8 bit, 96x72, CTB 32, non-uniform 2x2 tiles, two slices,
  // constrained intra prediction and a few inter-coded blocks.
  //
  // Tile boundaries in CTBs:  colBd = {0,1,3}, rowBd = {0,2,3}.
  // Slice ids per CTB (raster): a horizontal boundary at CTB row 2 and a
  // vertical boundary inside tile T1 (CTB (0,1) is a slice of its own), so
  // that slice and tile isolation can also be observed independently.
  // ---------------------------------------------------------------------
  {
    auto scene = make_scene("A-8bit-tiles-slices-constrained",
                            96, 72, 8, 5,
                            {1, 2}, {2, 1},
                            {0, 0, 0,
                             1, 0, 0,
                             1, 1, 1},
                            {{56, 40}, {56, 16}, {72, 16}},  // inter-coded 8x8 blocks
                            true, false, false);

    const border_case cases[] = {
      {"corner-top-left-nT16",          0,  0, 16, 0},
      {"corner-top-left-nT32",          0,  0, 32, 0},
      {"tile-isolation-left",          32, 16,  8, 0},
      {"slice-isolation-top",          16, 32,  8, 0},
      {"scan-order-left-below-nT8",    40, 36,  8, 0},
      {"scan-order-left-below-nT4",    40, 36,  4, 0},
      {"constrained-inter-left-nT16",  64, 32, 16, 0},
      {"constrained-topleft-topright-nT8", 64, 24, 8, 0},
      {"tile-slice-isolated-nT8",      32, 64,  8, 0},
      {"top-edge-left-available",      64,  0, 32, 0},
      {"bottom-edge-truncated",        64, 64,  8, 0},
      {"left-scan-shadow",             72, 32,  8, 0},
      {"chroma-corner-nT4",             0,  0,  4, 1},
      {"chroma-all-available-nT8",     24, 16,  8, 1},
      {"chroma-right-truncated-nT8",   40, 16,  8, 1},
    };

    for (const auto& c : cases) run_border(*scene, c);

    const prediction_case predictions[] = {
      {"pred-scan-order-planar",   40, 36,  8, 0, REF_INTRA_PLANAR},
      {"pred-scan-order-dc",       40, 36,  8, 0, REF_INTRA_DC},
      {"pred-scan-order-pos34",    40, 36,  8, 0, REF_INTRA_ANGULAR_34},
      {"pred-scan-order-neg18",    40, 36,  8, 0, REF_INTRA_ANGULAR_18},
      {"pred-scan-order-vert26",   40, 36,  8, 0, REF_INTRA_ANGULAR_26},
      {"pred-nT4-planar",          40, 36,  4, 0, REF_INTRA_PLANAR},
      {"pred-nT4-dc",              40, 36,  4, 0, REF_INTRA_DC},
      {"pred-nT4-pos34",           40, 36,  4, 0, REF_INTRA_ANGULAR_34},
      {"pred-nT4-neg18",           40, 36,  4, 0, REF_INTRA_ANGULAR_18},
      {"pred-constrained-planar",  64, 32, 16, 0, REF_INTRA_PLANAR},
      {"pred-constrained-dc",      64, 32, 16, 0, REF_INTRA_DC},
      {"pred-constrained-pos34",   64, 32, 16, 0, REF_INTRA_ANGULAR_34},
      {"pred-constrained-neg18",   64, 32, 16, 0, REF_INTRA_ANGULAR_18},
      {"pred-constrained-topleft-dc",    64, 24, 8, 0, REF_INTRA_DC},
      {"pred-constrained-topleft-pos34", 64, 24, 8, 0, REF_INTRA_ANGULAR_34},
      {"pred-all-unavailable-planar", 32, 0, 32, 0, REF_INTRA_PLANAR},
      {"pred-all-unavailable-dc",     32, 0, 32, 0, REF_INTRA_DC},
      {"pred-all-unavailable-pos34",  32, 0, 32, 0, REF_INTRA_ANGULAR_34},
      {"pred-all-unavailable-neg18",  32, 0, 32, 0, REF_INTRA_ANGULAR_18},
      {"pred-top-edge-planar",     64,  0, 32, 0, REF_INTRA_PLANAR},
      {"pred-top-edge-pos34",      64,  0, 32, 0, REF_INTRA_ANGULAR_34},
      {"pred-chroma-truncated-dc",     40, 16,  8, 1, REF_INTRA_DC},
      {"pred-chroma-truncated-pos34",  40, 16,  8, 1, REF_INTRA_ANGULAR_34},
      {"pred-chroma-truncated-planar", 40, 16,  8, 1, REF_INTRA_PLANAR},
      {"pred-chroma-truncated-neg18",  40, 16,  8, 1, REF_INTRA_ANGULAR_18},
      {"pred-chroma-nT4-dc",           24, 16,  4, 1, REF_INTRA_DC},
      {"pred-chroma-nT4-planar",       24, 16,  4, 1, REF_INTRA_PLANAR},
    };

    for (const auto& c : predictions) run_prediction(*scene, c);
  }

  // ---------------------------------------------------------------------
  // Scene B: 10 bit, same geometry, single tile.  A slice boundary makes the
  // left column of the block at (32,32) unavailable while its top-left sample
  // is available, so the first available reference sample is the top-left one
  // (and not a left-column sample).  Exercises the 16-bit pixel path.
  // ---------------------------------------------------------------------
  {
    auto scene = make_scene("B-10bit", 96, 72, 10, 5,
                            {}, {}, {0, 0, 0,
                                     1, 0, 0,
                                     0, 0, 0}, {},
                            false, false, false);

    const border_case cases[] = {
      {"corner-top-left-nT16",        0,  0, 16, 0},
      {"fully-available-nT8",        64, 32,  8, 0},
      {"slice-isolation-left-tl-firstvalue", 32, 32, 8, 0},
      {"scan-order-left-below-nT8",  40, 36,  8, 0},
      {"bottom-edge-truncated",      64, 64,  8, 0},
      {"chroma-corner-nT4",           0,  0,  4, 1},
      {"chroma-all-available-nT8",   24, 16,  8, 1},
      {"chroma-right-truncated-nT8", 40, 16,  8, 1},
    };

    for (const auto& c : cases) run_border(*scene, c);

    const prediction_case predictions[] = {
      {"pred-scan-order-planar", 40, 36,  8, 0, REF_INTRA_PLANAR},
      {"pred-scan-order-dc",     40, 36,  8, 0, REF_INTRA_DC},
      {"pred-scan-order-pos34",  40, 36,  8, 0, REF_INTRA_ANGULAR_34},
      {"pred-scan-order-neg18",  40, 36,  8, 0, REF_INTRA_ANGULAR_18},
      {"pred-tl-firstvalue-planar", 32, 32, 8, 0, REF_INTRA_PLANAR},
      {"pred-tl-firstvalue-dc",     32, 32, 8, 0, REF_INTRA_DC},
      {"pred-all-unavailable-pos34", 32, 0, 32, 0, REF_INTRA_ANGULAR_34},
      {"pred-all-unavailable-dc",    32, 0, 32, 0, REF_INTRA_DC},
      {"pred-chroma-truncated-pos34", 40, 16, 8, 1, REF_INTRA_ANGULAR_34},
      {"pred-chroma-truncated-planar",40, 16, 8, 1, REF_INTRA_PLANAR},
      {"pred-corner-dc", 0, 0, 16, 0, REF_INTRA_DC},
    };

    for (const auto& c : predictions) run_prediction(*scene, c);
  }

  // ---------------------------------------------------------------------
  // Scene C: 8 bit gradient with strong intra smoothing enabled and CTB 64.
  // The 32x32 block at (64,64) has a completely available, linear reference
  // border, so the bi-linear (strong smoothing) filtering path is exercised.
  // ---------------------------------------------------------------------
  {
    auto scene = make_scene("C-8bit-strong-smoothing-gradient",
                            128, 128, 8, 6,
                            {}, {}, {}, {},
                            false, true, true);

    const border_case cases[] = {
      {"fully-available-nT32", 64, 64, 32, 0},
    };
    for (const auto& c : cases) run_border(*scene, c);

    const prediction_case predictions[] = {
      {"pred-biint-planar", 64, 64, 32, 0, REF_INTRA_PLANAR},
      {"pred-biint-pos34",  64, 64, 32, 0, REF_INTRA_ANGULAR_34},
      {"pred-biint-neg18",  64, 64, 32, 0, REF_INTRA_ANGULAR_18},
      {"pred-biint-dc",     64, 64, 32, 0, REF_INTRA_DC},
    };
    for (const auto& c : predictions) run_prediction(*scene, c);
  }

  de265_free();

  std::printf("RESULT: %ld comparisons, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
