/*
 * render_regression.c
 *
 * Content-level regression tests for TinySoundFont rendering.
 *
 * Every test drives the *public* API only (tsf_load_memory -> tsf_set_output ->
 * tsf_note_on -> tsf_render_float) on a small, legal SF2 fixture that this file
 * generates itself from integer PCM. No internal region/voice structs are built
 * by the tests. The expected output is rebuilt from scratch in double precision
 * (fractional read pointer, linear interpolation, loop wrap, an independently
 * written biquad low-pass and a triangle LFO); it never calls or copies a
 * production render helper and no library output is recorded as golden data.
 *
 * Three groups are registered with task_support/check.h:
 *   sample_loop     - fractional read pointer, loop seam wraps, non-loop end
 *   filter_history  - fixed/varying low-pass, bypass, LFO clock, history keep
 *   layouts_mix     - mono / interleaved / planar layout relations, mixing
 *
 * The production implementation is left untouched. Only the C standard library
 * and libm are used.
 *
 * Method notes (the reference uses plain equations and a different state layout
 * than the production transposed-form filter):
 *
 *  - Read pointer: pos_n advances by pitchRatio each output frame. Linear
 *    interpolation mixes pcm[pos] and pcm[next], where next wraps to the loop
 *    start when pos is the last loop point. Once pos reaches loopEnd+1 the
 *    pointer is reduced by the loop length. A non-loop sample stops at its end
 *    and emits silence thereafter.
 *  - Filter: the earlevel RBJ low-pass coefficients are derived from the cutoff
 *    (Hz / output rate) and Q, then evaluated through the canonical difference
 *    equation with explicit input/output histories. Each history entry carries
 *    the coefficient snapshot in force when the sample entered the filter, so a
 *    later coefficient change never re-prices an already consumed sample; this
 *    is algebraically equivalent to the engine's frozen transposed state but
 *    does not reuse that state form. A bypassed block passes the signal through
 *    and does not consume filter history.
 *  - Effects timing: each render is processed in <=64-frame blocks. The block
 *    start LFO level sets the cutoff/coefficients, then the LFO clock advances
 *    for the block (a short final block still clocks it). The reference follows
 *    the exact same call plan, so split boundaries are part of the expectation
 *    rather than being assumed modulation-invariant.
 *  - Tolerance: the engine performs float (32-bit) interpolation/filtering while
 *    the reference uses double; an absolute bound of 2e-5 (plus a 1e-5 relative
 *    term for the larger layout values) covers that precision gap with margin,
 *    and is orders of magnitude below the content errors the mutants inject
 *    (wrong loop neighbour or a cleared filter history).
 */

#include "tsf.h"
#include "../task_support/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Fixture geometry and synthesis parameters                          */
/* ------------------------------------------------------------------ */

#define OUT_RATE        22050          /* output sample rate                 */
#define FONT_RATE       44100          /* sample rate stored in the SF2      */
#define ROOT_KEY        60             /* original pitch / overriding root   */
#define FINE_TUNE       7              /* pitchCorrection and FineTune, cents*/
#define COARSE_TUNE     1              /* CoarseTune generator, semitones    */
#define REGION_PAN      0.30f          /* non-centre instrument pan          */

#define LOOP_N          32             /* samples inside the loop            */
#define SAMPLE_N        64             /* looped sample length               */
#define ROM_N           48             /* one-shot aperiodic sample length   */
#define IMP_N           128            /* impulse sample length              */
#define GUARD_N         8              /* SF2 mandated room after the sample */

#define LOOP_START      12             /* absolute loop start index          */
#define LOOP_END        (LOOP_START + LOOP_N - 1)  /* last loop point = 43   */

#define BLOCK           64             /* TSF_RENDER_EFFECTSAMPLEBLOCK       */

/* Filter / LFO fixture configuration (cents, as the SF2 generators use). */
#define FILT_FC_CENTS   6000           /* initial cutoff, cents              */
#define FILT_Q_CENTS    60             /* initial filter Q, tenths of a dB   */
#define LFO_FREQ_CENTS  4000           /* mod LFO frequency, cents           */
#define LFO_DELAY_TC    (-6773)        /* ~0.02 s: 1200 * log2(0.02)         */
#define LFO_TO_FC       7000           /* depth large enough to cross bypass */

/* Floating point comparison bounds (see method notes). */
#define ABS_TOL         2.0e-5
#define REL_TOL         1.0e-5

enum { PCM_LOOP = 0, PCM_ONESHOT = 1, PCM_IMPULSE = 2 };
enum { FILT_NONE = 0, FILT_STATIC = 1, FILT_DYNAMIC = 2 };

/* ------------------------------------------------------------------ */
/* Minimal SF2 writer (block helpers modelled on checks/font_fixture.h) */
/* ------------------------------------------------------------------ */

static void w_u(FILE *f, unsigned x, int n) { while (n--) { fputc(x & 255, f); x >>= 8; } }
static long w_begin(FILE *f, const char id[4])
{
	fwrite(id, 1, 4, f);
	long p = ftell(f);
	w_u(f, 0, 4);
	return p;
}
static void w_end(FILE *f, long p)
{
	long e = ftell(f);
	fseek(f, p, SEEK_SET);
	w_u(f, (unsigned)(e - p - 4), 4);
	fseek(f, e, SEEK_SET);
	if ((e - p - 4) & 1) fputc(0, f);
}
static void w_name(FILE *f, const char *s)
{
	char b[20];
	memset(b, 0, sizeof(b));
	strncpy(b, s, 19);
	fwrite(b, 1, 20, f);
}
static void w_gen(FILE *f, unsigned op, unsigned amt) { w_u(f, op, 2); w_u(f, amt, 2); }
static void w_shdr(FILE *f, const char *nm, unsigned en, unsigned ls, unsigned le,
                   unsigned rate, unsigned char root, signed char corr)
{
	w_name(f, nm);
	w_u(f, 0, 4);                       /* start */
	w_u(f, en, 4); w_u(f, ls, 4); w_u(f, le, 4); w_u(f, rate, 4);
	fputc(root, f); fputc((unsigned char)corr, f);
	w_u(f, 0, 2);                       /* sampleLink */
	w_u(f, 1, 2);                       /* sampleType: mono (1) */
}

