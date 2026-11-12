/*
 * Numeric filter contract for libsoxr: independent analytical references for
 * pass-band gain, stop-band/alias rejection and phase-response behaviour.
 *
 * Usage: filter-contract {passband|alias|phase}
 *
 * The reference side is deliberately independent of the library under test:
 * stimuli are analytic sine waves generated here, amplitudes/frequencies are
 * recovered with least-squares sin/cos basis fits, alias frequencies are
 * derived from sampling-rate folding equations, and impulse energy/phase
 * metrics come from direct summations and complex-exponential projections.
 * No library coefficient table, internal FFT or second library run is used.
 *
 * Configuration exercised by every conversion: SOXR_VHQ quality, FLOAT64
 * input/output, single-threaded runtime.  Linear-phase cases use phase 50;
 * every other phase is set explicitly through the quality spec.
 */

#include "soxr.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ----------------------------- test harness ------------------------------ */

static char const *g_group;          /* "passband", "alias" or "phase" */
static long g_checks;                /* number of evaluated assertions */

static void vfail(char const *case_name, char const *fmt, va_list ap) {
  fprintf(stdout, "FAIL %s %s: ", g_group, case_name);
  vfprintf(stdout, fmt, ap);
  fputc('\n', stdout);
  exit(1);
}

static void fail(char const *case_name, char const *fmt, ...) {
  va_list ap; va_start(ap, fmt); vfail(case_name, fmt, ap); va_end(ap);
}

/* Setup/contract failure (bad error return, frame accounting, empty output):
 * distinct prefix so mutation drivers can tell it from a threshold breach. */
static void error_fail(char const *case_name, char const *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stdout, "ERROR %s %s: ", g_group, case_name);
  vfprintf(stdout, fmt, ap);
  fputc('\n', stdout);
  va_end(ap);
  exit(1);
}

static void check_numeric(int ok, char const *case_name, char const *what,
                          double observed, double tolerance) {
  ++g_checks;
  if (!ok) fail(case_name, "%s observed=%.12g tolerance=%.12g",
                what, observed, tolerance);
}

static void check_condition(int ok, char const *case_name, char const *what) {
  ++g_checks;
  if (!ok) error_fail(case_name, "%s", what);
}

/* --------------------------- soxr configuration -------------------------- */

static soxr_quality_spec_t vhq_quality(double phase_response) {
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_VHQ, 0);
  q.phase_response = phase_response;      /* 50 = linear, set explicitly */
  return q;
}

static soxr_io_spec_t io_spec(int split) {
  return soxr_io_spec(split ? SOXR_FLOAT64_S : SOXR_FLOAT64_I,
                      split ? SOXR_FLOAT64_S : SOXR_FLOAT64_I);
}

static soxr_runtime_spec_t single_thread(void) {
  return soxr_runtime_spec(1);
}

static size_t expected_frames(double irate, double orate, size_t ilen) {
  return (size_t)(orate * (double)ilen / irate + 0.5);
}

/* Assert the standard soxr return contract: no error, all input frames
 * consumed, exactly the analytically expected number of output frames. */
static void check_accounting(char const *case_name, soxr_error_t err,
                             size_t used, size_t ilen,
                             size_t done, size_t want_done) {
  check_condition(err == NULL, case_name,
                  err ? soxr_strerror(err) : "unexpected soxr error");
  ++g_checks;
  if (used != ilen)
    fail(case_name, "input frames consumed used=%zu ilen=%zu", used, ilen);
  ++g_checks;
  if (done != want_done)
    fail(case_name, "output frames done=%zu want=%zu", done, want_done);
  ++g_checks;
  if (done == 0) error_fail(case_name, "empty output");
}

static void check_bad_ratio_returns_error(char const *case_name) {
  soxr_io_spec_t io = io_spec(0);
  soxr_quality_spec_t q = vhq_quality(50);
  soxr_runtime_spec_t rt = single_thread();
  soxr_error_t err = NULL;
  soxr_t bad = soxr_create(0.0, 44100.0, 1, &err, &io, &q, &rt);
  check_condition(bad == NULL && err != NULL, case_name,
                  "zero input rate was not rejected");
  if (bad) soxr_delete(bad);
}

/* --------------------------- analytic reference -------------------------- */

