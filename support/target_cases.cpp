/*
 * Target cases for the bounded in-place deblocking/SAO pipeline.
 *
 * The test builds synthetic pictures with real SPS/PPS, scan tables and slice
 * metadata (no bitstream parsing), runs the production in-place pipeline on
 * the decoder's own thread pool, and compares every sample against the
 * retained sequential implementation (whole-image deblocking followed by the
 * whole-copy sequential SAO).
 */

#include "checks.h"

#include "libde265/decctx.h"
#include "libde265/filter_pipeline.h"
#include "libde265/deblock.h"
#include "libde265/sao.h"
#include "libde265/image.h"

#include <libde265/de265.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

constexpr int LOG2_CTB = 6;      // 64x64 CTB
constexpr int LOG2_MIN_CB = 4;   // 16x16 minimum CB
constexpr int WAIT_SECONDS = 15; // handshake failure deadline, not a timing probe

struct Geometry {
  int w, h, bitDepth;
  bool saoEnabled;
  bool tiles;                 // 2 tile columns, filtering across tiles disabled
  int sliceSplitRow;          // CTB row of the second slice, -1 for one slice
  bool bypassBlock;           // one CTB uses transquant bypass / PCM rules
};

struct BuiltPicture {
  std::shared_ptr<video_parameter_set> vps;
  std::shared_ptr<seq_parameter_set> sps;
  std::shared_ptr<pic_parameter_set> pps;
  std::vector<slice_segment_header*> slices;
  de265_image* img = nullptr;

  // Must be destroyed before the decoder context: ~de265_image() dereferences
  // its decctx through the image allocation functions.
  void reset() { delete img; img = nullptr; }
  ~BuiltPicture() { reset(); }
};

static void put_sample(de265_image* img, int c, int x, int y, int value) {
  uint8_t* p = img->get_image_plane_at_pos(c, x, y);
  if (img->high_bit_depth(c)) {
    *reinterpret_cast<uint16_t*>(p) = (uint16_t)value;
  } else {
    *p = (uint8_t)value;
  }
}

static int get_sample(const de265_image* img, int c, int x, int y) {
  const uint8_t* p = img->get_image_plane_at_pos(c, x, y);
  return img->high_bit_depth(c) ? *reinterpret_cast<const uint16_t*>(p) : *p;
}

// Non-flat deterministic content with gentle, filterable steps on every CTB
// boundary (both luma and chroma). Vertical and diagonal structure makes SAO
// edge-offset classification of one row depend on its neighbours.
static int boundary_pixel(const Geometry& g, int c, int x, int y, uint32_t salt) {
  const int maxVal = (1 << g.bitDepth) - 1;
  uint32_t u = (uint32_t)x * 1103515245u + (uint32_t)y * 12345u +
               (uint32_t)c * 2654435761u + salt * 40503u;
  int v = 32 + (int)((u >> 9) % 64);
  if ((x & 15) == 0) v = std::min(maxVal, v + 10);       // vertical edges
  v += ((x + y) / 11) & 7;                                // diagonal ramp
  if (v > maxVal) v = maxVal;
  if (c == 0 && y > 0 && (y % (1 << LOG2_CTB)) == 0) {
    v = std::min(maxVal, v + 5);                          // horizontal CTB step
  }
  if (c != 0 && y > 0 && (y % ((1 << LOG2_CTB) / 2)) == 0) {
    v = std::min(maxVal, v + 3);                          // chroma step
  }
  return v;
}

static void fill_planes(de265_image* img, const Geometry& g, uint32_t salt) {
  for (int c = 0; c < 3; c++) {
    const int w = img->get_width(c);
    const int h = img->get_height(c);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        put_sample(img, c, x, y, boundary_pixel(g, c, x, y, salt));
      }
    }
  }
}

// Fill the per-row stride slack with a sentinel. That memory is part of the
// allocated plane for every build (unlike the optional SIMD overread
// padding), so proving it untouched covers the "no buffer-slot coordinates
// instead of image coordinates" requirement.
static void paint_padding_sentinel(de265_image* img) {
  for (int c = 0; c < 3; c++) {
    const int bpp = img->get_bytes_per_pixel(c);
    const ptrdiff_t stride = img->get_image_stride(c);
    const int w = img->get_width(c);
    const int h = img->get_height(c);
    uint8_t* plane = img->get_image_plane(c);
    for (int y = 0; y < h; y++) {
      uint8_t* row = plane + (size_t)y * stride * bpp;
      for (int x = w; x < stride; x++) {
        row[x * bpp] = 0xA5;
        if (bpp == 2) row[x * bpp + 1] = 0x5A;
      }
    }
  }
}

