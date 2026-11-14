/*
 * linked_stereo.c - tests for linked left/right SoundFont sample pairs.
 *
 * Declarations only: the implementation is compiled separately from
 * task_support/production.c. No TSF_IMPLEMENTATION and no main() here.
 *
 * Every expected value is derived from the original PCM data through the
 * documented synthesis formulas (linear interpolation, loop wrapping,
 * biquad lowpass, stereo balance), never from tsf's own rendered output.
 */
#include "../task_support/check.h"
#include "../task_support/alloc.h"
#include "tsf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLOAT_TOL 2e-5

/* ------------------------------------------------------------------ */
/* Deterministic original PCM (all values are invented test signals).  */
/* ------------------------------------------------------------------ */
static int pcm_l(int i) { return ((i * 37) % 251 - 125) * 240; }
static int pcm_r(int i) { return ((i * 97) % 193 - 96) * 300; }
static int pcm_m(int i) { return ((i * 53) % 227 - 113) * 250; }

enum
{
	MAIN_LEFT = 0,
	MAIN_LEFT_END = 256,
	MAIN_RIGHT = 4000,
	MAIN_RIGHT_END = 4256,
	MAIN_LOOP_START = 32,
	MAIN_LOOP_END = 224,
	MAIN_COUNT = 4260
};

static short g_main[MAIN_COUNT];
static short g_mono[MAIN_COUNT];
static int g_init;

static void init_pcm(void)
{
	int i;
	if (g_init) return;
	g_init = 1;
	memset(g_main, 0, sizeof(g_main));
	for (i = 0; i < 256; ++i) g_main[MAIN_LEFT + i] = (short)pcm_l(i);
	for (i = 0; i < 256; ++i) g_main[MAIN_RIGHT + i] = (short)pcm_r(i);
	/* Non-zero sentinel directly after each playable end. Reading these
	 * through an unclamped interpolation neighbor is an error the render
	 * tests can detect. */
	g_main[MAIN_LEFT_END] = (short)-25000;
	g_main[MAIN_RIGHT_END] = (short)12345;
	memset(g_mono, 0, sizeof(g_mono));
	for (i = 0; i < 256; ++i) g_mono[i] = (short)pcm_m(i);
}

/* ------------------------------------------------------------------ */
/* Parameterized in-memory SF2 builder.                               */
/* ------------------------------------------------------------------ */
#define TB_MAX_SHDR  8
#define TB_MAX_ZONES 6
#define TB_MAX_GENS  24

typedef unsigned char tb_u8;
typedef unsigned short tb_u16;
typedef unsigned int tb_u32;

struct tb_gen { tb_u16 op; int amount; };
struct tb_shdr
{
	const char *name;
	tb_u32 start, end, startLoop, endLoop, rate;
	tb_u8 root;
	signed char corr;
	tb_u16 link, type;
};
struct tb_zone { int sample; int ngen; struct tb_gen gens[TB_MAX_GENS]; };
struct tb_font
{
	const short *pcm;
	int pcmCount;
	struct tb_shdr shdr[TB_MAX_SHDR];
	int shdrCount;
	struct tb_zone zone[TB_MAX_ZONES];
	int zoneCount;
};
struct tb_bytes { unsigned char *data; int size; };

static void tb_uint(FILE *f, unsigned x, int n)
{
	while (n--) { fputc((int)(x & 255), f); x >>= 8; }
}
static long tb_begin(FILE *f, const char *id)
{
	long p;
	fwrite(id, 1, 4, f);
	p = ftell(f);
	tb_uint(f, 0, 4);
	return p;
}
static void tb_end(FILE *f, long p)
{
	long e = ftell(f);
	fseek(f, p, SEEK_SET); tb_uint(f, (unsigned)(e - p - 4), 4); fseek(f, e, SEEK_SET);
	if ((e - p - 4) & 1) fputc(0, f);
}
static void tb_name(FILE *f, const char *s)
{
	char b[20] = {0};
	strncpy(b, s, 19);
	fwrite(b, 1, 20, f);
}
static void tb_gen(FILE *f, unsigned op, int amount)
{
	tb_uint(f, op, 2);
	tb_uint(f, (unsigned)(unsigned short)amount, 2);
}
static void tb_shdr_write(FILE *f, const struct tb_shdr *s)
{
	tb_name(f, s->name);
	tb_uint(f, s->start, 4); tb_uint(f, s->end, 4);
	tb_uint(f, s->startLoop, 4); tb_uint(f, s->endLoop, 4);
	tb_uint(f, s->rate, 4);
	tb_uint(f, s->root, 1); tb_uint(f, (unsigned)(unsigned char)s->corr, 1);
	tb_uint(f, s->link, 2); tb_uint(f, s->type, 2);
}

static struct tb_bytes tb_build(const struct tb_font *d)
{
	struct tb_bytes out = {NULL, 0};
	FILE *f = tmpfile();
	long riff, list, c;
	int i, z;
	if (!f) return out;

	riff = tb_begin(f, "RIFF"); fwrite("sfbk", 1, 4, f);
	list = tb_begin(f, "LIST"); fwrite("INFO", 1, 4, f);
	c = tb_begin(f, "ifil"); tb_uint(f, 2, 2); tb_uint(f, 1, 2); tb_end(f, c);
	tb_end(f, list);

	list = tb_begin(f, "LIST"); fwrite("sdta", 1, 4, f);
	c = tb_begin(f, "smpl");
	for (i = 0; i < d->pcmCount; ++i)
		tb_uint(f, (unsigned)(unsigned short)d->pcm[i], 2);
	tb_end(f, c);
	tb_end(f, list);

	list = tb_begin(f, "LIST"); fwrite("pdta", 1, 4, f);
	c = tb_begin(f, "phdr");
	{
		tb_name(f, "LinkedStereo");
		tb_uint(f, 0, 2); tb_uint(f, 0, 2); tb_uint(f, 0, 2);
		tb_uint(f, 0, 4); tb_uint(f, 0, 4); tb_uint(f, 0, 4);
		tb_name(f, "EOP");
		tb_uint(f, 0, 2); tb_uint(f, 0, 2); tb_uint(f, 1, 2);
		tb_uint(f, 0, 4); tb_uint(f, 0, 4); tb_uint(f, 0, 4);
	}
	tb_end(f, c);

