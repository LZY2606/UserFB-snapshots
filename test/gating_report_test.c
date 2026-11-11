/* See COPYING file for copyright and license details. */

/* Tests for the read-only gating report API:
 *   ebur128_loudness_report()
 *   ebur128_loudness_report_multiple()
 *
 * All test signals are generated at runtime (sine waves / silence), no
 * external audio files are needed.
 *
 * Build:
 *   cc -std=c99 -O2 -I ebur128 ebur128/ebur128.c \
 *      test/gating_report_test.c -lm -o /tmp/ebur128-gating
 */

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ebur128.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000u
/* 400 ms = 19200 frames; the library hops by 100 ms = 4800 frames. */
#define BLOCK_FRAMES (SR * 4 / 10)
#define HOP_FRAMES (SR / 10)

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      printf("FAILED: ");                                                      \
      printf(__VA_ARGS__);                                                     \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

/* Number of completed 400 ms gating blocks for a stream of n frames,
 * derived independently from the documented block geometry (first block
 * after 400 ms, a new block every 100 ms afterwards). This does not use any
 * loudness function of the library. */
static size_t expected_block_count(size_t n) {
  if (n < BLOCK_FRAMES) {
    return 0;
  }
  return 1 + (n - BLOCK_FRAMES) / HOP_FRAMES;
}

/* Fills n interleaved frames for "channels" channels. Channel c gets a sine
 * of amplitude amps[c]; sample values depend only on the absolute frame
 * index, so feeding the signal in arbitrary chunks produces identical
 * samples. */
static void gen_tone(double* buf, size_t first_frame, size_t n,
                     unsigned int channels, const double* amps,
                     double freq) {
  size_t i, c;
  for (i = 0; i < n; ++i) {
    double s = sin(2.0 * M_PI * freq *
                   (double) (first_frame + i) / (double) SR);
    for (c = 0; c < channels; ++c) {
      buf[i * channels + c] = amps[c] * s;
    }
  }
}

static void gen_silence(double* buf, size_t n, unsigned int channels) {
  memset(buf, 0, n * channels * sizeof(double));
}

/* Feeds a tone in arbitrary chunk sizes. */
static void feed_tone_chunked(ebur128_state* st, size_t frames,
                              const double* amps, double freq,
                              const size_t* chunks, size_t nchunks) {
  unsigned int channels = st->channels;
  size_t ci = 0, done = 0;
  double buf[8192];

  while (done < frames) {
    size_t n = chunks[ci % nchunks];
    if (n > sizeof(buf) / sizeof(buf[0]) / channels) {
      n = sizeof(buf) / sizeof(buf[0]) / channels;
    }
    if (n > frames - done) {
      n = frames - done;
    }
    gen_tone(buf, done, n, channels, amps, freq);
    ebur128_add_frames_double(st, buf, n);
    done += n;
    ++ci;
  }
}

static void feed_tone(ebur128_state* st, size_t frames, const double* amps,
                      double freq) {
  static const size_t one_shot[] = { (size_t) -1 };
  feed_tone_chunked(st, frames, amps, freq, one_shot, 1);
}

static void feed_silence(ebur128_state* st, size_t frames) {
  double buf[4096];
  unsigned int channels = st->channels;
  size_t chunk = sizeof(buf) / sizeof(buf[0]) / channels;
  size_t done = 0;
  while (done < frames) {
    size_t n = frames - done < chunk ? frames - done : chunk;
    gen_silence(buf, n, channels);
    ebur128_add_frames_double(st, buf, n);
    done += n;
  }
}

static double lufs_or_ninf(ebur128_state* st) {
  double v;
  ebur128_loudness_global(st, &v);
  return v;
}