static void fill_tone(double *x, size_t n, double irate,
                      double freq, double amp) {
  for (size_t i = 0; i < n; ++i)
    x[i] = amp * sin(2.0 * M_PI * freq * (double)i / irate);
}

typedef struct { double amp, phase, freq; } tone_fit_t;

/* Least-squares fit of x[n] = b*sin(w n) + d*cos(w n), w = 2*pi*f0/rate.
 * The normal equations are solved directly; for an integer-cycle window the
 * two basis vectors are orthogonal and the fit reduces to the DFT bin, but
 * the general solution is kept so non-exact bins remain unbiased. */
static void basis_fit(double const *x, size_t w0, size_t n, double rate,
                      double f0, double *amp, double *phase) {
  double const w = 2.0 * M_PI * f0 / rate;
  double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
  for (size_t k = 0; k < n; ++k) {
    double const a = w * (double)(w0 + k);
    double const s = sin(a), c = cos(a), y = x[w0 + k];
    ss += s * s; cc += c * c; sc += s * c;
    ys += y * s; yc += y * c;
  }
  double const det = ss * cc - sc * sc;
  double const b = (ys * cc - yc * sc) / det;
  double const d = (yc * ss - ys * sc) / det;
  *amp = hypot(b, d);
  *phase = atan2(b, d);
}

/* Full-window amplitude plus a frequency estimate from the phase drift
 * between two half-window fits: f = f0 + (phase2-phase1)/(2*pi*dt). */
static tone_fit_t fit_tone(double const *x, size_t w0, size_t n,
                           double rate, double f0) {
  tone_fit_t r;
  basis_fit(x, w0, n, rate, f0, &r.amp, &r.phase);
  size_t const h = n / 2;
  double a1, p1, a2, p2;
  basis_fit(x, w0, h, rate, f0, &a1, &p1);
  basis_fit(x, w0 + n - h, h, rate, f0, &a2, &p2);
  double dp = p2 - p1;
  while (dp > M_PI) dp -= 2.0 * M_PI;
  while (dp < -M_PI) dp += 2.0 * M_PI;
  double const dt = (double)(n - h) / rate;       /* half-window centre gap */
  r.freq = f0 + dp / (2.0 * M_PI * dt);
  return r;
}

/* Complex-exponential projection H(f) = (2/n) sum x[k] exp(-j 2 pi f k/rate).
 * Independent of the amplitude fit above; used for pass-band magnitude. */
static void complex_project(double const *x, size_t n, double rate,
                            double freq, double *re, double *im) {
  double sr = 0, si = 0;
  for (size_t k = 0; k < n; ++k) {
    double const a = 2.0 * M_PI * freq * (double)k / rate;
    sr += x[k] * cos(a);
    si -= x[k] * sin(a);
  }
  *re = 2.0 * sr / (double)n;
  *im = 2.0 * si / (double)n;
}

/* Check a recovered tone against its analytic source: relative amplitude and
 * absolute frequency.  The 1 s window gives 1 Hz bin resolution; the
 * half-window phase estimator resolves well below one bin (~1e-12 Hz here). */
static void check_tone(char const *case_name, double const *x, size_t w0,
                       size_t n, double orate, double freq, double amp,
                       double amp_tol, double freq_tol) {
  char label[96];
  tone_fit_t f = fit_tone(x, w0, n, orate, freq);
  snprintf(label, sizeof(label), "f=%.0fHz amp rel err (exp %.3g)", freq, amp);
  check_numeric(fabs(f.amp - amp) / amp <= amp_tol, case_name,
                label, fabs(f.amp - amp) / amp, amp_tol);
  snprintf(label, sizeof(label), "f=%.0fHz frequency error Hz", freq);
  check_numeric(fabs(f.freq - freq) <= freq_tol, case_name,
                label, fabs(f.freq - freq), freq_tol);
}

/* --------------------------- stream conversion --------------------------- */

typedef struct {
  soxr_error_t err;
  size_t used, done;
} stream_result_t;

/* Irregular per-call capacities (in frames); deliberately not powers of two. */
static size_t const k_chunks[] = {1000, 333, 777, 4096, 1, 2500};

