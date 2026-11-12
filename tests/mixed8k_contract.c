/*
 * mixed8k_contract.c - contract tests for the optional 8 kHz MPEG-2.5
 * mixed-block four-long-subband convention (MAD_OPTION_MIXED8K_4SUBBANDS).
 *
 * Three groups are exercised:
 *   test_layout - requantization/scalefactor/window layout at the 71/72
 *                 boundary with three independent short windows and gains
 *   test_hybrid - subband samples against an independent double-precision
 *                 cosine IMDCT reference with explicit windows, alias
 *                 reduction and persistent overlap-add
 *   test_stream - full mad_frame_decode()/mad_synth_frame() paths over
 *                 continuous mono/stereo streams, including joint stereo
 *
 * Every comparison goes through unit_check() with a concrete label; values
 * are reported as (channel, subband, time sample, expected, actual).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stream.h"
#include "frame.h"
#include "synth.h"
#include "check.h"
#include "fixture.h"
#include "mixed8k_gen.h"
#include "mixed8k_obs.h"

#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif

#define Q28 (double) (1L << 28)
#define ABS(x) ((x) < 0 ? -(x) : (x))

extern struct mad_mixed8k_obs mad_mixed8k_obs;

/* worst observed Q28 difference; reported, never used to average away
   local error */
static double g_worst;
static int g_worst_ch, g_worst_sb, g_worst_t;
static long g_worst_exp, g_worst_act;

static long ref_q28(double x)
{
  return lrint(x * Q28);
}

/* requantization uses tabulated fractional powers, so allow the small
   fixed-point difference against the exact double formula */
static void check_requant(const char *tag, long act, long exp)
{
  long diff = ABS(act - exp);
  printf("%s ch=0 line expected=%ld actual=%ld diff=%ld\n",
	 tag, exp, act, diff);
  unit_check(diff <= 8, tag);
}

static void report_worst(const char *tag)
{
  printf("%s worst ch=%d sb=%d t=%d expected=%ld actual=%ld diff=%.0f\n",
	 tag, g_worst_ch, g_worst_sb, g_worst_t,
	 g_worst_exp, g_worst_act, g_worst);
}

static void track_worst(int ch, int sb, int t, long exp, long act)
{
  double e = fabs((double) (act - exp));
  if (e > g_worst) {
    g_worst = e; g_worst_ch = ch; g_worst_sb = sb; g_worst_t = t;
    g_worst_exp = exp; g_worst_act = act;
  }
}

/* ------------------------------------------------------------------ */
/* 8 kHz scalefactor band widths (mirrors layer3.c, kept independent)  */
/* ------------------------------------------------------------------ */

/* pure short block: 13 bands, each stored windows 0,1,2 in turn */
static const unsigned char ref_short_all[13][3] = {
  { 8, 8, 8 }, { 8, 8, 8 }, { 8, 8, 8 },
  { 12, 12, 12 }, { 16, 16, 16 }, { 20, 20, 20 },
  { 24, 24, 24 }, { 28, 28, 28 }, { 36, 36, 36 },
  { 2, 2, 2 }, { 2, 2, 2 }, { 2, 2, 2 }, { 26, 26, 26 }
};

/* ------------------------------------------------------------------ */
/* independent short-block reorder into subband order                  */
/* ------------------------------------------------------------------ */

static void ref_reorder(const double in[576], double out[576],
			int block_type, int mixed, unsigned longlines)
{
  double tmp[32][3][6];
  unsigned sb, l, f, w, sbw[3], sw[3], band, first_short_band;
  const unsigned char (*widths)[3];

  memset(tmp, 0, sizeof tmp);

  if (block_type != 2) {
    memcpy(out, in, 576 * sizeof(double));
    return;
  }

  if (mixed) {
    memcpy(out, in, longlines * sizeof(double));
    sb = longlines / 18;
    first_short_band = 3;
    widths = ref_short_all;
    l = longlines;
  }
  else {
    sb = 0;
    first_short_band = 0;
    widths = ref_short_all;
    l = 0;
  }

  for (w = 0; w < 3; ++w) { sbw[w] = sb; sw[w] = 0; }

  for (band = first_short_band; band < 13 && l < 576; ++band) {
    for (w = 0; w < 3 && l < 576; ++w) {
      unsigned width = widths[band][w];
      for (f = 0; f < width && l < 576; ++f, ++l) {
	tmp[sbw[w]][w][sw[w]++] = in[l];
	if (sw[w] == 6) { sw[w] = 0; ++sbw[w]; }
      }
    }
  }

  for (sb = (mixed ? longlines / 18 : 0); sb < 32; ++sb)
    for (w = 0; w < 3; ++w)
      for (f = 0; f < 6; ++f)
	out[18 * sb + 6 * w + f] = tmp[sb][w][f];
}