/* ------------------------------------------------------------------ */
/* 1. Independently verifiable block counts, short tails, chunking.   */
/* ------------------------------------------------------------------ */
static void test_block_geometry_and_tail(void) {
  static const size_t lengths[] = { 0, 1, 19199, 19200, 23999, 24000,
                                    96000, 240000, 480000 };
  size_t k;
  double amp = 0.2;

  for (k = 0; k < sizeof(lengths) / sizeof(lengths[0]); ++k) {
    size_t frames = lengths[k];
    ebur128_state* st = ebur128_init(1, SR, EBUR128_MODE_I);
    ebur128_gating_report r;
    feed_tone(st, frames, &amp, 1000.0);
    ebur128_loudness_report(st, &r);
    CHECK(r.absolute_blocks == expected_block_count(frames),
          "geometry: frames=%zu expected %zu blocks, got %zu",
          frames, expected_block_count(frames), r.absolute_blocks);
    CHECK(r.gated_blocks == r.absolute_blocks,
          "geometry: uniform tone frames=%zu gated %zu != absolute %zu",
          frames, r.gated_blocks, r.absolute_blocks);
    ebur128_destroy(&st);
  }

  /* Arbitrary add_frames fragmentation must give identical statistics.
   * Block counts are exact by construction; LUFS are compared with a tight
   * tolerance because on platforms without SSE2 the library's manual
   * flush-to-zero runs once per add_frames call, so subnormal filter-state
   * roundoff can differ slightly across chunk boundaries. */
  {
    static const size_t weird[] = { 1, 4799, 4800, 37, 10000, 19201 };
    ebur128_state* a = ebur128_init(2, SR, EBUR128_MODE_I);
    ebur128_state* b = ebur128_init(2, SR, EBUR128_MODE_I);
    ebur128_gating_report ra, rb;
    double amps[2] = { 0.3, 0.3 };
    feed_tone(a, 123457, amps, 1000.0);
    feed_tone_chunked(b, 123457, amps, 1000.0, weird,
                      sizeof(weird) / sizeof(weird[0]));
    ebur128_loudness_report(a, &ra);
    ebur128_loudness_report(b, &rb);
    CHECK(ra.absolute_blocks == rb.absolute_blocks &&
              ra.gated_blocks == rb.gated_blocks,
          "chunking: counts differ (%zu/%zu vs %zu/%zu)",
          ra.absolute_blocks, ra.gated_blocks, rb.absolute_blocks,
          rb.gated_blocks);
    CHECK(fabs(ra.integrated_lufs - rb.integrated_lufs) < 1e-10,
          "chunking: integrated differs (%a vs %a)",
          ra.integrated_lufs, rb.integrated_lufs);
    ebur128_destroy(&a);
    ebur128_destroy(&b);
  }

  /* A tail shorter than 400 ms must never complete a new block. */
  {
    ebur128_state* st = ebur128_init(1, SR, EBUR128_MODE_I);
    ebur128_gating_report r;
    feed_tone(st, 19200, &amp, 1000.0);
    feed_tone(st, 4799, &amp, 1000.0); /* 23999 frames total */
    ebur128_loudness_report(st, &r);
    CHECK(r.absolute_blocks == 1, "short tail produced %zu blocks",
          r.absolute_blocks);
    ebur128_destroy(&st);
  }

  /* Float, int and short input must produce the same block counts. */
  {
    size_t n = 50000, i;
    ebur128_state* stf = ebur128_init(1, SR, EBUR128_MODE_I);
    ebur128_state* sti = ebur128_init(1, SR, EBUR128_MODE_I);
    ebur128_state* sts = ebur128_init(1, SR, EBUR128_MODE_I);
    ebur128_gating_report rf, ri, rs;
    float* fb = (float*) malloc(n * sizeof(float));
    int* ib = (int*) malloc(n * sizeof(int));
    short* sb = (short*) malloc(n * sizeof(short));
    for (i = 0; i < n; ++i) {
      double s = 0.2 * sin(2.0 * M_PI * 1000.0 * i / SR);
      fb[i] = (float) s;
      ib[i] = (int) (s * (double) INT_MAX);
      sb[i] = (short) (s * (double) SHRT_MAX);
    }
    ebur128_add_frames_float(stf, fb, n);
    ebur128_add_frames_int(sti, ib, n);
    ebur128_add_frames_short(sts, sb, n);
    free(fb);
    free(ib);
    free(sb);
    ebur128_loudness_report(stf, &rf);
    ebur128_loudness_report(sti, &ri);
    ebur128_loudness_report(sts, &rs);
    CHECK(rf.absolute_blocks == expected_block_count(n),
          "float feed: got %zu, expected %zu", rf.absolute_blocks,
          expected_block_count(n));
    CHECK(ri.absolute_blocks == rf.absolute_blocks &&
              rs.absolute_blocks == rf.absolute_blocks,
          "int/short feed: counts %zu/%zu vs float %zu",
          ri.absolute_blocks, rs.absolute_blocks, rf.absolute_blocks);
    ebur128_destroy(&stf);
    ebur128_destroy(&sti);
    ebur128_destroy(&sts);
  }
}

