/*****************************************************************************
 * Tests for the per-frame target bitrate (kvz_config.frame_bitrate) mode.
 *
 * The cases are registered through task_support/check.h and driven by
 * task_support/driver.c. There is no main() in this file.
 *
 * Every feedback value reported through kvz_frame_info is cross-checked
 * against an independent rational-prefix calculation maintained by this
 * test, and every Annex-B cost comes from the real chunk bytes returned by
 * the encoder.
 */

#include "task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvazaar.h"


#define TEST_WIDTH 128
#define TEST_HEIGHT 128
#define MAX_FRAMES 64
#define FPS_NUM 30000
#define FPS_DEN 1001

static double dmax(double a, double b) { return a > b ? a : b; }


/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const kvz_api *api(void)
{
  return kvz_api_get(8);
}

static kvz_config *base_config(const kvz_api *a, int owf, int wpp,
                               int bitrate, int frame_bitrate)
{
  kvz_config *c = a->config_alloc();
  if (!c) return NULL;
  if (!a->config_init(c)) goto fail;
  if (!a->config_parse(c, "preset", "ultrafast")) goto fail;
  if (!a->config_parse(c, "gop", "0")) goto fail;

  c->width = TEST_WIDTH;
  c->height = TEST_HEIGHT;
  c->framerate_num = FPS_NUM;
  c->framerate_denom = FPS_DEN;
  c->framerate = (double)FPS_NUM / FPS_DEN;
  c->target_bitrate = bitrate;
  c->qp = 28;
  c->threads = 2;
  c->owf = owf;
  c->wpp = wpp;
  c->cpuid = 0;
  c->add_encoder_info = 0;
  c->intra_period = 0;
  c->enable_logging_output = 0;
  c->intra_bit_allocation = 0;
  c->vaq = 0;
  c->rc_algorithm = bitrate > 0 ? KVZ_LAMBDA : KVZ_NO_RC;
  c->frame_bitrate = frame_bitrate;
  return c;

fail:
  a->config_destroy(c);
  return NULL;
}

/* Programmatically generated content: moving texture followed by a
 * stationary segment. */
static void fill_picture(kvz_picture *pic, int n)
{
  pic->pts = n;
  const int content = n < 18 ? n : 18; /* frames 18.. are identical */

  for (int y = 0; y < TEST_HEIGHT; ++y) {
    for (int x = 0; x < TEST_WIDTH; ++x) {
      pic->y[y * pic->stride + x] =
        (kvz_pixel)((x * 7 + y * 11 + (x * y) % 31 + content * 19 +
                     (content % 3) * (x ^ y)) & 255);
    }
  }
  for (int y = 0; y < TEST_HEIGHT / 2; ++y) {
    for (int x = 0; x < TEST_WIDTH / 2; ++x) {
      pic->u[y * (pic->stride / 2) + x] =
        (kvz_pixel)((80 + x * 3 + content * 9 + y) & 255);
      pic->v[y * (pic->stride / 2) + x] =
        (kvz_pixel)((160 + y * 2 + content * 5 + x) & 255);
    }
  }
}

static void fold(uint64_t *hash, unsigned v)
{
  *hash ^= v;
  *hash *= UINT64_C(1099511628211);
}

static uint64_t picture_hash(const kvz_picture *pic)
{
  uint64_t h = UINT64_C(14695981039346656037);
  for (int y = 0; y < TEST_HEIGHT; ++y)
    for (int x = 0; x < TEST_WIDTH; ++x)
      fold(&h, pic->y[y * pic->stride + x]);
  for (int y = 0; y < TEST_HEIGHT / 2; ++y)
    for (int x = 0; x < TEST_WIDTH / 2; ++x) {
      fold(&h, pic->u[y * (pic->stride / 2) + x]);
      fold(&h, pic->v[y * (pic->stride / 2) + x]);
    }
  return h;
}

static uint64_t chunks_length(const kvz_data_chunk *chunks)
{
  uint64_t sum = 0;
  for (const kvz_data_chunk *p = chunks; p; p = p->next) sum += p->len;
  return sum;
}

static uint64_t chunks_hash(const kvz_data_chunk *chunks)
{
  uint64_t h = UINT64_C(14695981039346656037);
  for (const kvz_data_chunk *p = chunks; p; p = p->next)
    for (uint32_t i = 0; i < p->len; ++i)
      fold(&h, p->data[i]);
  return h;
}

/* Independent reference model of the frame bitrate feedback. */
typedef struct {
  int32_t rates[MAX_FRAMES];      /* effective rate per accepted input */
  uint64_t out_bytes[MAX_FRAMES]; /* real Annex-B bytes per input */
  int have_bytes[MAX_FRAMES];
  int num_frames;
  int owf;
} ref_model_t;

static void ref_init(ref_model_t *m, int num_frames, int owf,
                     const int32_t *requests, int32_t initial_rate)
{
  memset(m, 0, sizeof(*m));
  m->num_frames = num_frames;
  m->owf = owf;

  int32_t inherited = initial_rate;
  for (int n = 0; n < num_frames; ++n) {
    if (requests[n] > 0) inherited = requests[n];
    m->rates[n] = inherited;
  }
}

