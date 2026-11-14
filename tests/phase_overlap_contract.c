/*
  FILE........: tests/phase_overlap_contract.c
  PURPOSE.....: Contract tests for the phase-continuity and overlap-add
                numerics of the Codec 2 voiced synthesis chain, written
                ahead of swapping the FFT backend.  All expected values
                are produced by an independent reference model (direct
                inverse-DFT summation, own complex arithmetic, own
                piecewise-linear window); the production FFT is never
                used to generate expectations.  See PHASE_TESTS.md.

  The file exports task_cases/task_case_count for task_support/driver.c;
  it does not define main.  Every comparison goes through TASK_CHECK and
  each test function returns 0 on success.
*/

#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "codec2.h"
#include "codec2_fft.h"
#include "comp.h"
#include "defines.h"
#include "phase.h"
#include "sine.h"

/* ------------------------------------------------------------------ *
 *  Contract tolerances (PHASE_TESTS.md "误差上限")                    *
 * ------------------------------------------------------------------ */

#define PHASE_TOL 2e-5   /* unit-circle distance for phase comparisons   */
#define SAMP_ABS_TOL 2e-3 /* pointwise tolerance on synthesis buffers:  */
#define SAMP_REL_TOL 2e-5 /*   |got-want| <= 2e-3 + 2e-5*|want|         */
#define WIN_TOL 1e-4      /* synthesis window vs piecewise-linear def   */

#define PI_REF 3.14159265358979323846
#define MAX_NS 160 /* largest n_samp (16 kHz config) */
#define REF_LEN 8192
#define MAX_REC 4

static int mismatch_reported;

static void content_mismatch(const char *what, long frame, long index,
                             double got, double want, double tol) {
  if (mismatch_reported) return;
  mismatch_reported = 1;
  printf("CONTENT_MISMATCH %s frame=%ld index=%ld got=%.9g want=%.9g tol=%.9g\n",
         what, frame, index, got, want, tol);
}

/* Distance between two phases measured on the unit circle. */
static double circ_dist(double a, double b) {
  double dc = cos(a) - cos(b);
  double ds = sin(a) - sin(b);
  return sqrt(dc * dc + ds * ds);
}

static int check_phase(const char *what, long frame, long index, double got,
                       double want) {
  double d = circ_dist(got, want);
  if (d > PHASE_TOL) content_mismatch(what, frame, index, got, want, PHASE_TOL);
  TASK_CHECK(d <= PHASE_TOL);
  return 0;
}

static int check_sample(const char *what, long frame, long index, double got,
                        double want) {
  double tol = SAMP_ABS_TOL + SAMP_REL_TOL * fabs(want);
  double d = fabs(got - want);
  if (d > tol) content_mismatch(what, frame, index, got, want, tol);
  TASK_CHECK(d <= tol);
  return 0;
}

static int check_window_pt(const char *what, long frame, long index,
                           double got, double want) {
  double d = fabs(got - want);
  if (d > WIN_TOL) content_mismatch(what, frame, index, got, want, WIN_TOL);
  TASK_CHECK(d <= WIN_TOL);
  return 0;
}

/* ------------------------------------------------------------------ *
 *  Own complex arithmetic (float, single precision like production)  *
 * ------------------------------------------------------------------ */

typedef struct {
  float re, im;
} fcpx;

static fcpx fc_conj(fcpx a) {
  fcpx r = {a.re, -a.im};
  return r;
}

static fcpx fc_mul(fcpx a, fcpx b) {
  fcpx r = {a.re * b.re - a.im * b.im, a.im * b.re + a.re * b.im};
  return r;
}

static fcpx fc_from_comp(const COMP *c) {
  fcpx r = {c->real, c->imag};
  return r;
}

/* Fundamental clock: advance by Wo*n_samp and wrap back into one
   period, exactly the rule the contract pins down.  Sets *wrapped when
   the pre-wrap value crosses the +/-pi line. */
static float ref_clock_advance(float ex_phase, float Wo, int n_samp,
                               int *wrapped) {
  float pre = ex_phase + Wo * n_samp;
  float k = floorf(pre / TWO_PI + 0.5);
  if (wrapped && k != 0.0f) *wrapped = 1;
  return pre - TWO_PI * k;
}