static bool padding_intact(const de265_image* img) {
  for (int c = 0; c < 3; c++) {
    const int bpp = img->get_bytes_per_pixel(c);
    const ptrdiff_t stride = img->get_image_stride(c);
    const int w = img->get_width(c);
    const int h = img->get_height(c);
    const uint8_t* plane = img->get_image_plane(c);
    for (int y = 0; y < h; y++) {
      const uint8_t* row = plane + (size_t)y * stride * bpp;
      for (int x = w; x < stride; x++) {
        if (row[x * bpp] != 0xA5 || (bpp == 2 && row[x * bpp + 1] != 0x5A)) {
          return false;
        }
      }
    }
  }
  return true;
}

static std::shared_ptr<seq_parameter_set> make_sps(const Geometry& g) {
  auto sps = std::make_shared<seq_parameter_set>();
  sps->set_defaults();
  sps->chroma_format_idc = CHROMA_420;
  sps->ChromaArrayType = CHROMA_420;
  sps->bit_depth_luma = (uint8_t)g.bitDepth;
  sps->bit_depth_chroma = (uint8_t)g.bitDepth;
  sps->pic_width_in_luma_samples = (uint16_t)g.w;
  sps->pic_height_in_luma_samples = (uint16_t)g.h;
  sps->log2_min_luma_coding_block_size = LOG2_MIN_CB;
  sps->log2_diff_max_min_luma_coding_block_size = LOG2_CTB - LOG2_MIN_CB;
  // Keep the defaults for transform block sizes (min 8, max 16): the library
  // requires Log2MaxTrafoSize <= 5, and hierarchy depth 1 stays valid.
  sps->sample_adaptive_offset_enabled_flag = g.saoEnabled;
  sps->max_transform_hierarchy_depth_inter = 1;
  sps->max_transform_hierarchy_depth_intra = 1;
  sps->sps_read = true;
  task_check(sps->compute_derived_values() == DE265_OK, "sps derived values");
  return sps;
}

static std::shared_ptr<pic_parameter_set> make_pps(
    const std::shared_ptr<seq_parameter_set>& sps, const Geometry& g) {
  auto pps = std::make_shared<pic_parameter_set>();
  pps->set_defaults();
  pps->pps_read = true;
  pps->sps = sps;
  pps->seq_parameter_set_id = sps->seq_parameter_set_id;
  pps->pic_init_qp = 34;
  if (g.tiles) {
    pps->tiles_enabled_flag = true;
    pps->num_tile_columns = 2;
    pps->num_tile_rows = 1;
    pps->uniform_spacing_flag = true;
    pps->loop_filter_across_tiles_enabled_flag = false;
  }
  pps->set_derived_values(sps.get());
  return pps;
}

static std::shared_ptr<video_parameter_set> make_vps() {
  auto vps = std::make_shared<video_parameter_set>();
  vps->set_defaults(Profile_Main, 6, 2);
  return vps;
}

static slice_segment_header* make_slice(
    const std::shared_ptr<pic_parameter_set>& pps, uint32_t addr, int idx,
    bool crossSliceFilter) {
  auto* shdr = new slice_segment_header();
  shdr->set_defaults();
  shdr->pps = pps;
  shdr->slice_index = idx;
  shdr->slice_segment_address = addr;
  shdr->SliceAddrRS = addr;
  shdr->slice_type = SLICE_TYPE_I;
  shdr->slice_sao_luma_flag = true;
  shdr->slice_sao_chroma_flag = true;
  shdr->slice_loop_filter_across_slices_enabled_flag = crossSliceFilter;
  shdr->compute_derived_values(pps.get());
  return shdr;
}