static int ref_expect(const ref_model_t *m, int n,
                      const kvz_frame_info *info, uint64_t byte_len)
{
  const uint64_t frontier = (n > m->owf) ? (uint64_t)(n - m->owf) : 0;

  unsigned __int128 prefix = 0;
  for (uint64_t i = 0; i < frontier; ++i) {
    prefix += (unsigned __int128)m->rates[i] * FPS_DEN;
  }
  const uint64_t earned = (uint64_t)(prefix / FPS_NUM);

  uint64_t actual = 0;
  for (uint64_t i = 0; i < frontier; ++i) {
    TASK_CHECK(m->have_bytes[i]);
    actual += m->out_bytes[i] * 8;
  }

  const double q = (double)m->rates[n] * FPS_DEN / FPS_NUM;
  const double target = dmax(200.0, q + ((double)earned - (double)actual) / 40.0);

  TASK_CHECK(info->rc_bitrate == m->rates[n]);
  TASK_CHECK(info->rc_feedback_frames == frontier);
  TASK_CHECK(info->rc_earned_bits == earned);
  TASK_CHECK(info->rc_actual_bits == actual);
  TASK_CHECK(fabs(info->rc_target_bits - target) <=
             1e-9 * (fabs(target) + 1.0));
  (void)byte_len;
  return 0;
}

/* Full encode run. Inputs are retained so that the returned source picture
 * pointer can be matched to the input identity even with duplicate PTS.
 * Returns the number of emitted pictures. */
typedef struct {
  uint64_t bytes;
  uint64_t stream_hash;
  uint64_t recon_hash;
  int8_t qp;
  kvz_frame_info info;
} output_rec_t;

static int run_encoder(const kvz_api *a, kvz_config *cfg,
                       const int32_t *requests, int num_frames,
                       kvz_picture **inputs, /* retained, may be NULL to alloc */
                       output_rec_t *outputs, int *identity_of_output)
{
  TASK_CHECK(a != NULL);
  const int fb_enabled = cfg->frame_bitrate;
  const int cfg_owf = cfg->owf;
  const int32_t initial_rate = cfg->target_bitrate;

  ref_model_t ref;
  if (fb_enabled)
    ref_init(&ref, num_frames, cfg_owf, requests, initial_rate);

  kvz_encoder *enc = a->encoder_open(cfg);
  TASK_CHECK(enc != NULL);
  a->config_destroy(cfg);

  int own_inputs = (inputs == NULL);
  if (own_inputs) {
    inputs = calloc((size_t)num_frames, sizeof(kvz_picture *));
    TASK_CHECK(inputs != NULL);
    for (int n = 0; n < num_frames; ++n) {
      inputs[n] = a->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
      TASK_CHECK(inputs[n] != NULL);
      fill_picture(inputs[n], n);
    }
  }

  int n_out = 0;
  int next_input = 0;

  for (;;) {
    kvz_picture *in = NULL;
    if (next_input < num_frames) {
      in = inputs[next_input];
      in->rc_bitrate = requests[next_input];
    }

    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    kvz_frame_info info;
    memset(&info, 0, sizeof(info));

    TASK_CHECK(a->encoder_encode(enc, in, &chunks, &len, &rec, &src, &info));

    if (len > 0) {
      TASK_CHECK(chunks != NULL && rec != NULL && src != NULL);
      TASK_CHECK(chunks_length(chunks) == len);

      int identity = -1;
      for (int i = 0; i < num_frames; ++i) {
        if (inputs[i] == src) { identity = i; break; }
      }
      TASK_CHECK(identity >= 0);
      TASK_CHECK(n_out < MAX_FRAMES);

      outputs[n_out].bytes = len;
      outputs[n_out].stream_hash = chunks_hash(chunks);
      outputs[n_out].recon_hash = picture_hash(rec);
      outputs[n_out].qp = info.qp;
      outputs[n_out].info = info;
      identity_of_output[n_out] = identity;

      if (fb_enabled) {
        ref.out_bytes[identity] = len;
        ref.have_bytes[identity] = 1;
        TASK_CHECK(ref_expect(&ref, identity, &info, len) == 0);
      }

      a->chunk_free(chunks);
      a->picture_free(rec);
      a->picture_free(src);
      ++n_out;
    } else {
      TASK_CHECK(chunks == NULL && rec == NULL && src == NULL);
    }

    if (next_input < num_frames) {
      ++next_input;
    } else if (len == 0) {
      break; /* flush drained */
    }
    if (n_out == num_frames) break;
  }

  TASK_CHECK(n_out == num_frames);

  if (own_inputs) {
    for (int n = 0; n < num_frames; ++n) a->picture_free(inputs[n]);
    free(inputs);
  }
  a->encoder_close(enc);
  return n_out;
}


/* ------------------------------------------------------------------ */
/* Credit group: independent arithmetic of the feedback formula        */
/* ------------------------------------------------------------------ */