/* Excitation phase of harmonic m from the fundamental clock. */
static fcpx ref_excitation(float ex_phase, int m) {
  fcpx e = {cosf(ex_phase * m), sinf(ex_phase * m)};
  return e;
}

/* Harmonic phase after filtering the excitation by H. */
static float ref_harm_phase(fcpx H, float ex_phase, int m) {
  fcpx A = fc_mul(H, ref_excitation(ex_phase, m));
  return atan2f(A.im, A.re + 1E-12);
}

/* Nearest analysis bin of harmonic m (encoder FFT grid). */
static int ref_enc_bin(float Wo, int m) {
  float r = TWO_PI / (FFT_ENC);
  return (int)(m * Wo / r + 0.5);
}

/* Nearest synthesis bin of harmonic l (decoder FFT grid), clamped to
   the highest usable bin. */
static int ref_dec_bin(float Wo, int l) {
  int b = (int)(l * Wo * FFT_DEC / TWO_PI + 0.5);
  if (b > (FFT_DEC / 2) - 1) b = (FFT_DEC / 2) - 1;
  return b;
}

/* Trapezoidal synthesis window from its piecewise-linear definition
   (computed independently, not derived from the production window). */
static double ref_window(int i, int n_samp, int tw) {
  int rise0 = n_samp / 2 - tw, rise1 = n_samp / 2 + tw;
  int fall0 = 3 * n_samp / 2 - tw, fall1 = 3 * n_samp / 2 + tw;
  if (i < rise0 || i >= fall1) return 0.0;
  if (i < rise1) return (double)(i - rise0) / (2.0 * tw);
  if (i < fall0) return 1.0;
  return 1.0 - (double)(i - fall0) / (2.0 * tw);
}

/* Direct inverse-DFT summation for one synthesis frame.  Handles the
   conjugate (negative-frequency) images of the real signal and the
   unity gain of the current KISS FFT inverse.  sw[j] is the sample at
   natural time n = j - (n_samp - 1), j in [0, 2*n_samp). */
static void ref_idft_segment(const MODEL *model, int n_samp, double sw[]) {
  int j, l;
  for (j = 0; j < 2 * n_samp; j++) {
    int n = j - (n_samp - 1);
    double acc = 0.0;
    for (l = 1; l <= model->L; l++) {
      int b = ref_dec_bin(model->Wo, l);
      acc += 2.0 * (double)model->A[l] *
             cos((double)model->phi[l] +
                 2.0 * PI_REF * (double)b * (double)n / FFT_DEC);
    }
    sw[j] = acc;
  }
}

/* Overlap reference on a natural (absolute) time axis: windowed
   segments are stored at their absolute position and consecutive
   contributions merge there. */
typedef struct {
  int n_samp;
  int tw;
  long T;              /* absolute time of the current frame start */
  double acc[REF_LEN]; /* absolute-time overlap accumulator        */
  double win[2 * MAX_NS];
} ovl_ref;

static void ovl_ref_init(ovl_ref *o, int n_samp, int tw) {
  int i;
  o->n_samp = n_samp;
  o->tw = tw;
  o->T = 0;
  memset(o->acc, 0, sizeof(o->acc));
  for (i = 0; i < 2 * n_samp; i++) o->win[i] = ref_window(i, n_samp, tw);
}

static void ovl_ref_frame(ovl_ref *o, const MODEL *model, int shift) {
  double sw[2 * MAX_NS];
  int i;
  if (shift) {
    o->T += o->n_samp;
    /* the junction sample at the frame boundary starts from zero:
       the previous frame's last tail sample is not carried over */
    o->acc[o->T + o->n_samp - 1] = 0.0;
  }
  ref_idft_segment(model, o->n_samp, sw);
  for (i = 0; i < 2 * o->n_samp; i++)
    o->acc[o->T + i] += sw[i] * o->win[i];
}

/* Deterministic test material -------------------------------------- */