static void install_sao_info(de265_image* img, int xCtb, int yCtb) {
  sao_info info;
  memset(&info, 0, sizeof(info));
  const int pattern = (xCtb + 2 * yCtb) % 4;
  if (pattern < 3) {
    // Edge offset, classes 0 (horizontal neighbours), 2 and 3 (diagonals):
    // these read the rows above and below, so one row's classification
    // depends on the neighbour rows.
    info.SaoTypeIdx = (2 << 0) | (2 << 2) | (2 << 4);
    info.SaoEoClass = (uint8_t)((pattern << 0) | (pattern << 2) | (pattern << 4));
  } else {
    info.SaoTypeIdx = (1 << 0) | (1 << 2) | (1 << 4);
    info.sao_band_position[0] = (uint8_t)((xCtb + yCtb) % 28);
    info.sao_band_position[1] = info.sao_band_position[0];
    info.sao_band_position[2] = info.sao_band_position[0];
  }
  for (int c = 0; c < 3; c++) {
    info.saoOffsetVal[c][0] = (int8_t)(1 + (xCtb % 3));
    info.saoOffsetVal[c][1] = (int8_t)(-(1 + (yCtb % 2)));
    info.saoOffsetVal[c][2] = (int8_t)(1 + ((xCtb + yCtb) % 2));
    info.saoOffsetVal[c][3] = (int8_t)(-(1 + (yCtb % 3)));
  }
  img->set_sao_info(xCtb, yCtb, &info);
}

static BuiltPicture build_picture(decoder_context* ctx, const Geometry& g,
                                  uint32_t salt) {
  BuiltPicture out;
  out.sps = make_sps(g);
  out.pps = make_pps(out.sps, g);
  out.vps = make_vps();

  out.img = new de265_image();
  de265_error err = out.img->alloc_image(
      g.w, g.h, de265_chroma_420, out.sps, /*allocMetadata=*/true, ctx,
      0, nullptr, /*useCustomAllocFunc=*/false);
  task_check(err == DE265_OK, "alloc_image");
  out.img->set_headers(out.vps, out.sps, out.pps);
  out.img->decctx = ctx;

  const int ctbW = out.sps->PicWidthInCtbsY;
  const int ctbH = out.sps->PicHeightInCtbsY;
  const int ctbSize = out.sps->CtbSizeY;

  const bool singleSlice = g.sliceSplitRow < 0;
  out.slices.push_back(make_slice(out.pps, 0, 0, singleSlice));
  if (!singleSlice) {
    uint32_t addr = (uint32_t)g.sliceSplitRow * ctbW;
    out.slices.push_back(make_slice(out.pps, addr, 1, false));
  }
  for (slice_segment_header* shdr : out.slices) {
    out.img->add_slice_segment_header(shdr);
  }

  for (int yCtb = 0; yCtb < ctbH; yCtb++) {
    for (int xCtb = 0; xCtb < ctbW; xCtb++) {
      int x0 = xCtb * ctbSize;
      int y0 = yCtb * ctbSize;
      int sliceIdx = (!singleSlice && yCtb >= g.sliceSplitRow) ? 1 : 0;

      out.img->set_SliceAddrRS(xCtb, yCtb, out.slices[sliceIdx]->SliceAddrRS);
      out.img->set_SliceHeaderIndex(x0, y0, sliceIdx);
      install_sao_info(out.img, xCtb, yCtb);

      const bool full = (x0 + ctbSize <= g.w) && (y0 + ctbSize <= g.h);
      if (full) {
        out.img->set_pred_mode(x0, y0, out.sps->Log2CtbSizeY, MODE_INTRA);
        out.img->set_log2CbSize(x0, y0, out.sps->Log2CtbSizeY, true);
        out.img->set_QPY(x0, y0, out.sps->Log2CtbSizeY, out.pps->pic_init_qp);
        if (g.bypassBlock && xCtb == ctbW - 1 && yCtb == 1) {
          out.img->set_cu_transquant_bypass(x0, y0, out.sps->Log2CtbSizeY, 1);
          out.img->set_pcm_flag(x0, y0, out.sps->Log2CtbSizeY, 1);
        }
      } else {
        // Truncated CTB: install metadata per minimum CB, clipped to the
        // visible area, exactly covering the in-picture min-CB grid.
        const int minCb = 1 << LOG2_MIN_CB;
        for (int yy = y0; yy < std::min(y0 + ctbSize, g.h); yy += minCb) {
          for (int xx = x0; xx < std::min(x0 + ctbSize, g.w); xx += minCb) {
            out.img->set_pred_mode(xx, yy, LOG2_MIN_CB, MODE_INTRA);
            out.img->set_log2CbSize(xx, yy, LOG2_MIN_CB, true);
            out.img->set_QPY(xx, yy, LOG2_MIN_CB, out.pps->pic_init_qp);
            if (g.bypassBlock && xCtb == ctbW - 1 && yCtb == 1) {
              out.img->set_cu_transquant_bypass(xx, yy, LOG2_MIN_CB, 1);
              out.img->set_pcm_flag(xx, yy, LOG2_MIN_CB, 1);
            }
          }
        }
      }
    }
  }

  fill_planes(out.img, g, salt);
  paint_padding_sentinel(out.img);
  out.img->mark_all_CTB_progress(CTB_PROGRESS_PREFILTER);
  return out;
}