static stream_result_t stream_interleaved(double irate, double orate,
    unsigned channels, double const *in, size_t ilen,
    double *out, size_t outcap, double phase) {
  stream_result_t r = {NULL, 0, 0};
  soxr_io_spec_t io = io_spec(0);
  soxr_quality_spec_t q = vhq_quality(phase);
  soxr_runtime_spec_t rt = single_thread();
  soxr_error_t err = NULL;
  soxr_t s = soxr_create(irate, orate, channels, &err, &io, &q, &rt);
  if (!s) { r.err = err ? err : "creation failed"; return r; }
  size_t pos = 0, ci = 0;
  while (pos < ilen) {
    size_t want = k_chunks[ci++ % (sizeof(k_chunks) / sizeof(*k_chunks))];
    if (want > ilen - pos) want = ilen - pos;
    size_t idone = 0, odone = 0;
    err = soxr_process(s, (void *)(in + channels * pos), want, &idone,
                       out + channels * r.done, outcap - r.done, &odone);
    if (err) { r.err = err; soxr_delete(s); return r; }
    if (idone == 0 && odone == 0) {        /* no forward progress: stall */
      r.err = "soxr_process made no progress"; soxr_delete(s); return r;
    }
    pos += idone; r.used += idone; r.done += odone;
  }
  for (;;) {                                  /* drain the filter tail */
    size_t idone = 0, odone = 0;
    err = soxr_process(s, NULL, 0, &idone,
                       out + channels * r.done, outcap - r.done, &odone);
    if (err) { r.err = err; soxr_delete(s); return r; }
    r.done += odone;
    if (odone == 0) break;
  }
  soxr_delete(s);
  return r;
}

static stream_result_t stream_split(double irate, double orate,
    double const *const *in, size_t ilen,
    double *const *out, size_t outcap, double phase) {
  stream_result_t r = {NULL, 0, 0};
  soxr_io_spec_t io = io_spec(1);
  soxr_quality_spec_t q = vhq_quality(phase);
  soxr_runtime_spec_t rt = single_thread();
  soxr_error_t err = NULL;
  soxr_t s = soxr_create(irate, orate, 2, &err, &io, &q, &rt);
  if (!s) { r.err = err ? err : "creation failed"; return r; }
  size_t pos = 0, ci = 0;
  while (pos < ilen) {
    size_t want = k_chunks[ci++ % (sizeof(k_chunks) / sizeof(*k_chunks))];
    if (want > ilen - pos) want = ilen - pos;
    double const *src[2] = {in[0] + pos, in[1] + pos};
    double *dst[2] = {out[0] + r.done, out[1] + r.done};
    size_t idone = 0, odone = 0;
    err = soxr_process(s, (void *)src, want, &idone,
                       dst, outcap - r.done, &odone);
    if (err) { r.err = err; soxr_delete(s); return r; }
    if (idone == 0 && odone == 0) {        /* no forward progress: stall */
      r.err = "soxr_process made no progress"; soxr_delete(s); return r;
    }
    pos += idone; r.used += idone; r.done += odone;
  }
  for (;;) {                                  /* drain the filter tail */
    double *dst[2] = {out[0] + r.done, out[1] + r.done};
    size_t idone = 0, odone = 0;
    err = soxr_process(s, NULL, 0, &idone, dst, outcap - r.done, &odone);
    if (err) { r.err = err; soxr_delete(s); return r; }
    r.done += odone;
    if (odone == 0) break;
  }
  soxr_delete(s);
  return r;
}

/* ------------------------------ passband --------------------------------- */