	c = tb_begin(f, "pbag");
	tb_uint(f, 0, 2); tb_uint(f, 0, 2);
	tb_uint(f, 2, 2); tb_uint(f, 0, 2);
	tb_end(f, c);

	c = tb_begin(f, "pmod");
	for (i = 0; i < 10; ++i) fputc(0, f);
	tb_end(f, c);

	c = tb_begin(f, "pgen");
	tb_gen(f, 41, 0);
	tb_gen(f, 0, 0);
	tb_end(f, c);

	c = tb_begin(f, "inst");
	tb_name(f, "PairInst"); tb_uint(f, 0, 2);
	tb_name(f, "EOI"); tb_uint(f, (unsigned)d->zoneCount, 2);
	tb_end(f, c);

	c = tb_begin(f, "ibag");
	{
		int gndx = 0;
		for (z = 0; z < d->zoneCount; ++z)
		{
			tb_uint(f, (unsigned)gndx, 2); tb_uint(f, 0, 2);
			gndx += d->zone[z].ngen + 2; /* gens + SampleID + terminator */
		}
		tb_uint(f, (unsigned)gndx, 2); tb_uint(f, 0, 2);
	}
	tb_end(f, c);

	c = tb_begin(f, "imod");
	for (i = 0; i < 10; ++i) fputc(0, f);
	tb_end(f, c);

	c = tb_begin(f, "igen");
	for (z = 0; z < d->zoneCount; ++z)
	{
		int k;
		for (k = 0; k < d->zone[z].ngen; ++k)
			tb_gen(f, d->zone[z].gens[k].op, d->zone[z].gens[k].amount);
		tb_gen(f, 53, d->zone[z].sample); /* SampleID */
		tb_gen(f, 0, 0);
	}
	tb_end(f, c);

	c = tb_begin(f, "shdr");
	for (i = 0; i < d->shdrCount; ++i) tb_shdr_write(f, &d->shdr[i]);
	{
		struct tb_shdr eos;
		memset(&eos, 0, sizeof(eos));
		eos.name = "EOS"; eos.start = eos.end = eos.startLoop = eos.endLoop = (tb_u32)d->pcmCount;
		eos.rate = 22050; eos.root = 60; eos.type = 1;
		tb_shdr_write(f, &eos);
	}
	tb_end(f, c);

	tb_end(f, list); tb_end(f, riff);
	{
		long n = ftell(f);
		if (n <= 0 || n > 4000000 || ferror(f)) { fclose(f); return out; }
		out.data = (unsigned char *)malloc((size_t)n);
		if (out.data)
		{
			rewind(f);
			if (fread(out.data, 1, (size_t)n, f) == (size_t)n) out.size = (int)n;
			else { free(out.data); out.data = NULL; }
		}
	}
	fclose(f);
	return out;
}

static tsf *tb_load(const struct tb_font *d)
{
	struct tb_bytes b = tb_build(d);
	tsf *f = (b.data ? tsf_load_memory(b.data, b.size) : NULL);
	free(b.data);
	return f;
}

/* ------------------------------------------------------------------ */
/* Independent reference math.                                        */
/* ------------------------------------------------------------------ */
static float pcm_f(const short *pcm, unsigned index)
{
	return (float)((double)pcm[index] / 32767.0);
}

/* Linear interpolation at a fractional position relative to a side's
 * start. Looping uses the next loop sample at the seam; a non-looping
 * end clamps the neighbor to the last playable sample of that side. */
static float ref_interp(const short *pcm, unsigned base, double rel,
	unsigned endRel, int loop, unsigned loopStart, unsigned loopEnd)
{
	unsigned pos = (unsigned)rel, next = pos + 1;
	float alpha;
	if (loop && next >= loopEnd) next = loopStart;
	else if (!loop && next >= endRel) next = endRel - 1;
	alpha = (float)(rel - (double)pos);
	return pcm_f(pcm, base + pos) * (1.0f - alpha) + pcm_f(pcm, base + next) * alpha;
}

struct ref_bq { double a0, a1, b1, b2, z1, z2; };

static void ref_bq_setup(struct ref_bq *q, float fcNormalized)
{
	double K = tan((double)3.14159265358979323846 * (double)fcNormalized);
	double KK = K * K;
	double norm = 1.0 / (1.0 + K * 1.0 + KK); /* QInv = 1 for initialFilterQ 0 */
	q->a0 = KK * norm;
	q->a1 = 2.0 * q->a0;
	q->b1 = 2.0 * (KK - 1.0) * norm;
	q->b2 = (1.0 - K * 1.0 + KK) * norm;
	q->z1 = q->z2 = 0.0;
}
static float ref_bq_process(struct ref_bq *q, double in)
{
	double out = in * q->a0 + q->z1;
	q->z1 = in * q->a1 + q->z2 - q->b1 * out;
	q->z2 = in * q->a0 - q->b2 * out;
	return (float)out;
}

/* One frame of a linked stereo voice at pitch ratio with optional looping.
 * Returns left/right values before gain. */
static void ref_frame(const short *pcm, unsigned leftBase, unsigned rightBase,
	unsigned endRel, int loop, unsigned loopStart, unsigned loopEnd,
	double rel, struct ref_bq *fqL, struct ref_bq *fqR,
	float *outL, float *outR)
{
	*outL = ref_interp(pcm, leftBase, rel, endRel, loop, loopStart, loopEnd);
	*outR = ref_interp(pcm, rightBase, rel, endRel, loop, loopStart, loopEnd);
	if (fqL) *outL = ref_bq_process(fqL, (double)*outL);
	if (fqR) *outR = ref_bq_process(fqR, (double)*outR);
}

static double ref_pan_left(float p)
{
	if (p > 0.5f) p = 0.5f;
	if (p < -0.5f) p = -0.5f;
	return sqrt((double)(1.0f - 2.0f * (p > 0.0f ? p : 0.0f)));
}
static double ref_pan_right(float p)
{
	if (p > 0.5f) p = 0.5f;
	if (p < -0.5f) p = -0.5f;
	return sqrt((double)(1.0f + 2.0f * (p < 0.0f ? p : 0.0f)));
}

