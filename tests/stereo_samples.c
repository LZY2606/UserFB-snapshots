/* Stereo S3M sample support tests, CC0.
 *
 * All Scream Tracker 3 modules used here are synthesized in C: no external
 * recordings are loaded. The expected audio is produced by a small,
 * independently written reference player (period/frequency -> microstep
 * phase -> interpolation/loop), never by copying values out of libxm's
 * playback functions. */

#include "contract.h"
#include <xm.h>

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Match the library defaults used by the check builds (int16 points). */
#define REF_MICROSTEPS 4096u
#define REF_TICK_SUBSAMPLES 8192u
#define REF_AMPLIFICATION 0.25f
#define REF_RAMPING_POINTS 255u
#define REF_RATE 48000u
#define REF_BPM 125u
#define REF_TEMPO 1u

/* ----- small byte writer ----- */

struct buf {
	unsigned char* data;
	uint32_t len;
	uint32_t cap;
};

static void buf_reserve(struct buf* b, uint32_t n) {
	if(b->len + n > b->cap) {
		b->cap = (b->cap + n) * 2;
		b->data = realloc(b->data, b->cap);
		assert(b->data);
	}
}

static void buf_put(struct buf* b, const void* p, uint32_t n) {
	buf_reserve(b, n);
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

static void buf_u8(struct buf* b, unsigned v) {
	unsigned char x = (unsigned char)v;
	buf_put(b, &x, 1);
}

static void buf_zero(struct buf* b, uint32_t n) {
	buf_reserve(b, n);
	memset(b->data + b->len, 0, n);
	b->len += n;
}

static void put_u16_at(unsigned char* p, unsigned v) {
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32_at(unsigned char* p, uint32_t v) {
	put_u16_at(p, v & 0xFFFFu);
	put_u16_at(p + 2, v >> 16);
}

static void buf_align16(struct buf* b) {
	while(b->len & 15u) buf_u8(b, 0);
}

/* ----- synthesized S3M module ----- */

struct ref_sample {
	uint32_t length;       /* frames per channel as declared in the file */
	uint32_t loop_start;
	uint32_t loop_end;
	uint8_t flags;         /* S3M sample flags: bit1 stereo, bit2 16-bit */
	const int16_t* left;   /* logical frame values, `length` entries */
	const int16_t* right;  /* non-NULL only for stereo */
};

struct ref_row {
	uint8_t note;          /* 0 = no note */
	uint8_t instrument;    /* 1-based, 0 = none */
	uint8_t volume;        /* 0 = leave unset, else 1..64 */
	uint8_t effect;        /* 0 = none (raw S3M effect number) */
	uint8_t effect_param;
};

struct ref_module {
	bool signed_samples;
	bool stereo_flag;      /* module stereo flag (header byte 51) */
	uint8_t num_samples;
	const struct ref_sample* samples;
	uint8_t num_rows;
	const struct ref_row* rows;
};

/* Serialize one module. pcm_abs[] receives the absolute file offset of each
 * sample's PCM block (for raw layout assertions). */
static struct buf build_s3m(const struct ref_module* m, uint32_t* pcm_abs) {
	struct buf b = {0};
	const uint8_t ns = m->num_samples;
	const uint8_t np = 1;

	/* ---- 96-byte header ---- */
	buf_zero(&b, 96);
	memcpy(b.data, "stereo synth S3M", 16);
	b.data[28] = 0x1A;
	b.data[29] = 0x10;
	put_u16_at(b.data + 32, 1);                 /* orders */
	put_u16_at(b.data + 34, ns);                /* samples */
	put_u16_at(b.data + 36, np);                /* patterns */
	put_u16_at(b.data + 40, 0x1300);            /* tracker version */
	put_u16_at(b.data + 42, m->signed_samples ? 1u : 0u);
	memcpy(b.data + 44, "SCRM", 4);
	b.data[48] = 64;                            /* global volume */
	b.data[49] = REF_TEMPO;
	b.data[50] = REF_BPM;
	b.data[51] = m->stereo_flag ? 0x80 : 0x00;
	b.data[64] = 0;                             /* channel 0 = left PCM */
	for(uint8_t i = 1; i < 32; ++i) b.data[64 + i] = 255;

	/* ---- pointer table: POT, sample paras, pattern paras ---- */
	buf_reserve(&b, 98u + 2u * ns + 2u * np - 96u);
	b.data[96] = 0;                            /* POT[0] -> pattern 0 */
	/* Layout each instrument header at a 16-byte aligned offset. */
	uint32_t pt = 98u + 2u * ns + 2u * np;
	uint32_t instr_abs[64];
	for(uint8_t i = 0; i < ns; ++i) {
		pt = (pt + 15u) & ~15u;
		put_u16_at(b.data + 97 + 2 * i, pt / 16u);
		instr_abs[i] = pt;
		pt += 96;
	}
	uint32_t pattern_para_pos = 97u + 2u * ns;
	uint32_t pattern_abs = (pt + 15u) & ~15u;

	/* ---- pattern (serialized first so its size is known) ---- */
	struct buf pat = {0};
	for(uint8_t r = 0; r < m->num_rows; ++r) {
		const struct ref_row* e = &m->rows[r];
		uint8_t what = 0;
		if(e->note && e->instrument) what |= 0x20;
		if(e->volume) what |= 0x40;
		if(e->effect) what |= 0x80;
		if(what) {
			buf_u8(&pat, what);
			if(what & 0x20) {
				uint8_t oct = (uint8_t)((e->note - 1u) / 12u);
				uint8_t semi = (uint8_t)((e->note - 1u) % 12u);
				buf_u8(&pat, (uint8_t)((oct << 4) | semi));
				buf_u8(&pat, e->instrument);
			}
			if(what & 0x40) buf_u8(&pat, e->volume);
			if(what & 0x80) {
				buf_u8(&pat, e->effect);
				buf_u8(&pat, e->effect_param);
			}
		}
		buf_u8(&pat, 0);                       /* end of row */
	}

	/* Predict the aligned PCM block offsets so the instrument headers can
	 * carry their 24-bit data paragraph pointer. */
	uint32_t cursor = (pattern_abs + 2u + pat.len + 15u) & ~15u;
	for(uint8_t i = 0; i < ns; ++i) {
		const struct ref_sample* s = &m->samples[i];
		uint32_t point = (s->flags & 4) ? 2u : 1u;
		uint32_t planes = (s->flags & 2) ? 2u : 1u;
		pcm_abs[i] = cursor;
		cursor += s->length * point * planes;
		cursor = (cursor + 15u) & ~15u;
	}

	/* pointer-table entries were written by direct indexing above, so the
	 * logical end of the buffer must be advanced explicitly. */
	b.len = 98u + 2u * ns + 2u * np;

	/* ---- instrument headers (96 bytes each) ---- */
	for(uint8_t i = 0; i < ns; ++i) {
		buf_align16(&b);
		assert(b.len == instr_abs[i]);
		uint32_t h = b.len;
		buf_zero(&b, 96);
		const struct ref_sample* s = &m->samples[i];
		b.data[h + 0] = 1;
		b.data[h + 28] = 64;
		b.data[h + 31] = s->flags;
		put_u32_at(b.data + h + 16, s->length);
		put_u32_at(b.data + h + 20, s->loop_start);
		put_u32_at(b.data + h + 24, s->loop_end);
		put_u32_at(b.data + h + 32, 8363u);
		/* 24-bit PCM paragraph pointer at offset 13..15 */
		uint32_t para = pcm_abs[i] / 16u;
		b.data[h + 13] = (unsigned char)(para >> 16);
		put_u16_at(b.data + h + 14, para & 0xFFFFu);
		memcpy(b.data + h + 48, "sample", 6);
	}

	/* ---- emit the pattern block: u16 packed length then the rows ---- */
	buf_align16(&b);
	assert(b.len == pattern_abs);
	put_u16_at(b.data + pattern_para_pos, pattern_abs / 16u);
	{
		unsigned char plen[2];
		put_u16_at(plen, pat.len);
		buf_put(&b, plen, 2);
	}
	buf_put(&b, pat.data, pat.len);
	free(pat.data);

	/* ---- PCM blocks, each 16-byte aligned ---- */
	for(uint8_t i = 0; i < ns; ++i) {
		const struct ref_sample* s = &m->samples[i];
		bool is16 = s->flags & 4;
		bool stereo = s->flags & 2;
		buf_align16(&b);
		assert(b.len == pcm_abs[i]);
		for(uint8_t plane = 0; plane < (stereo ? 2u : 1u); ++plane) {
			const int16_t* src = plane ? s->right : s->left;
			for(uint32_t k = 0; k < s->length; ++k) {
				int16_t v = src ? src[k] : 0;
				if(is16) {
					buf_u8(&b, (unsigned)(v & 0xFF));
					buf_u8(&b, (unsigned)((v >> 8) & 0xFF));
				} else if(m->signed_samples) {
					buf_u8(&b, (unsigned)(uint8_t)(int8_t)v);
				} else {
					buf_u8(&b, (unsigned)(uint8_t)(v + 128));
				}
			}
		}
	}

	return b;
}

/* ----- context helpers ----- */

static xm_context_t* load_buf(const struct buf* b) {
	xm_prescan_data_t* p = malloc(XM_PRESCAN_DATA_SIZE);
	assert(p);
	bool ok = xm_prescan_module((const char*)b->data, b->len, p);
	assert(ok);
	void* mem = malloc(xm_size_for_context(p));
	assert(mem);
	xm_context_t* ctx = xm_create_context(mem, p,
	                                      (const char*)b->data, b->len);
	free(p);
	xm_set_sample_rate(ctx, REF_RATE);
	return ctx;
}

static void reset_ctx(xm_context_t* ctx) {
	/* xm_reset_context() clears the configured output rate, so restore it
	 * the same way normal users do after a reset. */
	xm_reset_context(ctx);
	xm_set_sample_rate(ctx, REF_RATE);
}

static bool feq(double a, double b) {
	double d = fabs(a - b);
	double m = fmax(1e-12, fmax(fabs(a), fabs(b)));
	return d <= 1e-4 * m;
}

static uint32_t frames_per_row(void) {
	uint32_t tick = REF_RATE * (10u * REF_TICK_SUBSAMPLES / 4u) / REF_BPM;
	return tick / REF_TICK_SUBSAMPLES * REF_TEMPO;
}

static double* read_plane(xm_context_t* ctx, uint16_t smp, uint8_t ch,
                          uint32_t* out_len) {
	uint32_t n = 0;
	xm_sample_point_t* w =
		xm_get_sample_waveform_channel(ctx, smp, ch, &n);
	assert(w);
	double* out = malloc(sizeof(double) * (n ? n : 1));
	for(uint32_t i = 0; i < n; ++i) out[i] = (double)w[i] / 32768.0;
	if(out_len) *out_len = n;
	return out;
}

/* Reference forward-loop / one-shot scalar fetch. */
static double ref_fetch(const double* w, uint32_t length, uint32_t loop_len,
                        uint32_t pos) {
	uint32_t M = REF_MICROSTEPS;
	if(loop_len == 0) {
		if(pos >= length * M) return 0.0;
	} else if(pos >= length * M) {
		uint32_t off = (length - loop_len) * M;
		pos -= off;
		pos %= loop_len * M;
		pos += off;
	}
	uint32_t a = pos / M;
	uint32_t c = (loop_len == 0)
		? ((a + 1 < length) ? a + 1 : a)
		: ((a + 1 == length) ? length - loop_len : a + 1);
	double t = (double)(pos % M) / (double)M;
	return w[a] + t * (w[c] - w[a]);
}

/* Center-pan gain with volume 64 and global volume 64. */
static double ref_gain(void) {
	int64_t base = 64;
	base *= 64;
	base *= 32767;
	base /= 4;
	base *= 64;
	return (double)base / (double)INT32_MAX * sqrt(0.5);
}

/* Whether this libxm build ramps note changes (the XM_RAMPING macro is not
 * exported, so detect it from the first generated frame). */
static bool ramping_active(void) {
	static int16_t v[4] = { 64, 64, 64, 64 };
	struct ref_sample s = { 4, 0, 0, 0, v, 0 };
	struct ref_row rows[1] = { { 25, 1, 64, 0, 0 } };
	struct ref_module m = { true, false, 1, &s, 1, rows };
	uint32_t pcm[1];
	struct buf b = build_s3m(&m, pcm);
	xm_context_t* ctx = load_buf(&b);
	float f[2];
	xm_generate_samples(ctx, f, 1);
	bool ramp = (f[0] == 0.0f);
	free(ctx);
	free(b.data);
	return ramp;
}

/* ============================ load group ============================ */

void check_load(struct check_result* r) {
	static int16_t l8[40], r8[40];
	static int16_t l16[11], r16[11];
	static int16_t mono16[7];
	for(uint32_t i = 0; i < 40; ++i) {
		l8[i] = (int16_t)(20 + 2 * (int)i);    /* 8-bit signed range */
		r8[i] = (int16_t)(-20 - 2 * (int)i);
	}
	for(uint32_t i = 0; i < 11; ++i) {
		l16[i] = (int16_t)(4000 + 100 * (int)i);
		r16[i] = (int16_t)(-4000 - 100 * (int)i);
	}
	for(uint32_t i = 0; i < 7; ++i) mono16[i] = (int16_t)(1500 + 50 * (int)i);

	/* Sample 0 is 8-bit stereo, looped, with a nonzero tail past loop_end
	 * (length 40, loop 4..18). Sample 1 is 16-bit stereo with an odd frame
	 * count. Sample 2 is 16-bit mono. */
	struct ref_sample smp[3] = {
		{ 40, 4, 18, 0b011, l8, r8 },
		{ 11, 0, 0, 0b110, l16, r16 },
		{ 7, 0, 0, 0b100, mono16, 0 },
	};
	struct ref_row rows[2] = { { 25, 1, 64, 0, 0 }, { 0, 0, 0, 0, 0 } };
	struct ref_module m = { true, true, 3, smp, 2, rows };
	uint32_t pcm[3] = {0};
	struct buf b = build_s3m(&m, pcm);
	xm_context_t* ctx = load_buf(&b);

	Z_CHECK(r, xm_get_sample_channels(ctx, 0) == 2);
	Z_CHECK(r, xm_get_sample_channels(ctx, 1) == 2);
	Z_CHECK(r, xm_get_sample_channels(ctx, 2) == 1);
	Z_CHECK(r, xm_get_sample_channels(ctx, 99) == 0);

	uint32_t n0 = 0, n1 = 0, n2 = 0;
	xm_sample_point_t* w0l =
		xm_get_sample_waveform_channel(ctx, 0, 0, &n0);
	xm_sample_point_t* w0r =
		xm_get_sample_waveform_channel(ctx, 0, 1, 0);
	xm_sample_point_t* w1l =
		xm_get_sample_waveform_channel(ctx, 1, 0, &n1);
	xm_sample_point_t* w2 =
		xm_get_sample_waveform_channel(ctx, 2, 0, &n2);
	Z_CHECK(r, w0l && w0r && w1l && w2);
	Z_CHECK(r, n0 == 18);            /* trimmed to loop_end, frame count */
	Z_CHECK(r, n1 == 11);            /* odd 16-bit length is never halved */
	Z_CHECK(r, n2 == 7);
	Z_CHECK(r, xm_get_sample_waveform_channel(ctx, 0, 2, 0) == NULL);
	Z_CHECK(r, xm_get_sample_waveform_channel(ctx, 42, 0, 0) == NULL);
	uint32_t badlen = 777;
	Z_CHECK(r, xm_get_sample_waveform_channel(ctx, 42, 0, &badlen) == NULL
	           && badlen == 0);

	Z_CHECK(r, w1l[0] == (xm_sample_point_t)4000);
	/* 8-bit points are scaled to the internal 16-bit range (x256) */
	Z_CHECK(r, w0l[0] == (xm_sample_point_t)(20 * 256));
	Z_CHECK(r, w0r[0] == (xm_sample_point_t)(-20 * 256));

	/* right plane origin uses the declared untrimmed length (40 frames);
	 * this sample is 8-bit, so one byte per frame. */
	uint32_t right0_file = pcm[0] + 40u;
	int8_t file_r0 = (int8_t)b.data[right0_file];
	Z_CHECK(r, file_r0 == (int8_t)r8[0]);
	Z_CHECK(r, w0r[5] == (xm_sample_point_t)(r8[5] * 256));

	/* the file keeps a nonzero tail (frames 18..39) after loop_end even
	 * though only 18 frames are retained. */
	Z_CHECK(r, pcm[0] + 40u <= b.len);
	bool tail_nonzero = false;
	for(uint32_t k = 18; k < 40; ++k) {
		if((int8_t)b.data[pcm[0] + k] != 0
		   || (int8_t)b.data[right0_file + k] != 0) {
			tail_nonzero = true;
		}
	}
	Z_CHECK(r, tail_nonzero);
	Z_CHECK(r, l8[39] != 0 && r8[39] != 0);
	/* and that tail is not part of the retained planes */
	Z_CHECK(r, n0 == 18);

	/* adjacent, non-overlapping layout of the retained planes */
	uint32_t base_len = 0;
	xm_sample_point_t* base =
		xm_get_sample_waveform(ctx, 0, &base_len);
	Z_CHECK(r, w1l == base + 18u * 2u);
	Z_CHECK(r, w2 == w1l + 11u * 2u);
	Z_CHECK(r, (uint32_t)(w2 + 7 - base)
	           == 18u * 2u + 11u * 2u + 7u);

	uint32_t legacy_len = 0;
	xm_sample_point_t* legacy =
		xm_get_sample_waveform(ctx, 0, &legacy_len);
	Z_CHECK(r, legacy == w0l && legacy_len == 18);

	free(ctx);
	free(b.data);

	/* unsigned 8-bit mono: opposite sign convention, same logical values */
	static int16_t u8v[8];
	for(uint32_t i = 0; i < 8; ++i) u8v[i] = (int16_t)(i - 4);
	struct ref_sample s8 = { 8, 0, 0, 0, u8v, 0 };
	struct ref_row r8rows[1] = { { 25, 1, 64, 0, 0 } };
	struct ref_module m8 = { false, false, 1, &s8, 1, r8rows };
	uint32_t p8[1];
	struct buf b8 = build_s3m(&m8, p8);
	xm_context_t* c8 = load_buf(&b8);
	uint32_t n8 = 0;
	xm_sample_point_t* w8 =
		xm_get_sample_waveform_channel(c8, 0, 0, &n8);
	Z_CHECK(r, n8 == 8 && xm_get_sample_channels(c8, 0) == 1);
	/* unsigned 0..255 maps onto signed -128..127, then int8->int16 scales
	 * by 256. */
	Z_CHECK(r, w8[0] == (xm_sample_point_t)(-4 * 256));
	Z_CHECK(r, w8[7] == (xm_sample_point_t)(3 * 256));
	free(c8);
	free(b8.data);
}

/* ============================ play group ============================ */

void check_play(struct check_result* r) {
	/* Sample 0: long non-looped stereo sample (offset stays valid).
	 * Sample 1: short looped stereo sample used for the loop-seam check. */
	static int16_t L[2048], R[2048], LL[32], RR[32];
	for(uint32_t i = 0; i < 2048; ++i) {
		L[i] = (int16_t)(2000 + 300 * (int)(i % 90));
		R[i] = (int16_t)(-1000 - 170 * (int)(i % 80));
	}
	for(uint32_t i = 0; i < 32; ++i) {
		LL[i] = (int16_t)(1500 + 250 * (int)i);
		RR[i] = (int16_t)(-1500 - 250 * (int)i);
	}
	struct ref_sample smp[2] = {
		{ 2048, 0, 0, 0b110, L, R },     /* non-looped stereo 16-bit */
		{ 32, 4, 24, 0b111, LL, RR },    /* looped stereo 16-bit */
	};
	struct ref_row rows[5] = {
		{ 25, 1, 64, 0, 0 },
		{ 25, 1, 64, 15, 1 },  /* O01: offset 1*256 frames */
		{ 25, 1, 64, 0, 0 },   /* plain retrigger */
		{ 25, 2, 64, 0, 0 },   /* switch to the looped sample */
		{ 0, 0, 0, 0, 0 },
	};
	struct ref_module m = { true, false, 2, smp, 5, rows };
	uint32_t pcm[2];
	struct buf b = build_s3m(&m, pcm);
	xm_context_t* ctx = load_buf(&b);

	uint32_t len = 0;
	double* wl = read_plane(ctx, 0, 0, &len);
	double* wr = read_plane(ctx, 0, 1, 0);
	uint32_t llen = 0;
	double* wll = read_plane(ctx, 1, 0, &llen);
	double* wrr = read_plane(ctx, 1, 1, 0);
	Z_CHECK(r, len == 2048 && llen == 24);
	/* The pitch is constant, so read the engine's current frequency once
	 * and derive the microstep; frame position, interpolation, looping and
	 * PCM conversion are then all computed independently below. */
	float one[2];
	xm_generate_samples_noninterleaved(ctx, one, one + 1, 1);
	double step = floor(xm_get_frequency_of_channel(ctx, 1)
	                    * REF_MICROSTEPS / (double)REF_RATE + 0.5);
	reset_ctx(ctx);
	const double g = ref_gain();
	const uint32_t row = frames_per_row();

	enum { NWIN = 512 };
	float* il = malloc(2 * NWIN * sizeof(float));
	float* ir = il + NWIN;
	float* mix = malloc(2 * NWIN * sizeof(float));
	float* unmix = malloc(2 * NWIN * sizeof(float));
	float* big = malloc(2 * (4 * row + 900) * sizeof(float));
	assert(il && mix && unmix && big);

	/* independent reference for row 0, compared after ramping settles */
	xm_generate_samples_noninterleaved(ctx, il, ir, NWIN);
	for(uint32_t n = 300; n < 400; ++n) {
		uint32_t pos = (uint32_t)((double)n * step);
		double el = REF_AMPLIFICATION * g * ref_fetch(wl, len, 0, pos);
		double er = REF_AMPLIFICATION * g * ref_fetch(wr, len, 0, pos);
		Z_CHECK(r, feq(il[n], el));
		Z_CHECK(r, feq(ir[n], er));
		Z_CHECK(r, !feq(il[n], ir[n]));
	}

	/* the three generation layouts describe the same process */
	reset_ctx(ctx);
	xm_generate_samples(ctx, mix, NWIN);
	reset_ctx(ctx);
	xm_generate_samples_unmixed(ctx, unmix, NWIN);
	for(uint32_t n = 0; n < NWIN; ++n) {
		Z_CHECK(r, mix[2 * n] == il[n] && mix[2 * n + 1] == ir[n]);
		Z_CHECK(r, unmix[2 * n] == il[n] && unmix[2 * n + 1] == ir[n]);
	}

	/* 1/7/113-frame chunking gives identical output to one big call */
	reset_ctx(ctx);
	uint32_t produced = 0;
	const uint16_t sizes[3] = { 1, 7, 113 };
	uint32_t si = 0;
	while(produced < NWIN) {
		uint16_t want = (uint16_t)fminf((float)sizes[si % 3],
		                                (float)(NWIN - produced));
		xm_generate_samples(ctx, mix + 2 * produced, want);
		produced += want;
		++si;
	}
	reset_ctx(ctx);
	xm_generate_samples(ctx, unmix, NWIN);
	for(uint32_t n = 0; n < 2 * NWIN; ++n) Z_CHECK(r, mix[n] == unmix[n]);

	/* continuous stream for offset / retrigger / seam checks */
	reset_ctx(ctx);
	xm_generate_samples(ctx, big, (uint16_t)(3 * row + 900));

	/* offset row (row 1): shared phase restarts at 256 frames */
	for(uint32_t n = 300; n < 360; ++n) {
		uint32_t global = row + n;
		uint32_t pos = 256u * REF_MICROSTEPS
		               + (uint32_t)((double)n * step);
		double el = REF_AMPLIFICATION * g * ref_fetch(wl, len, 0, pos);
		double er = REF_AMPLIFICATION * g * ref_fetch(wr, len, 0, pos);
		Z_CHECK(r, feq(big[2 * global], el));
		Z_CHECK(r, feq(big[2 * global + 1], er));
	}

	/* retrigger row (row 2): phase resets to zero once for both planes */
	for(uint32_t n = 300; n < 360; ++n) {
		uint32_t global = 2 * row + n;
		uint32_t pos = (uint32_t)((double)n * step);
		double el = REF_AMPLIFICATION * g * ref_fetch(wl, len, 0, pos);
		double er = REF_AMPLIFICATION * g * ref_fetch(wr, len, 0, pos);
		Z_CHECK(r, feq(big[2 * global], el));
		Z_CHECK(r, feq(big[2 * global + 1], er));
	}

	/* loop seam row (row 3): past ramping, the phase has wrapped around
	 * the loop on both planes. Loop spans frames 4..24 (length 20). */
	for(uint32_t n = 800; n < 860; ++n) {
		uint32_t global = 3 * row + n;
		uint32_t pos = (uint32_t)((double)n * step);
		double el = REF_AMPLIFICATION * g * ref_fetch(wll, llen, 20, pos);
		double er = REF_AMPLIFICATION * g * ref_fetch(wrr, llen, 20, pos);
		Z_CHECK(r, feq(big[2 * global], el));
		Z_CHECK(r, feq(big[2 * global + 1], er));
		Z_CHECK(r, !feq(big[2 * global], big[2 * global + 1]));
	}
	/* the reference itself shows the wrap maps the end region back to the
	 * loop start. */
	uint32_t wrap_pos = llen * REF_MICROSTEPS + 5u * REF_MICROSTEPS;
	Z_CHECK(r, feq(ref_fetch(wll, llen, 20, wrap_pos),
	               ref_fetch(wll, llen, 20,
	                         (llen - 20u + 5u) * REF_MICROSTEPS)));
	Z_CHECK(r, feq(ref_fetch(wrr, llen, 20, wrap_pos),
	               ref_fetch(wrr, llen, 20,
	                         (llen - 20u + 5u) * REF_MICROSTEPS)));

	free(big);
	free(unmix);
	free(mix);
	free(il);
	free(wl);
	free(wr);
	free(wll);
	free(wrr);
	free(ctx);
	free(b.data);

	/* Equivalence: a stereo sample whose two planes are identical must
	 * produce exactly the same PCM as the mono version — no extra
	 * center-pan gain, and mono still reaches both sides. */
	static int16_t eL[48], eR[48];
	for(uint32_t i = 0; i < 48; ++i) {
		eL[i] = (int16_t)(900 + 60 * (int)(i % 30));
		eR[i] = eL[i];                 /* deliberately identical planes */
	}
	struct ref_sample mono_eq = { 48, 6, 30, 0b101, eL, 0 };
	struct ref_sample stereo_eq = { 48, 6, 30, 0b111, eL, eR };
	struct ref_row eq_rows[1] = { { 30, 1, 64, 0, 0 } };
	struct ref_module mm_eq = { true, false, 1, &mono_eq, 1, eq_rows };
	struct ref_module sm_eq = { true, false, 1, &stereo_eq, 1, eq_rows };
	uint32_t pcm_m[1], pcm_s[1];
	struct buf bm = build_s3m(&mm_eq, pcm_m);
	struct buf bs = build_s3m(&sm_eq, pcm_s);
	xm_context_t* cm = load_buf(&bm);
	xm_context_t* cs = load_buf(&bs);
	Z_CHECK(r, xm_get_sample_channels(cm, 0) == 1);
	Z_CHECK(r, xm_get_sample_channels(cs, 0) == 2);
	float pm[1024], ps[1024];
	xm_generate_samples(cm, pm, 512);
	xm_generate_samples(cs, ps, 512);
	Z_CHECK(r, memcmp(pm, ps, sizeof(pm)) == 0);
	/* identical left/right output on both representations */
	for(uint32_t n = 300; n < 400; ++n) {
		Z_CHECK(r, ps[2 * n] == ps[2 * n + 1]);
	}
	free(cm);
	free(cs);
	free(bm.data);
	free(bs.data);
}

/* ============================ ramp group ============================ */

void check_ramp(struct check_result* r) {
	bool ramp = ramping_active();
	static int16_t aL[800], aR[800], bL[800], bR[800], mono[800];
	for(uint32_t i = 0; i < 800; ++i) {
		aL[i] = 8000; aR[i] = -8000;
		bL[i] = 2000; bR[i] = -4000;
		mono[i] = 1000;
	}
	struct ref_sample smp[3] = {
		{ 800, 0, 0, 0b110, aL, aR },
		{ 800, 0, 0, 0b110, bL, bR },
		{ 800, 0, 0, 0b100, mono, 0 },
	};
	struct ref_row rows[5];
	memset(rows, 0, sizeof(rows));
	rows[0] = (struct ref_row){ 25, 1, 64, 0, 0 };
	rows[1] = (struct ref_row){ 25, 2, 64, 0, 0 }; /* stereo -> stereo */
	rows[2] = (struct ref_row){ 25, 3, 64, 0, 0 }; /* stereo -> mono */
	rows[3] = (struct ref_row){ 25, 1, 64, 0, 0 }; /* mono -> stereo */
	rows[4] = (struct ref_row){ 25, 2, 64, 0, 0 };
	struct ref_module m = { true, false, 3, smp, 5, rows };
	uint32_t pcm[3];
	struct buf b = build_s3m(&m, pcm);
	xm_context_t* ctx = load_buf(&b);
	const double g = ref_gain();
	const uint32_t row = frames_per_row();
	uint32_t nframes = 3 * row + 64;
	float* out = malloc(2 * nframes * sizeof(float));
	assert(out);
	xm_generate_samples(ctx, out, (uint16_t)nframes);

	double k = REF_AMPLIFICATION * g / 32768.0;
	uint32_t at = row + 4;
	if(ramp) {
		double t = 4.0 / (double)REF_RAMPING_POINTS;
		/* stereo -> stereo: independent left/right tails */
		Z_CHECK(r, feq(out[2 * at], k * (aL[0] + t * (bL[0] - aL[0]))));
		Z_CHECK(r, feq(out[2 * at + 1], k * (aR[0] + t * (bR[0] - aR[0]))));
	} else {
		Z_CHECK(r, feq(out[2 * at], k * bL[0]));
		Z_CHECK(r, feq(out[2 * at + 1], k * bR[0]));
	}
	Z_CHECK(r, !feq(out[2 * at + 1], out[2 * at]));

	/* stereo -> mono: the previous sample is B, so its constant planes are
	 * the ramp tails; the right output keeps its own (bR) tail. */
	at = 2 * row + 4;
	if(ramp) {
		double t = 4.0 / (double)REF_RAMPING_POINTS;
		Z_CHECK(r, feq(out[2 * at], k * (bL[0] + t * (mono[0] - bL[0]))));
		Z_CHECK(r, feq(out[2 * at + 1],
		               k * (bR[0] + t * (mono[0] - bR[0]))));
	} else {
		Z_CHECK(r, feq(out[2 * at], k * mono[0]));
		Z_CHECK(r, feq(out[2 * at + 1], k * mono[0]));
	}

	free(out);
	free(ctx);
	free(b.data);

	/* stereo -> empty sample: the right tail fades on its own */
	static int16_t empty_l[1] = { 0 };
	struct ref_sample esmp[2] = {
		{ 800, 0, 0, 0b110, aL, aR },
		{ 0, 0, 0, 0b000, empty_l, 0 },
	};
	struct ref_row erows[2] = {
		{ 25, 1, 64, 0, 0 },
		{ 25, 2, 64, 0, 0 },
	};
	struct ref_module em = { true, false, 2, esmp, 2, erows };
	uint32_t epcm[2];
	struct buf eb = build_s3m(&em, epcm);
	xm_context_t* ectx = load_buf(&eb);
	uint32_t en = row + 128;
	float* eo = malloc(2 * en * sizeof(float));
	assert(eo);
	xm_generate_samples(ectx, eo, (uint16_t)en);
	at = row + 4;
	if(ramp) {
		double t = 4.0 / (double)REF_RAMPING_POINTS;
		Z_CHECK(r, feq(eo[2 * at], k * aL[0] * (1.0 - t)));
		Z_CHECK(r, feq(eo[2 * at + 1], k * aR[0] * (1.0 - t)));
		Z_CHECK(r, fabsf(eo[2 * at + 1]) > 0.0f);
		Z_CHECK(r, !feq(eo[2 * at + 1], eo[2 * at]));
	} else {
		Z_CHECK(r, eo[2 * at] == 0.0f && eo[2 * at + 1] == 0.0f);
	}

	free(eo);
	free(ectx);
	free(eb.data);
}

/* ============================ formats group ============================ */

static struct buf make_mixed_module(void) {
	static int16_t sl[16], sr[16], ml[10];
	for(uint32_t i = 0; i < 16; ++i) {
		sl[i] = (int16_t)(1000 + 250 * (int)i);
		sr[i] = (int16_t)(-1000 - 250 * (int)i);
	}
	for(uint32_t i = 0; i < 10; ++i) ml[i] = (int16_t)(500 + 100 * (int)i);
	static struct ref_sample smp[2] = {
		{ 16, 0, 0, 0b110, sl, sr },
		{ 10, 0, 0, 0b100, ml, 0 },
	};
	static struct ref_row rows[2] = { { 25, 1, 64, 0, 0 },
	                                  { 0, 0, 0, 0, 0 } };
	static struct ref_module m = { true, false, 2, smp, 2, rows };
	uint32_t pcm[2];
	return build_s3m(&m, pcm);
}

static uint32_t read_u32(const char* p, uint32_t off) {
	const unsigned char* q = (const unsigned char*)p + off;
	return (uint32_t)q[0] | ((uint32_t)q[1] << 8)
	     | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

void check_formats(struct check_result* r) {
	/* --- version 0: a mono-only module stays version 0 --- */
	static int16_t mono[12];
	for(uint32_t i = 0; i < 12; ++i) mono[i] = (int16_t)(700 + 30 * (int)i);
	struct ref_sample ms = { 12, 0, 0, 0, mono, 0 };
	struct ref_row mrows[1] = { { 25, 1, 64, 0, 0 } };
	struct ref_module mm = { true, false, 1, &ms, 1, mrows };
	uint32_t mpcm[1];
	struct buf mb = build_s3m(&mm, mpcm);
	xm_context_t* m0 = load_buf(&mb);

	uint32_t save_sz = xm_save_size(m0);
	char* saved = malloc(save_sz);
	assert(saved);
	xm_save_context(m0, saved);
	Z_CHECK(r, (uint8_t)saved[1] == 0);
	Z_CHECK(r, memcmp(saved + 2, "LIBXMIF\xFF", 8) == 0);
	Z_CHECK(r, read_u32(saved, 0x10) == 12);

	struct buf sb = { (unsigned char*)saved, save_sz, save_sz };
	xm_context_t* m1 = load_buf(&sb);
	Z_CHECK(r, xm_get_sample_channels(m1, 0) == 1);
	float before[256], after[256];
	reset_ctx(m0);
	reset_ctx(m1);
	xm_generate_samples(m0, before, 128);
	xm_generate_samples(m1, after, 128);
	Z_CHECK(r, memcmp(before, after, sizeof(before)) == 0);
	free(saved);
	free(m1);

	/* --- version 1: mixed mono/stereo module round-trips both planes --- */
	struct buf xb = make_mixed_module();
	xm_context_t* x0 = load_buf(&xb);
	uint32_t xsz = xm_save_size(x0);
	char* xsaved = malloc(xsz);
	assert(xsaved);
	xm_save_context(x0, xsaved);
	Z_CHECK(r, (uint8_t)xsaved[1] == 1);
	Z_CHECK(r, read_u32(xsaved, 0x10) == 16u * 2u + 10u);

	struct buf xsb = { (unsigned char*)xsaved, xsz, xsz };
	xm_context_t* x1 = load_buf(&xsb);
	Z_CHECK(r, xm_get_sample_channels(x1, 0) == 2);
	Z_CHECK(r, xm_get_sample_channels(x1, 1) == 1);
	uint32_t l0 = 0, r0 = 0;
	xm_sample_point_t* wl =
		xm_get_sample_waveform_channel(x1, 0, 0, &l0);
	xm_sample_point_t* wr =
		xm_get_sample_waveform_channel(x1, 0, 1, &r0);
	Z_CHECK(r, l0 == 16 && r0 == 16);
	Z_CHECK(r, wl[3] == (xm_sample_point_t)1750);
	Z_CHECK(r, wr[3] == (xm_sample_point_t)-1750);

	reset_ctx(x0);
	reset_ctx(x1);
	float xb0[256], xb1[256];
	xm_generate_samples(x0, xb0, 128);
	xm_generate_samples(x1, xb1, 128);
	Z_CHECK(r, memcmp(xb0, xb1, sizeof(xb0)) == 0);

	/* --- dump/restore halfway through a tick, on pristine data. Both
	 * contexts come from the same saved buffer so they are identical. --- */
	xm_context_t* dref = load_buf(&xsb);
	uint32_t half = frames_per_row() / 2u + 3u;
	float* da = malloc(2 * half * sizeof(float));
	float* db = malloc(2 * half * sizeof(float));
	assert(da && db);
	reset_ctx(x1);
	xm_generate_samples(x1, da, (uint16_t)half);
	xm_generate_samples(dref, db, (uint16_t)half);
	Z_CHECK(r, memcmp(da, db, sizeof(float) * 2 * half) == 0);

	uint32_t dump_sz = xm_dump_size(x1);
	char* dump = malloc(dump_sz);
	assert(dump);
	xm_dump_context(x1, dump);
	xm_context_t* restored = xm_restore_context(dump);

	uint32_t dl = 0;
	xm_sample_point_t* dwr =
		xm_get_sample_waveform_channel(restored, 0, 1, &dl);
	Z_CHECK(r, dl == 16 && dwr[3] == (xm_sample_point_t)-1750);

	float cont_src[512], cont_dmp[512], cont_ref[512];
	xm_generate_samples(x1, cont_src, 200);
	xm_generate_samples(restored, cont_dmp, 200);
	xm_generate_samples(dref, cont_ref, 200);
	Z_CHECK(r, memcmp(cont_src, cont_dmp, sizeof(float) * 400) == 0);
	Z_CHECK(r, memcmp(cont_src, cont_ref, sizeof(float) * 400) == 0);
	/* restored aliases `dump`, so free it exactly once. */
	free(dump);
	free(dref);
	free(da);
	free(db);

	/* --- writeable plane: an in-place edit is audible, no copy. Overwrite
	 * the points inside the interpolation window of the comparison frame
	 * (at ~frame 100 the phase sits around plane indices 4..5). --- */
	for(uint32_t q = 0; q < 8; ++q) wr[q] = (xm_sample_point_t)-9999;
	xm_context_t* xfresh = load_buf(&xb);
	float edited[256], fresh[256];
	reset_ctx(x1);
	xm_generate_samples(x1, edited, 128);
	xm_generate_samples(xfresh, fresh, 128);
	Z_CHECK(r, wr[0] == (xm_sample_point_t)-9999); /* still the live plane */
	/* left plane untouched, right plane changed (after ramp settles) */
	Z_CHECK(r, edited[2 * 100] == fresh[2 * 100]);
	Z_CHECK(r, edited[2 * 100 + 1] != fresh[2 * 100 + 1]);

	/* --- reset reproduces a freshly loaded context --- */
	float ra[256], rb[256];
	reset_ctx(x1);
	xm_generate_samples(x1, ra, 128);
	reset_ctx(xfresh);
	xm_generate_samples(xfresh, rb, 128);
	/* x1 still carries the edit, so compare the unedited left side only */
	Z_CHECK(r, ra[2 * 100] == rb[2 * 100]);

	free(xfresh);
	free(xsaved);
	free(x1);
	free(x0);
	free(xb.data);
	free(m0);
	free(mb.data);
}