/* ------------------------------------------------------------------ */
/* 2. Silence and completely empty states.                            */
/* ------------------------------------------------------------------ */
static void test_silence(void) {
  ebur128_state* st = ebur128_init(1, SR, EBUR128_MODE_I);
  ebur128_gating_report r;
  double global_lufs;

  /* No frames at all. */
  ebur128_loudness_report(st, &r);
  CHECK(r.absolute_blocks == 0 && r.gated_blocks == 0,
        "empty state: counts %zu/%zu", r.absolute_blocks, r.gated_blocks);
  CHECK(r.relative_threshold_lufs == -70.0,
        "empty state: threshold %a", r.relative_threshold_lufs);
  CHECK(r.integrated_lufs == -HUGE_VAL,
        "empty state: integrated %a", r.integrated_lufs);

  /* Long silence: blocks below -70 LUFS are not stored. */
  feed_silence(st, 120000);
  ebur128_loudness_report(st, &r);
  CHECK(r.absolute_blocks == 0 && r.gated_blocks == 0,
        "silence: counts %zu/%zu", r.absolute_blocks, r.gated_blocks);
  CHECK(r.relative_threshold_lufs == -70.0,
        "silence: threshold %a", r.relative_threshold_lufs);
  CHECK(r.integrated_lufs == -HUGE_VAL,
        "silence: integrated %a", r.integrated_lufs);
  ebur128_loudness_global(st, &global_lufs);
  CHECK(global_lufs == -HUGE_VAL, "silence: global %a", global_lufs);
  ebur128_destroy(&st);

  /* All-NULL / zero-length arrays behave like an empty measurement. */
  {
    ebur128_state* all_null[3] = { NULL, NULL, NULL };
    ebur128_gating_report rn;
    CHECK(ebur128_loudness_report_multiple(all_null, 3, &rn) ==
              EBUR128_SUCCESS,
          "all-NULL array rejected");
    CHECK(rn.absolute_blocks == 0 && rn.gated_blocks == 0 &&
              rn.relative_threshold_lufs == -70.0 &&
              rn.integrated_lufs == -HUGE_VAL,
          "all-NULL array: unexpected empty report");
    CHECK(ebur128_loudness_report_multiple(all_null, 0, &rn) ==
              EBUR128_SUCCESS,
          "zero-size array rejected");
    CHECK(rn.absolute_blocks == 0 && rn.gated_blocks == 0,
          "zero-size array: unexpected counts");
  }
}

/* ------------------------------------------------------------------ */
/* 3. UNUSED channels and surround weighting.                         */
/* ------------------------------------------------------------------ */
static void test_channels(void) {
  /* 3-channel state: LEFT, UNUSED (loud signal must be ignored),
   * LEFT_SURROUND (weight 1.41). */
  ebur128_state* st3 = ebur128_init(3, SR, EBUR128_MODE_I);
  /* Stereo reference: two ordinary channels, same amplitude. */
  ebur128_state* st2 = ebur128_init(2, SR, EBUR128_MODE_I);
  ebur128_gating_report r3, r2;
  double amps3[3] = { 0.1, 1.0, 0.1 };
  double amps2[2] = { 0.1, 0.1 };
  double expected_delta;

  ebur128_set_channel(st3, 0, EBUR128_LEFT);
  ebur128_set_channel(st3, 1, EBUR128_UNUSED);
  ebur128_set_channel(st3, 2, EBUR128_LEFT_SURROUND);

  feed_tone(st3, 96000, amps3, 1000.0);
  feed_tone(st2, 96000, amps2, 1000.0);
  ebur128_loudness_report(st3, &r3);
  ebur128_loudness_report(st2, &r2);

  CHECK(r3.absolute_blocks == r2.absolute_blocks,
        "channels: block count differs %zu vs %zu", r3.absolute_blocks,
        r2.absolute_blocks);
  CHECK(r3.absolute_blocks == expected_block_count(96000),
        "channels: %zu blocks, geometry says %zu", r3.absolute_blocks,
        expected_block_count(96000));

  /* Independent weight arithmetic: (1.0 + 1.41) / (1.0 + 1.0) = 1.205,
   * i.e. 10*log10(1.205) ~= 0.81 LU. The loud UNUSED channel contributes
   * nothing. */
  expected_delta = 10.0 * log10(2.41 / 2.0);
  CHECK(fabs((r3.integrated_lufs - r2.integrated_lufs) -
                   expected_delta) < 0.02,
        "channels: surround weight delta %a, expected %a",
        r3.integrated_lufs - r2.integrated_lufs, expected_delta);
  ebur128_destroy(&st3);
  ebur128_destroy(&st2);

  /* Every channel UNUSED: even full-scale signal yields no blocks. */
  {
    ebur128_state* stu = ebur128_init(2, SR, EBUR128_MODE_I);
    ebur128_gating_report ru;
    double amps[2] = { 1.0, 1.0 };
    ebur128_set_channel(stu, 0, EBUR128_UNUSED);
    ebur128_set_channel(stu, 1, EBUR128_UNUSED);
    feed_tone(stu, 96000, amps, 1000.0);
    ebur128_loudness_report(stu, &ru);
    CHECK(ru.absolute_blocks == 0 && ru.gated_blocks == 0,
          "all-UNUSED: stored %zu blocks", ru.absolute_blocks);
    CHECK(ru.integrated_lufs == -HUGE_VAL,
          "all-UNUSED: integrated %a", ru.integrated_lufs);
    ebur128_destroy(&stu);
  }
}