/* Advance a shared relative position exactly as the renderer documents. */
static double ref_advance(double rel, double ratio, int loop, unsigned loopStart, unsigned loopEnd)
{
	rel += ratio;
	if (loop)
	{
		double span = (double)(loopEnd - loopStart);
		while (rel >= (double)loopEnd) rel -= span;
	}
	return rel;
}

static short ref_short_single(float v)
{
	return (v < -1.00004566f ? (short)-32768 : (v > 1.00001514f ? (short)32767 : (short)(v * 32767.5f)));
}

static void sh_main_pair(struct tb_font *d, const short *pcm)
{
	memset(d, 0, sizeof(*d));
	d->pcm = pcm;
	d->pcmCount = MAIN_COUNT;
	d->shdr[0].name = "Left";
	d->shdr[0].start = MAIN_LEFT; d->shdr[0].end = MAIN_LEFT_END;
	d->shdr[0].startLoop = MAIN_LOOP_START; d->shdr[0].endLoop = MAIN_LOOP_END;
	d->shdr[0].rate = 22050; d->shdr[0].root = 60; d->shdr[0].corr = 0;
	d->shdr[0].link = 1; d->shdr[0].type = 4;
	d->shdr[1].name = "Right";
	d->shdr[1].start = MAIN_RIGHT; d->shdr[1].end = MAIN_RIGHT_END;
	d->shdr[1].startLoop = MAIN_RIGHT + (MAIN_LOOP_START - MAIN_LEFT);
	d->shdr[1].endLoop = MAIN_RIGHT + (MAIN_LOOP_END - MAIN_LEFT);
	d->shdr[1].rate = 22050; d->shdr[1].root = 60; d->shdr[1].corr = 0;
	d->shdr[1].link = 0; d->shdr[1].type = 2;
	d->shdrCount = 2;
}

static void zbuf(struct tb_zone *z, int sample, int ngen, const struct tb_gen *gens)
{
	int k;
	memset(z, 0, sizeof(*z));
	z->sample = sample;
	z->ngen = ngen;
	for (k = 0; k < ngen; ++k) z->gens[k] = gens[k];
}

static void zone_play(struct tb_zone *z, int sample)
{
	struct tb_gen g[2];
	g[0].op = 54; g[0].amount = 1; /* looping */
	g[1].op = 8; g[1].amount = 13500; /* bypass filter, unity gain */
	zbuf(z, sample, 2, g);
}

static void zone_gens(struct tb_zone *z, int sample, int ngen, const struct tb_gen *gens)
{
	zbuf(z, sample, ngen, gens);
}

/* ------------------------------------------------------------------ */
/* Buffers and small helpers.                                         */
/* ------------------------------------------------------------------ */
#define MAXF 8192
static float g_bufA[MAXF * 2], g_bufB[MAXF * 2], g_bufC[MAXF * 2];
static short g_shortBuf[MAXF * 2];

static int close_f(float a, float b, float tol)
{
	return fabs((double)a - (double)b) <= (double)tol;
}

/* Reference render of the main linked pair for a whole buffer. */
struct ref_opts
{
	double ratio;
	unsigned endRel;
	int loop;
	unsigned loopStart, loopEnd;
	float gainL, gainR;
	int filter;      /* enable independent biquad history */
	float filterFcHz;
};

static void ref_render(const short *pcm, unsigned leftBase, unsigned rightBase,
	const struct ref_opts *o, int n, float *outL, float *outR)
{
	struct ref_bq fqL, fqR;
	double rel = 0.0;
	int i;
	if (o->filter) { ref_bq_setup(&fqL, o->filterFcHz); fqR = fqL; }
	for (i = 0; i < n; ++i)
	{
		float l, r;
		ref_frame(pcm, leftBase, rightBase, o->endRel, o->loop, o->loopStart, o->loopEnd,
			rel, o->filter ? &fqL : NULL, o->filter ? &fqR : NULL, &l, &r);
		outL[i] = l * o->gainL;
		outR[i] = r * o->gainR;
		rel = ref_advance(rel, o->ratio, o->loop, o->loopStart, o->loopEnd);
	}
}

static void ref_opts_default(struct ref_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->ratio = 22050.0 / 32000.0;
	o->endRel = MAIN_LEFT_END - MAIN_LEFT;
	o->loop = 1;
	o->loopStart = MAIN_LOOP_START - MAIN_LEFT;
	o->loopEnd = MAIN_LOOP_END - MAIN_LEFT;
	o->gainL = 1.0f;
	o->gainR = 1.0f;
}