/* One-shot, two channels carrying different frequencies/amplitudes. */
static void oneshot_two(char const *case_name, int split,
                        double irate, double orate,
                        double f0, double a0, double f1, double a1) {
  size_t const ilen = (size_t)(4.0 * irate);           /* 4 s of input */
  size_t const want = expected_frames(irate, orate, ilen);
  size_t const cap = want + 64;

  double *ch_in[2], *ch_out[2];
  double *inter_in = NULL, *inter_out = NULL;
  for (int c = 0; c < 2; ++c) {
    ch_in[c] = malloc(ilen * sizeof(double));
    ch_out[c] = calloc(cap, sizeof(double));
    if (!ch_in[c] || !ch_out[c]) error_fail(case_name, "allocation failed");
  }
  fill_tone(ch_in[0], ilen, irate, f0, a0);
  fill_tone(ch_in[1], ilen, irate, f1, a1);

  soxr_io_spec_t io = io_spec(split);
  soxr_quality_spec_t q = vhq_quality(50);
  size_t used = 0, done = 0;
  soxr_error_t err;

  if (split) {
    err = soxr_oneshot(irate, orate, 2, ch_in, ilen, &used,
                       ch_out, cap, &done, &io, &q, NULL);
  } else {
    inter_in = malloc(2 * ilen * sizeof(double));
    inter_out = calloc(2 * cap, sizeof(double));
    if (!inter_in || !inter_out) error_fail(case_name, "allocation failed");
    for (size_t i = 0; i < ilen; ++i) {
      inter_in[2 * i] = ch_in[0][i];
      inter_in[2 * i + 1] = ch_in[1][i];
    }
    err = soxr_oneshot(irate, orate, 2, inter_in, ilen, &used,
                       inter_out, cap, &done, &io, &q, NULL);
    for (size_t i = 0; i < done; ++i) {
      ch_out[0][i] = inter_out[2 * i];
      ch_out[1][i] = inter_out[2 * i + 1];
    }
  }
  check_accounting(case_name, err, used, ilen, done, want);

  /* Middle 1 s of output: skips both head and tail transients.  Every tone
   * frequency is an integer number of Hz so the window holds whole cycles. */
  size_t const n = (size_t)orate;
  size_t const w0 = (done - n) / 2;
  check_tone(case_name, ch_out[0], w0, n, orate, f0, a0, 1e-5, 1e-3);
  check_tone(case_name, ch_out[1], w0, n, orate, f1, a1, 1e-5, 1e-3);

  for (int c = 0; c < 2; ++c) { free(ch_in[c]); free(ch_out[c]); }
  free(inter_in); free(inter_out);
}

static void streaming_two(char const *case_name, int split,
                          double irate, double orate,
                          double f0, double a0, double f1, double a1) {
  size_t const ilen = (size_t)(2.0 * irate);           /* 2 s of input */
  size_t const want = expected_frames(irate, orate, ilen);
  size_t const cap = want + 64;
  double *in[2], *out[2];
  for (int c = 0; c < 2; ++c) {
    in[c] = malloc(ilen * sizeof(double));
    out[c] = calloc(cap, sizeof(double));
    if (!in[c] || !out[c]) error_fail(case_name, "allocation failed");
  }
  fill_tone(in[0], ilen, irate, f0, a0);
  fill_tone(in[1], ilen, irate, f1, a1);

  stream_result_t r;
  if (split) {
    r = stream_split(irate, orate,
        (double const *const *)in, ilen, out, cap, 50);
  } else {
    double *xi = malloc(2 * ilen * sizeof(double));
    double *xo = calloc(2 * cap, sizeof(double));
    if (!xi || !xo) error_fail(case_name, "allocation failed");
    for (size_t i = 0; i < ilen; ++i) {
      xi[2 * i] = in[0][i]; xi[2 * i + 1] = in[1][i];
    }
    r = stream_interleaved(irate, orate, 2, xi, ilen, xo, cap, 50);
    for (size_t i = 0; i < r.done; ++i) {
      out[0][i] = xo[2 * i]; out[1][i] = xo[2 * i + 1];
    }
    free(xi); free(xo);
  }
  check_accounting(case_name, r.err, r.used, ilen, r.done, want);

  size_t const n = (size_t)orate;
  size_t const w0 = (r.done - n) / 2;
  check_tone(case_name, out[0], w0, n, orate, f0, a0, 1e-5, 1e-3);
  check_tone(case_name, out[1], w0, n, orate, f1, a1, 1e-5, 1e-3);

  for (int c = 0; c < 2; ++c) { free(in[c]); free(out[c]); }
}

static int run_passband(void) {
  /* 48 kHz -> 32 kHz down-sampling, both channel layouts. */
  oneshot_two("down-interleaved", 0, 48000, 32000, 1000, 0.70, 6000, 0.40);
  oneshot_two("down-split", 1, 48000, 32000, 10000, 0.50, 1000, 0.60);
  /* 32 kHz -> 96 kHz up-sampling, both channel layouts. */
  oneshot_two("up-interleaved", 0, 32000, 96000, 1000, 0.55, 10000, 0.45);
  oneshot_two("up-split", 1, 32000, 96000, 6000, 0.65, 10000, 0.35);
  /* Irregular chunked streaming with explicit tail drain. */
  streaming_two("down-stream-interleaved", 0, 48000, 32000,
                1000, 0.70, 6000, 0.40);
  streaming_two("up-stream-split", 1, 32000, 96000,
                6000, 0.65, 10000, 0.35);
  check_bad_ratio_returns_error("bad-ratio");
  printf("PASS passband checks=%ld\n", g_checks);
  return 0;
}

