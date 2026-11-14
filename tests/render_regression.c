/*
 * render_regression.c
 *
 * Content-level regression tests for TinySoundFont offline rendering.
 *
 * Method overview
 * ---------------
 * A small, legal SoundFont 2 file is generated in memory for every scenario.
 * Its PCM is a deterministic, aperiodic sequence of integer amplitudes (plus a
 * separate impulse font) so that reading the wrong neighbour at a loop seam,
 * or dropping filter state, changes actual sample values - not just energy.
 * The font is always exercised through the public API only:
 * tsf_load_memory -> tsf_set_output -> tsf_note_on -> tsf_render_float.
 * No internal region/voice structure is ever constructed by these tests.
 *
 * Expected audio is produced by an independent reference written from the
 * SF2/mathematical conventions alone:
 *   - fractional read pointer advanced by a pitch ratio derived from root key,
 *     coarse/fine tune, key and sample/output rates;
 *   - linear interpolation with explicit loop wrap for the +1 neighbour;
 *   - a second-order low-pass expressed in Direct Form I with explicit past
 *     input/output samples (not the production transposed-DF2 z1/z2 state),
 *     evaluated in double; when coefficients change per 64-frame effect block
 *     each delayed term keeps the coefficients in force when that sample
 *     entered, and bypassed samples leave the history frozen;
 *   - a triangle modulation LFO with start delay and level reflection, whose
 *     clock is advanced once per effect block (short tail blocks included),
 *     using the block-start level for coefficient calculation.
 * No production rendering helper is called or copied, and no library output is
 * recorded as golden data.
 *
 * Tolerance: the library works in float for interpolation/LFO levels while the
 * reference keeps the biquad state in double; expected agreement is bounded by
 * a few float ulp of values <= 1.  REF_TOL (2e-6) is ~34 ulp at magnitude 1 and
 * over 100x smaller than any content error the seeded mutations produce.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../task_support/check.h"
#include "tsf.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define REF_TOL 2.0e-6
#define EFFECT_BLOCK 64
#define OUT_RATE 22050
#define PCM_N 96

/* ------------------------------------------------------------------ */
/* Generic SF2 chunk writer (same RIFF block approach as font_fixture) */
/* ------------------------------------------------------------------ */

static void ff_u(FILE *f, unsigned x, int n) { while (n--) { fputc(x & 255, f); x >>= 8; } }

static long ff_begin(FILE *f, const char id[4])
{
	fwrite(id, 1, 4, f);
	long p = ftell(f);
	ff_u(f, 0, 4);
	return p;
}

static void ff_end(FILE *f, long p)
{
	long e = ftell(f);
	fseek(f, p, SEEK_SET);
	ff_u(f, (unsigned)(e - p - 4), 4);
	fseek(f, e, SEEK_SET);
	if ((e - p - 4) & 1) fputc(0, f);
}

static void ff_name(FILE *f, const char *s)
{
	char b[20];
	memset(b, 0, sizeof(b));
	strncpy(b, s, 19);
	fwrite(b, 1, 20, f);
}

static void ff_gen(FILE *f, unsigned op, unsigned amt) { ff_u(f, op, 2); ff_u(f, amt, 2); }

static void ff_shdr(FILE *f, const char *nm, unsigned start, unsigned end,
	unsigned ls, unsigned le, unsigned rate, unsigned char root,
	signed char corr, unsigned short link, unsigned short stype)
{
	ff_name(f, nm);
	ff_u(f, start, 4); ff_u(f, end, 4); ff_u(f, ls, 4); ff_u(f, le, 4);
	ff_u(f, rate, 4);
	fputc(root, f);
	fputc((unsigned char)corr, f);
	ff_u(f, link, 2); ff_u(f, stype, 2);
}

/* Aperiodic integer PCM: period 97 in the raw counter, so within PCM_N the
 * window never repeats; values around the loop seam differ from those outside. */
static short pcm_value(int i)
{
	return (short)((((i * 73 + 17) % 97) - 48) * 100);
}

struct font_cfg {
	int loop_mode;        /* 0 = no loop, 1 = continuous */
	unsigned loop_start, loop_end;
	unsigned sample_rate;
	int root;             /* shdr original pitch (MIDI key) */
	int coarse, fine;     /* gens 51/52 in semitones/cents */
	int corr;             /* shdr pitch correction, cents */
	int pan;              /* gen 17, tenths (-500..500) */
	int fc;               /* gen 8 initial filter fc, cents */
	int q;                /* gen 9 initial filter Q, centibel */
	int lfo_to_fc;        /* gen 10, cents */
	int lfo_freq;         /* gen 22, cents */
	double lfo_delay;     /* gen 21, seconds (<0 = leave default) */
	int impulse;          /* nonzero: replace PCM by a unit impulse */
};