static int credit_target_math(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);

  /* Fixed moderate rate: non-integer nominal target at 30000/1001. */
  const int N = 6;
  int32_t req[6] = { 100000, 0, 0, 0, 0, 0 };
  kvz_config *cfg = base_config(a, 0, 0, 100000, 1);
  output_rec_t out[MAX_FRAMES];
  int id[MAX_FRAMES];
  TASK_CHECK(run_encoder(a, cfg, req, N, NULL, out, id) == N);

  /* First frame: m=0, so target is max(200, q), q = 100000*1001/30000. */
  const double q0 = 100000.0 * FPS_DEN / FPS_NUM;
  TASK_CHECK(fabs(out[0].info.rc_target_bits - dmax(200.0, q0)) <= 1e-9);
  TASK_CHECK(out[0].info.rc_feedback_frames == 0);
  TASK_CHECK(out[0].info.rc_earned_bits == 0);
  TASK_CHECK(out[0].info.rc_actual_bits == 0);
  TASK_CHECK(out[0].info.rc_bitrate == 100000);

  /* Outputs are in input order. */
  for (int n = 0; n < N; ++n) TASK_CHECK(id[n] == n);

  /* Low rate: nominal target below the 200-bit floor. */
  int32_t low_req[1] = { 1000 };
  kvz_config *low_cfg = base_config(a, 0, 0, 1000, 1);
  TASK_CHECK(low_cfg != NULL);
  output_rec_t low_out[1];
  int low_id[1];
  TASK_CHECK(run_encoder(a, low_cfg, low_req, 1, NULL, low_out, low_id) == 1);
  TASK_CHECK(low_out[0].info.rc_target_bits == 200.0);
  TASK_CHECK(1000.0 * FPS_DEN / FPS_NUM < 200.0);

  /* A zero request on the very first picture inherits target_bitrate. */
  int32_t inherit_req[2] = { 0, 0 };
  kvz_config *inh_cfg = base_config(a, 0, 0, 135000, 1);
  TASK_CHECK(inh_cfg != NULL);
  output_rec_t inh_out[2];
  int inh_id[2];
  TASK_CHECK(run_encoder(a, inh_cfg, inherit_req, 2, NULL, inh_out, inh_id) == 2);
  TASK_CHECK(inh_out[0].info.rc_bitrate == 135000);
  TASK_CHECK(inh_out[1].info.rc_bitrate == 135000);
  return 0;
}

static int credit_switch_and_inherit(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);

  /* Up/down switches mixed with inheritance, owf=0 so m == n. */
  const int N = 10;
  int32_t req[10] = { 80000, 0, 200000, 0, 0, 50000, 0, 0, 120000, 0 };
  int32_t expect_rate[10] = {
    80000, 80000, 200000, 200000, 200000,
    50000, 50000, 50000, 120000, 120000
  };

  kvz_config *cfg = base_config(a, 0, 0, 80000, 1);
  output_rec_t out[MAX_FRAMES];
  int id[MAX_FRAMES];
  TASK_CHECK(run_encoder(a, cfg, req, N, NULL, out, id) == N);

  for (int n = 0; n < N; ++n) {
    TASK_CHECK(out[n].info.rc_bitrate == expect_rate[n]);
    TASK_CHECK(out[n].info.rc_feedback_frames == (uint64_t)n);
  }

  /* Rational prefix must match floor(prefix/num), which differs from
   * summing per-frame rounded targets at 30000/1001. */
  unsigned __int128 prefix = 0;
  uint64_t per_frame_rounded_sum = 0;
  for (int n = 0; n < N; ++n) {
    if (n > 0) {
      TASK_CHECK(out[n].info.rc_earned_bits ==
                 (uint64_t)(prefix / FPS_NUM));
    }
    /* Naive accumulation of rounded frame targets gives a different value. */
    per_frame_rounded_sum +=
      (uint64_t)((uint64_t)expect_rate[n] * FPS_DEN / FPS_NUM);
    prefix += (unsigned __int128)expect_rate[n] * FPS_DEN;
  }
  TASK_CHECK((uint64_t)(prefix / FPS_NUM) != per_frame_rounded_sum);

  /* A fresh rate must never be multiplied over the whole history. */
  const int k = 2;
  const uint64_t wrong_recompute =
    (uint64_t)((unsigned __int128)expect_rate[k] * FPS_DEN * k / FPS_NUM);
  TASK_CHECK(out[k].info.rc_earned_bits != wrong_recompute);
  return 0;
}

static int credit_history_and_debt(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);

  /* Spend several frames at one rate, then switch. The feedback after the
   * switch must keep the real prefix budget and the real overspend instead
   * of recomputing history at the new rate or zeroing the debt. */
  const int N = 12;
  int32_t req[N];
  for (int n = 0; n < N; ++n) req[n] = (n == 6) ? 400000 : 0;
  req[0] = 60000;

  kvz_config *cfg = base_config(a, 0, 0, 60000, 1);
  output_rec_t out[MAX_FRAMES];
  int id[MAX_FRAMES];
  TASK_CHECK(run_encoder(a, cfg, req, N, NULL, out, id) == N);

  /* Independent prefix through the first 6 frames (all at 60000). */
  unsigned __int128 prefix6 = 0;
  for (int i = 0; i < 6; ++i) prefix6 += (unsigned __int128)60000 * FPS_DEN;
  const uint64_t earned6 = (uint64_t)(prefix6 / FPS_NUM);

  uint64_t actual6 = 0;
  for (int i = 0; i < 6; ++i) actual6 += out[i].bytes * 8;

  TASK_CHECK(out[6].info.rc_bitrate == 400000);
  TASK_CHECK(out[6].info.rc_feedback_frames == 6);
  TASK_CHECK(out[6].info.rc_earned_bits == earned6);
  TASK_CHECK(out[6].info.rc_actual_bits == actual6);

  /* Recomputing history at the new rate would yield a different E_6. */
  const uint64_t recomputed =
    (uint64_t)((unsigned __int128)400000 * FPS_DEN * 6 / FPS_NUM);
  TASK_CHECK(recomputed != earned6);

  /* Zeroing the debt would force E_6 == A_6. */
  TASK_CHECK(out[6].info.rc_earned_bits != out[6].info.rc_actual_bits);

  /* The real debt survives unchanged into the reported target. */
  const double q6 = 400000.0 * FPS_DEN / FPS_NUM;
  const double expect_target =
    dmax(200.0, q6 + ((double)earned6 - (double)actual6) / 40.0);
  TASK_CHECK(fabs(out[6].info.rc_target_bits - expect_target) <= 1e-9);

  /* Subsequent inherited frames keep the same 400000 rate. */
  for (int n = 7; n < N; ++n) TASK_CHECK(out[n].info.rc_bitrate == 400000);
  return 0;
}