/* ------------------------------------------------------------------ */
/* alias reduction, IMDCT, windowing, overlap, frequency inversion     */
/* ------------------------------------------------------------------ */

static const double ref_c[8] = {
  -0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037
};

static void ref_aliasreduce(double xr[576], unsigned lines)
{
  unsigned base, i;
  for (base = 18; base < lines; base += 18) {
    for (i = 0; i < 8; ++i) {
      double cs = 1.0 / sqrt(1.0 + ref_c[i] * ref_c[i]);
      double ca = ref_c[i] / sqrt(1.0 + ref_c[i] * ref_c[i]);
      double a = xr[base - 1 - i], b = xr[base + i];
      xr[base - 1 - i] = a * cs - b * ca;
      xr[base + i]     = b * cs + a * ca;
    }
  }
}

static void ref_imdct_long(const double X[18], double z[36])
{
  int i, k;
  for (i = 0; i < 36; ++i) {
    double s = 0.0;
    for (k = 0; k < 18; ++k)
      s += X[k] * cos(M_PI / 72.0 * (2 * i + 19) * (2 * k + 1));
    z[i] = s * sin(M_PI / 36.0 * (i + 0.5));
  }
}

static void ref_imdct_short(const double X[18], double z[36])
{
  double y[3][12];
  int w, i, k;
  for (w = 0; w < 3; ++w)
    for (i = 0; i < 12; ++i) {
      double s = 0.0;
      for (k = 0; k < 6; ++k)
	s += X[6 * w + k] * cos(M_PI / 24.0 * (2 * i + 7) * (2 * k + 1));
      y[w][i] = s;
    }

  memset(z, 0, 36 * sizeof(double));
  for (i = 0; i < 6; ++i) {
    double w0 = sin(M_PI / 12.0 * (i + 0.5));
    double w6 = sin(M_PI / 12.0 * (i + 6 + 0.5));
    z[6 + i]  = y[0][i] * w0;
    z[12 + i] = y[0][6 + i] * w6 + y[1][i] * w0;
    z[18 + i] = y[1][6 + i] * w6 + y[2][i] * w0;
    z[24 + i] = y[2][6 + i] * w6;
  }
}

/* one channel's reference state; overlap history persists across frames */
struct ref_hybrid {
  double overlap[32][18];
  double sbsample[18][32];
};

static void ref_hybrid_frame(struct ref_hybrid *h, const double in[576],
			     int block_type, int mixed, unsigned longlines)
{
  double xr[576], ord[576], output[36];
  unsigned sb, t, longsb;

  ref_reorder(in, ord, block_type, mixed, longlines);
  memcpy(xr, ord, sizeof xr);

  if (block_type == 2) {
    if (mixed)
      ref_aliasreduce(xr, longlines);
  }
  else
    ref_aliasreduce(xr, 576);

  longsb = (block_type == 2 && mixed) ? longlines / 18 : 0;

  for (sb = 0; sb < 32; ++sb) {
    int is_long;
    if (block_type == 2)
      is_long = (mixed && sb < longsb);
    else
      is_long = 1;

    if (is_long)
      ref_imdct_long(&xr[18 * sb], output);
    else
      ref_imdct_short(&xr[18 * sb], output);

    for (t = 0; t < 18; ++t) {
      h->sbsample[t][sb] = output[t] + h->overlap[sb][t];
      h->overlap[sb][t] = output[t + 18];
      /* frequency inversion: odd subbands negate their odd time lines */
      if ((sb & 1) && (t & 1))
	h->sbsample[t][sb] = -h->sbsample[t][sb];
    }
  }
}