/* Builds the font into a freshly malloc'd buffer (caller frees). */
static int build_font(const struct font_cfg *cfg, void **out_buf, int *out_size)
{
	FILE *f = tmpfile();
	long riff, list, c;
	unsigned i;
	if (!f) return 0;

	riff = ff_begin(f, "RIFF"); fwrite("sfbk", 1, 4, f);

	list = ff_begin(f, "LIST"); fwrite("INFO", 1, 4, f);
	c = ff_begin(f, "ifil"); ff_u(f, 2, 2); ff_u(f, 1, 2); ff_end(f, c);
	ff_end(f, list);

	list = ff_begin(f, "LIST"); fwrite("sdta", 1, 4, f);
	c = ff_begin(f, "smpl");
	for (i = 0; i < PCM_N; i++)
	{
		short s = cfg->impulse ? (i == 0 ? 16000 : 0) : pcm_value((int)i);
		ff_u(f, (unsigned short)s, 2);
	}
	for (i = 0; i < 46; i++) ff_u(f, 0, 2); /* standard ROM zero pad */
	ff_end(f, c);
	ff_end(f, list);

	list = ff_begin(f, "LIST"); fwrite("pdta", 1, 4, f);

	/* Preset hydra: one preset plus the required EOP record. */
	c = ff_begin(f, "phdr");
	ff_name(f, "RegPreset"); ff_u(f, 0, 2); ff_u(f, 0, 2); ff_u(f, 0, 2);
	ff_u(f, 0, 4); ff_u(f, 0, 4); ff_u(f, 0, 4);
	ff_name(f, "EOP"); ff_u(f, 0, 2); ff_u(f, 0, 2); ff_u(f, 1, 2);
	ff_u(f, 0, 4); ff_u(f, 0, 4); ff_u(f, 0, 4);
	ff_end(f, c);

	c = ff_begin(f, "pbag");
	ff_u(f, 0, 2); ff_u(f, 0, 2);
	ff_u(f, 1, 2); ff_u(f, 0, 2);
	ff_end(f, c);

	c = ff_begin(f, "pmod"); for (i = 0; i < 10; i++) fputc(0, f); ff_end(f, c);

	c = ff_begin(f, "pgen");
	ff_gen(f, 41, 0); /* instrument index 0 */
	ff_gen(f, 0, 0);  /* terminal */
	ff_end(f, c);

	/* Instrument hydra: one instrument plus the required EOI record. */
	c = ff_begin(f, "inst");
	ff_name(f, "RegInst"); ff_u(f, 0, 2);
	ff_name(f, "EOI"); ff_u(f, 1, 2);
	ff_end(f, c);

	c = ff_begin(f, "ibag");
	/* The EOI bag index equals the zone's generator count (terminal excluded). */
	int zone_gens = 1; /* sample id */
	if (cfg->fc) zone_gens++;
	if (cfg->q) zone_gens++;
	if (cfg->lfo_to_fc) zone_gens++;
	if (cfg->pan) zone_gens++;
	if (cfg->lfo_delay > 0.0) zone_gens++;
	if (cfg->lfo_freq) zone_gens++;
	if (cfg->coarse) zone_gens++;
	if (cfg->fine) zone_gens++;
	if (cfg->loop_mode) zone_gens++;
	ff_u(f, 0, 2); ff_u(f, 0, 2);
	ff_u(f, (unsigned)zone_gens, 2); ff_u(f, 0, 2);
	ff_end(f, c);

	c = ff_begin(f, "imod"); for (i = 0; i < 10; i++) fputc(0, f); ff_end(f, c);

	c = ff_begin(f, "igen");
	if (cfg->fc) ff_gen(f, 8, (unsigned)cfg->fc);
	if (cfg->q) ff_gen(f, 9, (unsigned)cfg->q);
	if (cfg->lfo_to_fc) ff_gen(f, 10, (unsigned)cfg->lfo_to_fc);
	if (cfg->pan) ff_gen(f, 17, (unsigned short)cfg->pan);
	if (cfg->lfo_delay > 0.0) ff_gen(f, 21, (unsigned short)(int)(1200.0 * log2(cfg->lfo_delay)));
	if (cfg->lfo_freq) ff_gen(f, 22, (unsigned short)cfg->lfo_freq);
	if (cfg->coarse) ff_gen(f, 51, (unsigned short)cfg->coarse);
	if (cfg->fine) ff_gen(f, 52, (unsigned short)cfg->fine);
	if (cfg->loop_mode) ff_gen(f, 54, 1);
	ff_gen(f, 53, 0); /* sample id 0 */
	ff_gen(f, 0, 0);  /* terminal */
	ff_end(f, c);

	c = ff_begin(f, "shdr");
	/* SF2 endLoop is one past the inclusive end; the loader subtracts 1. */
	ff_shdr(f, cfg->impulse ? "Impulse" : "Aperiodic",
		0, PCM_N - 1, cfg->loop_start, cfg->loop_mode ? cfg->loop_end + 1 : 0,
		cfg->sample_rate,
		(unsigned char)cfg->root, (signed char)cfg->corr, 0, 1);
	ff_shdr(f, "EOS", 0, 0, 0, 0, 0, 0, 0, 0, 0);
	ff_end(f, c);

	ff_end(f, list);
	ff_end(f, riff);

	{
		long n = ftell(f);
		void *b;
		rewind(f);
		if (n <= 0) { fclose(f); return 0; }
		b = malloc((size_t)n);
		if (!b) { fclose(f); return 0; }
		if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return 0; }
		fclose(f);
		*out_buf = b;
		*out_size = (int)n;
	}
	return 1;
}