/* Integer, deliberately aperiodic PCM. Each sequence is a modular permutation
 * (step coprime to its length), so every index holds a distinct value, the mean
 * is near zero (no DC ramp through the low-pass), and successive values are
 * pseudo-random rather than periodic. The point just outside the loop differs
 * strongly from the loop start, so a read that picks an out-of-loop neighbour
 * cannot match the reference. */
static short loop_pcm(int i)
{
	int p = (17 * i + 5) % 64;            /* gcd(17,64)=1 -> bijection */
	return (short)((p - 32) * 150);
}
static short rom_pcm(int i)
{
	int p = (17 * i + 3) % 48;            /* gcd(17,48)=1 -> bijection */
	return (short)((p - 24) * 200);
}

struct font_opts
{
	int pcm_kind;       /* PCM_LOOP / PCM_ONESHOT / PCM_IMPULSE */
	int filter;         /* FILT_NONE / FILT_STATIC / FILT_DYNAMIC */
	int lfo_delay;      /* nonzero -> give the mod LFO a delay */
	float pan;
};

static int font_sample_len(int kind)
{
	if (kind == PCM_LOOP) return SAMPLE_N;
	if (kind == PCM_ONESHOT) return ROM_N;
	return IMP_N;
}

/* Build a legal one-preset / one-instrument / one-region SF2 in memory. The
 * returned buffer may be freed immediately (the loader copies the PCM). */