/* observed post-stereo requantized spectrum as doubles */
static void obs_spectrum(int ch, double out[576])
{
  int i;
  for (i = 0; i < 576; ++i)
    out[i] = (double) mad_mixed8k_obs.xr_requant[ch][i] / Q28;
}

static int decode_buf(unsigned char const *d, unsigned len, int options,
		      struct mad_stream *s, struct mad_frame *f,
		      struct mad_synth *sy)
{
  mad_stream_buffer(s, d, len);
  s->options = options;
  if (mad_frame_decode(f, s))
    return -1;
  mad_synth_frame(sy, f);
  return 0;
}

static struct m8k_frame_spec *base_mixed_spec(struct m8k_frame_spec *spec,
					      int id25)
{
  memset(spec, 0, sizeof *spec);
  spec->id25 = id25;
  spec->mode = MAD_MODE_SINGLE_CHANNEL;
  spec->ch[0].block_type = 2;
  spec->ch[0].mixed = 1;
  spec->ch[0].global_gain = 200;
  spec->ch[0].scalefac_compress = 122;	/* slen 1,1,0,0 */
  spec->ch[0].subblock_gain[0] = 1;
  spec->ch[0].subblock_gain[1] = 2;
  spec->ch[0].subblock_gain[2] = 3;
  return spec;
}

/* ================================================================== */
/* layout group                                                        */
/* ================================================================== */

void test_layout(void)
{
  struct m8k_frame_spec spec;
  unsigned char d[160];
  struct mad_stream s;
  struct mad_frame f;
  struct mad_synth sy;
  struct m8k_channel *c;
  long expv, actv;
  int exponent;

  mad_stream_init(&s);
  mad_frame_init(&f);
  mad_synth_init(&sy);

  base_mixed_spec(&spec, 1);
  c = &spec.ch[0];
  c->scalefac[3] = 1;				/* long band over line 40 */
  c->scalefac[5] = 1;				/* long band over 70/71 */
  c->scalefac[6] = 2; c->scalefac[7] = 3; c->scalefac[8] = 1;
  c->coef[40] = 1;
  c->coef[70] = 1; c->coef[71] = -1;		/* last long line */
  c->coef[72] = 1;				/* first short line, win 0 */
  c->coef[84] = 1;				/* window 1 */
  c->coef[96] = 1;				/* window 2 */
  m8k_build(d, &spec, 1);

  unit_check(decode_buf(d, 152, MAD_OPTION_MIXED8K_4SUBBANDS,
			&s, &f, &sy) == 0, "layout: mixed4 decodes");
  unit_check(mad_mixed8k_obs.longlines[0] == 72, "layout: 72 long lines");
  unit_check(mad_mixed8k_obs.samplerate == 8000, "layout: 8 kHz");
  unit_check(mad_mixed8k_obs.mpeg25 == 1, "layout: MPEG-2.5");
  unit_check(mad_mixed8k_obs.block_type[0] == 2 &&
	     mad_mixed8k_obs.mixed[0] == 1, "layout: mixed block type 2");

  /* scalefac_scale=0: each scalefactor contributes -2 to the quarter-dB
     exponent (libmad applies scalefac << 1); short windows also subtract
     8 * subblock_gain */

  /* line 40: long, no subblock gain, long scalefac band 3 = 1 */
  exponent = 200 - 210 - 2 * 1;
  expv = ref_q28(pow(2.0, exponent / 4.0));
  actv = mad_mixed8k_obs.xr_requant[0][40];
  check_requant("layout: line40 long requant", actv, expv);

  /* lines 70/71 stay long, signs preserved */
  expv = ref_q28(pow(2.0, exponent / 4.0));
  check_requant("layout: line70 long side of boundary",
		mad_mixed8k_obs.xr_requant[0][70], expv);
  check_requant("layout: line71 long, sign kept",
		mad_mixed8k_obs.xr_requant[0][71], -expv);

  /* lines 72/84/96 pick up windows 0/1/2 with distinct subblock gains */
  exponent = 200 - 210 - 8 * 1 - 2 * 2;
  expv = ref_q28(pow(2.0, exponent / 4.0));
  check_requant("layout: line72 window0 gain only",
		mad_mixed8k_obs.xr_requant[0][72], expv);

  exponent = 200 - 210 - 8 * 2 - 2 * 3;
  expv = ref_q28(pow(2.0, exponent / 4.0));
  check_requant("layout: line84 window1 gain only",
		mad_mixed8k_obs.xr_requant[0][84], expv);

  exponent = 200 - 210 - 8 * 3 - 2 * 1;
  expv = ref_q28(pow(2.0, exponent / 4.0));
  check_requant("layout: line96 window2 gain only",
		mad_mixed8k_obs.xr_requant[0][96], expv);

  unit_check(mad_mixed8k_obs.xr_requant[0][72] !=
	     mad_mixed8k_obs.xr_requant[0][84], "layout: gains 0/1 differ");
  unit_check(mad_mixed8k_obs.xr_requant[0][84] !=
	     mad_mixed8k_obs.xr_requant[0][96], "layout: gains 1/2 differ");

  /* option off: historical two-subband layout on the identical frame */
  unit_check(decode_buf(d, 152, 0, &s, &f, &sy) == 0,
	     "layout: option-off decodes");
  unit_check(mad_mixed8k_obs.longlines[0] == 36,
	     "layout: option-off keeps 36 long lines");

  /* option is a no-op for a 16 kHz MPEG-2 mixed block */
  base_mixed_spec(&spec, 0);
  spec.ch[0].scalefac_compress = 0;
  spec.ch[0].coef[40] = 1;
  m8k_build(d, &spec, 1);
  unit_check(decode_buf(d, 80, MAD_OPTION_MIXED8K_4SUBBANDS, &s, &f, &sy) == 0,
	     "layout: 16k mixed decodes");
  unit_check(mad_mixed8k_obs.samplerate == 16000 &&
	     mad_mixed8k_obs.longlines[0] == 36,
	     "layout: 16k unaffected by option");

  mad_synth_finish(&sy);
  mad_frame_finish(&f);
  mad_stream_finish(&s);
}