/* Compare a stereo render of the main pair against the reference. */
static int check_main_render(const float *buf, int n, const struct ref_opts *o)
{
	int i;
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, o, n, g_bufC, g_bufC + MAXF);
	for (i = 0; i < n; ++i)
	{
		TASK_CHECK(close_f(buf[i * 2], g_bufC[i], FLOAT_TOL));
		TASK_CHECK(close_f(buf[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
	}
	return 0;
}

/* A pair of linked samples far enough apart to exercise coarse offsets. */
enum
{
	BIG_LEN = 70000,
	BIG_LEFT = 0,
	BIG_RIGHT = 70000,
	BIG_COUNT = 140000
};
static short g_big[BIG_COUNT];
static int g_bigInit;

static int big_l(int i) { return ((i * 29) % 241 - 120) * 200; }
static int big_r(int i) { return ((i * 61) % 199 - 99) * 250; }

static void init_big(void)
{
	int i;
	if (g_bigInit) return;
	g_bigInit = 1;
	for (i = 0; i < BIG_LEN; ++i) g_big[BIG_LEFT + i] = (short)big_l(i);
	for (i = 0; i < BIG_LEN; ++i) g_big[BIG_RIGHT + i] = (short)big_r(i);
}

static void big_pair_font(struct tb_font *d)
{
	memset(d, 0, sizeof(*d));
	d->pcm = g_big;
	d->pcmCount = BIG_COUNT;
	d->shdr[0].name = "BigL";
	d->shdr[0].start = BIG_LEFT; d->shdr[0].end = BIG_LEFT + BIG_LEN;
	d->shdr[0].startLoop = BIG_LEFT; d->shdr[0].endLoop = BIG_LEFT;
	d->shdr[0].rate = 22050; d->shdr[0].root = 60; d->shdr[0].link = 1; d->shdr[0].type = 4;
	d->shdr[1].name = "BigR";
	d->shdr[1].start = BIG_RIGHT; d->shdr[1].end = BIG_RIGHT + BIG_LEN;
	d->shdr[1].startLoop = BIG_RIGHT; d->shdr[1].endLoop = BIG_RIGHT;
	d->shdr[1].rate = 22050; d->shdr[1].root = 60; d->shdr[1].link = 0; d->shdr[1].type = 2;
	d->shdrCount = 2;
}

/* ------------------------------------------------------------------ */
/* Pairing group.                                                     */
/* ------------------------------------------------------------------ */
static int case_pair_left_entry(void)
{
	struct tb_font d;
	tsf *f;
	enum { N = 512 };
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 4));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	TASK_CHECK(tsf_active_voice_count(f) == 1); /* one voice holding both sides */
	tsf_render_float(f, g_bufA, N, 0);
	{
		struct ref_opts o; int i;
		ref_opts_default(&o);
		TASK_CHECK(check_main_render(g_bufA, N, &o) == 0);
		ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
		for (i = 0; i < N; ++i)
		{
			TASK_CHECK(close_f(g_bufA[i * 2], g_bufC[i], FLOAT_TOL));
			TASK_CHECK(close_f(g_bufA[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
		}
	}
	tsf_close(f);
	return 0;
}

static int case_pair_right_entry(void)
{
	struct tb_font d;
	tsf *f;
	enum { N = 512 };
	int i;
	struct ref_opts o;
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 1); /* reference the right end instead */
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 4));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	TASK_CHECK(tsf_active_voice_count(f) == 1);
	tsf_render_float(f, g_bufA, N, 0);
	ref_opts_default(&o);
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
	for (i = 0; i < N; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i * 2], g_bufC[i], FLOAT_TOL));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
		/* Deliberately different left/right source signals must survive. */
	}
	TASK_CHECK(fabs((double)g_bufA[0] - (double)g_bufA[1]) > 1e-3);
	tsf_close(f);
	return 0;
}

/* Build the two-shdr main pair with one property of the right end altered. */
static int pair_mutation_variant(int which)
{
	struct tb_font d;
	tsf *f;
	init_pcm();
	sh_main_pair(&d, g_main);
	switch (which)
	{
		case 0: d.shdr[1].rate = 44100; break;
		case 1: d.shdr[1].root = 61; break;
		case 2: d.shdr[1].corr = 7; break;
		case 3: d.shdr[1].end = MAIN_RIGHT + 255; break;               /* length differs */
		case 4: d.shdr[1].startLoop = MAIN_RIGHT + 33; break;          /* loop start offset */
		case 5: d.shdr[1].endLoop = MAIN_RIGHT + 191; break;           /* loop length */
		case 6: d.shdr[1].link = 5; break;                             /* not reciprocal */
		case 7: d.shdr[0].link = 99; break;                            /* missing end */
		case 8: d.shdr[0].link = 2; break;                             /* points at terminator */
		case 9: d.shdr[1].type = 1; break;                             /* wrong type */
		default: break;
	}
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 4));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	TASK_CHECK(tsf_active_voice_count(f) == 0); /* region skipped, no sound */
	tsf_close(f);
	return 0;
}

static int case_pair_reject_metadata(void)
{
	int v;
	for (v = 0; v <= 5; ++v) { if (pair_mutation_variant(v)) return 1; }
	return 0;
}

static int case_pair_reject_links(void)
{
	int v;
	for (v = 6; v <= 9; ++v) { if (pair_mutation_variant(v)) return 1; }
	return 0;
}

static int case_pair_reject_compressed_rom(void)
{
	struct tb_font d;
	tsf *f;
	int variant;
	init_pcm();
	for (variant = 0; variant < 4; ++variant)
	{
		sh_main_pair(&d, g_main);
		if (variant < 2)
		{
			/* Compression flags on top of the stereo markers. */
			d.shdr[0].type = (tb_u16)(0x0010 | 4);
			d.shdr[1].type = (tb_u16)(0x0010 | 2);
		}
		else
		{
			/* ROM sample markers. */
			d.shdr[0].type = (tb_u16)(0x8000 | 4);
			d.shdr[1].type = (tb_u16)(0x8000 | 2);
		}
		zone_play(&d.zone[0], variant & 1 ? 1 : 0);
		d.zoneCount = 1;
		f = tb_load(&d);
		TASK_CHECK(f != NULL);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
		TASK_CHECK(tsf_set_max_voices(f, 4));
		TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
		TASK_CHECK(tsf_active_voice_count(f) == 0);
		tsf_close(f);
	}
	return 0;
}

static int case_pair_mono_unchanged(void)
{
	struct tb_font d;
	tsf *f;
	enum { N = 256 };
	int i;
	init_pcm();
	memset(&d, 0, sizeof(d));
	d.pcm = g_mono; d.pcmCount = 256;
	d.shdr[0].name = "Mono";
	d.shdr[0].start = 0; d.shdr[0].end = 256;
	d.shdr[0].startLoop = 32; d.shdr[0].endLoop = 224;
	d.shdr[0].rate = 22050; d.shdr[0].root = 60; d.shdr[0].link = 0; d.shdr[0].type = 1;
	d.shdrCount = 1;
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	TASK_CHECK(tsf_active_voice_count(f) == 1);
	tsf_render_float(f, g_bufA, N, 0);
	for (i = 0; i < N; ++i)
	{
		int pos = i < 224 ? i : 32 + (i - 224) % 192;
		double expected = (double)g_mono[pos] / 32767.0 * sqrt(0.5);
		TASK_CHECK(close_f(g_bufA[i * 2], (float)expected, 2e-6f));
		TASK_CHECK(g_bufA[i * 2] == g_bufA[i * 2 + 1]);
	}
	tsf_close(f);
	return 0;
}