/* ------------------------------------------------------------------ */
/* 4. Headline: joint relative gate across programmes of different    */
/*    length and level, and why averaging per-state LUFS is wrong.    */
/* ---------------------------------------------------------------- */
static void test_joint_gating(int histogram) {
  int mode = EBUR128_MODE_I | (histogram ? EBUR128_MODE_HISTOGRAM : 0);
  ebur128_state* loud = ebur128_init(1, SR, mode);
  ebur128_state* quiet = ebur128_init(1, SR, mode);
  ebur128_state* mix[2];
  ebur128_gating_report rl, rq, rm;
  double a_loud = 0.5, a_quiet = 0.05; /* 20 dB apart */
  double global_loud, global_quiet, global_mix, naive_average;
  size_t loud_blocks = expected_block_count(480000); /* 10 s */
  size_t quiet_blocks = expected_block_count(96000); /* 2 s */

  feed_tone(loud, 480000, &a_loud, 1000.0);
  feed_tone(quiet, 96000, &a_quiet, 1000.0);

  ebur128_loudness_report(loud, &rl);
  ebur128_loudness_report(quiet, &rq);
  mix[0] = loud;
  mix[1] = quiet;
  ebur128_loudness_report_multiple(mix, 2, &rm);

  CHECK(rl.absolute_blocks == loud_blocks,
          "joint(%s): loud blocks %zu != %zu",
          histogram ? "hist" : "exact", rl.absolute_blocks, loud_blocks);
  CHECK(rq.absolute_blocks == quiet_blocks,
          "joint(%s): quiet blocks %zu != %zu",
          histogram ? "hist" : "exact", rq.absolute_blocks, quiet_blocks);

  /* On its own, every block of the quiet programme passes its own
   * relative gate (its blocks sit ~10 LU above that gate). */
  CHECK(rq.gated_blocks == rq.absolute_blocks,
          "joint(%s): quiet alone gated %zu of %zu",
          histogram ? "hist" : "exact", rq.gated_blocks, rq.absolute_blocks);

  /* The joint gate is pulled up by the long loud programme: none of the
   * quiet programme's blocks survive, every loud block does. The margin is
   * ~9 LU on both sides, two orders of magnitude larger than the 0.1 LU
   * histogram bins, so the counts are exact even in histogram mode. */
  CHECK(rm.absolute_blocks == loud_blocks + quiet_blocks,
          "joint(%s): merged absolute count %zu != %zu",
          histogram ? "hist" : "exact", rm.absolute_blocks,
          loud_blocks + quiet_blocks);
  CHECK(rm.gated_blocks == loud_blocks,
          "joint(%s): gated %zu, expected only the %zu loud blocks",
          histogram ? "hist" : "exact", rm.gated_blocks, loud_blocks);
  CHECK(rm.relative_threshold_lufs > rq.relative_threshold_lufs + 5.0,
          "joint(%s): joint gate %a not well above quiet-only gate %a",
          histogram ? "hist" : "exact", rm.relative_threshold_lufs,
          rq.relative_threshold_lufs);

  /* Averaging the per-state LUFS (or per-state thresholds) is wrong. */
  global_loud = lufs_or_ninf(loud);
  global_quiet = lufs_or_ninf(quiet);
  ebur128_loudness_global_multiple(mix, 2, &global_mix);
  naive_average = (global_loud + global_quiet) / 2.0;
  CHECK(fabs(global_mix - naive_average) > 1.0,
          "joint(%s): merged LUFS %a indistinguishable from naive average %a",
          histogram ? "hist" : "exact", global_mix, naive_average);
  CHECK(rm.integrated_lufs == global_mix,
          "joint(%s): report LUFS %a != global_multiple %a",
          histogram ? "hist" : "exact", rm.integrated_lufs, global_mix);

  ebur128_destroy(&loud);
  ebur128_destroy(&quiet);
}

/* ------------------------------------------------------------------ */
/* 5. One programme with a loud part and a quiet tail: the quiet      */
/*    section is retained for the absolute gate but dropped by the    */
/*    relative gate.                                                  */
/* ------------------------------------------------------------------ */

/* Independent expectation of the relative-gate decision for a block whose
 * window overlaps the loud/quiet boundary. The blocks are 400 ms windows
 * hopping by 100 ms; a block at position p covers samples [p*HOP,
 * p*HOP+4HOP). With constant energies E_L (loud) and E_Q (quiet), the block
 * energy is the weighted mean (w*E_L + (4-w)*E_Q)/4 where w is the number
 * of 100 ms quarters inside the loud section. The relative gate equals the
 * mean of all block energies divided by 10; this sums the same weights over
 * all blocks, so the decision reduces to:
 *   (w*E_L + (4-w)*E_Q)/4  >=  (W_TOTAL*E_L + (N*4-W_TOTAL)*E_Q)/(40*N)
 * with no LUFS function involved. */