/* ================================================================== */
/* hybrid group                                                        */
/* ================================================================== */

void test_hybrid(void)
{
  struct m8k_frame_spec spec, long_spec;
  unsigned char d[160], d2[160];
  struct mad_stream s;
  struct mad_frame f, f_mixed, f_long;
  struct mad_synth sy;
  struct ref_hybrid ref;
  double xr[576];
  int sb, t;

  mad_stream_init(&s);
  mad_frame_init(&f);
  mad_synth_init(&sy);

  /* ---- mixed4 energy on both sides of the 71/72 boundary ---- */
  base_mixed_spec(&spec, 1);
  spec.ch[0].scalefac[2] = 1;
  spec.ch[0].scalefac[6] = 1;
  spec.ch[0].coef[10] = 1;		/* long subband 0 */
  spec.ch[0].coef[40] = 1;		/* long subband 2 */
  spec.ch[0].coef[70] = -1;		/* long subband 3, near boundary */
  spec.ch[0].coef[72] = 1;		/* short subband 4, window 0 */
  spec.ch[0].coef[108] = 1;		/* short, later band */
  m8k_build(d, &spec, 1);

  unit_check(decode_buf(d, 152, MAD_OPTION_MIXED8K_4SUBBANDS, &s, &f, &sy) == 0,
	     "hybrid: mixed4 decodes");

  memset(&ref, 0, sizeof ref);
  obs_spectrum(0, xr);
  ref_hybrid_frame(&ref, xr, 2, 1, 72);

  g_worst = 0.0;
  for (sb = 0; sb < 32; ++sb)
    for (t = 0; t < 18; ++t)
      track_worst(0, sb, t, ref_q28(ref.sbsample[t][sb]),
		  f.sbsample[0][t][sb]);
  report_worst("hybrid-mixed4");
  unit_check(g_worst <= 128.0, "hybrid: mixed4 subbands within 128 Q28");

  /* window identity: short subband 4 only has its window-0 slot occupied */
  unit_check(mad_mixed8k_obs.xr_reorder[0][18 * 4 + 0] != 0,
	     "hybrid: short window0 occupied");
  unit_check(mad_mixed8k_obs.xr_reorder[0][18 * 4 + 6] == 0 &&
	     mad_mixed8k_obs.xr_reorder[0][18 * 4 + 12] == 0,
	     "hybrid: short windows 1/2 keep identity");

  /* ---- exact long equivalence: energy only in low long subbands ---- */
  base_mixed_spec(&spec, 1);
  spec.ch[0].coef[0] = 1;
  spec.ch[0].coef[1] = -1;
  spec.ch[0].coef[40] = 1;
  spec.ch[0].coef[41] = 1;
  m8k_build(d, &spec, 1);

  memset(&long_spec, 0, sizeof long_spec);
  long_spec.id25 = 1;
  long_spec.mode = MAD_MODE_SINGLE_CHANNEL;
  long_spec.ch[0].block_type = 0;
  long_spec.ch[0].global_gain = 200;
  long_spec.ch[0].scalefac_compress = 122;
  long_spec.ch[0].coef[0] = 1;
  long_spec.ch[0].coef[1] = -1;
  long_spec.ch[0].coef[40] = 1;
  long_spec.ch[0].coef[41] = 1;
  m8k_build(d2, &long_spec, 1);

  mad_frame_init(&f_long);
  mad_frame_init(&f_mixed);
  {
    struct mad_synth sl, sm;
    mad_synth_init(&sl);
    mad_synth_init(&sm);
    unit_check(decode_buf(d2, 152, MAD_OPTION_MIXED8K_4SUBBANDS, &s, &f_long, &sl) == 0,
	       "hybrid: pure-long reference decodes");
    unit_check(decode_buf(d, 152, MAD_OPTION_MIXED8K_4SUBBANDS, &s, &f_mixed, &sm) == 0,
	       "hybrid: mixed low-energy decodes");
    mad_synth_finish(&sl);
    mad_synth_finish(&sm);
  }

  {
    int exact = 1, rep_sb = -1, rep_t = -1;
    long le = 0, la = 0;
    for (sb = 0; sb < 32 && exact; ++sb)
      for (t = 0; t < 18; ++t)
	if (f_mixed.sbsample[0][t][sb] != f_long.sbsample[0][t][sb]) {
	  exact = 0; rep_sb = sb; rep_t = t;
	  le = f_long.sbsample[0][t][sb];
	  la = f_mixed.sbsample[0][t][sb];
	  break;
	}
    if (!exact)
      printf("hybrid-long mismatch ch=0 sb=%d t=%d expected=%ld actual=%ld\n",
	     rep_sb, rep_t, le, la);
    unit_check(exact, "hybrid: low four subbands exactly match long block");
  }
  mad_frame_finish(&f_mixed);
  mad_frame_finish(&f_long);

  /* ---- pure-short reference (all subbands short, independent windows) ---- */
  {
    struct m8k_frame_spec ss;
    struct ref_hybrid ref2;
    struct mad_frame f_ps;
    struct mad_synth sy_ps;
    unsigned char d3[160];
    memset(&ss, 0, sizeof ss);
    ss.id25 = 1;
    ss.mode = MAD_MODE_SINGLE_CHANNEL;
    ss.ch[0].block_type = 2;
    ss.ch[0].mixed = 0;
    ss.ch[0].global_gain = 200;
    ss.ch[0].scalefac_compress = 0;
    ss.ch[0].subblock_gain[0] = 0;
    ss.ch[0].subblock_gain[1] = 1;
    ss.ch[0].subblock_gain[2] = 2;
    ss.ch[0].coef[0] = 1;
    ss.ch[0].coef[6] = -1;
    m8k_build(d3, &ss, 1);

    /* clean objects so no overlap history from earlier subtests leaks in */
    mad_frame_init(&f_ps);
    mad_synth_init(&sy_ps);
    unit_check(decode_buf(d3, 152, MAD_OPTION_MIXED8K_4SUBBANDS,
			  &s, &f_ps, &sy_ps) == 0,
	       "hybrid: pure-short decodes");

    memset(&ref2, 0, sizeof ref2);
    obs_spectrum(0, xr);
    ref_hybrid_frame(&ref2, xr, 2, 0, 0);

    g_worst = 0.0;
    for (sb = 0; sb < 10; ++sb)
      for (t = 0; t < 18; ++t)
	track_worst(0, sb, t, ref_q28(ref2.sbsample[t][sb]),
		    f_ps.sbsample[0][t][sb]);
    report_worst("hybrid-short");
    unit_check(g_worst <= 128.0, "hybrid: pure-short subbands within 128 Q28");
    mad_synth_finish(&sy_ps);
    mad_frame_finish(&f_ps);
  }

  mad_synth_finish(&sy);
  mad_frame_finish(&f);
  mad_stream_finish(&s);
}