static bool planes_equal(const de265_image* a, const de265_image* b) {
  for (int c = 0; c < 3; c++) {
    const int w = a->get_width(c);
    const int h = a->get_height(c);
    if (b->get_width(c) != w || b->get_height(c) != h) return false;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        if (get_sample(a, c, x, y) != get_sample(b, c, x, y)) {
          std::fprintf(stderr, "plane %d mismatch at %d,%d: %d != %d\n",
                       c, x, y, get_sample(a, c, x, y), get_sample(b, c, x, y));
          return false;
        }
      }
    }
  }
  return true;
}

static void run_sequential_reference(de265_image* img, bool deblock, bool sao) {
  if (deblock) apply_deblocking_filter(img);
  if (sao) apply_sample_adaptive_offset_sequential(img);
}

static decoder_context* new_ctx_with_workers(int workers) {
  auto* ctx = reinterpret_cast<decoder_context*>(de265_new_decoder());
  task_check(ctx != nullptr, "new decoder");
  if (workers > 0) {
    task_check(de265_start_worker_threads(
                   reinterpret_cast<de265_decoder_context*>(ctx), workers) ==
                   DE265_OK,
               "start worker threads");
  }
  return ctx;
}

static void release_ctx(decoder_context* ctx) {
  de265_free_decoder(reinterpret_cast<de265_decoder_context*>(ctx));
}

static void run_pipeline(decoder_context* ctx, de265_image* img,
                         filter_pipeline_observer* observer) {
  auto* imgunit = new image_unit();
  imgunit->img = img;
  filter_pipeline_set_observer(ctx, observer);
  task_check(run_inplace_filter_pipeline(ctx, imgunit), "in-place pipeline ran");
  filter_pipeline_set_observer(ctx, nullptr);
  task_check(img->debug_is_completed(), "all pipeline tasks finished");
  delete imgunit;
}

static void compare_config(const char* label, const Geometry& g, int workers,
                           bool deblock = true, bool sao = true) {
  const char* only = std::getenv("TARGET_ONLY");
  if (only && std::strstr(label, only) == nullptr) return;
  decoder_context* ctxA = new_ctx_with_workers(workers);
  decoder_context* ctxB = reinterpret_cast<decoder_context*>(de265_new_decoder());
  task_check(ctxA && ctxB, "decoder creation");
  if (!deblock) {
    de265_set_parameter_bool(
        reinterpret_cast<de265_decoder_context*>(ctxA),
        DE265_DECODER_PARAM_DISABLE_DEBLOCKING, 1);
  }
  if (!sao) {
    de265_set_parameter_bool(
        reinterpret_cast<de265_decoder_context*>(ctxA),
        DE265_DECODER_PARAM_DISABLE_SAO, 1);
  }

  BuiltPicture pip = build_picture(ctxA, g, 1234);
  BuiltPicture ref = build_picture(ctxB, g, 1234);

  run_pipeline(ctxA, pip.img, nullptr);
  run_sequential_reference(ref.img, deblock, sao && g.saoEnabled);

  task_check(planes_equal(pip.img, ref.img), label);
  task_check(padding_intact(pip.img), "padding untouched");
  pip.reset();
  ref.reset();
  release_ctx(ctxA);
  release_ctx(ctxB);
}

// ---------------------------------------------------------------------------
// Handshake observer: records ordering events with condition variables and
// can hold specific rows inside worker callbacks. All waits have a deadline
// so a broken pipeline fails the test instead of hanging.
// ---------------------------------------------------------------------------
class HandshakeObserver : public filter_pipeline_observer {
public:
  int holdDeblockHRow = -1;
  int holdSaoRow = -1;

  std::mutex m;
  std::condition_variable cv;
  bool releaseDeblock = false;
  bool releaseSao = false;