static int model_block_passes(size_t p, size_t quiet_start) {
  const double e_loud = 100.0;  /* amplitudes 0.5 vs 0.05 -> 100x energy */
  const double e_quiet = 1.0;
  const size_t n_blocks = 57;   /* 6 s: first block at 0.4 s, hop 0.1 s */
  size_t w, w_total = 0, k;
  double block_e, gate_e;

  for (k = 0; k < n_blocks; ++k) {
    /* number of 100 ms quarters of block k lying in the loud 3 s part */
    size_t lo = k < quiet_start / HOP_FRAMES ? k : quiet_start / HOP_FRAMES;
    size_t hi = (k + 4 < quiet_start / HOP_FRAMES) ? (k + 4)
                                                   : quiet_start / HOP_FRAMES;
    w_total += hi > lo ? hi - lo : 0;
  }
  {
    size_t lo = p < quiet_start / HOP_FRAMES ? p : quiet_start / HOP_FRAMES;
    size_t hi = (p + 4 < quiet_start / HOP_FRAMES) ? (p + 4)
                                                   : quiet_start / HOP_FRAMES;
    w = hi > lo ? hi - lo : 0;
  }
  block_e = ((double) w * e_loud + (double) (4 - w) * e_quiet) / 4.0;
  gate_e = ((double) w_total * e_loud +
            (double) (4 * n_blocks - w_total) * e_quiet) /
           (40.0 * (double) n_blocks);
  return block_e >= gate_e;
}

static void test_quiet_section_single_state(int histogram) {
  int mode = EBUR128_MODE_I | (histogram ? EBUR128_MODE_HISTOGRAM : 0);
  ebur128_state* st = ebur128_init(1, SR, mode);
  ebur128_gating_report r;
  double a_loud = 0.5, a_quiet = 0.05;
  double global_lufs;
  size_t total = 288000; /* 6 s: 3 s loud + 3 s quiet, 57 blocks */
  size_t expected_gated = 0, p;

  feed_tone(st, 144000, &a_loud, 1000.0);
  feed_tone(st, 144000, &a_quiet, 1000.0);
  ebur128_loudness_report(st, &r);
  ebur128_loudness_global(st, &global_lufs);

  for (p = 0; p < expected_block_count(total); ++p) {
    expected_gated += model_block_passes(p, 144000) ? 1 : 0;
  }

  CHECK(r.absolute_blocks == expected_block_count(total),
          "quiet-tail(%s): absolute %zu != %zu",
          histogram ? "hist" : "exact", r.absolute_blocks,
          expected_block_count(total));
  /* The model and the library disagree only if a block energy lands within
   * floating-point roundoff of the gate. Here the closest block is ~7 LU
   * away, while histogram bins are 0.1 LU wide, so the count is exact in
   * both modes. */
  CHECK(r.gated_blocks == expected_gated,
          "quiet-tail(%s): gated %zu != independent model %zu",
          histogram ? "hist" : "exact", r.gated_blocks, expected_gated);
  CHECK(r.gated_blocks < r.absolute_blocks,
          "quiet-tail(%s): relative gate removed nothing",
          histogram ? "hist" : "exact");
  CHECK(r.integrated_lufs == global_lufs,
          "quiet-tail(%s): report %a != global %a",
          histogram ? "hist" : "exact", r.integrated_lufs, global_lufs);
  ebur128_destroy(&st);
}

/* ------------------------------------------------------------------ */
/* 6. NULL elements and repeated states.                              */
/* ------------------------------------------------------------------ */
static void test_null_and_duplicates(void) {
  ebur128_state* st = ebur128_init(1, SR, EBUR128_MODE_I);
  ebur128_state* arr[4];
  ebur128_gating_report single, rep, with_nulls;
  double amp = 0.3, single_global, rep_global;

  feed_tone(st, 96000, &amp, 1000.0);
  ebur128_loudness_report(st, &single);

  arr[0] = st;
  arr[1] = st;
  ebur128_loudness_report_multiple(arr, 2, &rep);
  CHECK(rep.absolute_blocks == 2 * single.absolute_blocks &&
              rep.gated_blocks == 2 * single.gated_blocks,
          "duplicate: counts not doubled (%zu/%zu vs %zu/%zu)",
          rep.absolute_blocks, rep.gated_blocks, 2 * single.absolute_blocks,
          2 * single.gated_blocks);
  /* Duplicating an identical state cannot move the merged mean. */
  CHECK(rep.integrated_lufs == single.integrated_lufs,
          "duplicate: integrated changed %a vs %a", rep.integrated_lufs,
          single.integrated_lufs);

  arr[0] = NULL;
  arr[1] = st;
  arr[2] = NULL;
  arr[3] = st;
  ebur128_loudness_report_multiple(arr, 4, &with_nulls);
  CHECK(with_nulls.absolute_blocks == rep.absolute_blocks &&
              with_nulls.gated_blocks == rep.gated_blocks &&
              with_nulls.integrated_lufs == rep.integrated_lufs,
          "NULLs: report differs from duplicate-only report");

  ebur128_loudness_global(st, &single_global);
  ebur128_loudness_global_multiple(arr, 4, &rep_global);
  CHECK(rep_global == single_global,
          "duplicate: global_multiple %a != global %a", rep_global,
          single_global);
  ebur128_destroy(&st);
}