/* ================================================================== */
/* stream group                                                        */
/* ================================================================== */

/* run the in-tree six-frame fixture through frame decode + PCM synth,
   capturing one channel of PCM and per-frame decode status */
static int run_fixture(int option, int mixed_stream,
		       mad_fixed_t pcm[6][576])
{
  unsigned char data[6 * 144 + 8];
  unsigned n = make_stream(data, mixed_stream);
  struct mad_stream st;
  struct mad_frame fr;
  struct mad_synth sy;
  int k;

  mad_stream_init(&st);
  mad_frame_init(&fr);
  mad_synth_init(&sy);
  mad_stream_buffer(&st, data, n);
  st.options = option;

  for (k = 0; k < 6; ++k) {
    if (mad_frame_decode(&fr, &st)) {
      mad_frame_finish(&fr);
      mad_stream_finish(&st);
      return -1;
    }
    mad_synth_frame(&sy, &fr);
    if (sy.pcm.length != 576 || sy.pcm.samplerate != 8000) {
      mad_frame_finish(&fr);
      mad_stream_finish(&st);
      return -2;
    }
    memcpy(pcm[k], sy.pcm.samples[0], 576 * sizeof(mad_fixed_t));
  }

  mad_frame_finish(&fr);
  mad_stream_finish(&st);
  return 0;
}