static void gen_analysis_response(COMP Aw[]) {
  int k;
  for (k = 0; k < FFT_ENC; k++) {
    Aw[k].real = 0.9f + 0.4f * cosf(0.110f * k);
    Aw[k].imag = 0.6f * sinf(0.053f * k) + 0.25f * cosf(0.021f * k);
  }
}

static void gen_model(MODEL *m, float Wo, int voiced, long f, double amp) {
  int l;
  memset(m, 0, sizeof(*m));
  m->Wo = Wo;
  m->L = (int)(PI / Wo);
  if (m->L > MAX_AMP) m->L = MAX_AMP;
  m->voiced = voiced;
  for (l = 1; l <= m->L; l++) {
    m->A[l] = (float)(amp / (1.0 + 0.3 * l));
    m->phi[l] = (float)(fmod(0.7 * l + 0.31 * f, 2.0 * PI_REF) - PI_REF);
  }
}

/* ------------------------------------------------------------------ *
 *  group "phase"                                                      *
 * ------------------------------------------------------------------ */

/* sample_phase: conjugate of the LPC analysis response at the nearest
   bin of each harmonic, with a non-real filter response. */
static int case_phase_sample_bins(void) {
  COMP Aw[FFT_ENC];
  COMP H[MAX_AMP + 1];
  static const float wos[] = {0.05f, 0.123f, 0.2f, 0.314f};
  int nonreal = 0;
  int w;
  gen_analysis_response(Aw);
  for (w = 0; w < 4; w++) {
    MODEL m;
    int l;
    memset(&m, 0, sizeof(m));
    m.Wo = wos[w];
    m.L = (int)(PI / m.Wo);
    m.voiced = 1;
    sample_phase(&m, H, Aw);
    for (l = 1; l <= m.L; l++) {
      int b = ref_enc_bin(m.Wo, l);
      fcpx want = fc_conj(fc_from_comp(&Aw[b]));
      if (Aw[b].imag != 0.0f) nonreal++;
      TASK_CHECK(H[l].real == want.re);
      TASK_CHECK(H[l].imag == want.im);
    }
  }
  /* the filter response really is non-real at the sampled bins */
  TASK_CHECK(nonreal > 0);
  return 0;
}

/* Voiced phase track over many frames: fundamental clock update,
   wrap across +/-pi, changing Wo (hence changing L), per-harmonic
   numerical equality with sample_phase + phase_synth_zero_order. */
static int case_phase_voiced_track(void) {
  COMP Aw[FFT_ENC];
  static const int nsamps[2] = {80, 160};
  int cfg;
  gen_analysis_response(Aw);
  for (cfg = 0; cfg < 2; cfg++) {
    int n_samp = nsamps[cfg];
    float prod_ex = 2.9f; /* start just below +pi so the clock wraps */
    float ref_ex = 2.9f;
    int wrapped = 0;
    int Lseen[64];
    int nL = 0;
    int f;
    memset(Lseen, 0, sizeof(Lseen));
    for (f = 0; f < 24; f++) {
      float Wo = 0.06f + 0.012f * (f % 6) + 0.0008f * f;
      MODEL m;
      COMP H[MAX_AMP + 1];
      int l, i, found;
      memset(&m, 0, sizeof(m));
      m.Wo = Wo;
      m.L = (int)(PI / Wo);
      m.voiced = 1;

      sample_phase(&m, H, Aw);
      phase_synth_zero_order(n_samp, &m, &prod_ex, H);

      ref_ex = ref_clock_advance(ref_ex, Wo, n_samp, &wrapped);

      /* fundamental clock */
      if (check_phase("phase-clock", f, 0, prod_ex, ref_ex)) return 1;

      /* per-harmonic excitation+filter phase */
      for (l = 1; l <= m.L; l++) {
        int b = ref_enc_bin(Wo, l);
        fcpx h = fc_conj(fc_from_comp(&Aw[b]));
        float want = ref_harm_phase(h, ref_ex, l);
        if (check_phase("phase-harmonic", f, l, m.phi[l], want)) return 1;
      }

      found = 0;
      for (i = 0; i < nL; i++)
        if (Lseen[i] == m.L) found = 1;
      if (!found && nL < 64) Lseen[nL++] = m.L;
    }
    /* the run really crossed the +/-pi line and used several L */
    TASK_CHECK(wrapped);
    TASK_CHECK(nL >= 3);
  }
  return 0;
}