  int highestSaoFinished = -1;
  std::set<int> saoRowsEntered;
  std::set<std::thread::id> saoThreadIds;
  // Rows (other than the held one) that entered on_sao_enter while the held
  // row was actually blocked inside its worker, with the worker thread id.
  std::set<std::pair<int, std::thread::id>> overlappingSaoRows;
  std::thread::id heldSaoThread;
  bool heldSaoRowBlocked = false;
  std::set<int> deblockHRowsEntered;
  bool heldRowStillBlocked = true;
  std::vector<int> edgeStripBefore;   // luma row captured inside held H task

  std::atomic<size_t> bytesAllocated{0};
  std::atomic<size_t> bytesFreed{0};
  std::atomic<int> allocEvents{0};
  std::atomic<int> freeEvents{0};
  std::atomic<bool> filteringStarted{false};
  std::atomic<bool> allocAfterFilterStart{false};
  std::set<int> slotIdsSeen;
  std::set<std::pair<int, int>> slotRowPairs;  // (slot id, ctb row)

  template <class Pred>
  bool wait_until(Pred pred, const char* what) {
    std::unique_lock<std::mutex> lock(m);
    bool ok = cv.wait_for(lock, std::chrono::seconds(WAIT_SECONDS), pred);
    if (!ok) std::fprintf(stderr, "handshake timeout: %s\n", what);
    return ok;
  }

  void allow_release() {
    { std::lock_guard<std::mutex> lock(m); releaseDeblock = releaseSao = true; }
    cv.notify_all();
  }

  void on_deblock_h_enter(int ctb_y) override {
    filteringStarted = true;
    std::unique_lock<std::mutex> lock(m);
    deblockHRowsEntered.insert(ctb_y);
    cv.notify_all();
    if (ctb_y == holdDeblockHRow) {
      cv.wait(lock, [&] { return releaseDeblock; });
      heldRowStillBlocked = false;
    }
  }

  void on_sao_enter(int ctb_y) override {
    filteringStarted = true;
    std::unique_lock<std::mutex> lock(m);
    saoRowsEntered.insert(ctb_y);
    saoThreadIds.insert(std::this_thread::get_id());
    if (ctb_y == holdSaoRow) {
      heldSaoThread = std::this_thread::get_id();
      heldSaoRowBlocked = true;
    } else if (heldSaoRowBlocked) {
      // The held row is currently blocked inside its worker; record this row
      // and its worker so the test can prove genuine concurrent execution.
      overlappingSaoRows.insert({ctb_y, std::this_thread::get_id()});
    }
    cv.notify_all();
    if (ctb_y == holdSaoRow) {
      cv.wait(lock, [&] { return releaseSao; });
      heldSaoRowBlocked = false;
    }
  }

  void on_sao_finish(int ctb_y) override {
    {
      std::lock_guard<std::mutex> lock(m);
      highestSaoFinished = std::max(highestSaoFinished, ctb_y);
    }
    cv.notify_all();
  }

  void on_slot_acquire(int ctb_y, int slot_id) override {
    std::lock_guard<std::mutex> lock(m);
    slotIdsSeen.insert(slot_id);
    slotRowPairs.insert({slot_id, ctb_y});
  }

  void on_slot_release(int ctb_y, int slot_id) override {
    std::lock_guard<std::mutex> lock(m);
    slotRowPairs.insert({slot_id, ctb_y});
  }

  void on_workspace_alloc(size_t bytes) override {
    if (filteringStarted.load()) allocAfterFilterStart.store(true);
    bytesAllocated += bytes;
    allocEvents++;
  }

  void on_workspace_free(size_t bytes) override {
    bytesFreed += bytes;
    freeEvents++;
  }
};

static size_t expected_bound_bytes(const de265_image* img, int workers) {
  size_t sum = 0;
  const seq_parameter_set& sps = img->get_sps();
  for (int c = 0; c < 3; c++) {
    const int subH = (c == 0) ? 1 : sps.SubHeightC;
    const int hc = sps.CtbSizeY / subH;
    sum += (size_t)img->get_image_stride(c) * (hc + 2) *
           img->get_bytes_per_pixel(c);
  }
  return sum * (size_t)(workers + 4);
}