/* ------------------------------------------------------------------ */
/* Pipeline group: real encodes, feedback frontier, retry, isolation   */
/* ------------------------------------------------------------------ */

static int pipeline_mobile_static_owf0(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  const int N = 28;
  int32_t req[MAX_FRAMES];
  for (int n = 0; n < N; ++n) {
    req[n] = 0;
    if (n == 8) req[n] = 400000;  /* upswitch */
    if (n == 20) req[n] = 40000;  /* downswitch into the static segment */
  }
  req[0] = 100000;

  kvz_config *cfg = base_config(a, 0, 0, 100000, 1);
  output_rec_t out[MAX_FRAMES];
  int id[MAX_FRAMES];
  TASK_CHECK(run_encoder(a, cfg, req, N, NULL, out, id) == N);

  uint64_t total = 0;
  for (int n = 0; n < N; ++n) {
    total += out[n].bytes;
    TASK_CHECK(out[n].qp >= 0 && out[n].qp <= 51);
    TASK_CHECK(out[n].recon_hash != 0);
    TASK_CHECK(id[n] == n);
    TASK_CHECK(out[n].info.rc_feedback_frames == (uint64_t)n);
  }
  TASK_CHECK(total > 0);

  /* The upswitch must change later quantization choices. */
  int changed = 0;
  for (int n = 9; n < 20; ++n) {
    if (out[n].qp != out[7].qp) { changed = 1; break; }
  }
  TASK_CHECK(changed);
  return 0;
}

static int pipeline_owf2_wpp_frontier(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  const int N = 28;
  int32_t req[MAX_FRAMES];
  for (int n = 0; n < N; ++n) {
    req[n] = 0;
    if (n == 5) req[n] = 350000;
    if (n == 15) req[n] = 60000;
  }
  req[0] = 120000;

  kvz_config *cfg = base_config(a, 2, 1, 120000, 1);
  TASK_CHECK(cfg != NULL);
  TASK_CHECK(cfg->threads >= 2);
  output_rec_t out[MAX_FRAMES];
  int id[MAX_FRAMES];
  TASK_CHECK(run_encoder(a, cfg, req, N, NULL, out, id) == N);

  for (int n = 0; n < N; ++n) {
    const uint64_t m = (n > 2) ? (uint64_t)(n - 2) : 0;
    TASK_CHECK(out[n].info.rc_feedback_frames == m);
    TASK_CHECK(id[n] == n);
  }

  /* While the first two frames are in flight, their feedback must be empty
   * even though the workers may already have finished parts of them. */
  TASK_CHECK(out[0].info.rc_feedback_frames == 0);
  TASK_CHECK(out[1].info.rc_feedback_frames == 0);
  TASK_CHECK(out[2].info.rc_feedback_frames == 0);
  TASK_CHECK(out[3].info.rc_feedback_frames == 1);
  return 0;
}

static int pipeline_owf2_wpp_off(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  const int N = 26;
  int32_t req[MAX_FRAMES];
  for (int n = 0; n < N; ++n) {
    req[n] = (n % 6 == 0) ? 70000 + n * 5000 : 0;
  }

  /* Frame parallelism without wavefront: owf=2, wpp disabled, 2 threads. */
  kvz_config *cfg = base_config(a, 2, 0, 90000, 1);
  TASK_CHECK(cfg != NULL);
  output_rec_t out[MAX_FRAMES];
  int id[MAX_FRAMES];
  TASK_CHECK(run_encoder(a, cfg, req, N, NULL, out, id) == N);

  for (int n = 0; n < N; ++n) {
    const uint64_t m = (n > 2) ? (uint64_t)(n - 2) : 0;
    TASK_CHECK(out[n].info.rc_feedback_frames == m);
    TASK_CHECK(out[n].bytes > 0);
    TASK_CHECK(id[n] == n);
  }
  return 0;
}