static int case_pair_generator_shortens(void)
{
	struct tb_font d;
	tsf *f;
	struct tb_gen g[3];
	enum { N = 300 };
	int i;
	init_big();
	big_pair_font(&d);
	/* Fine address generators shorten both ends by 16 samples. */
	g[0].op = 0; g[0].amount = 16;   /* startAddrsOffset */
	g[1].op = 1; g[1].amount = -16;  /* endAddrsOffset */
	g[2].op = 8; g[2].amount = 13500;
	zone_gens(&d.zone[0], 0, 3, g); /* loop mode left at none */
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	TASK_CHECK(tsf_active_voice_count(f) == 1);
	tsf_render_float(f, g_bufA, N, 0);
	for (i = 0; i < N; ++i)
	{
		double expectedL = (double)g_big[BIG_LEFT + 16 + i] / 32767.0;
		double expectedR = (double)g_big[BIG_RIGHT + 16 + i] / 32767.0;
		TASK_CHECK(close_f(g_bufA[i * 2], (float)expectedL, 2e-6f));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], (float)expectedR, 2e-6f));
	}
	tsf_close(f);
	return 0;
}

static int case_pair_generator_coarse_signed(void)
{
	struct tb_font d;
	tsf *f;
	struct tb_gen g[3];
	enum { NEWLEN = BIG_LEN - 65536 };
	int i;
	init_big();
	big_pair_font(&d);
	/* Start coarse +1 (32768) and end fine -32768 shrink the interval to
	 * 4464 samples, still inside each original [start, end). */
	g[0].op = 4; g[0].amount = 1;
	g[1].op = 1; g[1].amount = -32768;
	g[2].op = 8; g[2].amount = 13500;
	zone_gens(&d.zone[0], 0, 3, g);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	TASK_CHECK(tsf_active_voice_count(f) == 1);
	tsf_render_float(f, g_bufA, 64, 0);
	for (i = 0; i < 64; ++i)
	{
		double expectedL = (double)g_big[BIG_LEFT + 32768 + i] / 32767.0;
		double expectedR = (double)g_big[BIG_RIGHT + 32768 + i] / 32767.0;
		TASK_CHECK(close_f(g_bufA[i * 2], (float)expectedL, 2e-6f));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], (float)expectedR, 2e-6f));
	}
	/* Play until the shortened interval is consumed; without looping the
	 * voice must end and the next block must not read past it. */
	tsf_render_float(f, g_bufA, NEWLEN + 256, 0);
	TASK_CHECK(tsf_active_voice_count(f) == 0);
	tsf_close(f);
	return 0;
}

static int case_pair_generator_overflow_rejected(void)
{
	struct tb_font d;
	tsf *f;
	struct tb_gen g[3];
	int variant;
	init_big();
	for (variant = 0; variant < 3; ++variant)
	{
		big_pair_font(&d);
		g[0].op = 8; g[0].amount = 13500;
		if (variant == 0)
		{
			/* Start fine -32768: unsigned arithmetic would wrap to an
			 * apparently in-range 7232 and play the wrong samples. */
			g[1].op = 0; g[1].amount = -32768;
			g[2].op = 1; g[2].amount = 0;
		}
		else if (variant == 1)
		{
			/* End fine +32767 extends past the original end. */
			g[1].op = 0; g[1].amount = 0;
			g[2].op = 1; g[2].amount = 32767;
		}
		else
		{
			/* Start coarse +2 pushes start past the original end. */
			g[1].op = 4; g[1].amount = 2;
			g[2].op = 1; g[2].amount = -32768;
		}
		zone_gens(&d.zone[0], 0, 3, g);
		d.zoneCount = 1;
		f = tb_load(&d);
		TASK_CHECK(f != NULL);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		TASK_CHECK(tsf_set_max_voices(f, 2));
		TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
		TASK_CHECK(tsf_active_voice_count(f) == 0);
		tsf_render_float(f, g_bufA, 64, 0);
		TASK_CHECK(tsf_active_voice_count(f) == 0);
		tsf_close(f);
	}
	return 0;
}

static int case_pair_bad_does_not_pollute(void)
{
	struct tb_font d;
	tsf *f;
	enum { N = 256 };
	int order;
	init_pcm();
	for (order = 0; order < 2; ++order)
	{
		struct ref_opts o; int i;
		sh_main_pair(&d, g_main);
		/* A second pair (indices 2/3) with mismatched sample rates. */
		d.shdr[2].name = "BadL";
		d.shdr[2].start = 3000; d.shdr[2].end = 3128;
		d.shdr[2].startLoop = 3000; d.shdr[2].endLoop = 3000;
		d.shdr[2].rate = 22050; d.shdr[2].root = 60; d.shdr[2].link = 3; d.shdr[2].type = 4;
		d.shdr[3].name = "BadR";
		d.shdr[3].start = 3200; d.shdr[3].end = 3328;
		d.shdr[3].startLoop = 3200; d.shdr[3].endLoop = 3200;
		d.shdr[3].rate = 44100; d.shdr[3].root = 60; d.shdr[3].link = 2; d.shdr[3].type = 2;
		d.shdrCount = 4;
		if (order == 0) { zone_play(&d.zone[0], 0); zone_play(&d.zone[1], 2); }
		else            { zone_play(&d.zone[0], 2); zone_play(&d.zone[1], 0); }
		d.zoneCount = 2;
		f = tb_load(&d);
		TASK_CHECK(f != NULL);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
		TASK_CHECK(tsf_set_max_voices(f, 4));
		TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
		TASK_CHECK(tsf_active_voice_count(f) == 1); /* only the valid layer */
		tsf_render_float(f, g_bufA, N, 0);
		ref_opts_default(&o);
		ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
		for (i = 0; i < N; ++i)
		{
			TASK_CHECK(close_f(g_bufA[i * 2], g_bufC[i], FLOAT_TOL));
			TASK_CHECK(close_f(g_bufA[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
		}
		tsf_close(f);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Render group.                                                      */
/* ------------------------------------------------------------------ */

/* Ratio that lands on fractional positions and wraps the loop several
 * times over the buffer. */
static int case_render_fractional_loop(void)
{
	struct tb_font d;
	tsf *f;
	enum { N = 1200 };
	struct ref_opts o;
	int i;
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	tsf_render_float(f, g_bufA, N, 0);
	ref_opts_default(&o);
	o.ratio = 22050.0 / 32000.0;
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
	for (i = 0; i < N; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i * 2], g_bufC[i], FLOAT_TOL));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
	}
	/* The seam must use each side's own loop samples, not the other side. */
	{
		int seam = -1;
		double rel = 0.0;
		for (i = 0; i < N; ++i)
		{
			double next = ref_advance(rel, o.ratio, 1, o.loopStart, o.loopEnd);
			if ((unsigned)rel < o.loopStart && (unsigned)next >= o.loopStart) { seam = i; }
			rel = next;
		}
		TASK_CHECK(seam >= 0);
	}
	/* Cross multiple render calls; state must carry over. */
	tsf_close(f);
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	tsf_render_float(f, g_bufA, 37, 0);
	tsf_render_float(f, g_bufA + 37 * 2, N - 37, 0);
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
	for (i = 0; i < N; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i * 2], g_bufC[i], FLOAT_TOL));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
	}
	tsf_close(f);
	return 0;
}