// Public-API regression: the bundled stream must hash identically with zero
// and two workers (exercises the full decode path, not only filters).
static uint64_t public_stream_hash(int workers, bool noSao, bool noDeblock) {
  std::ifstream f("support/fixtures/girlshy.h265", std::ios::binary);
  std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
  task_check(!data.empty(), "fixture readable");

  auto* c = de265_new_decoder();
  if (noSao) de265_set_parameter_bool(c, DE265_DECODER_PARAM_DISABLE_SAO, 1);
  if (noDeblock) de265_set_parameter_bool(c, DE265_DECODER_PARAM_DISABLE_DEBLOCKING, 1);
  if (workers) {
    task_check(de265_start_worker_threads(c, workers) == DE265_OK, "workers");
  }
  task_check(de265_push_data(c, data.data(), (int)data.size(), 0, nullptr) == DE265_OK,
             "push");
  task_check(de265_flush_data(c) == DE265_OK, "flush");

  int more = 1;
  uint64_t h = 1469598103934665603ULL;
  while (more) {
    de265_decode(c, &more);
    while (const de265_image* img = de265_get_next_picture(c)) {
      for (int p = 0; p < 3; p++) {
        int stride;
        auto plane = de265_get_image_plane(img, p, &stride);
        int w = de265_get_image_width(img, p) *
                (de265_get_bits_per_pixel(img, p) > 8 ? 2 : 1);
        int ht = de265_get_image_height(img, p);
        for (int y = 0; y < ht; y++)
          for (int x = 0; x < w; x++) {
            h ^= plane[y * stride + x];
            h *= 1099511628211ULL;
          }
      }
    }
  }
  de265_free_decoder(c);
  return h;
}

} // namespace