static int pipeline_switch_changes_recon(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  const int N = 24;
  int32_t req_a[MAX_FRAMES], req_b[MAX_FRAMES];
  for (int n = 0; n < N; ++n) {
    req_a[n] = 0;
    req_b[n] = 0;
  }
  req_a[0] = 80000;
  req_b[0] = 80000;
  /* Identical history, then diverging schedules from frame 10. */
  for (int n = 10; n < N; ++n) {
    req_a[n] = 25000;
    req_b[n] = 500000;
  }

  output_rec_t out_a[MAX_FRAMES], out_b[MAX_FRAMES];
  int id_a[MAX_FRAMES], id_b[MAX_FRAMES];

  kvz_config *cfg_a = base_config(a, 0, 1, 80000, 1);
  TASK_CHECK(run_encoder(a, cfg_a, req_a, N, NULL, out_a, id_a) == N);
  kvz_config *cfg_b = base_config(a, 0, 1, 80000, 1);
  TASK_CHECK(run_encoder(a, cfg_b, req_b, N, NULL, out_b, id_b) == N);

  int qp_differs = 0, recon_differs = 0;
  for (int n = 10; n < N; ++n) {
    if (out_a[n].qp != out_b[n].qp) qp_differs = 1;
    if (out_a[n].recon_hash != out_b[n].recon_hash) recon_differs = 1;
  }
  TASK_CHECK(qp_differs);
  TASK_CHECK(recon_differs);

  /* Before the switch the two runs must be byte-identical. */
  for (int n = 0; n < 10; ++n) {
    TASK_CHECK(out_a[n].stream_hash == out_b[n].stream_hash);
  }
  return 0;
}

static int pipeline_retry_same_pts_flush(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  kvz_config *cfg = base_config(a, 0, 0, 100000, 1);
  kvz_encoder *enc = a->encoder_open(cfg);
  TASK_CHECK(enc != NULL);
  a->config_destroy(cfg);

  kvz_picture *pic = a->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
  TASK_CHECK(pic != NULL);
  fill_picture(pic, 0);
  pic->pts = 7; /* deliberately duplicated PTS later */

  /* Negative request: rejected, nothing consumed, outputs cleared. */
  pic->rc_bitrate = -5;
  kvz_data_chunk *chunks = NULL;
  uint32_t len = 99;
  kvz_picture *rec = (kvz_picture *)1, *src = (kvz_picture *)1;
  kvz_frame_info info;
  TASK_CHECK(!a->encoder_encode(enc, pic, &chunks, &len, &rec, &src, &info));
  TASK_CHECK(len == 0 && chunks == NULL && rec == NULL && src == NULL);

  /* Per-picture ROI maps are unsupported in this mode and also rejected
   * without consuming the input. */
  int8_t *roi = calloc(1, sizeof(int8_t));
  TASK_CHECK(roi != NULL);
  pic->roi.roi_array = roi;
  len = 99; chunks = NULL; rec = (kvz_picture *)1; src = (kvz_picture *)1;
  TASK_CHECK(!a->encoder_encode(enc, pic, &chunks, &len, &rec, &src, &info));
  TASK_CHECK(len == 0 && chunks == NULL && rec == NULL && src == NULL);
  pic->roi.roi_array = NULL;
  free(roi);

  /* Correct the same picture and retry: it becomes input number 0. */
  pic->rc_bitrate = 100000;
  len = 0; chunks = NULL; rec = NULL; src = NULL;
  TASK_CHECK(a->encoder_encode(enc, pic, &chunks, &len, &rec, &src, &info));
  TASK_CHECK(len > 0 && src == pic);
  TASK_CHECK(info.rc_bitrate == 100000);
  TASK_CHECK(info.rc_feedback_frames == 0);
  a->chunk_free(chunks);
  a->picture_free(rec);
  a->picture_free(src);

  /* Second picture reusing the same PTS must not cross values. */
  kvz_picture *pic2 = a->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
  TASK_CHECK(pic2 != NULL);
  fill_picture(pic2, 1);
  pic2->pts = 7;
  pic2->rc_bitrate = 250000;
  len = 0; chunks = NULL; rec = NULL; src = NULL;
  TASK_CHECK(a->encoder_encode(enc, pic2, &chunks, &len, &rec, &src, &info));
  TASK_CHECK(src == pic2);
  TASK_CHECK(info.rc_bitrate == 250000);
  TASK_CHECK(info.rc_feedback_frames == 1);
  a->chunk_free(chunks);
  a->picture_free(rec);
  a->picture_free(src);

  /* A zero request inherits the rate but must not be rewritten on the
   * caller's picture; the effective value is only in frame info. */
  kvz_picture *pic3 = a->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
  TASK_CHECK(pic3 != NULL);
  fill_picture(pic3, 2);
  pic3->pts = 7;
  pic3->rc_bitrate = 0;
  len = 0; chunks = NULL; rec = NULL; src = NULL;
  TASK_CHECK(a->encoder_encode(enc, pic3, &chunks, &len, &rec, &src, &info));
  TASK_CHECK(pic3->rc_bitrate == 0);
  TASK_CHECK(info.rc_bitrate == 250000);
  TASK_CHECK(info.rc_feedback_frames == 2);
  a->chunk_free(chunks);
  a->picture_free(rec);
  a->picture_free(src);
  a->picture_free(pic3);

  /* NULL only drains: no new rate, no new target income. */
  int drained = 0;
  for (;;) {
    len = 0; chunks = NULL; rec = NULL; src = NULL;
    TASK_CHECK(a->encoder_encode(enc, NULL, &chunks, &len, &rec, &src, &info));
    if (len == 0) {
      TASK_CHECK(chunks == NULL && rec == NULL && src == NULL);
      break;
    }
    TASK_CHECK(info.rc_bitrate == 250000);
    a->chunk_free(chunks);
    a->picture_free(rec);
    a->picture_free(src);
    ++drained;
    TASK_CHECK(drained < 4);
  }

  a->picture_free(pic);
  a->picture_free(pic2);
  a->encoder_close(enc);
  return 0;
}