/* -------------------------------- alias ---------------------------------- */

static double *oneshot_mono(char const *case_name, double irate, double orate,
                            double const *in, size_t ilen,
                            size_t *used, size_t *done) {
  size_t const want = expected_frames(irate, orate, ilen);
  size_t const cap = want + 64;
  double *out = calloc(cap, sizeof(double));
  if (!out) error_fail(case_name, "allocation failed");
  soxr_io_spec_t io = io_spec(0);
  soxr_quality_spec_t q = vhq_quality(50);
  soxr_error_t err = soxr_oneshot(irate, orate, 1, in, ilen, used,
                                  out, cap, done, &io, &q, NULL);
  check_accounting(case_name, err, *used, ilen, *done, want);
  return out;
}

static void check_residual(char const *case_name, double const *out,
                           size_t done, double orate,
                           double alias_freq, double unit_amp) {
  char label[96];
  size_t const n = (size_t)orate;               /* 1 s, integer-cycle window */
  size_t const w0 = (done - n) / 2;
  tone_fit_t f = fit_tone(out, w0, n, orate, alias_freq);
  double const rel = f.amp / unit_amp;
  double const db = 20.0 * log10(rel + 1e-300);
  double const rel_limit = pow(10.0, -75.0 / 20.0);   /* -75 dB */
  snprintf(label, sizeof(label), "f=%.0fHz alias amp=%.3g dB vs -75",
           alias_freq, f.amp);
  check_numeric(rel <= rel_limit, case_name, label, db, -75.0);
  snprintf(label, sizeof(label), "f=%.0fHz alias frequency error Hz",
           alias_freq);
  check_numeric(fabs(f.freq - alias_freq) <= 1e-3, case_name,
                label, fabs(f.freq - alias_freq), 1e-3);
}

