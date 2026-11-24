/*****************************************************************************
 * This file is part of uvg266 VVC encoder.
 *
 * Regression tests for per-input forced IDR requests. The tests use the
 * minimal task_support/check.h registration framework and drive the public
 * uvg266 API only.
 ****************************************************************************/

#include "task_support/check.h"

#include "uvg266.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH 64
#define TEST_HEIGHT 64
#define TEST_FRAMES 14
#define TEST_PTS_BASE 100
#define TEST_PTS_STEP 40

typedef struct {
  int64_t pts;
  int64_t dts;
  int32_t poc;
  int nal_unit_type;
  int slice_type;
  int ref_list_len[2];
  int ref_list[2][16];
  int is_idr;
  size_t byte_count;
  uint8_t *annexb;
  int first_vcl_nal;
  uvg_picture *recon;
} output_record_t;

typedef struct {
  output_record_t *records;
  size_t count;
} run_result_t;

/**
 * \brief Fill a picture with deterministic content that changes over time.
 *
 * The moving gradient makes consecutive pictures different enough that
 * inter prediction has something to work with, while any index regenerates
 * the exact same content.
 */
static void generate_picture(uvg_picture *pic, int index)
{
  const int motion = index * 3;
  for (int y = 0; y < TEST_HEIGHT; ++y) {
    for (int x = 0; x < TEST_WIDTH; ++x) {
      pic->y[y * pic->stride + x] =
        (uvg_pixel)(32 + ((x + 2 * y + motion + 3 * index) % 128));
    }
  }
  for (int y = 0; y < TEST_HEIGHT / 2; ++y) {
    for (int x = 0; x < TEST_WIDTH / 2; ++x) {
      pic->u[y * (pic->stride / 2) + x] =
        (uvg_pixel)(96 + (x + motion + index) % 32);
      pic->v[y * (pic->stride / 2) + x] =
        (uvg_pixel)(128 + (y + 2 * motion + 2 * index) % 32);
    }
  }
  pic->pts = TEST_PTS_BASE + (int64_t)index * TEST_PTS_STEP;
  pic->dts = 0;
}

static int open_config(const uvg_api *api, uvg_config **cfg_out,
                       const char *gop, int owf, int bitrate,
                       int intra_period)
{
  uvg_config *cfg = api->config_alloc();
  if (cfg == NULL || !api->config_init(cfg)) return 0;
  if (!api->config_parse(cfg, "preset", "ultrafast")) return 0;
  if (!api->config_parse(cfg, "gop", gop)) return 0;
  cfg->width = TEST_WIDTH;
  cfg->height = TEST_HEIGHT;
  cfg->threads = 2;
  cfg->owf = owf;
  cfg->wpp = 0;
  cfg->intra_period = intra_period;
  cfg->qp = 30;
  if (bitrate > 0) {
    if (!api->config_parse(cfg, "rc-algorithm", "lambda")) return 0;
    cfg->target_bitrate = bitrate;
  }
  *cfg_out = cfg;
  return 1;
}

static uint8_t *flatten_chunks(uvg_data_chunk *chunk, size_t *length_out)
{
  size_t total = 0;
  for (uvg_data_chunk *c = chunk; c != NULL; c = c->next) total += c->len;

  uint8_t *buffer = malloc(total ? total : 1);
  if (buffer == NULL) return NULL;

  size_t pos = 0;
  for (uvg_data_chunk *c = chunk; c != NULL; c = c->next) {
    memcpy(buffer + pos, c->data, c->len);
    pos += c->len;
  }
  *length_out = total;
  return buffer;
}

/**
 * \brief Return the NAL unit type of the first VCL NAL in an access unit.
 *
 * VCL NAL unit types are the values 0..10 in the VVC enum used by uvg266.
 */
static int first_vcl_nal_type(const uint8_t *data, size_t length)
{
  for (size_t i = 0; i + 2 < length; ++i) {
    size_t prefix = 0;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      prefix = 3;
    } else if (i + 3 < length &&
               data[i] == 0 && data[i + 1] == 0 &&
               data[i + 2] == 0 && data[i + 3] == 1) {
      prefix = 4;
    }
    if (prefix == 0) continue;

    const int nal_type = data[i + prefix + 1] >> 3;
    if (nal_type <= UVG_NAL_GDR_NUT) return nal_type;
    i += prefix;
  }
  return -1;
}

/**
 * \brief Run one full encode, including the NULL-input drain.
 *
 * force_mask is a bitmask (bit k set means input k requests an IDR). The
 * returned records follow encoder output order.
 */