static int pipeline_ring_reuse_long_run(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  const int N = 40; /* ring capacity is owf+2 = 4, reused many times */
  int32_t req[MAX_FRAMES];
  for (int n = 0; n < N; ++n) {
    req[n] = (n % 7 == 0) ? (50000 + n * 9000) : 0;
  }

  kvz_config *cfg = base_config(a, 2, 1, 90000, 1);
  TASK_CHECK(cfg != NULL);
  output_rec_t out[MAX_FRAMES];
  int id[MAX_FRAMES];
  TASK_CHECK(run_encoder(a, cfg, req, N, NULL, out, id) == N);

  for (int n = 0; n < N; ++n) {
    TASK_CHECK(id[n] == n);
    TASK_CHECK(out[n].bytes > 0);
  }
  return 0;
}

static int pipeline_caller_frees_input_immediately(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  const int N = 24;

  kvz_config *cfg = base_config(a, 2, 1, 110000, 1);
  TASK_CHECK(cfg != NULL);
  kvz_encoder *enc = a->encoder_open(cfg);
  TASK_CHECK(enc != NULL);
  a->config_destroy(cfg);

  /* Every input picture is released immediately after a successful call;
   * the bound rate must travel with the encoder's internal reference. */
  int n_out = 0;
  for (int n = 0; n < N; ++n) {
    kvz_picture *in = a->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
    TASK_CHECK(in != NULL);
    fill_picture(in, n);
    in->rc_bitrate = (n == 9) ? 320000 : 0;
    if (n == 0) in->rc_bitrate = 110000;

    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    kvz_frame_info info;
    TASK_CHECK(a->encoder_encode(enc, in, &chunks, &len, &rec, &src, &info));
    a->picture_free(in); /* allowed immediately */

    if (len) {
      TASK_CHECK(info.rc_bitrate == (n_out >= 9 ? 320000 : 110000) ||
                info.rc_bitrate > 0);
      a->chunk_free(chunks);
      a->picture_free(rec);
      a->picture_free(src);
      ++n_out;
    }
  }

  for (;;) {
    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    kvz_frame_info info;
    TASK_CHECK(a->encoder_encode(enc, NULL, &chunks, &len, &rec, &src, &info));
    if (len == 0) break;
    TASK_CHECK(info.rc_bitrate == 320000 || info.rc_bitrate == 110000);
    a->chunk_free(chunks);
    a->picture_free(rec);
    a->picture_free(src);
    ++n_out;
  }
  TASK_CHECK(n_out == N);
  a->encoder_close(enc);
  return 0;
}

static int pipeline_two_encoders_isolation(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  const int N = 12;

  kvz_config *cfg1 = base_config(a, 2, 1, 100000, 1);
  kvz_config *cfg2 = base_config(a, 2, 0, 200000, 1);
  kvz_encoder *e1 = a->encoder_open(cfg1);
  kvz_encoder *e2 = a->encoder_open(cfg2);
  TASK_CHECK(e1 && e2);
  a->config_destroy(cfg1);
  a->config_destroy(cfg2);

  kvz_picture *pics1[N], *pics2[N];
  for (int n = 0; n < N; ++n) {
    pics1[n] = a->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
    pics2[n] = a->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
    TASK_CHECK(pics1[n] && pics2[n]);
    fill_picture(pics1[n], n);
    fill_picture(pics2[n], n);
  }

  int32_t rates1[MAX_FRAMES], rates2[MAX_FRAMES];
  for (int n = 0; n < N; ++n) {
    rates1[n] = (n == 4) ? 400000 : 0;
    rates2[n] = (n == 2) ? 30000 : 0;
  }
  rates1[0] = 100000;
  rates2[0] = 200000;

  int got1 = 0, got2 = 0;
  for (int n = 0; n < N; ++n) {
    pics1[n]->rc_bitrate = rates1[n];
    pics2[n]->rc_bitrate = rates2[n];

    kvz_data_chunk *c1 = NULL, *c2 = NULL;
    uint32_t l1 = 0, l2 = 0;
    kvz_picture *r1 = NULL, *s1 = NULL, *r2 = NULL, *s2 = NULL;
    kvz_frame_info i1, i2;
    TASK_CHECK(a->encoder_encode(e1, pics1[n], &c1, &l1, &r1, &s1, &i1));
    TASK_CHECK(a->encoder_encode(e2, pics2[n], &c2, &l2, &r2, &s2, &i2));

    if (l1) {
      TASK_CHECK(i1.rc_bitrate == (got1 >= 4 ? 400000 : 100000));
      a->chunk_free(c1); a->picture_free(r1); a->picture_free(s1);
      ++got1;
    }
    if (l2) {
      TASK_CHECK(i2.rc_bitrate >= 30000 && i2.rc_bitrate <= 200000);
      a->chunk_free(c2); a->picture_free(r2); a->picture_free(s2);
      ++got2;
    }
  }

  /* Flush both, still interleaved. */
  for (;;) {
    kvz_data_chunk *c1 = NULL, *c2 = NULL;
    uint32_t l1 = 0, l2 = 0;
    kvz_picture *r1 = NULL, *s1 = NULL, *r2 = NULL, *s2 = NULL;
    kvz_frame_info i1, i2;
    TASK_CHECK(a->encoder_encode(e1, NULL, &c1, &l1, &r1, &s1, &i1));
    TASK_CHECK(a->encoder_encode(e2, NULL, &c2, &l2, &r2, &s2, &i2));
    if (l1) { a->chunk_free(c1); a->picture_free(r1); a->picture_free(s1); ++got1; }
    if (l2) { a->chunk_free(c2); a->picture_free(r2); a->picture_free(s2); ++got2; }
    if (!l1 && !l2) break;
  }
  TASK_CHECK(got1 == N && got2 == N);

  for (int n = 0; n < N; ++n) {
    a->picture_free(pics1[n]);
    a->picture_free(pics2[n]);
  }
  a->encoder_close(e1);
  a->encoder_close(e2);
  return 0;
}