/* build a two-frame two-channel stream with the requested joint mode;
   left/right coefficients are deliberately asymmetric */
static unsigned build_stereo(unsigned char *buf, int mode_extension,
			     int block_type, int mixed)
{
  struct m8k_frame_spec spec;
  unsigned off = 0, frame_bytes, fr;
  int ch;

  for (fr = 0; fr < 2; ++fr) {
    memset(&spec, 0, sizeof spec);
    spec.id25 = 1;
    spec.mode = (mode_extension == 0) ? MAD_MODE_STEREO : MAD_MODE_JOINT_STEREO;
    spec.mode_extension = mode_extension;
    frame_bytes = m8k_frame_bytes(&spec);

    for (ch = 0; ch < 2; ++ch) {
      struct m8k_channel *c = &spec.ch[ch];
      c->block_type = block_type;
      c->mixed = (block_type == 2) ? mixed : 0;
      c->global_gain = 190 + ch * 8 + fr * 4;
      c->scalefac_compress = 0;
      c->subblock_gain[0] = ch;
      c->subblock_gain[1] = (ch + 1) % 3;
      c->subblock_gain[2] = (ch + 2) % 3;
      /* asymmetric energy, all inside the four long subbands for mixed */
      c->coef[2 + ch] = (ch == 0) ? 1 : -1;
      c->coef[40 + 2 * ch] = 1;
      c->coef[60 + ch] = (ch == 0) ? -1 : 1;
      if (block_type == 2 && !mixed)
	c->coef[6 + ch] = 1;	/* short-window probe for pure short */
    }
    off += m8k_build(buf + off, &spec, 1);
    (void) frame_bytes;
  }
  return off;
}