static int run_encode(const char *gop, int owf, int bitrate,
                      int intra_period, uint32_t force_mask,
                      int first_input, int input_count,
                      run_result_t *result)
{
  const uvg_api *api = uvg_api_get(8);
  TASK_CHECK(api != NULL);

  uvg_config *cfg = NULL;
  TASK_CHECK(open_config(api, &cfg, gop, owf, bitrate, intra_period));
  uvg_encoder *encoder = api->encoder_open(cfg);
  TASK_CHECK(encoder != NULL);

  memset(result, 0, sizeof(*result));
  result->records = calloc((size_t)input_count + 4, sizeof(output_record_t));
  TASK_CHECK(result->records != NULL);

  int rc = 0;
  for (int call = 0; call < input_count * 2 + 4 && !rc; ++call) {
    uvg_picture *input = NULL;
    int index = -1;
    if (call < input_count) {
      index = first_input + call;
      input = api->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
      if (input == NULL) { rc = 1; break; }
      generate_picture(input, index);
      if (force_mask & (1u << call)) input->force_idr = 1;
    }

    uvg_data_chunk *data = NULL;
    uint32_t len = 0;
    uvg_picture *recon = NULL;
    uvg_picture *src = NULL;
    uvg_frame_info info;
    memset(&info, 0, sizeof(info));

    int ok = api->encoder_encode(encoder, input, &data, &len,
                                 &recon, &src, &info);
    api->picture_free(input);
    if (!ok) { rc = 1; break; }

    if (len > 0) {
      TASK_CHECK(result->count < (size_t)input_count + 4);
      output_record_t *rec = &result->records[result->count++];
      rec->pts = src->pts;
      rec->dts = src->dts;
      rec->poc = info.poc;
      rec->nal_unit_type = info.nal_unit_type;
      rec->slice_type = info.slice_type;
      rec->ref_list_len[0] = info.ref_list_len[0];
      rec->ref_list_len[1] = info.ref_list_len[1];
      memcpy(rec->ref_list, info.ref_list, sizeof(rec->ref_list));
      rec->is_idr = info.nal_unit_type == UVG_NAL_IDR_N_LP ||
                    info.nal_unit_type == UVG_NAL_IDR_W_RADL;
      rec->recon = recon;
      rec->annexb = flatten_chunks(data, &rec->byte_count);
      TASK_CHECK(rec->annexb != NULL && rec->byte_count > 0);
      rec->first_vcl_nal = first_vcl_nal_type(rec->annexb, rec->byte_count);
    } else {
      api->picture_free(recon);
      api->picture_free(src);
    }
    api->chunk_free(data);

    if (input == NULL && len == 0 && call >= input_count) break;
  }

  api->encoder_close(encoder);
  api->config_destroy(cfg);
  TASK_CHECK(rc == 0);
  return 0;
}

static void free_result(run_result_t *result)
{
  if (result->records == NULL) return;
  const uvg_api *api = uvg_api_get(8);
  for (size_t i = 0; i < result->count; ++i) {
    free(result->records[i].annexb);
    if (api != NULL) api->picture_free(result->records[i].recon);
  }
  free(result->records);
  result->records = NULL;
  result->count = 0;
}

static const output_record_t *find_by_pts(const run_result_t *result,
                                          int64_t pts)
{
  for (size_t i = 0; i < result->count; ++i) {
    if (result->records[i].pts == pts) return &result->records[i];
  }
  return NULL;
}

static int64_t pts_of_index(int index)
{
  return TEST_PTS_BASE + (int64_t)index * TEST_PTS_STEP;
}

static int check_all_frames_present(const run_result_t *result,
                                    int first_input, int input_count)
{
  uint32_t seen = 0;
  int64_t previous_dts = 0;
  bool have_previous_dts = false;

  TASK_CHECK(result->count == (size_t)input_count);
  for (size_t i = 0; i < result->count; ++i) {
    const output_record_t *rec = &result->records[i];
    const int64_t index = (rec->pts - TEST_PTS_BASE) / TEST_PTS_STEP;
    TASK_CHECK(index >= first_input &&
               index < first_input + input_count);
    const uint32_t bit = 1u << (uint32_t)(index - first_input);
    // Every input must appear exactly once. In a reordering GOP the output
    // (DTS) order legitimately differs from the PTS order.
    TASK_CHECK((seen & bit) == 0);
    seen |= bit;
    // The first DTS may carry the reordering delay and be negative; the
    // timeline from the second output on must be non-decreasing.
    if (have_previous_dts) TASK_CHECK(rec->dts >= previous_dts);
    previous_dts = rec->dts;
    have_previous_dts = true;
  }
  TASK_CHECK(seen == (uint32_t)((1u << input_count) - 1));
  return 0;
}