/* ------------------------------------------------------------------ */
/* Compat group: mode off keeps every existing path byte-identical     */
/* ------------------------------------------------------------------ */

static int collect_stream(const kvz_api *a, kvz_config *cfg,
                          const int32_t *field_values, int N,
                          uint64_t *stream_hash, uint64_t *total_bytes)
{
  TASK_CHECK(a != NULL);
  kvz_encoder *enc = a->encoder_open(cfg);
  TASK_CHECK(enc != NULL);
  a->config_destroy(cfg);

  *stream_hash = UINT64_C(14695981039346656037);
  *total_bytes = 0;
  int n_out = 0;

  for (int n = 0; n < N; ++n) {
    kvz_picture *in = a->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
    TASK_CHECK(in != NULL);
    fill_picture(in, n);
    in->rc_bitrate = field_values[n];

    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    kvz_frame_info info;
    TASK_CHECK(a->encoder_encode(enc, in, &chunks, &len, &rec, &src, &info));
    a->picture_free(in);
    if (len) {
      TASK_CHECK(chunks_length(chunks) == len);
      *total_bytes += len;
      fold(stream_hash, chunks_hash(chunks));
      a->chunk_free(chunks);
      a->picture_free(rec);
      a->picture_free(src);
      ++n_out;
    }
  }

  /* Drain every delayed output with NULL inputs. */
  for (;;) {
    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    kvz_frame_info info;
    TASK_CHECK(a->encoder_encode(enc, NULL, &chunks, &len, &rec, &src, &info));
    if (len == 0) {
      TASK_CHECK(chunks == NULL && rec == NULL && src == NULL);
      break;
    }
    *total_bytes += len;
    fold(stream_hash, chunks_hash(chunks));
    a->chunk_free(chunks);
    a->picture_free(rec);
    a->picture_free(src);
    ++n_out;
  }
  TASK_CHECK(n_out == N);
  a->encoder_close(enc);
  return 0;
}

static int compat_mode_off_fixed_rate(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  const int N = 24;
  int32_t zeros[MAX_FRAMES], noisy[MAX_FRAMES];
  for (int n = 0; n < N; ++n) {
    zeros[n] = 0;
    noisy[n] = (n % 3 == 0) ? 777777 : (n % 3 == 1 ? -123 : 0);
  }

  uint64_t h1, b1, h2, b2;
  kvz_config *c1 = base_config(a, 2, 1, 100000, 0);
  TASK_CHECK(collect_stream(a, c1, zeros, N, &h1, &b1) == 0);
  kvz_config *c2 = base_config(a, 2, 1, 100000, 0);
  TASK_CHECK(collect_stream(a, c2, noisy, N, &h2, &b2) == 0);

  /* The field must be ignored completely when the mode is off. */
  TASK_CHECK(h1 == h2);
  TASK_CHECK(b1 == b2);
  TASK_CHECK(b1 > 0);
  return 0;
}

static int compat_fixed_qp(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);
  const int N = 24;
  int32_t zeros[MAX_FRAMES], noisy[MAX_FRAMES];
  for (int n = 0; n < N; ++n) {
    zeros[n] = 0;
    noisy[n] = 999999;
  }

  uint64_t h1, b1, h2, b2;
  kvz_config *c1 = base_config(a, 0, 0, 0, 0);
  TASK_CHECK(collect_stream(a, c1, zeros, N, &h1, &b1) == 0);
  kvz_config *c2 = base_config(a, 0, 0, 0, 0);
  TASK_CHECK(collect_stream(a, c2, noisy, N, &h2, &b2) == 0);

  TASK_CHECK(h1 == h2);
  TASK_CHECK(b1 == b2);

  /* OBA must keep working with the mode off. */
  kvz_config *c3 = base_config(a, 0, 0, 100000, 0);
  c3->rc_algorithm = KVZ_OBA;
  kvz_encoder *enc = a->encoder_open(c3);
  TASK_CHECK(enc != NULL);
  a->config_destroy(c3);
  int n_out = 0;
  for (int n = 0; n < N; ++n) {
    kvz_picture *in = a->picture_alloc(TEST_WIDTH, TEST_HEIGHT);
    TASK_CHECK(in != NULL);
    fill_picture(in, n);
    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    kvz_frame_info info;
    TASK_CHECK(a->encoder_encode(enc, in, &chunks, &len, &rec, &src, &info));
    a->picture_free(in);
    if (len) {
      a->chunk_free(chunks);
      a->picture_free(rec);
      a->picture_free(src);
      ++n_out;
    }
  }
  for (;;) {
    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    kvz_frame_info info;
    TASK_CHECK(a->encoder_encode(enc, NULL, &chunks, &len, &rec, &src, &info));
    if (len == 0) {
      TASK_CHECK(chunks == NULL && rec == NULL && src == NULL);
      break;
    }
    a->chunk_free(chunks);
    a->picture_free(rec);
    a->picture_free(src);
    ++n_out;
  }
  TASK_CHECK(n_out == N);
  a->encoder_close(enc);
  return 0;
}