/* ------------------------------------------------------------------ */
/* 7. Exact / histogram states mixed in one query, and LUFS parity.  */
/* ------------------------------------------------------------------ */
static void test_mode_mixing(void) {
  ebur128_state* ex = ebur128_init(1, SR, EBUR128_MODE_I);
  ebur128_state* hi =
      ebur128_init(1, SR, EBUR128_MODE_I | EBUR128_MODE_HISTOGRAM);
  ebur128_state* mixed[2] = { ex, hi };
  ebur128_state* exacts[2] = { ex, ex };
  ebur128_state* histonly[1];
  ebur128_gating_report rex, rhi, rmix, rex2, rhionly;
  double gex, ghi, gmix;
  double amp = 0.25;
  size_t blocks;

  feed_tone(ex, 120000, &amp, 1000.0);
  feed_tone(hi, 120000, &amp, 1000.0);
  blocks = expected_block_count(120000);

  ebur128_loudness_report(ex, &rex);
  ebur128_loudness_report(hi, &rhi);
  ebur128_loudness_report_multiple(mixed, 2, &rmix);
  ebur128_loudness_report_multiple(exacts, 2, &rex2);
  histonly[0] = hi;
  ebur128_loudness_report_multiple(histonly, 1, &rhionly);

  CHECK(rex.absolute_blocks == blocks && rhi.absolute_blocks == blocks,
          "mode-mix: single counts %zu/%zu vs %zu", rex.absolute_blocks,
          rhi.absolute_blocks, blocks);
  CHECK(rex.gated_blocks == rhi.gated_blocks,
          "mode-mix: gated counts differ %zu vs %zu", rex.gated_blocks,
          rhi.gated_blocks);
  CHECK(rmix.absolute_blocks == 2 * blocks &&
              rmix.gated_blocks == 2 * rex.gated_blocks,
          "mode-mix: mixed counts %zu/%zu", rmix.absolute_blocks,
          rmix.gated_blocks);
  CHECK(rmix.gated_blocks == rex2.gated_blocks,
          "mode-mix: mixed vs exact-only gated differ");

  /* Histogram bin centres are 0.1 LU apart, so cross-mode LUFS differ by
   * at most a small fraction of one bin; the 1e-9 requirement applies to
   * report-vs-old-API within one mode, where both run identical code. */
  ebur128_loudness_global(ex, &gex);
  ebur128_loudness_global(hi, &ghi);
  ebur128_loudness_global_multiple(mixed, 2, &gmix);
  CHECK(fabs(rex.integrated_lufs - gex) <= 1e-9,
          "mode-mix: exact report/global delta %a",
          rex.integrated_lufs - gex);
  CHECK(fabs(rhi.integrated_lufs - ghi) <= 1e-9,
          "mode-mix: histogram report/global delta %a",
          rhi.integrated_lufs - ghi);
  CHECK(fabs(rmix.integrated_lufs - gmix) <= 1e-9,
          "mode-mix: mixed report/global delta %a",
          rmix.integrated_lufs - gmix);
  CHECK(fabs(gex - ghi) < 0.1,
          "mode-mix: cross-mode LUFS delta %a", gex - ghi);
  CHECK(fabs(rmix.integrated_lufs - rex2.integrated_lufs) < 0.1,
          "mode-mix: mixed vs exact-only LUFS delta %a",
          rmix.integrated_lufs - rex2.integrated_lufs);
  CHECK(rhionly.integrated_lufs == rhi.integrated_lufs,
          "mode-mix: single-histogram vs multiple-of-one differ");

  ebur128_destroy(&ex);
  ebur128_destroy(&hi);
}

/* ------------------------------------------------------------------ */
/* 8. Invalid mode must leave the report bytes untouched.             */
/* ------------------------------------------------------------------ */
static void test_invalid_mode(void) {
  ebur128_state* good = ebur128_init(1, SR, EBUR128_MODE_I);
  ebur128_state* bad = ebur128_init(1, SR, EBUR128_MODE_M);
  ebur128_state* arr[2] = { good, bad };
  ebur128_gating_report r, snapshot;
  double amp = 0.2;
  double buf[1024];

  gen_tone(buf, 0, 1024, 1, &amp, 1000.0);
  ebur128_add_frames_double(good, buf, 1024);
  ebur128_add_frames_double(bad, buf, 1024);

  memset(&r, 0xAB, sizeof(r));
  memcpy(&snapshot, &r, sizeof(r));
  CHECK(ebur128_loudness_report(bad, &r) == EBUR128_ERROR_INVALID_MODE,
          "invalid mode: single state accepted");
  CHECK(memcmp(&r, &snapshot, sizeof(r)) == 0,
          "invalid mode: single state wrote to the report");

  memset(&r, 0xCD, sizeof(r));
  memcpy(&snapshot, &r, sizeof(r));
  CHECK(ebur128_loudness_report_multiple(arr, 2, &r) ==
              EBUR128_ERROR_INVALID_MODE,
          "invalid mode: mixed array accepted");
  CHECK(memcmp(&r, &snapshot, sizeof(r)) == 0,
          "invalid mode: mixed array wrote to the report");

  /* NULL elements are skipped; the bad state is still detected. */
  arr[0] = NULL;
  CHECK(ebur128_loudness_report_multiple(arr, 2, &r) ==
              EBUR128_ERROR_INVALID_MODE,
          "invalid mode: bad state behind NULL not detected");
  CHECK(memcmp(&r, &snapshot, sizeof(r)) == 0,
          "invalid mode: report modified on second failure");

  /* The good state still works afterwards. */
  arr[0] = good;
  arr[1] = NULL;
  CHECK(ebur128_loudness_report_multiple(arr, 2, &r) == EBUR128_SUCCESS,
          "invalid mode: good state broken afterwards");

  ebur128_destroy(&good);
  ebur128_destroy(&bad);
}