static int run_alias(void) {
  /* Down-sampling: 18 kHz at 48 kHz folds about the 16 kHz output Nyquist:
   * f_alias = 2*16000 - 18000 = 14000 Hz. */
  {
    char const *cn = "down-18k-folds-to-14k";
    size_t const ilen = 4 * 48000;
    double *in = malloc(ilen * sizeof(double));
    if (!in) error_fail(cn, "allocation failed");
    fill_tone(in, ilen, 48000, 18000, 1.0);
    size_t used, done;
    double *out = oneshot_mono(cn, 48000, 32000, in, ilen, &used, &done);
    double const f_alias = 2.0 * (32000.0 / 2.0) - 18000.0;  /* 14000 Hz */
    check_condition(fabs(f_alias - 14000.0) < 1e-9, cn,
                    "alias folding formula");
    check_residual(cn, out, done, 32000, f_alias, 1.0);
    /* The 18 kHz component itself must not appear in the pass-band. */
    tone_fit_t leak = fit_tone(out, (done - 32000) / 2, 32000, 32000, 18000);
    check_numeric(leak.amp <= pow(10.0, -75.0 / 20.0), cn,
                  "18kHz pass-band leak amplitude", leak.amp,
                  pow(10.0, -75.0 / 20.0));
    free(in); free(out);
  }

  /* Up-sampling: 10 kHz at 32 kHz images about the 32 kHz input sampling
   * frequency: f_img1 = 32000-10000 = 22000 Hz, f_img2 = 32000+10000. */
  {
    char const *cn = "up-10k-images-at-22k-and-42k";
    size_t const ilen = 4 * 32000;
    double *in = malloc(ilen * sizeof(double));
    if (!in) error_fail(cn, "allocation failed");
    fill_tone(in, ilen, 32000, 10000, 1.0);
    size_t used, done;
    double *out = oneshot_mono(cn, 32000, 96000, in, ilen, &used, &done);
    double const img1 = 32000.0 - 10000.0;            /* 22000 Hz */
    double const img2 = 32000.0 + 10000.0;            /* 42000 Hz */
    check_condition(fabs(img1 - 22000.0) < 1e-9 &&
                    fabs(img2 - 42000.0) < 1e-9, cn,
                    "image folding formula");
    check_residual(cn, out, done, 96000, img1, 1.0);
    check_residual(cn, out, done, 96000, img2, 1.0);
    /* The wanted 10 kHz tone survives with its unit amplitude. */
    check_tone(cn, out, (done - 96000) / 2, 96000, 96000, 10000, 1.0,
               1e-5, 1e-3);
    free(in); free(out);
  }

  /* Pass-band survival on the down-sampling path as well. */
  {
    char const *cn = "down-10k-survives";
    size_t const ilen = 4 * 48000;
    double *in = malloc(ilen * sizeof(double));
    if (!in) error_fail(cn, "allocation failed");
    fill_tone(in, ilen, 48000, 10000, 1.0);
    size_t used, done;
    double *out = oneshot_mono(cn, 48000, 32000, in, ilen, &used, &done);
    check_tone(cn, out, (done - 32000) / 2, 32000, 32000, 10000, 1.0,
               1e-5, 1e-3);
    free(in); free(out);
  }

  /* Boundary: DC must pass at unity with negligible ripple. */
  {
    char const *cn = "dc-boundary";
    size_t const ilen = 4 * 48000;
    double *in = malloc(ilen * sizeof(double));
    double *out;
    if (!in) error_fail(cn, "allocation failed");
    for (size_t i = 0; i < ilen; ++i) in[i] = 0.5;
    size_t used, done;
    out = oneshot_mono(cn, 48000, 32000, in, ilen, &used, &done);
    size_t const n = 32000, w0 = (done - n) / 2;
    double mean = 0, resid = 0;
    for (size_t k = 0; k < n; ++k) {
      mean += out[w0 + k];
      double d = out[w0 + k] - 0.5;
      resid += d * d;
    }
    mean /= (double)n;
    double rms = sqrt(resid / (double)n);
    check_numeric(fabs(mean - 0.5) / 0.5 <= 1e-9, cn,
                  "DC gain relative error", fabs(mean - 0.5) / 0.5, 1e-9);
    check_numeric(rms <= 1e-9, cn, "DC ripple RMS", rms, 1e-9);
    free(in); free(out);
  }

  /* Boundary: zero input must give exactly zero output, full frame count. */
  {
    char const *cn = "zero-boundary";
    size_t const ilen = 4 * 48000;
    double *in = calloc(ilen, sizeof(double));
    if (!in) error_fail(cn, "allocation failed");
    size_t used, done;
    double *out = oneshot_mono(cn, 48000, 32000, in, ilen, &used, &done);
    double maxabs = 0;
    for (size_t i = 0; i < done; ++i)
      if (fabs(out[i]) > maxabs) maxabs = fabs(out[i]);
    check_numeric(maxabs == 0.0, cn, "zero-input max amplitude", maxabs, 0.0);
    free(in); free(out);
  }

  check_bad_ratio_returns_error("bad-ratio");
  printf("PASS alias checks=%ld\n", g_checks);
  return 0;
}

/* -------------------------------- phase ---------------------------------- */

typedef struct {
  double energy, centroid_offset, tail_fraction, symmetry;
  double magnitude_db[3];
} impulse_metrics_t;