static tsf *build_font(const struct font_opts *o, void **out_buf, int *out_size)
{
	int len = font_sample_len(o->pcm_kind);
	int looped = (o->pcm_kind == PCM_LOOP);
	FILE *f = tmpfile();
	if (!f) return NULL;

	long riff = w_begin(f, "RIFF");
	fwrite("sfbk", 1, 4, f);

	/* INFO */
	long list = w_begin(f, "LIST");
	fwrite("INFO", 1, 4, f);
	long c = w_begin(f, "ifil");
	w_u(f, 2, 2); w_u(f, 1, 2);
	w_end(f, c);
	w_end(f, list);

	/* sdta: one sample followed by the required zero room. */
	list = w_begin(f, "LIST");
	fwrite("sdta", 1, 4, f);
	c = w_begin(f, "smpl");
	for (int i = 0; i < len; i++)
	{
		short v;
		if (o->pcm_kind == PCM_LOOP) v = loop_pcm(i);
		else if (o->pcm_kind == PCM_ONESHOT) v = rom_pcm(i);
		else v = (i == 0 ? 32767 : 0);         /* unit impulse */
		w_u(f, (unsigned short)v, 2);
	}
	for (int i = 0; i < GUARD_N; i++) w_u(f, 0, 2);
	w_end(f, c);
	w_end(f, list);

	/* pdta */
	list = w_begin(f, "LIST");
	fwrite("pdta", 1, 4, f);

	/* Count the instrument-zone generators so the terminal ibag record can
	 * point just past them (SF2 bag ranges are [bag[i].ndx, bag[i+1].ndx). */
	int igen_count = 5; /* pan, coarse tune, fine tune, sample mode, sample id */
	if (o->filter == FILT_STATIC || o->filter == FILT_DYNAMIC) igen_count += 2;
	if (o->filter == FILT_DYNAMIC) igen_count += 3;

	/* phdr: one preset + EOP (EOP bag index points past the single preset bag) */
	c = w_begin(f, "phdr");
	w_name(f, "Regr"); w_u(f, 0, 2); w_u(f, 0, 2); w_u(f, 0, 2);
	w_u(f, 0, 4); w_u(f, 0, 4); w_u(f, 0, 4);
	w_name(f, "EOP"); w_u(f, 0, 2); w_u(f, 0, 2); w_u(f, 1, 2);
	w_u(f, 0, 4); w_u(f, 0, 4); w_u(f, 0, 4);
	w_end(f, c);

	/* pbag: one preset bag (genNdx 0) + terminal (genNdx 1, past the one pgen) */
	c = w_begin(f, "pbag");
	w_u(f, 0, 2); w_u(f, 0, 2);
	w_u(f, 1, 2); w_u(f, 0, 2);
	w_end(f, c);

	/* pmod: one empty record */
	c = w_begin(f, "pmod");
	for (int i = 0; i < 10; i++) fputc(0, f);
	w_end(f, c);

	/* pgen: preset bag selects instrument 0, then terminal */
	c = w_begin(f, "pgen");
	w_gen(f, 41, 0);
	w_gen(f, 0, 0);
	w_end(f, c);

	/* inst: one instrument + EOI */
	c = w_begin(f, "inst");
	w_name(f, "Inst"); w_u(f, 0, 2);
	w_name(f, "EOI"); w_u(f, 1, 2);
	w_end(f, c);

	/* ibag: one instrument zone (genNdx 0) + terminal (genNdx past the zone) */
	c = w_begin(f, "ibag");
	w_u(f, 0, 2); w_u(f, 0, 2);
	w_u(f, (unsigned)igen_count, 2); w_u(f, 0, 2);
	w_end(f, c);

	/* imod: one empty record */
	c = w_begin(f, "imod");
	for (int i = 0; i < 10; i++) fputc(0, f);
	w_end(f, c);

	/* igen: the single instrument zone. */
	c = w_begin(f, "igen");
	if (o->filter == FILT_STATIC || o->filter == FILT_DYNAMIC)
	{
		w_gen(f, 8, (unsigned short)FILT_FC_CENTS);   /* InitialFilterFc  */
		w_gen(f, 9, (unsigned short)FILT_Q_CENTS);    /* InitialFilterQ   */
	}
	if (o->filter == FILT_DYNAMIC)
	{
		w_gen(f, 21, (unsigned short)(o->lfo_delay ? LFO_DELAY_TC : -12000)); /* DelayModLFO */
		w_gen(f, 22, (unsigned short)LFO_FREQ_CENTS); /* FreqModLFO       */
		w_gen(f, 10, (unsigned short)LFO_TO_FC);      /* ModLfoToFilterFc */
	}
	w_gen(f, 17, (unsigned short)(int)(o->pan * 1000.0f)); /* Pan           */
	w_gen(f, 51, (unsigned short)COARSE_TUNE);      /* CoarseTune         */
	w_gen(f, 52, (unsigned short)FINE_TUNE);        /* FineTune           */
	w_gen(f, 54, (unsigned short)(looped ? 1 : 0)); /* SampleModes        */
	w_gen(f, 53, 0);                                /* SampleID -> shdr 0 */
	w_gen(f, 0, 0);                                 /* terminal           */
	w_end(f, c);

	/* shdr: the referenced sample first, then EOS. */
	c = w_begin(f, "shdr");
	if (looped)
		/* shdr.endLoop is exclusive; TSF subtracts 1 to get the inclusive end. */
		w_shdr(f, "Loop", SAMPLE_N - 1, LOOP_START, LOOP_END + 1, FONT_RATE, ROOT_KEY, FINE_TUNE);
	else if (o->pcm_kind == PCM_ONESHOT)
		w_shdr(f, "One", ROM_N - 1, 0, 0, FONT_RATE, ROOT_KEY, FINE_TUNE);
	else
		w_shdr(f, "Imp", IMP_N - 1, 0, 0, FONT_RATE, ROOT_KEY, FINE_TUNE);
	w_name(f, "EOS");
	for (int i = 0; i < 26; i++) fputc(0, f);      /* 46-byte terminal record */
	w_end(f, c);

	w_end(f, list);
	w_end(f, riff);

	long n = ftell(f);
	rewind(f);
	void *buf = malloc((size_t)n);
	if (!buf) { fclose(f); return NULL; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
	fclose(f);

	tsf *t = tsf_load_memory(buf, (int)n);
	if (out_buf) *out_buf = buf; else free(buf);
	if (out_size) *out_size = (int)n;
	return t;
}

/* ------------------------------------------------------------------ */
/* Independent double-precision reference                             */
/* ------------------------------------------------------------------ */

/* Defined with the PCM arrays further down; the driver owns main() so the
 * reference initialises its integer PCM lazily on first use. */
static void fill_pcm(void);
static int g_pcm_ready;

struct ref_lfo
{
	int until;              /* samples remaining in the delay phase */
	double level, delta;
};

static double cents_to_hertz(double cents) { return 8.176 * pow(2.0, cents / 1200.0); }

static void ref_lfo_setup(struct ref_lfo *l, int delay_tc, int freq_cents, int out_rate)
{
	double delay_secs = (delay_tc <= -12000) ? 0.0 : pow(2.0, delay_tc / 1200.0);
	l->until = (int)(delay_secs * out_rate);
	l->delta = 4.0 * cents_to_hertz(freq_cents) / out_rate;
	l->level = 0.0;
}
/* Exact mirror of the production triangle clock for one block. */
static void ref_lfo_process(struct ref_lfo *l, int block_samples)
{
	if (l->until > block_samples) { l->until -= block_samples; return; }
	l->level += l->delta * block_samples;
	if      (l->level >  1.0) { l->delta = -l->delta; l->level =  2.0 - l->level; }
	else if (l->level < -1.0) { l->delta = -l->delta; l->level = -2.0 - l->level; }
}

struct ref_coeff { double b0, b1, b2, a1, a2; };

/* RBJ low-pass (the same bilinear-transform design the engine uses), returned
 * in canonical direct form rather than the engine's transposed state. */
static struct ref_coeff ref_make_coeff(double fc_norm, double q_inv)
{
	double K = tan(M_PI * fc_norm), KK = K * K;
	double norm = 1.0 / (1.0 + K * q_inv + KK);
	struct ref_coeff z;
	z.b0 = KK * norm;
	z.b1 = 2.0 * z.b0;
	z.b2 = z.b0;
	z.a1 = 2.0 * (KK - 1.0) * norm;
	z.a2 = (1.0 - K * q_inv + KK) * norm;
	return z;
}

struct ref_hist { double x, y; struct ref_coeff c; };

struct ref_filter
{
	int active, dynamic;
	double q_inv;
	int init_cents, lfo_to_fc;
	struct ref_coeff cur;
	struct ref_hist h1, h2;   /* h1 = most recent, h2 = one sample older */
};

static void ref_filter_setup(struct ref_filter *fp, int filter_mode)
{
	memset(fp, 0, sizeof(*fp));
	fp->q_inv = 1.0 / pow(10.0, (FILT_Q_CENTS / 10.0) / 20.0);
	fp->init_cents = FILT_FC_CENTS;
	fp->lfo_to_fc = (filter_mode == FILT_DYNAMIC) ? LFO_TO_FC : 0;
	fp->dynamic = (filter_mode == FILT_DYNAMIC);
	if (filter_mode == FILT_STATIC)
	{
		double fc = cents_to_hertz(FILT_FC_CENTS) / OUT_RATE;
		fp->active = (fc < 0.499);
		if (fp->active) fp->cur = ref_make_coeff(fc, fp->q_inv);
	}
}

/* Called at the start of every effects block with the block-start LFO level. */
static void ref_filter_blockstart(struct ref_filter *fp, double lfo_level)
{
	if (!fp->dynamic) return;                  /* static filter: keep coeffs  */
	double fres = fp->init_cents + lfo_level * fp->lfo_to_fc;
	double fc = (fres <= 13500.0) ? cents_to_hertz(fres) / OUT_RATE : 1.0;
	fp->active = (fc < 0.499);
	if (fp->active) fp->cur = ref_make_coeff(fc, fp->q_inv);
	/* when bypassed the history (with its coefficient tags) is left intact */
}

/* One sample through the canonical difference equation. Delayed terms use the
 * coefficients stored when those samples entered the filter. */
static double ref_filter_process(struct ref_filter *fp, double in)
{
	if (!fp->active) return in;                /* bypass consumes no history  */
	const struct ref_coeff *c = &fp->cur;
	double out = c->b0 * in
	           + fp->h1.c.b1 * fp->h1.x + fp->h2.c.b2 * fp->h2.x
	           - fp->h1.c.a1 * fp->h1.y - fp->h2.c.a2 * fp->h2.y;
	fp->h2 = fp->h1;
	fp->h1.x = in;
	fp->h1.y = out;
	fp->h1.c = *c;
	return out;
}

struct ref_voice
{
	const short *pcm;
	int end;
	int loop_start, loop_end;                  /* loop_end inclusive; 0 = none */
	int looping;
	double pos, ratio;
	struct ref_lfo lfo;
	struct ref_filter filt;
	int has_lfo;
};

/* Playback ratio derived exactly like tsf_voice_calcpitchratio for a note on
 * the root key with the fixture's coarse/fine tuning and shdr correction. */
static double ref_pitch_ratio(void)
{
	double note = ROOT_KEY + COARSE_TUNE + (FINE_TUNE + FINE_TUNE) / 100.0;
	double adjusted = ROOT_KEY + (note - ROOT_KEY);          /* key track 100% */
	double timecents = adjusted * 100.0;
	double factor = (double)FONT_RATE / (pow(2.0, ROOT_KEY * 100.0 / 1200.0) * OUT_RATE);
	return pow(2.0, timecents / 1200.0) * factor;
}

static void ref_voice_setup(struct ref_voice *v, const short *pcm, int kind, int filter_mode,
                            int lfo_delay)
{
	if (!g_pcm_ready) fill_pcm();       /* driver owns main(); init lazily */
	memset(v, 0, sizeof(*v));
	v->pcm = pcm;
	v->looping = (kind == PCM_LOOP);
	if (v->looping)
	{
		v->end = SAMPLE_N;
		v->loop_start = LOOP_START;
		v->loop_end = LOOP_END;
	}
	else
	{
		v->end = (kind == PCM_ONESHOT) ? ROM_N : IMP_N;
		v->loop_start = v->loop_end = 0;
	}
	v->pos = 0.0;
	v->ratio = ref_pitch_ratio();
	v->has_lfo = (filter_mode == FILT_DYNAMIC);
	ref_filter_setup(&v->filt, filter_mode);
	if (v->has_lfo)
		ref_lfo_setup(&v->lfo, lfo_delay ? LFO_DELAY_TC : -12000, LFO_FREQ_CENTS, OUT_RATE);
}

/* Fractional source read with linear interpolation and loop wrapping. */
static double ref_source_read(struct ref_voice *v)
{
	int ipos = (int)v->pos;
	double alpha = v->pos - ipos;
	int next = (v->looping && ipos >= v->loop_end) ? v->loop_start : ipos + 1;
	double s0 = (double)v->pcm[ipos] / 32767.0;
	double s1 = (double)v->pcm[next] / 32767.0;
	return s0 * (1.0 - alpha) + s1 * alpha;
}

/* Render `n` output frames following the same <=64-frame effects-block schedule
 * the engine uses for this exact call plan. Returns mono sample values. */
static void ref_render(struct ref_voice *v, double *out, int n)
{
	int done = 0;
	while (done < n)
	{
		int block = n - done;
		if (block > BLOCK) block = BLOCK;

		if (v->has_lfo)
		{
			ref_filter_blockstart(&v->filt, v->lfo.level);
			ref_lfo_process(&v->lfo, block);
		}

		for (int i = 0; i < block; i++)
		{
			double val = 0.0;
			if (v->pos < v->end)
			{
				val = ref_source_read(v);
				if (v->filt.active) val = ref_filter_process(&v->filt, val);
				v->pos += v->ratio;
				if (v->looping && v->pos >= (double)v->loop_end + 1.0)
					v->pos -= (v->loop_end - v->loop_start + 1.0);
			}
			out[done + i] = val;
		}
		done += block;
	}
}

/* ------------------------------------------------------------------ */
/* PCM arrays and engine harness                                       */
/* ------------------------------------------------------------------ */

static short g_loop[SAMPLE_N], g_rom[ROM_N], g_imp[IMP_N];
static int g_pcm_ready = 0;

static void fill_pcm(void)
{
	for (int i = 0; i < SAMPLE_N; i++) g_loop[i] = loop_pcm(i);
	for (int i = 0; i < ROM_N; i++) g_rom[i] = rom_pcm(i);
	g_imp[0] = 32767;
	for (int i = 1; i < IMP_N; i++) g_imp[i] = 0;
	g_pcm_ready = 1;
}
static const short *pcm_for(int kind)
{
	if (!g_pcm_ready) fill_pcm();       /* driver owns main(); init lazily */
	return kind == PCM_LOOP ? g_loop : (kind == PCM_ONESHOT ? g_rom : g_imp);
}

/* A call plan is a list of frame counts; each becomes one tsf_render_float.
 * Plans deliberately mix full 64-frame blocks and short tails. */
static const int plan_split[] = { 64, 64, 30, 64, 7, 64, 25 };
static const int plan_whole[] = { 318 };
static const int plan_mix[]   = { 64, 40, 64, 64, 64, 26 };

struct plan { const int *parts; int count; int total; };

static struct plan make_plan(const int *parts, int count)
{
	struct plan p;
	p.parts = parts; p.count = count; p.total = 0;
	for (int i = 0; i < count; i++) p.total += parts[i];
	return p;
}

/* Drive one note through the engine and build the matching double reference.
 * Returns the open tsf (caller closes it) or NULL on failure. */
static tsf *run_engine_and_ref(const struct font_opts *fo, enum TSFOutputMode mode,
                               const struct plan *plan, int flag_mixing,
                               float *engine_out, double *ref_mono)
{
	void *buf = NULL; int size = 0;
	tsf *t = build_font(fo, &buf, &size);
	if (!t) return NULL;
	tsf_set_output(t, mode, OUT_RATE, 0.0f);
	if (!tsf_note_on(t, 0, ROOT_KEY, 1.0f)) { tsf_close(t); free(buf); return NULL; }

	int chans = (mode == TSF_MONO) ? 1 : 2;
	memset(engine_out, 0, sizeof(float) * (size_t)plan->total * chans);

	/* Unweaved output is [L block | R block] per call, not one global pair of
	 * planes, so render each call into scratch and gather contiguous planes. */
	int max_part = 0;
	for (int i = 0; i < plan->count; i++)
		if (plan->parts[i] > max_part) max_part = plan->parts[i];
	float *scratch = (mode == TSF_STEREO_UNWEAVED)
	               ? (float *)malloc(sizeof(float) * 2 * (size_t)max_part) : NULL;
	if (mode == TSF_STEREO_UNWEAVED && !scratch) { tsf_close(t); free(buf); return NULL; }

	struct ref_voice rv;
	ref_voice_setup(&rv, pcm_for(fo->pcm_kind), fo->pcm_kind, fo->filter, fo->lfo_delay);

	int off = 0;
	for (int i = 0; i < plan->count; i++)
	{
		int parts = plan->parts[i];
		if (mode == TSF_STEREO_UNWEAVED)
		{
			memset(scratch, 0, sizeof(float) * 2 * (size_t)parts);
			tsf_render_float(t, scratch, parts, flag_mixing);
			memcpy(engine_out + off, scratch, sizeof(float) * (size_t)parts);                 /* L plane */
			memcpy(engine_out + plan->total + off, scratch + parts, sizeof(float) * (size_t)parts); /* R */
		}
		else
		{
			tsf_render_float(t, engine_out + (size_t)off * chans, parts, flag_mixing);
		}
		ref_render(&rv, ref_mono + off, parts);
		off += parts;
	}
	free(scratch);
	free(buf);
	return t;
}

/* Compare one engine channel against the mono reference scaled by a constant
 * gain. Reports the first mismatch with expected/actual and the compare count. */
static int compare_channel(const float *eng, int eng_stride, const double *ref,
                           int n, double gain, const char *tag)
{
	int compared = 0;
	for (int i = 0; i < n; i++)
	{
		double expected = ref[i] * gain;
		double actual = eng[(size_t)i * eng_stride];
		double tol = ABS_TOL + REL_TOL * fabs(expected);
		compared++;
		if (fabs(actual - expected) > tol)
		{
			fprintf(stderr,
			        "FAIL [%s] first differing sample index=%d expected=%.8f actual=%.8f "
			        "compared=%d\n", tag, i, expected, actual, compared);
			return 0;
		}
	}
	printf("OK   [%s] compared=%d gain=%.4f\n", tag, compared, gain);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Group: sample_loop                                                  */
/* ------------------------------------------------------------------ */

static int case_loop_fractional_wraps(void)
{
	struct font_opts fo = { PCM_LOOP, FILT_NONE, 0, REGION_PAN };
	struct plan plan = make_plan(plan_split, (int)(sizeof(plan_split) / sizeof(int)));
	float *eng = malloc(sizeof(float) * (size_t)plan.total);
	double *ref = malloc(sizeof(double) * (size_t)plan.total);
	if (!eng || !ref) { free(eng); free(ref); return 1; }

	tsf *t = run_engine_and_ref(&fo, TSF_MONO, &plan, 0, eng, ref);
	TASK_CHECK(t != NULL);

	/* The step stays within a single wrap per frame (loop length 32). */
	TASK_CHECK(ref_pitch_ratio() < (double)LOOP_N);
	TASK_CHECK(compare_channel(eng, 1, ref, plan.total, 1.0, "loop.fractional_wraps"));

	/* Independently confirm the reference crosses the seam on non-integer
	 * positions and wraps several times. */
	struct ref_voice rv;
	ref_voice_setup(&rv, g_loop, PCM_LOOP, FILT_NONE, 0);
	int seam_hits = 0, frac_hits = 0, wraps = 0;
	for (int i = 0; i < plan.total; i++)
	{
		if (rv.pos >= LOOP_END - 1 && rv.pos <= LOOP_END + 1)
		{
			seam_hits++;
			if (rv.pos != floor(rv.pos)) frac_hits++;
		}
		double before = rv.pos;
		ref_render(&rv, ref + i, 1);
		if (rv.pos < before) wraps++;     /* pointer jumped back -> wrapped */
	}
	TASK_CHECK(seam_hits >= 3);
	TASK_CHECK(frac_hits >= 1);
	TASK_CHECK(wraps >= 3);

	tsf_close(t);
	free(eng); free(ref);
	return 0;
}

static int case_loop_whole_vs_split(void)
{
	struct font_opts fo = { PCM_LOOP, FILT_NONE, 0, REGION_PAN };
	struct plan whole = make_plan(plan_whole, (int)(sizeof(plan_whole) / sizeof(int)));
	struct plan split = make_plan(plan_split, (int)(sizeof(plan_split) / sizeof(int)));
	TASK_CHECK(whole.total == split.total);

	float *eng_w = malloc(sizeof(float) * whole.total);
	float *eng_s = malloc(sizeof(float) * split.total);
	double *ref_s = malloc(sizeof(double) * split.total);
	if (!eng_w || !eng_s || !ref_s) { free(eng_w); free(eng_s); free(ref_s); return 1; }

	tsf *t1 = run_engine_and_ref(&fo, TSF_MONO, &whole, 0, eng_w, ref_s);
	TASK_CHECK(t1 != NULL);
	tsf_close(t1);
	tsf *t2 = run_engine_and_ref(&fo, TSF_MONO, &split, 0, eng_s, ref_s);
	TASK_CHECK(t2 != NULL);

	/* With no modulation the read pointer is independent of block boundaries:
	 * the two call plans must produce identical content. */
	for (int i = 0; i < whole.total; i++)
		TASK_CHECK(fabs((double)eng_w[i] - eng_s[i]) <= ABS_TOL);
	TASK_CHECK(compare_channel(eng_s, 1, ref_s, split.total, 1.0, "loop.whole_vs_split"));

	tsf_close(t2);
	free(eng_w); free(eng_s); free(ref_s);
	return 0;
}

static int case_noloop_natural_end(void)
{
	struct font_opts fo = { PCM_ONESHOT, FILT_NONE, 0, REGION_PAN };
	const int parts[] = { 64, 64, 64, 64 };
	struct plan plan = make_plan(parts, 4);
	float *eng = malloc(sizeof(float) * plan.total);
	double *ref = malloc(sizeof(double) * plan.total);
	if (!eng || !ref) { free(eng); free(ref); return 1; }

	tsf *t = run_engine_and_ref(&fo, TSF_MONO, &plan, 0, eng, ref);
	TASK_CHECK(t != NULL);
	TASK_CHECK(compare_channel(eng, 1, ref, plan.total, 1.0, "noloop.natural_end"));

	/* The 48-sample one-shot is exhausted well before the render ends, the
	 * voice terminates and the remaining output is silence. */
	TASK_CHECK(tsf_active_voice_count(t) == 0);
	int silent_tail = 0;
	for (int i = plan.total - 32; i < plan.total; i++)
		if (eng[i] == 0.0f) silent_tail++;
	TASK_CHECK(silent_tail == 32);

	tsf_close(t);
	free(eng); free(ref);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Group: filter_history                                               */
/* ------------------------------------------------------------------ */

static int case_filter_fixed_impulse(void)
{
	struct font_opts fo = { PCM_IMPULSE, FILT_STATIC, 0, REGION_PAN };
	const int parts[] = { 64, 64 };
	struct plan plan = make_plan(parts, 2);
	float *eng = malloc(sizeof(float) * plan.total);
	double *ref = malloc(sizeof(double) * plan.total);
	if (!eng || !ref) { free(eng); free(ref); return 1; }

	tsf *t = run_engine_and_ref(&fo, TSF_MONO, &plan, 0, eng, ref);
	TASK_CHECK(t != NULL);
	TASK_CHECK(compare_channel(eng, 1, ref, plan.total, 1.0, "filter.fixed_impulse"));

	/* The impulse must actually excite the filter: the first output is the
	 * feed-through coefficient and the response is non-trivial afterwards. */
	TASK_CHECK(fabs(eng[0]) > 1e-4);
	int ring = 0;
	for (int i = 1; i < 40; i++) if (eng[i] != 0.0f) ring++;
	TASK_CHECK(ring >= 20);

	tsf_close(t);
	free(eng); free(ref);
	return 0;
}

static int case_filter_fixed_aperiodic(void)
{
	struct font_opts fo = { PCM_LOOP, FILT_STATIC, 0, REGION_PAN };
	const int parts[] = { 64, 64, 64, 50 };
	struct plan plan = make_plan(parts, 4);
	float *eng = malloc(sizeof(float) * plan.total);
	double *ref = malloc(sizeof(double) * plan.total);
	if (!eng || !ref) { free(eng); free(ref); return 1; }

	tsf *t = run_engine_and_ref(&fo, TSF_MONO, &plan, 0, eng, ref);
	TASK_CHECK(t != NULL);
	TASK_CHECK(compare_channel(eng, 1, ref, plan.total, 1.0, "filter.fixed_aperiodic"));
	tsf_close(t);
	free(eng); free(ref);
	return 0;
}

static int case_filter_lfo_dynamic(void)
{
	/* Delayed, turning triangle LFO; the cutoff changes across many 64-frame
	 * blocks and the reference clocks the LFO with the identical call plan. */
	struct font_opts fo = { PCM_LOOP, FILT_DYNAMIC, 1, REGION_PAN };
	const int parts[] = { 64, 64, 64, 33, 64, 64, 64, 64, 64, 64, 64, 19 };
	struct plan plan = make_plan(parts, 12);
	float *eng = malloc(sizeof(float) * plan.total);
	double *ref = malloc(sizeof(double) * plan.total);
	if (!eng || !ref) { free(eng); free(ref); return 1; }

	tsf *t = run_engine_and_ref(&fo, TSF_MONO, &plan, 0, eng, ref);
	TASK_CHECK(t != NULL);
	TASK_CHECK(compare_channel(eng, 1, ref, plan.total, 1.0, "filter.lfo_dynamic"));

	/* Re-walk the reference LFO and count active/bypass transitions and turns. */
	struct ref_lfo lfo;
	ref_lfo_setup(&lfo, LFO_DELAY_TC, LFO_FREQ_CENTS, OUT_RATE);
	int transitions = 0, last_active = -1, delay_blocks = 0;
	for (int b = 0; b < plan.total; b += BLOCK)
	{
		int blk = plan.total - b; if (blk > BLOCK) blk = BLOCK;
		if (lfo.until > 0) delay_blocks++;
		double fres = FILT_FC_CENTS + lfo.level * LFO_TO_FC;
		double fc = (fres <= 13500.0) ? cents_to_hertz(fres) / OUT_RATE : 1.0;
		int active = (fc < 0.499);
		if (last_active >= 0 && active != last_active) transitions++;
		last_active = active;
		ref_lfo_process(&lfo, blk);
	}
	TASK_CHECK(delay_blocks >= 3);       /* delay genuinely observed first */
	TASK_CHECK(transitions >= 2);        /* cutoff swept through bypass etc. */

	tsf_close(t);
	free(eng); free(ref);
	return 0;
}

/* Enable -> bypass -> re-enable in one continuous voice, checking that the
 * re-enable still uses the pre-bypass filter history. */
static int case_filter_bypass_history(void)
{
	struct font_opts fo = { PCM_LOOP, FILT_DYNAMIC, 0, REGION_PAN };
	const int parts[] = { 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 40 };
	struct plan plan = make_plan(parts, 14);
	float *eng = malloc(sizeof(float) * plan.total);
	double *ref = malloc(sizeof(double) * plan.total);
	if (!eng || !ref) { free(eng); free(ref); return 1; }

	tsf *t = run_engine_and_ref(&fo, TSF_MONO, &plan, 0, eng, ref);
	TASK_CHECK(t != NULL);
	TASK_CHECK(compare_channel(eng, 1, ref, plan.total, 1.0, "filter.bypass_history"));

	/* Independently confirm the reference itself bypassed and then re-enabled
	 * without ever clearing its history. */
	struct ref_voice rv;
	ref_voice_setup(&rv, g_loop, PCM_LOOP, FILT_DYNAMIC, 0);
	int saw_bypass = 0, saw_re_enable = 0, was_active = 0;
	double saved_h1x = 0.0;
	for (int b = 0; b < plan.total; b += BLOCK)
	{
		int blk = plan.total - b; if (blk > BLOCK) blk = BLOCK;
		ref_filter_blockstart(&rv.filt, rv.lfo.level);
		if (!rv.filt.active && was_active) { saw_bypass = 1; saved_h1x = rv.filt.h1.x; }
		if (rv.filt.active && !was_active && saw_bypass) saw_re_enable = 1;
		was_active = rv.filt.active;
		ref_lfo_process(&rv.lfo, blk);
		for (int i = 0; i < blk && rv.pos < rv.end; i++)
		{
			double val = ref_source_read(&rv);
			if (rv.filt.active) (void)ref_filter_process(&rv.filt, val);
			rv.pos += rv.ratio;
			if (rv.looping && rv.pos >= (double)rv.loop_end + 1.0)
				rv.pos -= (rv.loop_end - rv.loop_start + 1.0);
		}
	}
	TASK_CHECK(saw_bypass);
	TASK_CHECK(saw_re_enable);
	TASK_CHECK(saved_h1x != 0.0);       /* history was non-trivial at bypass */

	tsf_close(t);
	free(eng); free(ref);
	return 0;
}

/* One scenario that simultaneously crosses a fractional loop seam and changes
 * filter coefficients within the same effects block. */
static int case_filter_seam_and_coeff(void)
{
	struct font_opts fo = { PCM_LOOP, FILT_DYNAMIC, 0, REGION_PAN };
	const int parts[] = { 64, 64, 64, 64, 64, 64, 64, 64, 64, 64 };
	struct plan plan = make_plan(parts, 10);
	float *eng = malloc(sizeof(float) * plan.total);
	double *ref = malloc(sizeof(double) * plan.total);
	if (!eng || !ref) { free(eng); free(ref); return 1; }

	tsf *t = run_engine_and_ref(&fo, TSF_MONO, &plan, 0, eng, ref);
	TASK_CHECK(t != NULL);
	TASK_CHECK(compare_channel(eng, 1, ref, plan.total, 1.0, "filter.seam_and_coeff"));

	/* Count blocks that both update coefficients and contain a fractional seam. */
	struct ref_voice rv;
	ref_voice_setup(&rv, g_loop, PCM_LOOP, FILT_DYNAMIC, 0);
	double prev_b0 = -1.0;
	int overlap_blocks = 0;
	for (int b = 0; b < plan.total; b += BLOCK)
	{
		int blk = plan.total - b; if (blk > BLOCK) blk = BLOCK;
		ref_filter_blockstart(&rv.filt, rv.lfo.level);
		int coeff_changed = (prev_b0 >= 0.0 && rv.filt.cur.b0 != prev_b0);
		prev_b0 = rv.filt.cur.b0;
		int seam = 0;
		for (int i = 0; i < blk && rv.pos < rv.end; i++)
		{
			double p = rv.pos;
			if (rv.looping && p >= LOOP_END - 1 && p <= LOOP_END + 1 && p != floor(p)) seam = 1;
			rv.pos += rv.ratio;
			if (rv.looping && rv.pos >= (double)rv.loop_end + 1.0)
				rv.pos -= (rv.loop_end - rv.loop_start + 1.0);
		}
		ref_lfo_process(&rv.lfo, blk);
		if (coeff_changed && seam) overlap_blocks++;
	}
	TASK_CHECK(overlap_blocks >= 1);

	tsf_close(t);
	free(eng); free(ref);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Group: layouts_mix                                                  */
/* ------------------------------------------------------------------ */

static double pan_left(void)  { return sqrt(0.5 - REGION_PAN); }
static double pan_right(void) { return sqrt(0.5 + REGION_PAN); }

static int case_layout_relations(void)
{
	struct font_opts fo = { PCM_LOOP, FILT_STATIC, 0, REGION_PAN };
	const int parts[] = { 64, 37, 64, 64, 29 };
	struct plan plan = make_plan(parts, 5);
	int n = plan.total;

	float *mono = malloc(sizeof(float) * n);
	float *inter = malloc(sizeof(float) * 2 * n);
	float *plane = malloc(sizeof(float) * 2 * n);
	double *ref = malloc(sizeof(double) * n);
	if (!mono || !inter || !plane || !ref)
	{ free(mono); free(inter); free(plane); free(ref); return 1; }

	tsf *t;
	t = run_engine_and_ref(&fo, TSF_MONO, &plan, 0, mono, ref);
	TASK_CHECK(t != NULL); tsf_close(t);
	t = run_engine_and_ref(&fo, TSF_STEREO_INTERLEAVED, &plan, 0, inter, ref);
	TASK_CHECK(t != NULL); tsf_close(t);
	t = run_engine_and_ref(&fo, TSF_STEREO_UNWEAVED, &plan, 0, plane, ref);
	TASK_CHECK(t != NULL); tsf_close(t);

	/* Per-channel pan scaling against the same mono reference. */
	TASK_CHECK(compare_channel(inter, 2, ref, n, pan_left(),  "layout.inter.L"));
	TASK_CHECK(compare_channel(inter + 1, 2, ref, n, pan_right(), "layout.inter.R"));
	TASK_CHECK(compare_channel(plane, 1, ref, n, pan_left(),  "layout.plane.L"));
	TASK_CHECK(compare_channel(plane + n, 1, ref, n, pan_right(), "layout.plane.R"));
	/* Mono carries the un-panned voice (gain 1.0). */
	TASK_CHECK(compare_channel(mono, 1, ref, n, 1.0, "layout.mono"));

	for (int i = 0; i < n; i++)
	{
		/* 3 dB pan law: stereo channels sum to ref*(panL+panR), not to mono. */
		double downmix = (double)inter[2 * i] + inter[2 * i + 1];
		double expect_sum = ref[i] * (pan_left() + pan_right());
		TASK_CHECK(fabs(downmix - expect_sum) <= ABS_TOL + REL_TOL * fabs(expect_sum));
		/* Planar and interleaved carry identical per-channel content. */
		TASK_CHECK(fabs((double)inter[2 * i] - plane[i]) <= ABS_TOL);
		TASK_CHECK(fabs((double)inter[2 * i + 1] - plane[n + i]) <= ABS_TOL);
	}

	/* Filter history must persist across the short-tail calls: late output is
	 * still structured (a reset would already have failed the compare above). */
	int nonzero_late = 0;
	for (int i = n - 16; i < n; i++) if (fabs(mono[i]) > 1e-4) nonzero_late++;
	TASK_CHECK(nonzero_late >= 8);

	free(mono); free(inter); free(plane); free(ref);
	return 0;
}

/* Mixing flag: pre-fill with a known non-zero pattern and check overwrite
 * (flag 0) versus accumulation (flag 1) against an analytic expectation. */
static int case_mixing_overwrite_accumulate(void)
{
	struct font_opts fo = { PCM_LOOP, FILT_NONE, 0, REGION_PAN };
	struct plan plan = make_plan(plan_mix, (int)(sizeof(plan_mix) / sizeof(int)));
	int n = plan.total;

	float *over = malloc(sizeof(float) * 2 * n);
	float *mix = malloc(sizeof(float) * 2 * n);
	float *seed = malloc(sizeof(float) * 2 * n);
	double *ref = malloc(sizeof(double) * n);
	if (!over || !mix || !seed || !ref)
	{ free(over); free(mix); free(seed); free(ref); return 1; }

	for (int i = 0; i < 2 * n; i++) seed[i] = 0.01f + 0.0007f * i;
	memcpy(over, seed, sizeof(float) * 2 * n);
	memcpy(mix, seed, sizeof(float) * 2 * n);

	void *buf = NULL; int size = 0;
	tsf *t = build_font(&fo, &buf, &size);
	TASK_CHECK(t != NULL);
	tsf_set_output(t, TSF_STEREO_INTERLEAVED, OUT_RATE, 0.0f);
	TASK_CHECK(tsf_note_on(t, 0, ROOT_KEY, 1.0f));
	struct ref_voice rv;
	ref_voice_setup(&rv, g_loop, PCM_LOOP, FILT_NONE, 0);
	int off = 0;
	for (int i = 0; i < plan.count; i++)
	{
		tsf_render_float(t, over + (size_t)off * 2, plan.parts[i], 0);  /* overwrite */
		ref_render(&rv, ref + off, plan.parts[i]);
		off += plan.parts[i];
	}
	tsf_close(t); free(buf);

	buf = NULL;
	t = build_font(&fo, &buf, &size);
	TASK_CHECK(t != NULL);
	tsf_set_output(t, TSF_STEREO_INTERLEAVED, OUT_RATE, 0.0f);
	TASK_CHECK(tsf_note_on(t, 0, ROOT_KEY, 1.0f));
	off = 0;
	for (int i = 0; i < plan.count; i++)
	{
		tsf_render_float(t, mix + (size_t)off * 2, plan.parts[i], 1);   /* accumulate */
		off += plan.parts[i];
	}
	tsf_close(t); free(buf);

	double gl = pan_left(), gr = pan_right();
	for (int i = 0; i < n; i++)
	{
		double el = ref[i] * gl, er = ref[i] * gr;
		/* overwrite: seed replaced by rendered content */
		TASK_CHECK(fabs((double)over[2 * i] - el) <= ABS_TOL + REL_TOL * fabs(el));
		TASK_CHECK(fabs((double)over[2 * i + 1] - er) <= ABS_TOL + REL_TOL * fabs(er));
		/* accumulate: seed retained and added to rendered content */
		TASK_CHECK(fabs((double)mix[2 * i] - (el + seed[2 * i])) <=
		           ABS_TOL + REL_TOL * (fabs(el) + fabs((double)seed[2 * i])));
		TASK_CHECK(fabs((double)mix[2 * i + 1] - (er + seed[2 * i + 1])) <=
		           ABS_TOL + REL_TOL * (fabs(er) + fabs((double)seed[2 * i + 1])));
		/* the two paths genuinely differ wherever the seed is non-zero */
		TASK_CHECK(fabs((double)mix[2 * i] - over[2 * i]) > 0.0);
		TASK_CHECK(fabs((double)mix[2 * i + 1] - over[2 * i + 1]) > 0.0);
	}

	free(over); free(mix); free(seed); free(ref);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Registration (driver.c provides main)                              */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
	{ "sample_loop", "fractional_wraps",   case_loop_fractional_wraps   },
	{ "sample_loop", "whole_vs_split",     case_loop_whole_vs_split     },
	{ "sample_loop", "noloop_natural_end", case_noloop_natural_end      },

	{ "filter_history", "fixed_impulse",    case_filter_fixed_impulse    },
	{ "filter_history", "fixed_aperiodic",  case_filter_fixed_aperiodic  },
	{ "filter_history", "lfo_dynamic",      case_filter_lfo_dynamic      },
	{ "filter_history", "bypass_history",   case_filter_bypass_history   },
	{ "filter_history", "seam_and_coeff",   case_filter_seam_and_coeff   },

	{ "layouts_mix", "layout_relations",       case_layout_relations           },
	{ "layouts_mix", "mixing_overwrite_accum", case_mixing_overwrite_accumulate },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