/* ------------------------------------------------------------------ */
/* 9. Queries are read-only: no consumption, no filter reset, the     */
/*    unfinished tail survives, repeated queries are identical.       */
/* ------------------------------------------------------------------ */
static void test_read_only(void) {
  ebur128_state* queried = ebur128_init(1, SR, EBUR128_MODE_I);
  ebur128_state* control = ebur128_init(1, SR, EBUR128_MODE_I);
  ebur128_gating_report r1, r2;
  double amp = 0.25;
  double gq, gc;
  static const size_t chunks[] = { 333, 7000, 19199 };
  size_t nchunks = sizeof(chunks) / sizeof(chunks[0]);

  /* Both states get byte-identical frame sequences with identical chunk
   * boundaries; only "queried" is inspected in between. This isolates the
   * report call as the sole possible source of divergence (the manual FTZ
   * path can make different chunk plans round differently, which is
   * pre-existing library behaviour unrelated to this API). */
  feed_tone_chunked(queried, 1000, &amp, 1000.0, chunks, nchunks);
  feed_tone_chunked(control, 1000, &amp, 1000.0, chunks, nchunks);
  ebur128_loudness_report(queried, &r1);
  CHECK(r1.absolute_blocks == 0,
          "read-only: %zu blocks before 400 ms", r1.absolute_blocks);

  feed_tone_chunked(queried, 95000, &amp, 1000.0, chunks, nchunks);
  feed_tone_chunked(control, 95000, &amp, 1000.0, chunks, nchunks);

  ebur128_loudness_report(queried, &r1);
  ebur128_loudness_report(queried, &r2);
  CHECK(memcmp(&r1, &r2, sizeof(r1)) == 0,
          "read-only: repeated reports differ");
  ebur128_loudness_report(control, &r2);
  CHECK(r1.absolute_blocks == r2.absolute_blocks &&
              r1.gated_blocks == r2.gated_blocks &&
              r1.integrated_lufs == r2.integrated_lufs,
          "read-only: mid-stream query changed results "
          "(%zu/%zu/%a vs %zu/%zu/%a)",
          r1.absolute_blocks, r1.gated_blocks, r1.integrated_lufs,
          r2.absolute_blocks, r2.gated_blocks, r2.integrated_lufs);

  /* add_frames after a query keeps working: extend both identically and
   * compare. */
  feed_tone(queried, 24000, &amp, 1000.0);
  feed_tone(control, 24000, &amp, 1000.0);
  ebur128_loudness_global(queried, &gq);
  ebur128_loudness_global(control, &gc);
  CHECK(gq == gc,
          "read-only: add_frames after report diverged (%a vs %a)", gq, gc);
  ebur128_loudness_report(queried, &r1);
  ebur128_loudness_report(control, &r2);
  CHECK(r1.absolute_blocks == r2.absolute_blocks,
          "read-only: post-query block counts differ");

  ebur128_destroy(&queried);
  ebur128_destroy(&control);
}

/* ------------------------------------------------------------------ */
/* 10. max_history eviction (exact mode); histogram keeps everything. */
/* ------------------------------------------------------------------ */
static void test_history(void) {
  ebur128_state* st = ebur128_init(1, SR, EBUR128_MODE_I);
  ebur128_state* hi =
      ebur128_init(1, SR, EBUR128_MODE_I | EBUR128_MODE_HISTOGRAM);
  ebur128_gating_report r, rh;
  double amp = 0.2, global_lufs;
  size_t all_blocks = expected_block_count(240000); /* 5 s, 47 blocks */

  ebur128_set_max_history(st, 1000); /* block_list_max = 1000/100 = 10 */
  ebur128_set_max_history(hi, 1000); /* no effect on histogram storage */
  feed_tone(st, 240000, &amp, 1000.0);
  feed_tone(hi, 240000, &amp, 1000.0);

  ebur128_loudness_report(st, &r);
  ebur128_loudness_report(hi, &rh);
  CHECK(all_blocks == 47, "history: fixture block count %zu", all_blocks);
  CHECK(r.absolute_blocks == 10,
          "history: exact mode retained %zu blocks, expected 10",
          r.absolute_blocks);
  CHECK(rh.absolute_blocks == all_blocks,
          "history: histogram lost blocks (%zu vs %zu)",
          rh.absolute_blocks, all_blocks);
  ebur128_loudness_global(st, &global_lufs);
  CHECK(r.integrated_lufs == global_lufs,
          "history: report %a != global %a", r.integrated_lufs,
          global_lufs);
  ebur128_destroy(&st);
  ebur128_destroy(&hi);
}