static impulse_metrics_t measure_impulse(char const *case_name,
                                         double phase_response) {
  size_t const ilen = 48000, cap = 46000;
  double *in = calloc(ilen, sizeof(double));
  double *out = calloc(cap, sizeof(double));
  if (!in || !out) error_fail(case_name, "allocation failed");
  in[24000] = 1.0;                          /* unit impulse at frame 24000 */

  soxr_io_spec_t io = io_spec(0);
  soxr_quality_spec_t q = vhq_quality(phase_response);
  size_t used = 0, done = 0;
  soxr_error_t err = soxr_oneshot(48000, 44100, 1, in, ilen, &used,
                                  out, cap, &done, &io, &q, NULL);
  check_accounting(case_name, err, used, ilen, done, 44100);

  impulse_metrics_t m;
  /* Energy and centroid use every valid output frame, tails included. */
  double energy = 0, moment = 0, peak = 0;
  for (size_t i = 0; i < done; ++i) {
    energy += out[i] * out[i];
    moment += (double)i * out[i] * out[i];
    if (fabs(out[i]) > peak) peak = fabs(out[i]);
  }
  m.energy = energy;
  m.centroid_offset = moment / energy - 22050.0;

  /* Support window around the response; report the energy left outside it
   * as evidence that truncating the window loses negligible energy. */
  double const thr = 1e-7 * peak;
  size_t lo = 0, hi = done - 1;
  while (lo < hi && fabs(out[lo]) < thr) ++lo;
  while (hi > lo && fabs(out[hi]) < thr) --hi;
  double win_energy = 0;
  for (size_t i = lo; i <= hi; ++i) win_energy += out[i] * out[i];
  m.tail_fraction = (energy - win_energy) / energy;

  /* Linear-phase symmetry about output frame 22050. */
  double sym = 0;
  for (size_t k = 1; k < 22050; ++k) {
    double d = out[22050 - k] - out[22050 + k];
    sym += d * d;
  }
  m.symmetry = sqrt(sym / energy);

  /* Pass-band magnitude from independent complex-exponential projections. */
  static double const freqs[3] = {1000, 6000, 10000};
  for (int k = 0; k < 3; ++k) {
    double re, im;
    complex_project(out, done, 44100, freqs[k], &re, &im);
    m.magnitude_db[k] = 20.0 * log10(hypot(re, im));
  }
  free(in); free(out);
  return m;
}

static int run_phase(void) {
  impulse_metrics_t mn = measure_impulse("minimum-phase", 0);
  impulse_metrics_t li = measure_impulse("linear-phase", 50);
  impulse_metrics_t mx = measure_impulse("maximum-phase", 100);

  /* Energy centroid relative to the linear-phase centre, frame 22050. */
  check_numeric(fabs(li.centroid_offset) < 1e-3, "linear-phase",
                "centroid offset frames", li.centroid_offset, 1e-3);
  check_numeric(mn.centroid_offset > 1.0, "minimum-phase",
                "centroid offset frames", mn.centroid_offset, 1.0);
  check_numeric(mx.centroid_offset < -1.0, "maximum-phase",
                "centroid offset frames", mx.centroid_offset, -1.0);

  /* The three designs carry the same energy. */
  double emin = fmin(fmin(mn.energy, li.energy), mx.energy);
  double emax = fmax(fmax(mn.energy, li.energy), mx.energy);
  check_numeric((emax - emin) / emin < 1e-5, "energy-match",
                "relative energy spread", (emax - emin) / emin, 1e-5);

  /* Linear-phase impulse is symmetric about frame 22050. */
  check_numeric(li.symmetry < 1e-9, "linear-phase",
                "symmetry residual", li.symmetry, 1e-9);

  /* Truncation evidence: the 1e-7 support window holds essentially all
   * energy for every phase design. */
  check_numeric(mn.tail_fraction < 1e-9, "minimum-phase",
                "window tail energy fraction", mn.tail_fraction, 1e-9);
  check_numeric(li.tail_fraction < 1e-9, "linear-phase",
                "window tail energy fraction", li.tail_fraction, 1e-9);
  check_numeric(mx.tail_fraction < 1e-9, "maximum-phase",
                "window tail energy fraction", mx.tail_fraction, 1e-9);

  /* Pass-band magnitudes agree across phase designs within 0.02 dB. */
  for (int k = 0; k < 3; ++k) {
    double lo = fmin(fmin(mn.magnitude_db[k], li.magnitude_db[k]),
                     mx.magnitude_db[k]);
    double hi = fmax(fmax(mn.magnitude_db[k], li.magnitude_db[k]),
                     mx.magnitude_db[k]);
    check_numeric(hi - lo < 0.02, "passband-magnitude",
                  "magnitude spread dB", hi - lo, 0.02);
  }

  check_bad_ratio_returns_error("bad-ratio");
  printf("PASS phase checks=%ld\n", g_checks);
  return 0;
}

/* --------------------------------- main ---------------------------------- */

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s passband|alias|phase\n", argv[0]);
    return 2;
  }
  g_group = argv[1];
  if (strcmp(g_group, "passband") == 0) return run_passband();
  if (strcmp(g_group, "alias") == 0) return run_alias();
  if (strcmp(g_group, "phase") == 0) return run_phase();
  fprintf(stderr, "unknown group: %s\n", g_group);
  return 2;
}