static int case_render_zone_independence(void)
{
	/* Two zones referencing the same linked pair are separate timbre layers:
	 * both must play, neither is deduplicated across zones. */
	struct tb_font d;
	tsf *f;
	enum { N = 128 };
	int i;
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 0);
	zone_play(&d.zone[1], 1); /* same pair, other end, own layer */
	d.zoneCount = 2;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 4));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	TASK_CHECK(tsf_active_voice_count(f) == 2);
	tsf_render_float(f, g_bufA, N, 0);
	{
		struct ref_opts o;
		ref_opts_default(&o);
		ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
	}
	/* Each layer contributes the same left/right stream, summed. */
	for (i = 0; i < N; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i * 2], (float)(2.0 * g_bufC[i]), FLOAT_TOL));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], (float)(2.0 * g_bufC[MAXF + i]), FLOAT_TOL));
	}
	tsf_close(f);
	return 0;
}

static int case_render_independent_filter_history(void)
{
	struct tb_font d;
	tsf *f;
	enum { N = 400 };
	struct tb_gen g[2];
	struct ref_opts o;
	int i;
	init_pcm();
	sh_main_pair(&d, g_main);
	/* Enable the lowpass at a modest cutoff so its history matters. */
	g[0].op = 54; g[0].amount = 1;
	g[1].op = 8; g[1].amount = 6000;
	zone_gens(&d.zone[0], 0, 2, g);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	tsf_render_float(f, g_bufA, N, 0);
	ref_opts_default(&o);
	o.ratio = 1.0;
	o.filter = 1;
	o.filterFcHz = 8.176f * (float)pow(2.0, 6000.0 / 1200.0);
	o.filterFcHz /= 22050.0f; /* normalized to output rate */
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
	for (i = 0; i < N; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i * 2], g_bufC[i], FLOAT_TOL));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
	}
	/* Right filter history must never feed the left output. Recompute the
	 * left channel with a zeroed right signal; the left must be identical. */
	{
		struct ref_bq fqL, fqR;
		double rel = 0.0;
		ref_bq_setup(&fqL, o.filterFcHz);
		fqR = fqL;
		for (i = 0; i < N; ++i)
		{
			float l, r;
			ref_frame(g_main, MAIN_LEFT, MAIN_RIGHT, o.endRel, 1, o.loopStart, o.loopEnd,
				rel, &fqL, &fqR, &l, &r);
			TASK_CHECK(close_f(g_bufA[i * 2], l, FLOAT_TOL));
			rel = ref_advance(rel, o.ratio, 1, o.loopStart, o.loopEnd);
		}
	}
	tsf_close(f);
	return 0;
}

/* Render a fixed balance, compare each side independently. */
static int render_balance(float pan, float panOffset, int expectedStereo)
{
	struct tb_font d;
	tsf *f;
	enum { N = 300 };
	struct tb_gen gens[3];
	struct ref_opts o;
	float p;
	int i;
	init_pcm();
	sh_main_pair(&d, g_main);
	gens[0].op = 54; gens[0].amount = 1;
	gens[1].op = 8;  gens[1].amount = 13500;
	gens[2].op = 17; gens[2].amount = (int)(pan * 1000.0f);
	zone_gens(&d.zone[0], 0, 3, gens);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	if (expectedStereo)
	{
		TASK_CHECK(tsf_channel_set_presetindex(f, 0, 0));
		TASK_CHECK(tsf_channel_set_pan(f, 0, panOffset + 0.5f));
		TASK_CHECK(tsf_channel_note_on(f, 0, 60, 1.0f));
	}
	else TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	TASK_CHECK(tsf_active_voice_count(f) == 1);
	tsf_render_float(f, g_bufA, N, 0);
	p = (expectedStereo ? pan + panOffset : pan);
	if (p > 0.5f) p = 0.5f;
	if (p < -0.5f) p = -0.5f;
	ref_opts_default(&o);
	o.gainL = (float)ref_pan_left(p);
	o.gainR = (float)ref_pan_right(p);
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
	for (i = 0; i < N; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i * 2], g_bufC[i], FLOAT_TOL));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
	}
	tsf_close(f);
	return 0;
}

static int case_render_balance_center(void)
{
	/* At center both linked sides are delivered at unit gain. */
	if (render_balance(0.0f, 0.0f, 0)) return 1;
	if (render_balance(0.0f, 0.0f, 1)) return 1;
	return 0;
}

static int case_render_balance_noncenter(void)
{
	if (render_balance(0.25f, 0.0f, 0)) return 1;
	if (render_balance(-0.375f, 0.0f, 0)) return 1;
	if (render_balance(0.0f, 0.25f, 1)) return 1;   /* region pan + channel offset */
	if (render_balance(0.2f, -0.3f, 1)) return 1;
	return 0;
}