static tsf *load_built_font(const struct font_cfg *cfg)
{
	void *buf; int size;
	tsf *f;
	if (!build_font(cfg, &buf, &size)) return NULL;
	f = tsf_load_memory(buf, size);
	free(buf);
	return f;
}

/* ------------------------------------------------------------------ */
/* Independent reference: low-pass, LFO, playhead and block rendering  */
/* ------------------------------------------------------------------ */

/* Independent second-order low-pass.
 *
 * The production filter keeps transposed-DF2 state (two mixed z terms).  This
 * reference instead uses Direct Form I with explicit input/output history:
 *   y[n] = a0 x[n] + a1 x[n-1] + a0 x[n-2] - b1 y[n-1] - b2 y[n-2]
 * When coefficients change between effect blocks, every delayed sample keeps
 * the coefficient set that was in effect when it entered the filter: the lag-1
 * terms use the previous sample's coefficients and the lag-2 terms those from
 * two samples back.  Bypassed samples are never pushed through, so the history
 * and its attached coefficients stay frozen until the filter re-enables. */
struct ref_lowpass {
	double qinv, a0, a1, b1, b2;
	double x1, x2, y1, y2;             /* explicit past inputs/outputs */
	double a0_1, a1_1, b1_1, b2_1;     /* coefficients at sample n-1 */
	double a0_2, a1_2, b1_2, b2_2;     /* coefficients at sample n-2 */
	int active;
};

static void ref_lp_setup(struct ref_lowpass *lp, double fc_norm, double qinv)
{
	double k = tan(M_PI * fc_norm), kk = k * k;
	double norm = 1.0 / (1.0 + k * qinv + kk);
	lp->qinv = qinv;
	lp->a0 = kk * norm;
	lp->a1 = 2.0 * lp->a0;
	lp->b1 = 2.0 * (kk - 1.0) * norm;
	lp->b2 = (1.0 - k * qinv + kk) * norm;
}

static float ref_lp_process(struct ref_lowpass *lp, double in)
{
	double out = lp->a0 * in + lp->a1_1 * lp->x1 + lp->a0_2 * lp->x2
		- lp->b1_1 * lp->y1 - lp->b2_2 * lp->y2;
	lp->x2 = lp->x1; lp->x1 = in;
	lp->y2 = lp->y1; lp->y1 = out;
	lp->a0_2 = lp->a0_1; lp->a1_2 = lp->a1_1; lp->b1_2 = lp->b1_1; lp->b2_2 = lp->b2_1;
	lp->a0_1 = lp->a0;   lp->a1_1 = lp->a1;   lp->b1_1 = lp->b1;   lp->b2_1 = lp->b2;
	return (float)out;
}

struct ref_lfo { int samples_until; float level, delta; };

static void ref_lfo_setup(struct ref_lfo *lfo, double delay_s, int freq_cents, double out_rate)
{
	float hz = 8.176f * powf(2.0f, freq_cents / 1200.0f);
	lfo->samples_until = (int)(delay_s * out_rate);
	lfo->delta = 4.0f * hz / (float)out_rate;
	lfo->level = 0.0f;
}

static void ref_lfo_process(struct ref_lfo *lfo, int block)
{
	if (lfo->samples_until > block) { lfo->samples_until -= block; return; }
	lfo->level += lfo->delta * (float)block;
	if (lfo->level > 1.0f) { lfo->delta = -lfo->delta; lfo->level = 2.0f - lfo->level; }
	else if (lfo->level < -1.0f) { lfo->delta = -lfo->delta; lfo->level = -2.0f - lfo->level; }
}

struct ref_voice {
	const struct font_cfg *cfg;
	int key;
	double out_rate;
	double pitch_ratio;
	double pos;
	struct ref_lowpass lp;
	struct ref_lfo lfo;
	int dynamic;
	int saw_active_blocks, saw_bypass_blocks, saw_lfo_turns;
};

static double ref_pitch_ratio(const struct font_cfg *cfg, int key, double out_rate)
{
	double note = key + cfg->coarse + (cfg->fine + cfg->corr) / 100.0;
	double adjusted = cfg->root + (note - cfg->root) * 1.0; /* key tracking 100 cents/key */
	return pow(2.0, adjusted * 100.0 / 1200.0)
		* cfg->sample_rate / (pow(2.0, cfg->root * 100.0 / 1200.0) * out_rate);
}