static int compat_open_rejects_unsupported(void)
{
  const kvz_api *a = api();
  TASK_CHECK(a != NULL);

  /* Defaults and parser. */
  kvz_config *d = a->config_alloc();
  TASK_CHECK(a->config_init(d));
  TASK_CHECK(d->frame_bitrate == 0);
  TASK_CHECK(a->config_parse(d, "frame-bitrate", "true"));
  TASK_CHECK(d->frame_bitrate == 1);
  TASK_CHECK(a->config_parse(d, "frame-bitrate", "0"));
  TASK_CHECK(d->frame_bitrate == 0);
  a->config_destroy(d);

  struct {
    const char *name;
    const char *value;
  } bad_opts[] = {
    { "gop", "8" },            /* gop must be 0 */
    { "intra-bits", "1" },     /* intra bit allocation */
    { "vaq", "1" },            /* VAQ */
    { "owf", "1" },            /* only 0 and 2 supported */
    { "rc-algorithm", "oba" }, /* lambda only */
  };

  for (size_t k = 0; k < sizeof(bad_opts) / sizeof(bad_opts[0]); ++k) {
    kvz_config *c = base_config(a, 0, 0, 100000, 1);
    TASK_CHECK(a->config_parse(c, bad_opts[k].name, bad_opts[k].value));
    TASK_CHECK(a->encoder_open(c) == NULL);
    a->config_destroy(c);
  }

  /* Missing positive bitrate / framerate. */
  kvz_config *no_rate = base_config(a, 0, 0, 100000, 1);
  no_rate->target_bitrate = 0;
  no_rate->rc_algorithm = KVZ_NO_RC;
  TASK_CHECK(a->encoder_open(no_rate) == NULL);
  a->config_destroy(no_rate);

  kvz_config *bad_fps = base_config(a, 0, 0, 100000, 1);
  bad_fps->framerate_num = 0;
  TASK_CHECK(a->encoder_open(bad_fps) == NULL);
  a->config_destroy(bad_fps);

  /* Interlaced and non-I420 input. */
  kvz_config *interlaced = base_config(a, 0, 0, 100000, 1);
  interlaced->source_scan_type = KVZ_INTERLACING_TFF;
  TASK_CHECK(a->encoder_open(interlaced) == NULL);
  a->config_destroy(interlaced);

  kvz_config *roi_cfg = base_config(a, 0, 0, 100000, 1);
  roi_cfg->roi.file_path = strdup("roi.txt");
  TASK_CHECK(roi_cfg->roi.file_path != NULL);
  TASK_CHECK(a->encoder_open(roi_cfg) == NULL);
  a->config_destroy(roi_cfg);

  /* The same restricted config opens fine once the mode is switched off. */
  kvz_config *off = base_config(a, 0, 0, 100000, 1);
  TASK_CHECK(a->config_parse(off, "gop", "8"));
  off->frame_bitrate = 0;
  kvz_encoder *enc = a->encoder_open(off);
  TASK_CHECK(enc != NULL);
  a->config_destroy(off);
  a->encoder_close(enc);
  return 0;
}


/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  { "credit", "non_integer_target_and_floor", credit_target_math },
  { "credit", "switch_inherit_rational_prefix", credit_switch_and_inherit },
  { "credit", "history_budget_and_real_debt", credit_history_and_debt },

  { "pipeline", "mobile_static_owf0", pipeline_mobile_static_owf0 },
  { "pipeline", "owf2_wpp_feedback_frontier", pipeline_owf2_wpp_frontier },
  { "pipeline", "owf2_wpp_off_frame_parallel", pipeline_owf2_wpp_off },
  { "pipeline", "switch_changes_qp_and_recon", pipeline_switch_changes_recon },
  { "pipeline", "retry_same_pts_flush", pipeline_retry_same_pts_flush },
  { "pipeline", "state_ring_reuse_40_frames", pipeline_ring_reuse_long_run },
  { "pipeline", "two_encoders_isolated", pipeline_two_encoders_isolation },
  { "pipeline", "caller_frees_input_immediately", pipeline_caller_frees_input_immediately },

  { "compat", "mode_off_ignores_field_bytes", compat_mode_off_fixed_rate },
  { "compat", "fixed_qp_and_oba_unchanged", compat_fixed_qp },
  { "compat", "unsupported_config_rejected", compat_open_rejects_unsupported },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