/* ------------------------------------------------------------------ */
/* 11. With blocks present, the relative gate is not clamped to -70.  */
/* ------------------------------------------------------------------ */
static void test_relative_gate_not_clamped(void) {
  ebur128_state* st = ebur128_init(1, SR, EBUR128_MODE_I);
  ebur128_gating_report r;
  double amp = 0.0012; /* blocks above -70 LUFS, integrated ~-61 LUFS */
  double old_threshold;

  feed_tone(st, 96000, &amp, 1000.0);
  ebur128_loudness_report(st, &r);
  ebur128_relative_threshold(st, &old_threshold);
  CHECK(r.absolute_blocks > 0, "no-clamp: no blocks stored");
  CHECK(r.relative_threshold_lufs < -70.0,
          "no-clamp: relative gate %a clamped to -70",
          r.relative_threshold_lufs);
  CHECK(r.relative_threshold_lufs == old_threshold,
          "no-clamp: report threshold %a != relative_threshold %a",
          r.relative_threshold_lufs, old_threshold);
  ebur128_destroy(&st);
}

/* ------------------------------------------------------------------ */
/* 12. Report values vs the old API, incl. relative_threshold.        */
/* ------------------------------------------------------------------ */
static void test_compatibility(void) {
  static const double amps_list[][2] = { { 0.5, 0.5 }, { 0.02, 0.02 } };
  int k;
  for (k = 0; k < 2; ++k) {
    int histogram = k;
    int mode = EBUR128_MODE_I | (histogram ? EBUR128_MODE_HISTOGRAM : 0);
    ebur128_state* a = ebur128_init(2, SR, mode);
    ebur128_state* b = ebur128_init(2, SR, mode);
    ebur128_state* arr[2] = { a, b };
    ebur128_gating_report ra, rb, rm;
    double ga, gb, gm, ta, tb;
    double amps[2] = { amps_list[k][0], amps_list[k][1] };

    feed_tone(a, 72000, amps, 440.0);
    feed_tone(b, 168000, amps, 440.0);

    ebur128_loudness_report(a, &ra);
    ebur128_loudness_report(b, &rb);
    ebur128_loudness_report_multiple(arr, 2, &rm);

    ebur128_loudness_global(a, &ga);
    ebur128_loudness_global(b, &gb);
    ebur128_loudness_global_multiple(arr, 2, &gm);
    ebur128_relative_threshold(a, &ta);
    ebur128_relative_threshold(b, &tb);

    CHECK(ra.integrated_lufs == ga,
          "compat(%s): single A report %a != global %a",
          histogram ? "hist" : "exact", ra.integrated_lufs, ga);
    CHECK(rb.integrated_lufs == gb,
          "compat(%s): single B report %a != global %a",
          histogram ? "hist" : "exact", rb.integrated_lufs, gb);
    CHECK(rm.integrated_lufs == gm,
          "compat(%s): multiple report %a != global %a",
          histogram ? "hist" : "exact", rm.integrated_lufs, gm);
    CHECK(fabs(ra.integrated_lufs - ga) <= 1e-9 &&
              fabs(rb.integrated_lufs - gb) <= 1e-9 &&
              fabs(rm.integrated_lufs - gm) <= 1e-9,
          "compat(%s): 1e-9 LU tolerance violated",
          histogram ? "hist" : "exact");
    CHECK(ra.relative_threshold_lufs == ta,
          "compat(%s): threshold A %a != relative_threshold %a",
          histogram ? "hist" : "exact", ra.relative_threshold_lufs, ta);
    CHECK(rb.relative_threshold_lufs == tb,
          "compat(%s): threshold B %a != relative_threshold %a",
          histogram ? "hist" : "exact", rb.relative_threshold_lufs, tb);
    CHECK(rm.absolute_blocks ==
              ra.absolute_blocks + rb.absolute_blocks,
          "compat(%s): merged absolute count mismatch",
          histogram ? "hist" : "exact");
    /* A threshold above -70 must not be clamped. */
    CHECK(rm.relative_threshold_lufs > -70.0,
          "compat(%s): threshold clamped to -70 (%a)",
          histogram ? "hist" : "exact", rm.relative_threshold_lufs);

    ebur128_destroy(&a);
    ebur128_destroy(&b);
  }
}

int main(void) {
  printf("running gating report tests...\n");

  test_block_geometry_and_tail();
  test_silence();
  test_channels();
  test_joint_gating(0);
  test_joint_gating(1);
  test_quiet_section_single_state(0);
  test_quiet_section_single_state(1);
  test_null_and_duplicates();
  test_mode_mixing();
  test_invalid_mode();
  test_read_only();
  test_history();
  test_relative_gate_not_clamped();
  test_compatibility();

  printf("\n%d checks, %d failures\n", checks, failures);
  if (failures) {
    return 1;
  }
  printf("ALL TESTS PASSED\n");
  return 0;
}