static double ref_source_sample(const struct ref_voice *v, int i)
{
	if (i < 0 || i >= PCM_N) return 0.0; /* past the natural end: silence */
	if (v->cfg->impulse) return (i == 0 ? 16000.0 : 0.0) / 32767.0;
	return pcm_value(i) / 32767.0;
}

static void ref_voice_init(struct ref_voice *v, const struct font_cfg *cfg, int key)
{
	float hz, fc_norm;
	v->cfg = cfg;
	v->key = key;
	v->out_rate = OUT_RATE;
	v->pitch_ratio = ref_pitch_ratio(cfg, key, v->out_rate);
	v->pos = 0.0;
	memset(&v->lp, 0, sizeof(v->lp));
	v->dynamic = (cfg->lfo_to_fc != 0);
	hz = 8.176f * powf(2.0f, cfg->fc / 1200.0f);
	fc_norm = hz / (float)v->out_rate;
	v->lp.active = (fc_norm < 0.499f);
	v->lp.qinv = pow(10.0, -cfg->q / 200.0);
	if (v->lp.active) ref_lp_setup(&v->lp, (double)fc_norm, v->lp.qinv);
	if (v->dynamic)
		ref_lfo_setup(&v->lfo, cfg->lfo_delay, cfg->lfo_freq, v->out_rate);
	v->saw_active_blocks = v->saw_bypass_blocks = v->saw_lfo_turns = 0;
}

/* One interpolation step with explicit loop neighbour handling. */
static double ref_read(struct ref_voice *v)
{
	const struct font_cfg *cfg = v->cfg;
	unsigned ipos = (unsigned)v->pos;
	int looping = (cfg->loop_mode && cfg->loop_start < cfg->loop_end);
	unsigned next = (looping && ipos >= cfg->loop_end) ? cfg->loop_start : ipos + 1;
	float alpha = (float)(v->pos - (double)ipos);
	double s0 = ref_source_sample(v, (int)ipos);
	double s1 = ref_source_sample(v, (int)next);
	return s0 * (1.0 - (double)alpha) + s1 * (double)alpha;
}

static void ref_advance(struct ref_voice *v)
{
	const struct font_cfg *cfg = v->cfg;
	int looping = (cfg->loop_mode && cfg->loop_start < cfg->loop_end);
	v->pos += v->pitch_ratio;
	if (looping && v->pos >= (double)cfg->loop_end + 1.0)
		v->pos -= (double)(cfg->loop_end - cfg->loop_start + 1);
}

/* Renders the pre-gain voice signal following an explicit call plan (each call
 * is split internally into <=64-frame effect blocks, exactly like the library). */