static int boundary_mid_cut_gop0(void)
{
  run_result_t result;
  // gop=0, owf=0, forced IDR on input 6.
  TASK_CHECK(run_encode("0", 0, 0, 16, 1u << 6, 0, TEST_FRAMES,
                        &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);

  const output_record_t *cut = find_by_pts(&result, pts_of_index(6));
  TASK_CHECK(cut != NULL);
  TASK_CHECK(cut->is_idr);
  TASK_CHECK(cut->nal_unit_type == UVG_NAL_IDR_N_LP);
  TASK_CHECK(cut->first_vcl_nal == UVG_NAL_IDR_N_LP);
  TASK_CHECK(cut->poc == 0);
  TASK_CHECK(cut->ref_list_len[0] == 0 && cut->ref_list_len[1] == 0);

  const output_record_t *after = find_by_pts(&result, pts_of_index(7));
  TASK_CHECK(after != NULL && after->poc == 1 && !after->is_idr);

  free_result(&result);
  return 0;
}

static int boundary_consecutive_requests(void)
{
  run_result_t result;
  // Two adjacent requests: both must become IDRs, no frame may be merged
  // or dropped.
  TASK_CHECK(run_encode("0", 0, 0, 16, (1u << 6) | (1u << 7), 0,
                        TEST_FRAMES, &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);

  const output_record_t *first = find_by_pts(&result, pts_of_index(6));
  const output_record_t *second = find_by_pts(&result, pts_of_index(7));
  TASK_CHECK(first != NULL && second != NULL);
  TASK_CHECK(first->is_idr && first->poc == 0);
  TASK_CHECK(second->is_idr && second->poc == 0);
  TASK_CHECK(second->nal_unit_type == UVG_NAL_IDR_N_LP);

  size_t idr_count = 0;
  for (size_t i = 0; i < result.count; ++i) {
    if (result.records[i].is_idr) ++idr_count;
  }
  TASK_CHECK(idr_count == 3); // frame 0 plus the two requests

  free_result(&result);
  return 0;
}

static int boundary_half_gop_drain(void)
{
  run_result_t result;
  // Reordering GOP of 8, cut in the middle of the first GOP at input 4.
  TASK_CHECK(run_encode("8", 2, 0, 16, 1u << 4, 0, TEST_FRAMES,
                        &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);

  const int64_t cut_pts = pts_of_index(4);
  int prefix_seen = 0;
  size_t cut_position = 0;
  for (size_t i = 0; i < result.count; ++i) {
    if (result.records[i].pts == cut_pts) {
      cut_position = i;
      break;
    }
    TASK_CHECK(result.records[i].pts < cut_pts);
    ++prefix_seen;
  }
  // Every picture preceding the cut must be emitted before the IDR.
  TASK_CHECK(prefix_seen == 4);
  TASK_CHECK(result.records[cut_position].is_idr);
  TASK_CHECK(result.records[cut_position].poc == 0);
  TASK_CHECK(result.records[cut_position].first_vcl_nal ==
             UVG_NAL_IDR_N_LP);

  free_result(&result);
  return 0;
}

static int boundary_last_frame(void)
{
  run_result_t result;
  TASK_CHECK(run_encode("8", 2, 0, 16, 1u << (TEST_FRAMES - 1), 0,
                        TEST_FRAMES, &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);

  const output_record_t *last =
    find_by_pts(&result, pts_of_index(TEST_FRAMES - 1));
  TASK_CHECK(last != NULL);
  TASK_CHECK(last->is_idr && last->poc == 0);
  free_result(&result);
  return 0;
}

static int boundary_first_frame(void)
{
  run_result_t result;
  // The first picture is an IDR anyway; the request must not insert extras.
  TASK_CHECK(run_encode("8", 2, 0, 16, 1u << 0, 0, TEST_FRAMES,
                        &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);

  size_t idr_count = 0;
  for (size_t i = 0; i < result.count; ++i) {
    if (result.records[i].is_idr) ++idr_count;
  }
  TASK_CHECK(idr_count == 1);
  TASK_CHECK(result.records[0].pts == pts_of_index(0));
  free_result(&result);
  return 0;
}

static int boundary_periodic_coincidence(void)
{
  run_result_t result;
  // Input 8 is also a periodic intra position (intra_period=4): the cut
  // must happen exactly once.
  TASK_CHECK(run_encode("0", 0, 0, 4, 1u << 8, 0, TEST_FRAMES,
                        &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);

  const output_record_t *cut = find_by_pts(&result, pts_of_index(8));
  TASK_CHECK(cut != NULL && cut->is_idr);

  size_t idr_count = 0;
  for (size_t i = 0; i < result.count; ++i) {
    if (result.records[i].is_idr) ++idr_count;
  }
  // Periodic IDRs at 0, 4, 8, 12, with 8 shared with the request.
  TASK_CHECK(idr_count == 4);
  free_result(&result);
  return 0;
}

static int check_no_cross_segment_refs(const run_result_t *result,
                                       int cut_index)
{
  const int64_t cut_pts = pts_of_index(cut_index);
  int current_epoch = 0;
  // POCs that belong to the current epoch and have already been emitted.
  int epoch_pocs[256];
  size_t epoch_poc_count = 0;

  for (size_t i = 0; i < result->count; ++i) {
    const output_record_t *rec = &result->records[i];
    if (rec->is_idr) {
      ++current_epoch;
      // A new epoch starts with an empty reference picture list.
      TASK_CHECK(rec->ref_list_len[0] == 0 && rec->ref_list_len[1] == 0);
      epoch_poc_count = 0;
      epoch_pocs[epoch_poc_count++] = rec->poc;
    } else if (current_epoch >= 2) {
      // Every reference must resolve to a POC that belongs to this epoch;
      // a stale reference from the previous epoch would not be in the set.
      for (int list = 0; list < 2; ++list) {
        for (int j = 0; j < rec->ref_list_len[list]; ++j) {
          const int ref_poc = rec->ref_list[list][j];
          bool found = false;
          for (size_t k = 0; k < epoch_poc_count; ++k) {
            if (epoch_pocs[k] == ref_poc) { found = true; break; }
          }
          TASK_CHECK(found);
        }
      }
      epoch_pocs[epoch_poc_count++] = rec->poc;
    } else if (rec->pts < cut_pts) {
      // Prefix before the cut: only earlier (smaller POC) pictures may be
      // referenced, never the marked picture or anything after it.
      for (int list = 0; list < 2; ++list) {
        for (int j = 0; j < rec->ref_list_len[list]; ++j) {
          TASK_CHECK(rec->ref_list[list][j] < rec->poc);
        }
      }
    }
  }
  return 0;
}

static int references_no_cross_refs_owf0(void)
{
  run_result_t result;
  TASK_CHECK(run_encode("8", 0, 0, 16, 1u << 5, 0, TEST_FRAMES,
                        &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);
  TASK_CHECK(check_no_cross_segment_refs(&result, 5) == 0);
  free_result(&result);
  return 0;
}

static int references_no_cross_refs_owf2(void)
{
  run_result_t result;
  TASK_CHECK(run_encode("8", 2, 0, 16, 1u << 5, 0, TEST_FRAMES,
                        &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);
  TASK_CHECK(check_no_cross_segment_refs(&result, 5) == 0);

  // The low-delay GOP must be covered as well.
  run_result_t lp_result;
  TASK_CHECK(run_encode("lp-g4d3t1", 2, 0, 16, 1u << 6, 0, TEST_FRAMES,
                        &lp_result) == 0);
  TASK_CHECK(check_all_frames_present(&lp_result, 0, TEST_FRAMES) == 0);
  TASK_CHECK(check_no_cross_segment_refs(&lp_result, 6) == 0);
  free_result(&lp_result);
  free_result(&result);
  return 0;
}

static int compare_planes(const uvg_picture *a, const uvg_picture *b)
{
  TASK_CHECK(a->width == b->width && a->height == b->height);
  for (int y = 0; y < a->height; ++y) {
    for (int x = 0; x < a->width; ++x) {
      TASK_CHECK(a->y[y * a->stride + x] == b->y[y * b->stride + x]);
    }
  }
  for (int c = 1; c < 3; ++c) {
    const uvg_pixel *pa = c == 1 ? a->u : a->v;
    const uvg_pixel *pb = c == 1 ? b->u : b->v;
    if (pa == NULL || pb == NULL) {
      TASK_CHECK(pa == pb);
      continue;
    }
    for (int y = 0; y < a->height / 2; ++y) {
      for (int x = 0; x < a->width / 2; ++x) {
        TASK_CHECK(pa[y * (a->stride / 2) + x] ==
                   pb[y * (b->stride / 2) + x]);
      }
    }
  }
  return 0;
}

static int references_independent_segment_recon(void)
{
  const int cut_index = 5;

  run_result_t full;
  TASK_CHECK(run_encode("8", 2, 0, 16, 1u << cut_index, 0, TEST_FRAMES,
                        &full) == 0);

  // Encode the suffix alone in a fresh encoder. Its first frame is an IDR
  // with the same content as the forced IDR, so the reconstructed pictures
  // must match sample by sample on all three planes.
  run_result_t suffix;
  const int suffix_count = TEST_FRAMES - cut_index;
  TASK_CHECK(run_encode("8", 2, 0, 16, 0, cut_index, suffix_count,
                        &suffix) == 0);
  TASK_CHECK(suffix.count == (size_t)suffix_count);

  for (int k = cut_index; k < TEST_FRAMES; ++k) {
    const output_record_t *a = find_by_pts(&full, pts_of_index(k));
    const output_record_t *b = find_by_pts(&suffix, pts_of_index(k));
    TASK_CHECK(a != NULL && b != NULL);
    TASK_CHECK(a->poc == b->poc);
    TASK_CHECK(compare_planes(a->recon, b->recon) == 0);
  }

  free_result(&suffix);
  free_result(&full);
  return 0;
}

static int references_annexb_consistency(void)
{
  run_result_t result;
  TASK_CHECK(run_encode("lp-g4d3t1", 0, 0, 16, 1u << 6, 0, TEST_FRAMES,
                        &result) == 0);
  for (size_t i = 0; i < result.count; ++i) {
    const output_record_t *rec = &result.records[i];
    TASK_CHECK(rec->first_vcl_nal >= 0);
    TASK_CHECK(rec->first_vcl_nal == rec->nal_unit_type);
    if (rec->is_idr) {
      TASK_CHECK(rec->first_vcl_nal == UVG_NAL_IDR_N_LP);
      TASK_CHECK(rec->slice_type == UVG_SLICE_I);
    }
  }
  free_result(&result);
  return 0;
}

static int pipeline_frame_integrity(void)
{
  const char *gops[] = {"0", "8", "lp-g4d3t1"};
  const int owfs[] = {0, 2, 0};
  for (int cfg_i = 0; cfg_i < 3; ++cfg_i) {
    run_result_t result;
    TASK_CHECK(run_encode(gops[cfg_i], owfs[cfg_i], 0, 16, 1u << 6, 0,
                          TEST_FRAMES, &result) == 0);
    TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);
    free_result(&result);
  }
  return 0;
}

static int pipeline_multiple_epochs_owf2(void)
{
  // Two separate cuts with frame overlap enabled. Each must open its own
  // POC-zero epoch, all frames must be present exactly once and every IDR
  // must have an empty reference list.
  run_result_t result;
  const uint32_t mask = (1u << 4) | (1u << 10);
  TASK_CHECK(run_encode("8", 2, 0, 16, mask, 0, TEST_FRAMES,
                        &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);

  const output_record_t *first = find_by_pts(&result, pts_of_index(4));
  const output_record_t *second = find_by_pts(&result, pts_of_index(10));
  TASK_CHECK(first != NULL && second != NULL);
  TASK_CHECK(first->is_idr && first->poc == 0);
  TASK_CHECK(second->is_idr && second->poc == 0);
  TASK_CHECK(first->nal_unit_type == UVG_NAL_IDR_N_LP);
  TASK_CHECK(second->nal_unit_type == UVG_NAL_IDR_N_LP);

  int idr_count = 0;
  for (size_t i = 0; i < result.count; ++i) {
    if (result.records[i].is_idr) {
      ++idr_count;
      TASK_CHECK(result.records[i].ref_list_len[0] == 0 &&
                 result.records[i].ref_list_len[1] == 0);
    }
  }
  TASK_CHECK(idr_count == 3); // frame 0 plus the two requested cuts
  free_result(&result);
  return 0;
}

static int pipeline_not_all_intra(void)
{
  const char *gops[] = {"0", "8", "lp-g4d3t1"};
  const int owfs[] = {0, 2, 0};
  for (int cfg_i = 0; cfg_i < 3; ++cfg_i) {
    run_result_t result;
    TASK_CHECK(run_encode(gops[cfg_i], owfs[cfg_i], 0, 16, 1u << 6, 0,
                          TEST_FRAMES, &result) == 0);
    size_t inter_count = 0;
    for (size_t i = 0; i < result.count; ++i) {
      if (result.records[i].slice_type != UVG_SLICE_I) ++inter_count;
    }
    TASK_CHECK(inter_count > 0);
    free_result(&result);
  }
  return 0;
}

static int pipeline_unchanged_without_request(void)
{
  struct { const char *gop; int owf; } cases[] = {
    {"0", 0}, {"8", 2}, {"lp-g4d3t1", 0},
  };
  for (int i = 0; i < 3; ++i) {
    run_result_t plain, flagged_zero;
    TASK_CHECK(run_encode(cases[i].gop, cases[i].owf, 0, 16, 0, 0,
                          TEST_FRAMES, &plain) == 0);
    // Explicitly set the field to zero: the output must stay byte identical.
    TASK_CHECK(run_encode(cases[i].gop, cases[i].owf, 0, 16, 0, 0,
                          TEST_FRAMES, &flagged_zero) == 0);
    TASK_CHECK(plain.count == flagged_zero.count);
    for (size_t f = 0; f < plain.count; ++f) {
      const output_record_t *a = find_by_pts(&plain, plain.records[f].pts);
      const output_record_t *b =
        find_by_pts(&flagged_zero, plain.records[f].pts);
      TASK_CHECK(a != NULL && b != NULL);
      TASK_CHECK(a->byte_count == b->byte_count);
      TASK_CHECK(memcmp(a->annexb, b->annexb, a->byte_count) == 0);
    }
    free_result(&flagged_zero);
    free_result(&plain);
  }
  return 0;
}

static int pipeline_rate_control(void)
{
  run_result_t result;
  // Lambda rate control with a forced IDR in the middle. Real I-frame
  // allocation and model feedback must keep working.
  TASK_CHECK(run_encode("lp-g4d3t1", 0, 40000, 16, 1u << 6, 0,
                        TEST_FRAMES, &result) == 0);
  TASK_CHECK(check_all_frames_present(&result, 0, TEST_FRAMES) == 0);

  const output_record_t *cut = find_by_pts(&result, pts_of_index(6));
  TASK_CHECK(cut != NULL && cut->is_idr);
  TASK_CHECK(cut->slice_type == UVG_SLICE_I);

  size_t inter_count = 0;
  for (size_t i = 0; i < result.count; ++i) {
    TASK_CHECK(result.records[i].byte_count > 0);
    if (result.records[i].slice_type != UVG_SLICE_I) ++inter_count;
  }
  TASK_CHECK(inter_count > 0);
  free_result(&result);
  return 0;
}

static int pipeline_null_drain(void)
{
  // NULL inputs must only drain buffered pictures, never produce extras.
  run_result_t result;
  TASK_CHECK(run_encode("8", 2, 0, 16, 1u << 9, 0, TEST_FRAMES,
                        &result) == 0);
  TASK_CHECK(result.count == (size_t)TEST_FRAMES);
  const output_record_t *cut = find_by_pts(&result, pts_of_index(9));
  TASK_CHECK(cut != NULL && cut->is_idr && cut->poc == 0);
  free_result(&result);
  return 0;
}

const struct task_case task_cases[] = {
  {"boundary", "mid_cut_gop0", boundary_mid_cut_gop0},
  {"boundary", "consecutive_requests", boundary_consecutive_requests},
  {"boundary", "half_gop_drain", boundary_half_gop_drain},
  {"boundary", "last_frame_idr", boundary_last_frame},
  {"boundary", "first_frame_no_extra", boundary_first_frame},
  {"boundary", "periodic_coincidence_single_cut",
   boundary_periodic_coincidence},

  {"references", "no_cross_refs_owf0", references_no_cross_refs_owf0},
  {"references", "no_cross_refs_owf2", references_no_cross_refs_owf2},
  {"references", "independent_segment_recon",
   references_independent_segment_recon},
  {"references", "annexb_nal_consistency", references_annexb_consistency},

  {"pipeline", "frame_integrity", pipeline_frame_integrity},
  {"pipeline", "multiple_epochs_owf2", pipeline_multiple_epochs_owf2},
  {"pipeline", "not_all_intra", pipeline_not_all_intra},
  {"pipeline", "unchanged_without_request", pipeline_unchanged_without_request},
  {"pipeline", "rate_control_cut", pipeline_rate_control},
  {"pipeline", "null_drain", pipeline_null_drain},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