void task_cases(const char* group) {
  if (strcmp(group, "normal") == 0) {
    // 8 bit with a truncated last CTB row.
    compare_config("normal-8bit-truncated",
                   Geometry{128, 64 * 3 + 16, 8, true, false, -1, false}, 2);
    // Tall picture: several SAO rows can be in flight with two workers.
    compare_config("normal-8bit-tall",
                   Geometry{128, 64 * 8, 8, true, false, -1, false}, 2);
    // 10 and 12 bit.
    compare_config("normal-10bit",
                   Geometry{128, 64 * 4, 10, true, false, -1, false}, 2);
    compare_config("normal-12bit",
                   Geometry{128, 64 * 4, 12, true, false, -1, false}, 4);
    // A single worker must give the identical result.
    compare_config("normal-1worker",
                   Geometry{128, 64 * 4, 8, true, false, -1, false}, 1);
    // PCM / transquant-bypass CTB rules are preserved.
    compare_config("normal-bypass",
                   Geometry{128, 64 * 3, 8, true, false, -1, true}, 2);
  }

  else if (strcmp(group, "boundary") == 0) {
    // Two tiles with loop filtering disabled across the tile boundary.
    compare_config("boundary-tiles",
                   Geometry{128, 64 * 4, 8, true, true, -1, false}, 2);
    // Two slices, no filtering across the slice boundary.
    compare_config("boundary-slices",
                   Geometry{128, 64 * 4, 8, true, false, 2, false}, 2);
    // Truncated final CTB row at high bit depth.
    compare_config("boundary-truncated-10bit",
                   Geometry{96, 64 * 3 + 32, 10, true, false, -1, false}, 2);
    // Even, 16-aligned width that truncates the last CTB horizontally;
    // chroma planes then have stride slack for the padding check.
    compare_config("boundary-partial-width",
                   Geometry{112, 64 * 3, 8, true, false, -1, false}, 2);

    // A late horizontal deblocking task must really change the edge pixels:
    // hold H(row 1) before it touches anything, snapshot the edge strip at
    // that moment, and prove the final pixels differ after release.
    if (!std::getenv("TARGET_ONLY") ||
        std::strstr("boundary-handshake", std::getenv("TARGET_ONLY"))) {
      const Geometry g{128, 64 * 4, 8, true, false, -1, false};
      decoder_context* ctx = new_ctx_with_workers(2);
      BuiltPicture pic = build_picture(ctx, g, 777);
      const int edgeY = 64;  // first horizontal CTB boundary
      HandshakeObserver obs;
      obs.holdDeblockHRow = 1;
      auto* imgunit = new image_unit();
      imgunit->img = pic.img;
      filter_pipeline_set_observer(ctx, &obs);

      std::thread runner([&] {
        task_check(run_inplace_filter_pipeline(ctx, imgunit), "pipeline");
      });

      task_check(obs.wait_until([&] {
                     return obs.deblockHRowsEntered.count(1) == 1;
                   },
                                "H(1) entered"), "handshake H enter");
      {
        std::lock_guard<std::mutex> lock(obs.m);
        obs.edgeStripBefore.resize(g.w);
        for (int x = 0; x < g.w; x++) {
          obs.edgeStripBefore[x] = get_sample(pic.img, 0, x, edgeY);
        }
      }
      obs.allow_release();
      runner.join();
      filter_pipeline_set_observer(ctx, nullptr);
      task_check(pic.img->debug_is_completed(), "pipeline completed");

      int changed = 0;
      for (int x = 0; x < g.w; x++) {
        if (get_sample(pic.img, 0, x, edgeY) != obs.edgeStripBefore[x]) changed++;
      }
      task_check(changed > 0, "late horizontal deblock changed edge pixels");
      task_check(padding_intact(pic.img), "padding intact after deblock");
      pic.reset();
      delete imgunit;
      release_ctx(ctx);
    }
  }

  else if (strcmp(group, "compat") == 0) {
    // Public bitstream regression across worker counts and filter settings.
    const uint64_t h0 = public_stream_hash(0, false, false);
    task_check(public_stream_hash(2, false, false) == h0, "w2 hash matches w0");
    task_check(public_stream_hash(4, false, false) == h0, "w4 hash matches w0");
    const uint64_t hNoSao = public_stream_hash(0, true, false);
    task_check(public_stream_hash(2, true, false) == hNoSao, "no-SAO match");
    const uint64_t hNoDb = public_stream_hash(0, false, true);
    task_check(public_stream_hash(2, false, true) == hNoDb, "no-deblock match");

    // Filter switch combinations on synthetic pictures, one worker included.
    compare_config("compat-both-off",
                   Geometry{128, 64 * 3, 8, false, false, -1, false}, 2,
                   /*deblock=*/false, /*sao=*/false);
    compare_config("compat-sao-off",
                   Geometry{128, 64 * 3, 8, true, false, -1, false}, 2,
                   true, false);
    compare_config("compat-deblock-off",
                   Geometry{128, 64 * 3, 8, true, false, -1, false}, 2,
                   false, true);
    compare_config("compat-one-worker",
                   Geometry{128, 64 * 4, 8, true, false, -1, false}, 1);

    // Workspace accounting and the (W+4) bounded-memory formula.
    {
      const Geometry tall{128, 64 * 10, 8, true, false, -1, false};
      decoder_context* ctx = new_ctx_with_workers(2);
      BuiltPicture pic = build_picture(ctx, tall, 42);
      HandshakeObserver obs;
      run_pipeline(ctx, pic.img, &obs);

      const size_t bound = expected_bound_bytes(pic.img, 2);
      task_check(obs.bytesAllocated.load() > 0, "workspace was allocated");
      task_check(obs.bytesAllocated.load() <= bound, "peak within (W+4) bound");
      task_check(obs.bytesAllocated.load() == obs.bytesFreed.load(),
                 "all workspace released");
      task_check(obs.allocEvents.load() == obs.freeEvents.load(),
                 "alloc/free event counts balance");
      task_check(!obs.allocAfterFilterStart.load(),
                 "no pixel allocation after filtering started");
      task_check((int)obs.slotIdsSeen.size() <= 2 + 4, "slot count bounded");
      // Tall picture reuses slots: at least one slot id serves several rows.
      bool reused = false;
      for (int id : obs.slotIdsSeen) {
        int rows = 0;
        for (auto& p : obs.slotRowPairs) if (p.first == id) rows++;
        if (rows >= 2) reused = true;
      }
      task_check(reused, "slots reused across CTB rows");
      pic.reset();
      release_ctx(ctx);

      // Same width, different height: the bound must not grow with height.
      const Geometry shorter{128, 64 * 4, 8, true, false, -1, false};
      decoder_context* ctx2 = new_ctx_with_workers(2);
      BuiltPicture pic2 = build_picture(ctx2, shorter, 43);
      HandshakeObserver obs2;
      run_pipeline(ctx2, pic2.img, &obs2);
      task_check(obs2.bytesAllocated.load() == obs.bytesAllocated.load(),
                 "workspace independent of picture height");
      task_check(obs2.bytesAllocated.load() <=
                     expected_bound_bytes(pic2.img, 2),
                 "shorter picture within bound");
      pic2.reset();
      release_ctx(ctx2);
    }

    // Handshake 1: front SAO rows finish while a tail horizontal deblocking
    // task is held inside the worker, before it exits.
    {
      const Geometry g{128, 64 * 8, 8, true, false, -1, false};
      decoder_context* ctx = new_ctx_with_workers(2);
      decoder_context* ctxRef = new_ctx_with_workers(0);
      BuiltPicture pip = build_picture(ctx, g, 99);
      BuiltPicture ref = build_picture(ctxRef, g, 99);

      HandshakeObserver obs;
      obs.holdDeblockHRow = 7;  // last CTB row
      auto* imgunit = new image_unit();
      imgunit->img = pip.img;
      filter_pipeline_set_observer(ctx, &obs);
      std::thread runner([&] {
        task_check(run_inplace_filter_pipeline(ctx, imgunit), "pipeline");
      });

      // First wait until the tail horizontal task has entered and is held
      // inside its worker (before touching pixels), then prove the front SAO
      // rows still run to completion while that task has not exited.
      task_check(obs.wait_until([&] {
                     return obs.deblockHRowsEntered.count(7) == 1;
                   },
                                "H(7) entered and held"),
                 "tail H enter");
      task_check(obs.wait_until([&] { return obs.highestSaoFinished >= 4; },
                                "front SAO while H(7) held"),
                 "front SAO progress");
      {
        std::lock_guard<std::mutex> lock(obs.m);
        task_check(obs.heldRowStillBlocked && !obs.releaseDeblock,
                   "tail H still inside worker");
      }
      obs.allow_release();
      runner.join();
      filter_pipeline_set_observer(ctx, nullptr);
      run_sequential_reference(ref.img, true, true);
      task_check(planes_equal(pip.img, ref.img), "pixels match after handshake");
      pip.reset();
      ref.reset();
      delete imgunit;
      release_ctx(ctx);
      release_ctx(ctxRef);
    }

    // Handshake 2: two different SAO rows run on two different workers.
    // Hold an early SAO row: only row 0 can precede it, while the whole
    // remaining chain (rows 2..N) is still pending. Once one worker blocks on
    // row 1, the other must drive the deblock/snapshot chain and enter a later
    // SAO row, which the observer records as genuine concurrent execution.
    {
      const Geometry g{128, 64 * 12, 8, true, false, -1, false};
      decoder_context* ctx = new_ctx_with_workers(2);
      BuiltPicture pip = build_picture(ctx, g, 100);
      decoder_context* ctxRef = new_ctx_with_workers(0);
      BuiltPicture ref = build_picture(ctxRef, g, 100);

      HandshakeObserver obs;
      obs.holdSaoRow = 1;
      auto* imgunit = new image_unit();
      imgunit->img = pip.img;
      filter_pipeline_set_observer(ctx, &obs);
      std::thread runner([&] {
        task_check(run_inplace_filter_pipeline(ctx, imgunit), "pipeline");
      });

      // Wait until the held row (4) is blocked inside its worker and a
      // different SAO row has subsequently entered on the OTHER worker.
      task_check(obs.wait_until(
                     [&] {
                       for (const auto& p : obs.overlappingSaoRows) {
                         if (p.second != obs.heldSaoThread) return true;
                       }
                       return false;
                     },
                     "second SAO row on another worker"),
                 "parallel SAO rows");
      {
        std::lock_guard<std::mutex> lock(obs.m);
        task_check(obs.saoRowsEntered.count(1) == 1, "held row entered");
        int distinctRowsOnOtherWorker = 0;
        for (const auto& p : obs.overlappingSaoRows) {
          task_check(p.first != 1, "overlapping row is a different CTB row");
          if (p.second != obs.heldSaoThread) distinctRowsOnOtherWorker++;
        }
        task_check(distinctRowsOnOtherWorker >= 1,
                   "another SAO row ran on a different worker concurrently");
      }
      obs.allow_release();
      runner.join();
      filter_pipeline_set_observer(ctx, nullptr);
      run_sequential_reference(ref.img, true, true);
      task_check(planes_equal(pip.img, ref.img), "SAO pixel exact match");
      pip.reset();
      ref.reset();
      delete imgunit;
      release_ctx(ctx);
      release_ctx(ctxRef);
    }

    // Repeated instances and recreate-after-release all terminate cleanly.
    for (int i = 0; i < 3; i++) {
      decoder_context* ctx = new_ctx_with_workers(2);
      BuiltPicture pic = build_picture(
          ctx, Geometry{128, 64 * 3, 8, true, false, -1, false}, 200 + i);
      run_pipeline(ctx, pic.img, nullptr);
      task_check(padding_intact(pic.img), "padding on repeated instance");
      pic.reset();
      release_ctx(ctx);
    }
  }
}