/* Unvoiced frames: random excitation phases are not pinned to the
   voiced trajectory; only finiteness and the fundamental clock are
   checked.  The global codec2_rand is left alone. */
static int case_phase_unvoiced_clock(void) {
  COMP Aw[FFT_ENC];
  COMP H[MAX_AMP + 1];
  float prod_ex = 1.0f;
  float ref_ex = 1.0f;
  int f;
  gen_analysis_response(Aw);
  for (f = 0; f < 8; f++) {
    float Wo = 0.11f + 0.01f * (f % 4);
    MODEL m;
    int l;
    memset(&m, 0, sizeof(m));
    m.Wo = Wo;
    m.L = (int)(PI / Wo);
    m.voiced = 0;
    sample_phase(&m, H, Aw);
    phase_synth_zero_order(80, &m, &prod_ex, H);
    ref_ex = ref_clock_advance(ref_ex, Wo, 80, NULL);
    /* fundamental clock still advances by the same rule */
    if (check_phase("phase-uv-clock", f, 0, prod_ex, ref_ex)) return 1;
    /* random phases: only require finite values in [-pi, pi] */
    for (l = 1; l <= m.L; l++) {
      TASK_CHECK(isfinite(m.phi[l]));
      TASK_CHECK(fabsf(m.phi[l]) <= (float)PI + 1e-4f);
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ *
 *  group "overlap"                                                    *
 * ------------------------------------------------------------------ */

typedef struct {
  int n_samp;
  int tw;
  codec2_fftr_cfg cfg;
  float Pn[2 * MAX_NS];  /* production window (artifact under test) */
  float Sn_[2 * MAX_NS]; /* production overlap buffer               */
  ovl_ref ref;
  long frames;
} ovl_rig;

static void ovl_rig_init(ovl_rig *r, int Fs) {
  C2CONST c2const = c2const_create(Fs, N_S);
  r->n_samp = c2const.n_samp;
  r->tw = c2const.tw;
  r->cfg = codec2_fftr_alloc(FFT_DEC, 1, NULL, NULL);
  make_synthesis_window(&c2const, r->Pn);
  memset(r->Sn_, 0, sizeof(r->Sn_));
  ovl_ref_init(&r->ref, r->n_samp, r->tw);
  r->frames = 0;
}

/* Run one production synthesise() frame and check every valid output
   and the retained tail (all 2*n_samp entries) against the independent
   natural-time-axis reference. */
static int ovl_run_frame(ovl_rig *r, const MODEL *model, int shift) {
  MODEL m = *model;
  int i;
  long T;
  synthesise(r->n_samp, r->cfg, r->Sn_, &m, r->Pn, shift);
  ovl_ref_frame(&r->ref, model, shift);
  T = r->ref.T;
  for (i = 0; i < 2 * r->n_samp; i++) {
    if (check_sample("overlap", r->frames, i, r->Sn_[i], r->ref.acc[T + i]))
      return 1;
  }
  r->frames++;
  return 0;
}

/* Explicit check of the junction position n_samp-1. */
static int ovl_check_junction(ovl_rig *r) {
  long T = r->ref.T;
  return check_sample("overlap-junction", r->frames - 1, r->n_samp - 1,
                      r->Sn_[r->n_samp - 1], r->ref.acc[T + r->n_samp - 1]);
}

/* make_synthesis_window vs the independent piecewise-linear definition,
   8 kHz and 16 kHz configurations. */
static int case_overlap_window_def(void) {
  static const int fss[2] = {8000, 16000};
  int c, i;
  for (c = 0; c < 2; c++) {
    C2CONST c2const = c2const_create(fss[c], N_S);
    float Pn[2 * MAX_NS];
    make_synthesis_window(&c2const, Pn);
    for (i = 0; i < 2 * c2const.n_samp; i++) {
      if (check_window_pt("window", c, i, Pn[i],
                          ref_window(i, c2const.n_samp, c2const.tw)))
        return 1;
    }
  }
  return 0;
}

/* shift=0 overlay and shift=1 advance, adjacent frames changing pitch
   and amplitudes, 8 kHz config. */
static int case_overlap_shift_modes_8k(void) {
  static ovl_rig r;
  MODEL m;
  int f;
  ovl_rig_init(&r, 8000);
  TASK_CHECK(r.cfg != NULL);
  /* two overlays without advancing the time axis */
  gen_model(&m, 0.12f, 1, 0, 100.0);
  if (ovl_run_frame(&r, &m, 0)) return 1;
  gen_model(&m, 0.12f, 1, 1, 100.0);
  if (ovl_run_frame(&r, &m, 0)) return 1;
  /* advancing frames; pitch and amplitudes change between neighbours */
  for (f = 2; f < 8; f++) {
    float Wo = (f % 2) ? 0.12f : 0.17f;
    double amp = (f % 2) ? 100.0 : 150.0;
    gen_model(&m, Wo, 1, f, amp);
    if (ovl_run_frame(&r, &m, 1)) return 1;
  }
  codec2_fftr_free(r.cfg);
  return 0;
}

/* Same contract under the 16 kHz configuration (n_samp=160). */
static int case_overlap_shift_modes_16k(void) {
  static ovl_rig r;
  MODEL m;
  int f;
  ovl_rig_init(&r, 16000);
  TASK_CHECK(r.cfg != NULL);
  gen_model(&m, 0.10f, 1, 0, 90.0);
  if (ovl_run_frame(&r, &m, 0)) return 1;
  for (f = 1; f < 7; f++) {
    float Wo = (f % 2) ? 0.10f : 0.15f;
    double amp = (f % 2) ? 90.0 : 140.0;
    gen_model(&m, Wo, 1, f, amp);
    if (ovl_run_frame(&r, &m, 1)) return 1;
  }
  codec2_fftr_free(r.cfg);
  return 0;
}

/* Silence flushes the tail: after the source goes quiet the retained
   contributions drain to zero. */
static int case_overlap_silence_tail(void) {
  static ovl_rig r;
  MODEL m;
  int f, i;
  float maxabs = 0.0f;
  ovl_rig_init(&r, 8000);
  TASK_CHECK(r.cfg != NULL);
  for (f = 0; f < 4; f++) {
    gen_model(&m, 0.13f, 1, f, 120.0);
    if (ovl_run_frame(&r, &m, 1)) return 1;
  }
  /* zero-amplitude frames flush the tail */
  for (f = 4; f < 7; f++) {
    gen_model(&m, 0.13f, 1, f, 0.0);
    if (ovl_run_frame(&r, &m, 1)) return 1;
  }
  for (i = 0; i < r.n_samp; i++)
    if (fabsf(r.Sn_[i]) > maxabs) maxabs = fabsf(r.Sn_[i]);
  TASK_CHECK(maxabs <= 1e-6f);
  codec2_fftr_free(r.cfg);
  return 0;
}

/* Last harmonic at the highest usable synthesis bin, including the
   clamping path when the nearest-bin rule lands above it. */
static int case_overlap_top_bin(void) {
  static ovl_rig r;
  MODEL m;
  int f;
  ovl_rig_init(&r, 8000);
  TASK_CHECK(r.cfg != NULL);
  /* Wo=0.314: raw bin of harmonic 10 rounds to 256 and clamps to 255 */
  TASK_CHECK((int)(10 * 0.314f * FFT_DEC / TWO_PI + 0.5) == 256);
  TASK_CHECK(ref_dec_bin(0.314f, 10) == (FFT_DEC / 2) - 1);
  /* Wo=0.3135: harmonic 10 sits exactly on bin 255 without clamping */
  TASK_CHECK(ref_dec_bin(0.3135f, 10) == (FFT_DEC / 2) - 1);
  TASK_CHECK((int)(10 * 0.3135f * FFT_DEC / TWO_PI + 0.5) == 255);
  for (f = 0; f < 5; f++) {
    float Wo = (f % 2) ? 0.314f : 0.3135f;
    gen_model(&m, Wo, 1, f, 110.0);
    if (ovl_run_frame(&r, &m, 1)) return 1;
  }
  codec2_fftr_free(r.cfg);
  return 0;
}

/* Eight consecutive advancing frames; the junction position n_samp-1
   is checked explicitly on every frame (in addition to the full sweep
   inside ovl_run_frame). */
static int case_overlap_junction(void) {
  static ovl_rig r;
  MODEL m;
  int f;
  ovl_rig_init(&r, 8000);
  TASK_CHECK(r.cfg != NULL);
  for (f = 0; f < 8; f++) {
    float Wo = 0.09f + 0.011f * (f % 4);
    gen_model(&m, Wo, 1, f, 100.0 + 30.0 * (f % 3));
    if (ovl_run_frame(&r, &m, 1)) return 1;
    if (ovl_check_junction(&r)) return 1;
  }
  TASK_CHECK(r.frames >= 6);
  codec2_fftr_free(r.cfg);
  return 0;
}

/* ------------------------------------------------------------------ *
 *  group "stream"                                                     *
 * ------------------------------------------------------------------ */

/* Per-frame record captured through the TASK_OBSERVE points. */
typedef struct {
  MODEL pre;             /* model after phase synthesis, pre-postfilter */
  COMP Aw[FFT_ENC];      /* LPC analysis response (phase update input)  */
  float ex_pre;          /* fundamental clock before the phase update   */
  float ex_post;         /* fundamental clock after the phase update    */
  float Pn[2 * MAX_NS];  /* production synthesis window                 */
  MODEL post;            /* model after the postfilter                  */
  float out[MAX_NS];     /* Sn_[0..n_samp) pre ear-protection/PCM clip  */
  int n_samp;
  int out_n;
  int have_input, have_phase, have_model, have_output;
} frame_rec;

typedef struct {
  const void *c2;
  int active;
  float ref_ex;      /* own fundamental-clock reference history  */
  ovl_ref ovl;       /* own window/overlap reference history     */
  frame_rec rec[MAX_REC];
  int nrec;
  long frames_done;
  long voiced_frames;
  long unvoiced_frames;
  double wo_sum;
} stream_inst;

static stream_inst insts[2];

static stream_inst *find_inst(const void *c2) {
  int i;
  for (i = 0; i < 2; i++)
    if (insts[i].active && insts[i].c2 == c2) return &insts[i];
  return NULL;
}

/* Read-only observation points called from codec2.c (TASK_OBSERVE
   builds only).  They only record; all checking happens in the test
   cases below. */
void task_observe_frame_input(const void *c2, const MODEL *model,
                              const COMP Aw[], float ex_phase, int n_samp,
                              const float Pn[]) {
  stream_inst *s = find_inst(c2);
  frame_rec *r;
  if (!s || s->nrec >= MAX_REC) return;
  r = &s->rec[s->nrec];
  r->pre = *model;
  memcpy(r->Aw, Aw, sizeof(r->Aw));
  r->ex_pre = ex_phase;
  memcpy(r->Pn, Pn, 2 * n_samp * sizeof(float));
  r->n_samp = n_samp;
  r->have_input = 1;
  r->have_phase = r->have_model = r->have_output = 0;
}

void task_observe_phase(const void *c2, const MODEL *model, float ex_phase) {
  stream_inst *s = find_inst(c2);
  frame_rec *r;
  if (!s || s->nrec >= MAX_REC) return;
  r = &s->rec[s->nrec];
  r->pre = *model; /* now carries the synthesised phases */
  r->ex_post = ex_phase;
  r->have_phase = 1;
}

void task_observe_model(const void *c2, const MODEL *model) {
  stream_inst *s = find_inst(c2);
  frame_rec *r;
  if (!s || s->nrec >= MAX_REC) return;
  r = &s->rec[s->nrec];
  r->post = *model;
  r->have_model = 1;
}

void task_observe_output(const void *c2, const float Sn_[], int n_samp) {
  stream_inst *s = find_inst(c2);
  frame_rec *r;
  if (!s || s->nrec >= MAX_REC) return;
  r = &s->rec[s->nrec];
  memcpy(r->out, Sn_, n_samp * sizeof(float));
  r->out_n = n_samp;
  r->have_output = 1;
  s->nrec++;
}

static void stream_reset_inst(int k, struct CODEC2 *dec) {
  memset(&insts[k], 0, sizeof(insts[k]));
  insts[k].c2 = dec;
  insts[k].active = 1;
  insts[k].ref_ex = 0.0f;
  ovl_ref_init(&insts[k].ovl, 80, 40); /* 3200 mode: 8 kHz, 10 ms */
}

/* Check one observed frame against the independently maintained
   reference histories (which persist across frames). */
static int stream_drain(stream_inst *s, long frame_base) {
  int q;
  for (q = 0; q < s->nrec; q++) {
    frame_rec *r = &s->rec[q];
    int i, l;
    long frame = frame_base + q;
    TASK_CHECK(r->have_input && r->have_phase && r->have_model &&
               r->have_output);
    TASK_CHECK(r->n_samp == r->out_n);

    /* window configuration observed at the phase-update input */
    for (i = 0; i < 2 * r->n_samp; i++)
      if (check_window_pt("stream-window", frame, i, r->Pn[i],
                          ref_window(i, r->n_samp, s->ovl.tw)))
        return 1;

    /* fundamental clock: input matches our history, update follows
       the Wo*n_samp rule */
    if (check_phase("stream-clock-in", frame, 0, r->ex_pre, s->ref_ex))
      return 1;
    s->ref_ex = ref_clock_advance(s->ref_ex, r->pre.Wo, r->n_samp, NULL);
    if (check_phase("stream-clock-out", frame, 0, r->ex_post, s->ref_ex))
      return 1;

    /* per-harmonic phase, before the postfilter */
    if (r->pre.voiced) {
      s->voiced_frames++;
      for (l = 1; l <= r->pre.L; l++) {
        int b = ref_enc_bin(r->pre.Wo, l);
        fcpx h = fc_conj(fc_from_comp(&r->Aw[b]));
        float want = ref_harm_phase(h, s->ref_ex, l);
        if (check_phase("stream-phase", frame, l, r->pre.phi[l], want))
          return 1;
      }
    } else {
      s->unvoiced_frames++;
      for (l = 1; l <= r->pre.L; l++) TASK_CHECK(isfinite(r->pre.phi[l]));
    }
    s->wo_sum += r->pre.Wo;

    /* overlap reference consumes the post-postfilter model */
    ovl_ref_frame(&s->ovl, &r->post, 1);
    for (i = 0; i < r->n_samp; i++)
      if (check_sample("stream-overlap", frame, i, r->out[i],
                       s->ovl.acc[s->ovl.T + i]))
        return 1;
  }
  s->nrec = 0;
  return 0;
}

static struct CODEC2 *s_enc[2], *s_dec[2];

/* Encode+decode one 20 ms 3200-mode frame on instance k and check the
   observed records.  kind 0: dual sines (per-instance pitch); kind 1:
   voiced segment then deterministic noise. */
static int stream_one_frame(int k, int f, int kind, long *energy) {
  short in[160], out[160];
  unsigned char bits[8];
  int i, nrec;
  for (i = 0; i < 160; i++) {
    int t = f * 160 + i;
    double v;
    if (kind == 0) {
      v = (k == 0) ? 2400 * sin(.071 * t) + 900 * cos(.133 * t)
                   : 1800 * sin(.043 * t + 0.5) + 700 * cos(.171 * t) +
                         300 * sin(.211 * t);
    } else {
      if (f < 7) {
        v = 2200 * sin(.059 * t) + 800 * cos(.121 * t);
      } else {
        unsigned h = (unsigned)t * 2654435761u;
        h ^= h >> 15;
        h *= 2246822519u;
        v = 700.0 * ((double)((h >> 8) & 0xFFFF) / 32768.0 - 1.0);
      }
    }
    in[i] = (short)v;
  }
  codec2_encode(s_enc[k], bits, in);
  codec2_decode(s_dec[k], out, bits);
  nrec = insts[k].nrec;
  TASK_CHECK(nrec == 2); /* two 10 ms subframes must be observed */
  if (stream_drain(&insts[k], insts[k].frames_done)) return 1;
  insts[k].frames_done += nrec;
  for (i = 0; i < 160; i++) *energy += (long)out[i] * out[i];
  return 0;
}

static int stream_create_pair(void) {
  s_enc[0] = codec2_create(CODEC2_MODE_3200);
  s_enc[1] = codec2_create(CODEC2_MODE_3200);
  s_dec[0] = codec2_create(CODEC2_MODE_3200);
  s_dec[1] = codec2_create(CODEC2_MODE_3200);
  TASK_CHECK(s_enc[0] && s_enc[1] && s_dec[0] && s_dec[1]);
  TASK_CHECK(codec2_samples_per_frame(s_enc[0]) == 160);
  TASK_CHECK(codec2_bytes_per_frame(s_enc[0]) == 8);
  return 0;
}

static void stream_destroy_pair(void) {
  int k;
  for (k = 0; k < 2; k++) {
    if (s_enc[k]) codec2_destroy(s_enc[k]);
    if (s_dec[k]) codec2_destroy(s_dec[k]);
    s_enc[k] = s_dec[k] = NULL;
    insts[k].active = 0;
  }
}

/* Two instances with different pitch/window histories, interleaved
   frame by frame. */
static int case_stream_dual_instance(void) {
  long e0 = 0, e1 = 0;
  int f;
  if (stream_create_pair()) return 1;
  stream_reset_inst(0, s_dec[0]);
  stream_reset_inst(1, s_dec[1]);
  for (f = 0; f < 14; f++) {
    if (stream_one_frame(0, f, 0, &e0)) return 1;
    if (stream_one_frame(1, f, 0, &e1)) return 1;
  }
  /* public decode output: sample count and non-silent content */
  TASK_CHECK(codec2_samples_per_frame(s_dec[0]) == 160);
  TASK_CHECK(codec2_samples_per_frame(s_dec[1]) == 160);
  TASK_CHECK(e0 > 1000000);
  TASK_CHECK(e1 > 1000000);
  /* the two instances really followed different pitch histories */
  TASK_CHECK(insts[0].wo_sum != insts[1].wo_sum);
  TASK_CHECK(insts[0].voiced_frames > 0 && insts[1].voiced_frames > 0);
  TASK_CHECK(insts[0].frames_done == 28 && insts[1].frames_done == 28);
  stream_destroy_pair();
  return 0;
}

/* One instance across a voiced -> unvoiced transition; the unvoiced
   branch of the phase contract is exercised on the real decoder. */
static int case_stream_voiced_unvoiced(void) {
  long e0 = 0;
  int f;
  if (stream_create_pair()) return 1;
  stream_reset_inst(0, s_dec[0]);
  insts[1].active = 0;
  for (f = 0; f < 14; f++) {
    if (stream_one_frame(0, f, 1, &e0)) return 1;
  }
  TASK_CHECK(codec2_samples_per_frame(s_dec[0]) == 160);
  TASK_CHECK(e0 > 1000000);
  TASK_CHECK(insts[0].voiced_frames > 0);
  TASK_CHECK(insts[0].unvoiced_frames > 0);
  TASK_CHECK(insts[0].frames_done == 28);
  stream_destroy_pair();
  return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"phase", "sample_bins", case_phase_sample_bins},
    {"phase", "voiced_track", case_phase_voiced_track},
    {"phase", "unvoiced_clock", case_phase_unvoiced_clock},
    {"overlap", "window_def", case_overlap_window_def},
    {"overlap", "shift_modes_8k", case_overlap_shift_modes_8k},
    {"overlap", "shift_modes_16k", case_overlap_shift_modes_16k},
    {"overlap", "silence_tail", case_overlap_silence_tail},
    {"overlap", "top_bin", case_overlap_top_bin},
    {"overlap", "junction", case_overlap_junction},
    {"stream", "dual_instance", case_stream_dual_instance},
    {"stream", "voiced_unvoiced", case_stream_voiced_unvoiced},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