static int case_render_layouts_and_mono(void)
{
	struct tb_font d;
	tsf *f;
	enum { N = 300 };
	struct ref_opts o;
	int i;
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_UNWEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	tsf_render_float(f, g_bufA, N, 0);
	ref_opts_default(&o);
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
	for (i = 0; i < N; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i], g_bufC[i], FLOAT_TOL));            /* left plane */
		TASK_CHECK(close_f(g_bufA[N + i], g_bufC[MAXF + i], FLOAT_TOL)); /* right plane */
	}
	tsf_close(f);

	/* Interleaved and unweaved must agree sample for sample. */
	{
		tsf *fi = tb_load(&d), *fu = tb_load(&d);
		TASK_CHECK(fi && fu);
		tsf_set_output(fi, TSF_STEREO_INTERLEAVED, 32000, 0);
		tsf_set_output(fu, TSF_STEREO_UNWEAVED, 32000, 0);
		TASK_CHECK(tsf_set_max_voices(fi, 2));
		TASK_CHECK(tsf_set_max_voices(fu, 2));
		TASK_CHECK(tsf_note_on(fi, 0, 60, 1.0f));
		TASK_CHECK(tsf_note_on(fu, 0, 60, 1.0f));
		tsf_render_float(fi, g_bufA, N, 0);
		tsf_render_float(fu, g_bufB, N, 0);
		for (i = 0; i < N; ++i)
		{
			TASK_CHECK(g_bufA[i * 2] == g_bufB[i]);
			TASK_CHECK(g_bufA[i * 2 + 1] == g_bufB[N + i]);
		}
		tsf_close(fi);
		tsf_close(fu);
	}

	/* Mono is half the weighted sum of the two balanced sides. */
	{
		struct ref_bq fqL, fqR; /* unused, kept for symmetry */
		double rel = 0.0;
		(void)fqL; (void)fqR;
		f = tb_load(&d);
		TASK_CHECK(f != NULL);
		tsf_set_output(f, TSF_MONO, 32000, 0);
		TASK_CHECK(tsf_set_max_voices(f, 2));
		TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
		tsf_render_float(f, g_bufA, N, 0);
		ref_opts_default(&o);
		ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
		for (i = 0; i < N; ++i)
		{
			float expected = (g_bufC[i] * 1.0f + g_bufC[MAXF + i] * 1.0f) * 0.5f;
			TASK_CHECK(close_f(g_bufA[i], expected, FLOAT_TOL));
		}
		(void)rel;
		tsf_close(f);
	}
	return 0;
}

static int case_render_short_and_mixing(void)
{
	struct tb_font d;
	tsf *f;
	enum { N = 200 };
	struct ref_opts o;
	int i;
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	ref_opts_default(&o);
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);

	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	for (i = 0; i < N * 2; ++i) g_shortBuf[i] = 0;
	tsf_render_short(f, g_shortBuf, N, 0);
	for (i = 0; i < N; ++i)
	{
		int el = (int)ref_short_single(g_bufC[i]);
		int er = (int)ref_short_single(g_bufC[MAXF + i]);
		TASK_CHECK(abs((int)g_shortBuf[i * 2] - el) <= 2);
		TASK_CHECK(abs((int)g_shortBuf[i * 2 + 1] - er) <= 2);
	}
	tsf_close(f);

	/* Mixing adds to pre-existing data with the same saturation rules. */
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	for (i = 0; i < N * 2; ++i) g_shortBuf[i] = (short)((i * 37) % 2001 - 1000);
	tsf_render_short(f, g_shortBuf, N, 1);
	for (i = 0; i < N; ++i)
	{
		int inL = (short)((i * 2 * 37) % 2001 - 1000);
		int inR = (short)(((i * 2 + 1) * 37) % 2001 - 1000);
		int el = (int)ref_short_single(g_bufC[i]);
		int er = (int)ref_short_single(g_bufC[MAXF + i]);
		TASK_CHECK(abs((int)g_shortBuf[i * 2] - (int)(short)(inL + el)) <= 2);
		TASK_CHECK(abs((int)g_shortBuf[i * 2 + 1] - (int)(short)(inR + er)) <= 2);
	}
	tsf_close(f);

	/* Saturation: a loud pair must clamp, not wrap. */
	{
		static short loud[MAIN_COUNT];
		float expected = (float)(32000.0 / 32767.0);
		int j;
		for (j = 0; j < MAIN_COUNT; ++j) loud[j] = 0;
		for (j = 0; j < 256; ++j) { loud[MAIN_LEFT + j] = 32000; loud[MAIN_RIGHT + j] = 32000; }
		sh_main_pair(&d, loud);
		zone_play(&d.zone[0], 0);
		d.zoneCount = 1;
		f = tb_load(&d);
		TASK_CHECK(f != NULL);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
		TASK_CHECK(tsf_set_max_voices(f, 2));
		TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
		tsf_render_short(f, g_shortBuf, 64, 0);
		for (i = 0; i < 128; ++i)
			TASK_CHECK(abs((int)g_shortBuf[i] - (int)ref_short_single(expected)) <= 2);
		tsf_close(f);

		/* The float render of the same loud source stays in [-1, 1] and
		 * matches the original PCM formula exactly. */
		f = tb_load(&d);
		TASK_CHECK(f != NULL);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
		TASK_CHECK(tsf_set_max_voices(f, 2));
		TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
		tsf_render_float(f, g_bufA, 64, 0);
		for (i = 0; i < 128; ++i)
			TASK_CHECK(close_f(g_bufA[i], expected, FLOAT_TOL));
		tsf_close(f);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Lifetime group.                                                    */
/* ------------------------------------------------------------------ */
static int case_lifetime_copy_independent(void)
{
	struct tb_font d;
	tsf *control, *copy;
	enum { N = 300 };
	struct ref_opts o;
	int i;
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	control = tb_load(&d);
	TASK_CHECK(control != NULL);
	tsf_set_output(control, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(control, 2));
	TASK_CHECK(tsf_note_on(control, 0, 60, 1.0f));
	copy = tsf_copy(control);
	TASK_CHECK(copy != NULL);
	tsf_close(control); /* shared read-only font survives */
	tsf_set_output(copy, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(copy, 2));
	TASK_CHECK(tsf_note_on(copy, 0, 60, 1.0f));
	tsf_render_float(copy, g_bufA, N, 0);
	ref_opts_default(&o);
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
	for (i = 0; i < N; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i * 2], g_bufC[i], FLOAT_TOL));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
	}
	tsf_note_off(copy, 0, 60);
	for (i = 0; i < 8; ++i) tsf_render_float(copy, g_bufB, N, 0);
	TASK_CHECK(tsf_active_voice_count(copy) == 0);
	tsf_close(copy);
	return 0;
}