static void ref_render(struct ref_voice *v, const int *calls, int n_calls, float *out)
{
	const struct font_cfg *cfg = v->cfg;
	int c, total = 0;
	for (c = 0; c < n_calls; c++)
	{
		int remaining = calls[c];
		while (remaining > 0)
		{
			int block = remaining > EFFECT_BLOCK ? EFFECT_BLOCK : remaining;
			int k;
			if (v->dynamic)
			{
				float fres = (float)cfg->fc + v->lfo.level * (float)cfg->lfo_to_fc;
				float fc_norm = (fres <= 13500.0f
					? 8.176f * powf(2.0f, fres / 1200.0f) / (float)v->out_rate
					: 1.0f);
				v->lp.active = (fc_norm < 0.499f);
				if (v->lp.active) ref_lp_setup(&v->lp, (double)fc_norm, v->lp.qinv);
			}
			if (v->lp.active) v->saw_active_blocks++; else v->saw_bypass_blocks++;
			if (v->dynamic)
			{
				float prev_delta = v->lfo.delta;
				ref_lfo_process(&v->lfo, block);
				if (v->lfo.delta != prev_delta) v->saw_lfo_turns++;
			}
			for (k = 0; k < block; k++)
			{
				float val = 0.0f;
				if (v->pos < (double)PCM_N)
				{
					val = (float)ref_read(v);
					if (v->lp.active) val = ref_lp_process(&v->lp, val);
					ref_advance(v);
				}
				out[total++] = val;
			}
			remaining -= block;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Library rendering and content comparison helpers                    */
/* ------------------------------------------------------------------ */

static int plan_total(const int *calls, int n_calls)
{
	int i, t = 0;
	for (i = 0; i < n_calls; i++) t += calls[i];
	return t;
}

static void lib_render(tsf *f, int channels, float *buf, const int *calls, int n_calls, int mixing)
{
	int i, off = 0;
	for (i = 0; i < n_calls; i++)
	{
		tsf_render_float(f, buf + (size_t)off * channels, calls[i], mixing);
		off += calls[i];
	}
}

/* Returns 1 when every finite sample agrees; on the first mismatch prints the
 * sample index, expected value, actual value and count already compared. */
static int compare_content(const char *tag, const float *expected, const float *actual, int n)
{
	int i;
	for (i = 0; i < n; i++)
	{
		if (!isfinite(actual[i]) || !isfinite(expected[i]))
		{
			fprintf(stderr, "FAIL %s non-finite sample=%d expected=%.9g actual=%.9g compared=%d\n",
				tag, i, expected[i], actual[i], i);
			return 0;
		}
		if (fabs((double)expected[i] - (double)actual[i]) > REF_TOL)
		{
			fprintf(stderr,
				"FAIL %s content sample=%d expected=%.9g actual=%.9g compared=%d\n",
				tag, i, expected[i], actual[i], i);
			return 0;
		}
	}
	return 1;
}

static tsf *start_voice_key(const struct font_cfg *cfg, enum TSFOutputMode mode, int key)
{
	tsf *f = load_built_font(cfg);
	if (!f) return NULL;
	tsf_set_output(f, mode, OUT_RATE, 0.0f);
	if (!tsf_note_on(f, 0, key, 1.0f)) { tsf_close(f); return NULL; }
	return f;
}

static tsf *start_voice(const struct font_cfg *cfg, enum TSFOutputMode mode)
{
	return start_voice_key(cfg, mode, 60);
}

/* ================================================================== */
/* Group sample_loop                                                    */
/* ================================================================== */

static int case_loop_fractional_seam(void)
{
	/* r = 1.3: fractional playhead, repeated wraps, non-integer seam points. */
	struct font_cfg cfg;
	int calls[] = { 64, 56 };
	int n = plan_total(calls, 2);
	float *got = malloc(sizeof(float) * n), *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	tsf *f;
	memset(&cfg, 0, sizeof(cfg));
	cfg.loop_mode = 1; cfg.loop_start = 16; cfg.loop_end = 47;
	cfg.sample_rate = (unsigned)(OUT_RATE * 1.3);
	cfg.root = 60; cfg.fc = 13500; /* filter inactive: isolate the playhead */
	TASK_CHECK(got && exp);
	ref_voice_init(&ref, &cfg, 60);
	ref_render(&ref, calls, 2, exp);
	f = start_voice(&cfg, TSF_MONO);
	TASK_CHECK(f != NULL);
	lib_render(f, 1, got, calls, 2, 0);
	TASK_CHECK(compare_content("loop_fractional_seam", exp, got, n));
	/* The voice must keep looping instead of ending naturally. */
	TASK_CHECK(tsf_active_voice_count(f) == 1);
	tsf_close(f);
	free(got); free(exp);
	return 0;
}

static int case_loop_pitch_params(void)
{
	/* Root key, coarse tune, fine tune and shdr pitch correction all enter the
	 * pitch ratio, together with a non-unity sample rate. */
	struct font_cfg cfg;
	int calls[] = { 20, 64, 64, 13 }; /* short tail calls advance clocks too */
	int n = plan_total(calls, 4);
	float *got = malloc(sizeof(float) * n), *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	tsf *f;
	double expected_ratio;
	memset(&cfg, 0, sizeof(cfg));
	cfg.loop_mode = 1; cfg.loop_start = 8; cfg.loop_end = 39;
	cfg.sample_rate = 24000; cfg.root = 57;
	cfg.coarse = -1; cfg.fine = 35; cfg.corr = -12; cfg.fc = 13500;
	expected_ratio = ref_pitch_ratio(&cfg, 64, OUT_RATE);
	TASK_CHECK(got && exp);
	TASK_CHECK(expected_ratio > 1.0 && expected_ratio < 2.0);
	ref_voice_init(&ref, &cfg, 64);
	TASK_CHECK(fabs(ref.pitch_ratio - expected_ratio) < 1e-15);
	ref_render(&ref, calls, 4, exp);
	f = start_voice_key(&cfg, TSF_MONO, 64);
	TASK_CHECK(f != NULL);
	lib_render(f, 1, got, calls, 4, 0);
	TASK_CHECK(compare_content("loop_pitch_params", exp, got, n));
	tsf_close(f);
	free(got); free(exp);
	return 0;
}

static int case_nonloop_natural_end(void)
{
	/* No loop: playback runs to the sample end and then emits silence. */
	struct font_cfg cfg;
	int calls[] = { 64, 64, 12 };
	int n = plan_total(calls, 3), i;
	float *got = malloc(sizeof(float) * n), *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	tsf *f;
	memset(&cfg, 0, sizeof(cfg));
	cfg.sample_rate = OUT_RATE; cfg.root = 60; cfg.fc = 13500; /* r = 1.0 */
	TASK_CHECK(got && exp);
	ref_voice_init(&ref, &cfg, 60);
	TASK_CHECK(fabs(ref.pitch_ratio - 1.0) < 1e-15);
	ref_render(&ref, calls, 3, exp);
	f = start_voice(&cfg, TSF_MONO);
	TASK_CHECK(f != NULL);
	lib_render(f, 1, got, calls, 3, 0);
	TASK_CHECK(compare_content("nonloop_natural_end", exp, got, n));
	/* Last real sample, then guaranteed silence once the playhead passes end. */
	TASK_CHECK(fabs(got[PCM_N - 1] - pcm_value(PCM_N - 1) / 32767.0) < REF_TOL);
	for (i = PCM_N; i < n; i++) TASK_CHECK(got[i] == 0.0f);
	TASK_CHECK(tsf_active_voice_count(f) == 0);
	tsf_close(f);
	free(got); free(exp);
	return 0;
}

/* ================================================================== */
/* Group filter_history                                                 */
/* ================================================================== */

static int case_filter_impulse(void)
{
	/* Fixed cutoff on a unit impulse: the whole ringing tail is filter memory. */
	struct font_cfg cfg;
	int calls[] = { 64, 64, 64, 8 };
	int n = plan_total(calls, 4);
	float *got = malloc(sizeof(float) * n), *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	struct ref_lowpass probe;
	tsf *f;
	memset(&cfg, 0, sizeof(cfg));
	cfg.impulse = 1; cfg.sample_rate = OUT_RATE; cfg.root = 60;
	cfg.fc = 5500; cfg.q = 200; /* ~198 Hz, QInv = 0.1 */
	TASK_CHECK(got && exp);
	ref_voice_init(&ref, &cfg, 60);
	ref_render(&ref, calls, 4, exp);
	/* Independent hand check of the first recurrence: zero history => y0=a0*x. */
	memset(&probe, 0, sizeof(probe));
	probe.qinv = pow(10.0, -cfg.q / 200.0);
	ref_lp_setup(&probe, (double)(8.176f * powf(2.0f, cfg.fc / 1200.0f) / OUT_RATE), probe.qinv);
	TASK_CHECK(fabs(exp[0] - (float)(16000.0 / 32767.0 * probe.a0)) < REF_TOL);
	TASK_CHECK(fabs(exp[1]) > 0.0f); /* tail driven only by retained state */
	f = start_voice(&cfg, TSF_MONO);
	TASK_CHECK(f != NULL);
	lib_render(f, 1, got, calls, 4, 0);
	TASK_CHECK(compare_content("filter_impulse", exp, got, n));
	tsf_close(f);
	free(got); free(exp);
	return 0;
}

static int case_filter_aperiodic_fixed(void)
{
	/* Fixed cutoff on the aperiodic PCM, across several render calls. */
	struct font_cfg cfg;
	int calls[] = { 30, 64, 64, 27 };
	int n = plan_total(calls, 4);
	float *got = malloc(sizeof(float) * n), *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	tsf *f;
	memset(&cfg, 0, sizeof(cfg));
	cfg.loop_mode = 1; cfg.loop_start = 12; cfg.loop_end = 43;
	cfg.sample_rate = OUT_RATE; cfg.root = 60;
	cfg.fc = 6000; cfg.q = 400;
	TASK_CHECK(got && exp);
	ref_voice_init(&ref, &cfg, 60);
	TASK_CHECK(ref.lp.active);
	ref_render(&ref, calls, 4, exp);
	f = start_voice(&cfg, TSF_MONO);
	TASK_CHECK(f != NULL);
	lib_render(f, 1, got, calls, 4, 0);
	TASK_CHECK(compare_content("filter_aperiodic_fixed", exp, got, n));
	tsf_close(f);
	free(got); free(exp);
	return 0;
}

static int case_filter_lfo_bypass(void)
{
	/* Triangle LFO swings the cutoff above the bypass threshold and back, with
	 * a start delay; bypassed blocks must not consume filter history. */
	struct font_cfg cfg;
	int calls[] = { 64, 64, 64, 64, 64, 64, 40 };
	int n = plan_total(calls, 7);
	float *got = malloc(sizeof(float) * n), *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	tsf *f;
	memset(&cfg, 0, sizeof(cfg));
	cfg.loop_mode = 1; cfg.loop_start = 10; cfg.loop_end = 41;
	cfg.sample_rate = OUT_RATE; cfg.root = 60;
	cfg.fc = 10500; cfg.q = 300;
	cfg.lfo_to_fc = 3500; cfg.lfo_freq = 3000; cfg.lfo_delay = 0.01;
	TASK_CHECK(got && exp);
	ref_voice_init(&ref, &cfg, 60);
	TASK_CHECK(ref.dynamic);
	ref_render(&ref, calls, 7, exp);
	/* The scenario really has to traverse delay, active, bypass and a turn. */
	TASK_CHECK(ref.saw_active_blocks > 0);
	TASK_CHECK(ref.saw_bypass_blocks > 0);
	TASK_CHECK(ref.saw_lfo_turns >= 1);
	f = start_voice(&cfg, TSF_MONO);
	TASK_CHECK(f != NULL);
	lib_render(f, 1, got, calls, 7, 0);
	TASK_CHECK(compare_content("filter_lfo_bypass", exp, got, n));
	tsf_close(f);
	free(got); free(exp);
	return 0;
}

static int case_filter_reenable_history(void)
{
	/* Active -> bypass -> active: after re-enabling, the retained delay terms
	 * must still be present (a per-block reset diverges from this tail). */
	struct font_cfg cfg;
	int calls[] = { 64, 64, 64, 64, 48 };
	int n = plan_total(calls, 5);
	float *got = malloc(sizeof(float) * n), *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	tsf *f;
	int i, reenable_seen = 0;
	memset(&cfg, 0, sizeof(cfg));
	cfg.loop_mode = 1; cfg.loop_start = 14; cfg.loop_end = 45;
	cfg.sample_rate = OUT_RATE; cfg.root = 60;
	cfg.fc = 7000; cfg.q = 250;
	cfg.lfo_to_fc = 7000; cfg.lfo_freq = 3800; cfg.lfo_delay = 0.002;
	TASK_CHECK(got && exp);
	ref_voice_init(&ref, &cfg, 60);
	ref_render(&ref, calls, 5, exp);
	TASK_CHECK(ref.saw_active_blocks > 1 && ref.saw_bypass_blocks > 0);
	f = start_voice(&cfg, TSF_MONO);
	TASK_CHECK(f != NULL);
	lib_render(f, 1, got, calls, 5, 0);
	TASK_CHECK(compare_content("filter_reenable_history", exp, got, n));
	/* Content after the bypass window must be nonzero (state carried forward). */
	for (i = n - 16; i < n; i++) if (got[i] != 0.0f) reenable_seen = 1;
	TASK_CHECK(reenable_seen);
	tsf_close(f);
	free(got); free(exp);
	return 0;
}

static int case_filter_seam_and_coeff(void)
{
	/* Combined path: fractional loop wraps while cutoff coefficients update. */
	struct font_cfg cfg;
	int calls[] = { 64, 64, 37 };
	int n = plan_total(calls, 3);
	float *got = malloc(sizeof(float) * n), *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	tsf *f;
	memset(&cfg, 0, sizeof(cfg));
	cfg.loop_mode = 1; cfg.loop_start = 16; cfg.loop_end = 47;
	cfg.sample_rate = (unsigned)(OUT_RATE * 1.3); cfg.root = 60;
	cfg.fc = 8000; cfg.q = 350;
	cfg.lfo_to_fc = 2500; cfg.lfo_freq = 4000; cfg.lfo_delay = 0.0;
	TASK_CHECK(got && exp);
	ref_voice_init(&ref, &cfg, 60);
	ref_render(&ref, calls, 3, exp);
	TASK_CHECK(ref.saw_lfo_turns >= 1);
	f = start_voice(&cfg, TSF_MONO);
	TASK_CHECK(f != NULL);
	lib_render(f, 1, got, calls, 3, 0);
	TASK_CHECK(compare_content("filter_seam_and_coeff", exp, got, n));
	tsf_close(f);
	free(got); free(exp);
	return 0;
}

/* ================================================================== */
/* Group layouts_mix                                                    */
/* ================================================================== */

/* Pan value shared by the layout case: gen 17 amount 200 -> 0.2 in region. */
static double cfg_pan_value(void) { return 0.2; }

static int case_layouts_relations(void)
{
	/* Same note and call plan through all three output modes, off-center pan,
	 * with a looping filtered voice so history crosses every render call. */
	struct font_cfg cfg;
	int calls[] = { 64, 30, 64, 7, 50 };
	int n = plan_total(calls, 5), i;
	float *mono = malloc(sizeof(float) * n);
	float *inter = malloc(sizeof(float) * 2 * n);
	float *plane = malloc(sizeof(float) * 2 * n);
	float *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	tsf *fm, *fi, *fu;
	double pan = cfg_pan_value();
	double pl = sqrt(0.5 - pan), pr = sqrt(0.5 + pan);
	memset(&cfg, 0, sizeof(cfg));
	cfg.loop_mode = 1; cfg.loop_start = 12; cfg.loop_end = 43;
	cfg.sample_rate = OUT_RATE; cfg.root = 60; cfg.pan = 200;
	cfg.fc = 6500; cfg.q = 300;
	TASK_CHECK(mono && inter && plane && exp);
	ref_voice_init(&ref, &cfg, 60);
	ref_render(&ref, calls, 5, exp);

	fm = start_voice(&cfg, TSF_MONO);
	fi = start_voice(&cfg, TSF_STEREO_INTERLEAVED);
	fu = start_voice(&cfg, TSF_STEREO_UNWEAVED);
	TASK_CHECK(fm && fi && fu);
	lib_render(fm, 1, mono, calls, 5, 0);
	lib_render(fi, 2, inter, calls, 5, 0);
	lib_render(fu, 2, plane, calls, 5, 0);

	/* UNWEAVED stores L then R inside each render call, so map per call. */
	{
		int c, k = 0;
		for (c = 0; c < 5; c++)
		{
			int m = calls[c], base = 2 * k, j;
			for (j = 0; j < m; j++, k++)
			{
				double el = exp[k] * pl, er = exp[k] * pr;
				double em = exp[k]; /* mono branch ignores pan, gain stays 1 */
				TASK_CHECK(fabs(inter[2 * k] - plane[base + j]) < REF_TOL);
				TASK_CHECK(fabs(inter[2 * k + 1] - plane[base + m + j]) < REF_TOL);
				TASK_CHECK(fabs(inter[2 * k] - el) < REF_TOL);
				TASK_CHECK(fabs(inter[2 * k + 1] - er) < REF_TOL);
				TASK_CHECK(fabs(mono[k] - em) < REF_TOL);
			}
		}
	}
	/* Off-center pan must actually separate the channels once the filter has
	 * built up signal (its first frame starts from zero state). */
	{
		double max_sep = 0.0;
		for (i = 0; i < n; i++)
		{
			double sep = fabs(inter[2 * i] - inter[2 * i + 1]);
			if (sep > max_sep) max_sep = sep;
		}
		TASK_CHECK(max_sep > 1e-3);
	}
	TASK_CHECK(fabs(pl / pr - sqrt((0.5 - pan) / (0.5 + pan))) < 1e-15);
	tsf_close(fm); tsf_close(fi); tsf_close(fu);
	free(mono); free(inter); free(plane); free(exp);
	return 0;
}

static int case_mixing_overwrite_accumulate(void)
{
	/* Known nonzero buffer proves overwrite (mix=0) vs accumulation (mix=1). */
	struct font_cfg cfg;
	int calls[] = { 64, 40 };
	int n = plan_total(calls, 2), i;
	const float sentinel = 0.25f;
	float *fresh = malloc(sizeof(float) * n);
	float *mixed = malloc(sizeof(float) * n);
	float *over = malloc(sizeof(float) * n);
	float *exp = malloc(sizeof(float) * n);
	struct ref_voice ref;
	tsf *f1, *f2, *f3;
	memset(&cfg, 0, sizeof(cfg));
	cfg.sample_rate = OUT_RATE; cfg.root = 60; cfg.fc = 13500;
	TASK_CHECK(fresh && mixed && over && exp);
	ref_voice_init(&ref, &cfg, 60);
	ref_render(&ref, calls, 2, exp);

	f1 = start_voice(&cfg, TSF_MONO);
	f2 = start_voice(&cfg, TSF_MONO);
	f3 = start_voice(&cfg, TSF_MONO);
	TASK_CHECK(f1 && f2 && f3);
	for (i = 0; i < n; i++) { fresh[i] = 0.0f; mixed[i] = sentinel; over[i] = sentinel; }
	lib_render(f1, 1, fresh, calls, 2, 0);
	lib_render(f2, 1, mixed, calls, 2, 1);
	tsf_render_float(f3, over, n, 0); /* separate overwrite render */
	for (i = 0; i < n; i++)
	{
		TASK_CHECK(fabs(fresh[i] - exp[i]) < REF_TOL);
		TASK_CHECK(fabs(mixed[i] - (sentinel + exp[i])) < REF_TOL);
		TASK_CHECK(fabs(over[i] - exp[i]) < REF_TOL);
	}
	/* After the non-looping voice ends, mixing keeps the sentinel but the
	 * overwrite render leaves silence. */
	TASK_CHECK(mixed[n - 1] == sentinel);
	TASK_CHECK(over[n - 1] == 0.0f);
	tsf_close(f1); tsf_close(f2); tsf_close(f3);
	free(fresh); free(mixed); free(over); free(exp);
	return 0;
}

const struct task_case task_cases[] = {
	{ "sample_loop", "fractional_seam_wraps", case_loop_fractional_seam },
	{ "sample_loop", "pitch_parameters", case_loop_pitch_params },
	{ "sample_loop", "nonloop_natural_end", case_nonloop_natural_end },
	{ "filter_history", "impulse_fixed_cutoff", case_filter_impulse },
	{ "filter_history", "aperiodic_fixed_cutoff", case_filter_aperiodic_fixed },
	{ "filter_history", "lfo_delay_bypass", case_filter_lfo_bypass },
	{ "filter_history", "bypass_reenable_state", case_filter_reenable_history },
	{ "filter_history", "seam_with_coeff_update", case_filter_seam_and_coeff },
	{ "layouts_mix", "mode_content_relations", case_layouts_relations },
	{ "layouts_mix", "mixing_overwrite_accumulate", case_mixing_overwrite_accumulate },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