void test_stream(void)
{
  mad_fixed_t on[6][576], off[6][576];
  int k, i, diff_mixed, same_other, rc;

  /* ---- continuous six-frame fixture, option on vs off, history kept ---- */
  rc = run_fixture(MAD_OPTION_MIXED8K_4SUBBANDS, 1, on);
  unit_check(rc == 0, "stream: mixed fixture decodes with option on");
  rc = run_fixture(0, 1, off);
  unit_check(rc == 0, "stream: mixed fixture decodes with option off");

  diff_mixed = 0;
  same_other = 1;
  /* frames 2,3 are mixed and must change; frames 0,1 precede the block
     switch, so neither the option nor any earlier overlap history can touch
     them and they stay bit-identical.  Frames 4,5 legitimately differ
     because the changed mixed frames leave different overlap history. */
  for (k = 0; k < 6; ++k) {
    int frame_differs = 0;
    for (i = 0; i < 576; ++i)
      if (on[k][i] != off[k][i]) frame_differs = 1;
    if (k == 2 || k == 3)
      diff_mixed += frame_differs;
    else if (k < 2 && frame_differs)
      same_other = 0;
  }
  unit_check(diff_mixed == 2, "stream: both mixed frames change under option");
  unit_check(same_other,
	     "stream: pre-switch frames 0/1 are bit-identical");

  /* the all-long fixture must be identical whether the option is on or not */
  {
    mad_fixed_t lon[6][576], loff[6][576];
    int identical = 1;
    unit_check(run_fixture(MAD_OPTION_MIXED8K_4SUBBANDS, 0, lon) == 0,
	       "stream: all-long fixture decodes (on)");
    unit_check(run_fixture(0, 0, loff) == 0,
	       "stream: all-long fixture decodes (off)");
    for (k = 0; k < 6 && identical; ++k)
      for (i = 0; i < 576; ++i)
	if (lon[k][i] != loff[k][i]) { identical = 0; break; }
    unit_check(identical, "stream: all-long output unaffected by option");
  }

  /* ---- option may be toggled before any frame without clearing history ---- */
  {
    unsigned char data[6 * 144 + 8];
    unsigned n = make_stream(data, 1);
    struct mad_stream st;
    struct mad_frame fr;
    struct mad_synth sy;
    mad_fixed_t toggled[6][576];
    int k, all_ok = 1, mixed_changed = 0;

    mad_stream_init(&st);
    mad_frame_init(&fr);
    mad_synth_init(&sy);
    mad_stream_buffer(&st, data, n);

    /* decode the stream with the option enabled only for the two mixed
       frames 2,3 and disabled otherwise, so it switches four times while
       the frame/synth objects are never re-initialized; overlap and
       polyphase history therefore persist across every option change. */
    for (k = 0; k < 6; ++k) {
      st.options = (k == 2 || k == 3) ? MAD_OPTION_MIXED8K_4SUBBANDS : 0;
      if (mad_frame_decode(&fr, &st)) { all_ok = 0; break; }
      mad_synth_frame(&sy, &fr);
      if (sy.pcm.length != 576 || sy.pcm.samplerate != 8000) {
	all_ok = 0; break;
      }
      memcpy(toggled[k], sy.pcm.samples[0], 576 * sizeof(mad_fixed_t));
      if (k == 2 || k == 3) {
	mixed_changed += (memcmp(toggled[k], off[k],
				 576 * sizeof(mad_fixed_t)) != 0);
      }
    }
    unit_check(all_ok, "stream: per-frame option toggle decodes all frames");
    unit_check(mixed_changed == 2,
	       "stream: toggled-on mixed frames differ from option-off run");
    mad_synth_finish(&sy);
    mad_frame_finish(&fr);
    mad_stream_finish(&st);
  }

  /* ---- stereo modes through the public API, two frames of history ---- */
  {
    static const int modes[4] = { 0, 2, 1, 3 }; /* normal, MS, IS, MS+IS */
    static const char *names[4] = {
      "normal", "MS", "intensity", "MS+intensity"
    };
    int m;
    for (m = 0; m < 4; ++m) {
      unsigned char buf[2 * 144 + 8];
      unsigned n = build_stereo(buf, modes[m], 2, 1);
      struct mad_stream st;
      struct mad_frame fr;
      struct mad_synth sy;
      struct ref_hybrid ref[2];
      int fr_i, ch, ok;

      mad_stream_init(&st);
      mad_frame_init(&fr);
      mad_synth_init(&sy);
      mad_stream_buffer(&st, buf, n + 8);
      st.options = MAD_OPTION_MIXED8K_4SUBBANDS;
      memset(ref, 0, sizeof ref);
      ok = 1;

      for (fr_i = 0; fr_i < 2; ++fr_i) {
	if (mad_frame_decode(&fr, &st)) { ok = 0; break; }
	mad_synth_frame(&sy, &fr);
	if (sy.pcm.channels != 2 || sy.pcm.length != 576) { ok = 0; break; }

	/* joint stereo flags actually present in the decoded header */
	if (modes[m] & 2)
	  ok = ok && !!(fr.header.flags & MAD_FLAG_MS_STEREO);
	if (modes[m] & 1)
	  ok = ok && !!(fr.header.flags & MAD_FLAG_I_STEREO);

	/* independent reference over the post-stereo observed spectrum,
		   with persistent overlap across the two frames */
	for (ch = 0; ch < 2; ++ch) {
	  double xr[576];
	  int sb, t;
	  obs_spectrum(ch, xr);
	  ref_hybrid_frame(&ref[ch], xr,
			   mad_mixed8k_obs.block_type[ch],
			   mad_mixed8k_obs.mixed[ch],
			   mad_mixed8k_obs.longlines[ch]);
	  g_worst = 0.0;
	  for (sb = 0; sb < 8; ++sb)
	    for (t = 0; t < 18; ++t)
	      track_worst(ch, sb, t, ref_q28(ref[ch].sbsample[t][sb]),
			  fr.sbsample[ch][t][sb]);
	  if (g_worst > 128.0) ok = 0;
	}
      }
      report_worst(names[m]);
      unit_check(ok, "stream: stereo mode subbands match reference");
      mad_synth_finish(&sy);
      mad_frame_finish(&fr);
      mad_stream_finish(&st);
    }

    /* asymmetric input yields asymmetric left/right output */
    {
      unsigned char buf[2 * 144 + 8];
      unsigned n = build_stereo(buf, 0, 2, 1);
      struct mad_stream st;
      struct mad_frame fr;
      struct mad_synth sy;
      int asym = 0, idx;
      mad_stream_init(&st);
      mad_frame_init(&fr);
      mad_synth_init(&sy);
      mad_stream_buffer(&st, buf, n + 8);
      st.options = MAD_OPTION_MIXED8K_4SUBBANDS;
      if (mad_frame_decode(&fr, &st) == 0) {
	mad_synth_frame(&sy, &fr);
	for (idx = 0; idx < 576; ++idx)
	  if (sy.pcm.samples[0][idx] != sy.pcm.samples[1][idx]) { asym = 1; break; }
      }
      unit_check(asym, "stream: normal stereo stays left/right asymmetric");
      mad_synth_finish(&sy);
      mad_frame_finish(&fr);
      mad_stream_finish(&st);
    }
  }

  /* ---- legacy / no-op invariants ---- */
  {
    unsigned char buf[2 * 144 + 8];
    struct mad_stream st1, st2;
    struct mad_frame f1, f2;
    struct mad_synth s1, s2;
    int identical, idx;

    /* option-off determinism: two independent runs match bit for bit */
    mad_fixed_t a[6][576], b[6][576];
    unit_check(run_fixture(0, 1, a) == 0 && run_fixture(0, 1, b) == 0,
	       "stream: legacy option-off runs decode");
    identical = 1;
    for (k = 0; k < 6 && identical; ++k)
      for (i = 0; i < 576; ++i)
	if (a[k][i] != b[k][i]) { identical = 0; break; }
    unit_check(identical, "stream: option-off is deterministic");

    /* pure short 8 kHz: option on == option off */
    build_stereo(buf, 0, 2, 0);
    mad_stream_init(&st1); mad_stream_init(&st2);
    mad_frame_init(&f1); mad_frame_init(&f2);
    mad_synth_init(&s1); mad_synth_init(&s2);
    mad_stream_buffer(&st1, buf, sizeof buf);
    mad_stream_buffer(&st2, buf, sizeof buf);
    st1.options = MAD_OPTION_MIXED8K_4SUBBANDS;
    st2.options = 0;
    identical = (mad_frame_decode(&f1, &st1) == 0 &&
		 mad_frame_decode(&f2, &st2) == 0);
    if (identical) {
      mad_synth_frame(&s1, &f1);
      mad_synth_frame(&s2, &f2);
      for (idx = 0; idx < 576; ++idx)
	if (s1.pcm.samples[0][idx] != s2.pcm.samples[0][idx] ||
	    s1.pcm.samples[1][idx] != s2.pcm.samples[1][idx]) {
	  identical = 0; break;
	}
    }
    unit_check(identical, "stream: pure-short 8k unaffected by option");
    mad_synth_finish(&s1); mad_synth_finish(&s2);
    mad_frame_finish(&f1); mad_frame_finish(&f2);
    mad_stream_finish(&st1); mad_stream_finish(&st2);
  }
}