static int case_lifetime_no_alloc_after_prealloc(void)
{
	struct tb_font d;
	tsf *f;
	enum { N = 300 };
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 4));
	TASK_CHECK(tsf_channel_set_presetindex(f, 0, 0)); /* pre-create channel */
	TASK_CHECK(tsf_channel_set_pan(f, 0, 0.5f));

	task_alloc_reset();
	TASK_CHECK(tsf_channel_note_on(f, 0, 60, 1.0f));
	TASK_CHECK(tsf_note_on(f, 0, 64, 0.8f));
	tsf_render_float(f, g_bufA, N, 0);
	tsf_note_off(f, 0, 60);
	tsf_channel_note_off(f, 0, 64);
	tsf_render_float(f, g_bufB, N, 0);
	TASK_CHECK(task_alloc_calls() == 0);
	tsf_close(f);
	return 0;
}

static int case_lifetime_shared_font_preserved(void)
{
	/* Two simultaneously live copies must each hear the same linked pair
	 * without duplicating the right side or reading shdr at render time. */
	struct tb_font d;
	tsf *a, *b;
	enum { N = 300 };
	struct ref_opts o;
	int i;
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	a = tb_load(&d);
	TASK_CHECK(a != NULL);
	b = tsf_copy(a);
	TASK_CHECK(b != NULL);
	tsf_set_output(a, TSF_STEREO_INTERLEAVED, 32000, 0);
	tsf_set_output(b, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(a, 2));
	TASK_CHECK(tsf_set_max_voices(b, 2));
	TASK_CHECK(tsf_note_on(a, 0, 60, 1.0f));
	TASK_CHECK(tsf_note_on(b, 0, 60, 1.0f));
	tsf_render_float(a, g_bufA, N, 0);
	tsf_render_float(b, g_bufB, N, 0);
	ref_opts_default(&o);
	ref_render(g_main, MAIN_LEFT, MAIN_RIGHT, &o, N, g_bufC, g_bufC + MAXF);
	for (i = 0; i < N; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i * 2], g_bufC[i], FLOAT_TOL));
		TASK_CHECK(close_f(g_bufB[i * 2], g_bufC[i], FLOAT_TOL));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
		TASK_CHECK(close_f(g_bufB[i * 2 + 1], g_bufC[MAXF + i], FLOAT_TOL));
	}
	tsf_close(a);
	tsf_close(b);
	return 0;
}

static int case_render_nonloop_end_clamp(void)
{
	/* A non-looping pair must end cleanly: the final interpolation neighbor
	 * is each side's own last sample, never the sentinel stored after it or
	 * the other side's data. */
	struct tb_font d;
	tsf *f;
	enum { N = 300 };
	struct tb_gen g[2];
	int i;
	init_pcm();
	sh_main_pair(&d, g_main);
	g[0].op = 54; g[0].amount = 0;   /* no looping */
	g[1].op = 8;  g[1].amount = 13500;
	zone_gens(&d.zone[0], 0, 2, g);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0); /* ratio 1 */
	TASK_CHECK(tsf_set_max_voices(f, 4));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	tsf_render_float(f, g_bufA, N, 0);
	/* Exactly 256 whole samples are playable, then the voice ends. */
	for (i = 0; i < 256; ++i)
	{
		TASK_CHECK(close_f(g_bufA[i * 2], pcm_f(g_main, MAIN_LEFT + i), 2e-6f));
		TASK_CHECK(close_f(g_bufA[i * 2 + 1], pcm_f(g_main, MAIN_RIGHT + i), 2e-6f));
	}
	for (i = 256; i < N; ++i)
	{
		TASK_CHECK(g_bufA[i * 2] == 0.0f);
		TASK_CHECK(g_bufA[i * 2 + 1] == 0.0f);
	}
	TASK_CHECK(tsf_active_voice_count(f) == 0);
	tsf_close(f);
	return 0;
}

static int case_lifetime_note_off_envelope(void)
{
	/* One envelope, LFO and noteoff state drive the whole stereo region. */
	struct tb_font d;
	tsf *f;
	enum { N = 4096 };
	init_pcm();
	sh_main_pair(&d, g_main);
	zone_play(&d.zone[0], 0);
	d.zoneCount = 1;
	f = tb_load(&d);
	TASK_CHECK(f != NULL);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 32000, 0);
	TASK_CHECK(tsf_set_max_voices(f, 2));
	TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
	tsf_render_float(f, g_bufA, N / 4, 0);
	TASK_CHECK(tsf_active_voice_count(f) == 1);
	tsf_note_off(f, 0, 60);
	tsf_render_float(f, g_bufA, N, 0);
	TASK_CHECK(tsf_active_voice_count(f) == 0);
	tsf_close(f);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Case table.                                                        */
/* ------------------------------------------------------------------ */
const struct task_case task_cases[] =
{
	{"pairing", "left_entry",                 case_pair_left_entry},
	{"pairing", "right_entry",                case_pair_right_entry},
	{"pairing", "reject_metadata",            case_pair_reject_metadata},
	{"pairing", "reject_links",               case_pair_reject_links},
	{"pairing", "reject_compressed_rom",      case_pair_reject_compressed_rom},
	{"pairing", "mono_unchanged",             case_pair_mono_unchanged},
	{"pairing", "generator_shortens",         case_pair_generator_shortens},
	{"pairing", "generator_coarse_signed",    case_pair_generator_coarse_signed},
	{"pairing", "generator_overflow_rejected",case_pair_generator_overflow_rejected},
	{"pairing", "bad_does_not_pollute",       case_pair_bad_does_not_pollute},

	{"render",  "fractional_loop",            case_render_fractional_loop},
	{"render",  "zone_independence",          case_render_zone_independence},
	{"render",  "independent_filter_history", case_render_independent_filter_history},
	{"render",  "balance_center",             case_render_balance_center},
	{"render",  "balance_noncenter",          case_render_balance_noncenter},
	{"render",  "layouts_and_mono",           case_render_layouts_and_mono},
	{"render",  "short_and_mixing",           case_render_short_and_mixing},
	{"render",  "nonloop_end_clamp",          case_render_nonloop_end_clamp},

	{"lifetime", "copy_independent",          case_lifetime_copy_independent},
	{"lifetime", "no_alloc_after_prealloc",   case_lifetime_no_alloc_after_prealloc},
	{"lifetime", "shared_font_preserved",     case_lifetime_shared_font_preserved},
	{"lifetime", "note_off_envelope",         case_lifetime_note_off_envelope}
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
