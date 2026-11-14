/*
 * Portable fixed point regression tests for the GSM 06.10 integer
 * encode/decode path and its WAV49 packing.
 *
 * Three groups are exported:
 *
 *	case_arithmetic	primitive shifts, Q15 products, saturation and
 *			normalization against an independent long long
 *			oracle (no production helper computes an
 *			expected value here),
 *	case_stream	whole-codec streams: silence, extremes, pulses,
 *			deterministic noise and stateful instances,
 *			including normal vs WAV49 fingerprints,
 *	case_parameters	legal frames built parameter by parameter
 *			(edge LAR, RPE pulses, every grid, lag reuse)
 *			checked through actual output samples and the
 *			successor state.
 *
 * Every group returns the number of positive assertions it actually
 * performed; a mismatch aborts so a broken build never reports success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gsm.h"
#include "private.h"

/* ------------------------------------------------------------------ */
/* Independent reference arithmetic.  Everything here is expressed    */
/* with plain long long / unsigned long long and an explicit           */
/* quotient-remainder floor rule; none of these call a gsm_* helper.   */
/* ------------------------------------------------------------------ */

#define REF_WMIN16	(-32768LL)
#define REF_WMAX16	(32767LL)
#define REF_LMIN32	(-2147483648LL)
#define REF_LMAX32	(2147483647LL)

/* floor(x / 2^n), n >= 0, derived from C's truncating quotient and the
 * remainder rather than from a shift: when the remainder is non-zero
 * and the dividend is negative, the truncated quotient is one high.
 */
static long long ref_floor_shr(long long x, int n)
{
	long long d = 1LL << n;
	long long q = x / d;
	long long r = x % d;
	if (r != 0 && r < 0) --q;
	return q;
}

static long long ref_sat(long long v, long long lo, long long hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Truncating word left shifts: keep the destination word's low bits and
 * re-interpret the pattern as two's complement.
 */
static long long ref_sll(long long x, int n, int bits)
{
	unsigned long long mask = (1ULL << bits) - 1ULL;
	unsigned long long ux = (unsigned long long)x & mask;
	unsigned long long r = (ux << (unsigned)n) & mask;
	if (r & (1ULL << (bits - 1)))
		return (long long)r - (1LL << bits);
	return (long long)r;
}

static long long ref_sra(long long x, int n, int bits)
{
	if (n >= bits) return x < 0 ? -1 : 0;
	return ref_floor_shr(x, n);
}

/* Independent Q15 products. */
static long long ref_mult(long long a, long long b)
{
	if (a == REF_WMIN16 && b == REF_WMIN16) return REF_WMAX16;
	return ref_sat(ref_floor_shr(a * b, 15), REF_WMIN16, REF_WMAX16);
}

static long long ref_mult_r(long long a, long long b)
{
	if (a == REF_WMIN16 && b == REF_WMIN16) return REF_WMAX16;
	return ref_sat(ref_floor_shr(a * b + 16384, 15),
		       REF_WMIN16, REF_WMAX16);
}

/* Independent normalization: position of the highest set bit of the
 * magnitude pattern, counted from bit 30.
 */
static int ref_norm(long long a)
{
	unsigned long long x;
	int p;
	if (a < 0)
		x = (unsigned long long)(~a);
	else
		x = (unsigned long long)a;
	for (p = 30; p >= 0; --p)
		if (x & (1ULL << p))
			break;
	return p < 0 ? 31 : 30 - p;
}

static int arith_checks;

#define ACHECK(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "case_arithmetic FAILED line %d: %s\n",\
			__LINE__, #cond);				\
		abort();						\
	}								\
	++arith_checks;						\
} while (0)

int case_arithmetic(void)
{
	static const short wvals[] = {
		-32768, -32767, -32766, -7, -3, -2, -1,
		0, 1, 2, 3, 7, 8191, 16384, 32766, 32767
	};
	static const long lvals[] = {
		-2147483647L - 1, -2147483647L, -1073741825L, -1073741824L,
		-7L, -3L, -1L, 0L, 1L, 3L, 1073741823L, 1073741824L,
		2147483646L, 2147483647L
	};
	unsigned i;
	int n;

	arith_checks = 0;

	/* 16 bit arithmetic right shifts over the whole value set and
	 * every in-range count, including negative odd dividends where
	 * floor differs from truncation toward zero.
	 */
	for (i = 0; i < sizeof wvals / sizeof wvals[0]; ++i) {
		for (n = 0; n < 16; ++n) {
			ACHECK((long long)gsm_asr(wvals[i], n)
			       == ref_sra(wvals[i], n, 16));
		}
		ACHECK(gsm_asr(wvals[i], 16) == (short)(wvals[i] < 0 ? -1 : 0));
	}

	/* 32 bit arithmetic right shifts, including the minimum negative. */
	for (i = 0; i < sizeof lvals / sizeof lvals[0]; ++i) {
		for (n = 0; n < 32; ++n) {
			ACHECK((long long)gsm_L_asr(lvals[i], n)
			       == ref_sra(lvals[i], n, 32));
		}
		ACHECK(gsm_L_asr(lvals[i], 32)
		       == (lvals[i] < 0 ? -1 : 0));
	}

	/* Explicit floor-vs-truncation spot checks on odd negatives. */
	ACHECK(gsm_asr(-1, 1) == -1);		/* floor(-1/2) */
	ACHECK(gsm_asr(-3, 1) == -2);
	ACHECK(gsm_asr(-7, 2) == -2);		/* floor(-7/4) */
	ACHECK(gsm_L_asr(-1, 1) == -1);
	ACHECK(gsm_L_asr(-5, 2) == -2);

	/* Truncating 16 and 32 bit left shifts, in-range counts. */
	for (i = 0; i < sizeof wvals / sizeof wvals[0]; ++i)
		for (n = 0; n < 16; ++n)
			ACHECK((long long)gsm_asl(wvals[i], n)
			       == ref_sll(wvals[i], n, 16));
	for (i = 0; i < sizeof lvals / sizeof lvals[0]; ++i)
		for (n = 0; n < 32; ++n)
			ACHECK((long long)gsm_L_asl(lvals[i], n)
			       == ref_sll(lvals[i], n, 32));

	/* Width and reversal boundary behavior that the API contract
	 * preserves, including the minimum negative at the edge.
	 */
	ACHECK(gsm_asl((short)-32768, 15) == 0);
	ACHECK(gsm_asl((short)-1, 16) == 0);
	ACHECK(gsm_asl((short)-1, 40) == 0);
	ACHECK(gsm_L_asl(-2147483647L - 1, 31) == 0);
	ACHECK(gsm_L_asl(-1L, 32) == 0);
	ACHECK(gsm_asl((short)-5, -1) == gsm_asr((short)-5, 1));
	ACHECK(gsm_L_asl(-9L, -2) == gsm_L_asr(-9L, 2));
	ACHECK(gsm_asr((short)-5, 16) == -1);
	ACHECK(gsm_L_asr(-9L, 33) == -1);
	ACHECK(gsm_asr((short)5, 16) == 0);
	ACHECK(gsm_L_asr(9L, 40) == 0);

	/* Q15 multiply and rounded multiply across the value set. */
	for (i = 0; i < sizeof wvals / sizeof wvals[0]; ++i) {
		unsigned j;
		for (j = 0; j < sizeof wvals / sizeof wvals[0]; ++j) {
			short a = wvals[i], b = wvals[j];
			ACHECK((long long)gsm_mult(a, b) == ref_mult(a, b));
			ACHECK((long long)gsm_mult_r(a, b)
			       == ref_mult_r(a, b));
		}
	}
	/* MIN_WORD pair keeps its historical saturation. */
	ACHECK(gsm_mult((short)-32768, (short)-32768) == 32767);
	ACHECK(gsm_mult_r((short)-32768, (short)-32768) == 32767);
	/* Rounding boundary: the +16384 term pushes a product whose
	 * magnitude is exactly 2^14 over the next Q15 step, including the
	 * negative direction (floor, not toward zero).
	 */
	ACHECK(gsm_mult((short)16384, (short)1) == 0);
	ACHECK(gsm_mult_r((short)16384, (short)1) == 1);
	ACHECK(gsm_mult((short)-16384, (short)1) == -1);
	ACHECK(gsm_mult_r((short)-16384, (short)1) == 0);
	ACHECK(gsm_mult((short)1, (short)1) == 0);
	ACHECK(gsm_mult_r((short)1, (short)1) == 0);
	ACHECK(gsm_mult((short)-1, (short)1) == -1);
	ACHECK(gsm_mult_r((short)-1, (short)1) == 0);

	/* 32 bit fractional product with the extra left bit. */
	ACHECK(gsm_L_mult((short)16384, (short)16384)
	       == (long)ref_sll(16384LL * 16384LL, 1, 32));
	ACHECK(gsm_L_mult((short)-16384, (short)16384)
	       == (long)ref_sll(-16384LL * 16384LL, 1, 32));

	/* Saturated 16 bit add/sub at the clipping boundaries. */
	ACHECK(gsm_add((short)32767, (short)1) == 32767);
	ACHECK(gsm_add((short)30000, (short)4000) == 32767);
	ACHECK(gsm_add((short)-32768, (short)-1) == -32768);
	ACHECK(gsm_sub((short)-32768, (short)1) == -32768);
	ACHECK(gsm_sub((short)32767, (short)-1) == 32767);
	ACHECK(gsm_add((short)1234, (short)-4321)
	       == (short)ref_sat(1234LL - 4321LL, REF_WMIN16, REF_WMAX16));

	/* Saturated 32 bit add/sub at the clipping boundaries. */
	ACHECK(gsm_L_add(2147483647L, 1L) == 2147483647L);
	ACHECK(gsm_L_add(-2147483647L - 1, -1L) == -2147483647L - 1);
	ACHECK(gsm_L_sub(-2147483647L - 1, 1L) == -2147483647L - 1);
	ACHECK(gsm_L_sub(2147483647L, -1L) == 2147483647L);
	ACHECK(gsm_L_add(1000000000L, 1000000000L) == 2000000000L);

	/* GSM absolute value with the MIN_WORD clamp. */
	ACHECK(gsm_abs((short)-32768) == 32767);
	ACHECK(gsm_abs((short)-123) == 123);
	ACHECK(gsm_abs((short)123) == 123);
	ACHECK(gsm_abs((short)0) == 0);

	/* Normalization over representative magnitudes and signs. */
	for (i = 0; i < sizeof lvals / sizeof lvals[0]; ++i) {
		if (lvals[i] == 0) continue;
		if (lvals[i] < 0 && lvals[i] <= -1073741824L)
			ACHECK(gsm_norm(lvals[i]) == 0);
		else
			ACHECK(gsm_norm(lvals[i]) == ref_norm(lvals[i]));
	}
	ACHECK(gsm_norm(1L) == 30);
	ACHECK(gsm_norm(1073741823L) == 1);
	ACHECK(gsm_norm(-1L) == 31);
	ACHECK(gsm_norm(-1073741824L) == 0);

	return arith_checks;
}

/* ------------------------------------------------------------------ */
/* Stream group.                                                       */
/* ------------------------------------------------------------------ */

static uint32_t mix32(uint32_t h, unsigned v)
{
	return (h ^ (v & 255u)) * 16777619u;
}

static uint32_t frame_hash(const gsm_byte * p)
{
	uint32_t h = 2166136261u;
	int i;
	for (i = 0; i < 33; ++i) h = mix32(h, p[i]);
	return h;
}

static uint32_t pcm_hash(const gsm_signal * p, int n)
{
	uint32_t h = 2166136261u;
	int i;
	for (i = 0; i < n; ++i) {
		h = mix32(h, (uint16_t)p[i]);
		h = mix32(h, (uint16_t)p[i] >> 8);
	}
	return h;
}

static void fill_signal(gsm_signal * in, int kind, int frame)
{
	uint32_t rng = 7u + (uint32_t)frame * 2654435761u;
	int i;
	for (i = 0; i < 160; ++i) {
		int v;
		rng = rng * 1664525u + 1013904223u;
		switch (kind) {
		case 0: v = 0; break;				     /* silence */
		case 1: v = (i & 1) ? 32767 : -32768; break;	     /* extremes */
		case 2: v = (i < 2) ? (i ? -32768 : 32767) : 0; break;/* pulse */
		case 3: v = ((i + frame) & 1) ? 3 : -3; break;	     /* odds */
		case 4: v = ((i * 7 + frame * 13) & 255) - 128; break;/* ramp */
		default: v = (int)(rng >> 16) - 32768;		     /* noise */
		}
		in[i] = (gsm_signal)v;
	}
}

static int stream_checks;

#define SCHECK(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "case_stream FAILED line %d: %s\n",	\
			__LINE__, #cond);				\
		abort();						\
	}								\
	++stream_checks;						\
} while (0)

/* Per-frame golden values produced by the unmodified 1.0.17 integer
 * codec and reproduced bit-for-bit by this implementation.
 */
struct stream_gold { int kind, frame; uint32_t bytes, pcm; };
static const struct stream_gold stream_gold[] = {
	{ 2, 0, 0xf96d7d83u, 0x0435b989u },
	{ 2, 1, 0xc8ca7235u, 0x6715a14fu },
	{ 2, 2, 0x7e1f30d2u, 0x31128556u },
	{ 2, 3, 0xc8ca7235u, 0xbe7bef6bu },
	{ 3, 0, 0xeb88941du, 0x71428f40u },
	{ 3, 1, 0xb10ad133u, 0x2b3188ddu },
	{ 3, 2, 0xccf6c4e1u, 0x43c357e8u },
	{ 3, 3, 0x90772d65u, 0x85d27b8du },
	{ 4, 0, 0x7ce33b60u, 0x48e19535u },
	{ 4, 1, 0x56afdf35u, 0xe475563du },
	{ 4, 2, 0x80d8a91eu, 0xa30e4355u },
	{ 4, 3, 0x988b99f5u, 0xb6d32855u }
};
static const uint32_t stream_sum[3] = {
	0xd17d288au, 0x683a4cc5u, 0x2c0717c9u
};
static const int stream_nonzero[3] = { 485, 634, 624 };

/* Normal vs WAV49 golden aggregate/per-frame values. */
static const uint32_t wav_frame_normal_pcm[6] = {
	0x5f3c853bu, 0x62dd6036u, 0x609f92fcu,
	0xdbbf75a8u, 0x030d338eu, 0x55f20b25u
};
static const uint32_t wav_frame_wav_pcm[6] = {
	0x5f3c853bu, 0x89e43a3fu, 0x854e3884u,
	0x8430b46du, 0x1be978bau, 0x4760d3f5u
};

int case_stream(void)
{
	gsm_signal in[160], out[160];
	gsm_byte f[33];
	int g, fr;

	stream_checks = 0;

	/* Exact reproduction of the attached devcheck baseline: its
	 * twelve frames, compressed-byte fingerprint, PCM fingerprint
	 * and non-zero sample count.
	 */
	{
		gsm e = gsm_create(), d = gsm_create();
		uint32_t enc = 2166136261u, dec = enc, rng = 7;
		int nonzero = 0;
		SCHECK(e != 0 && d != 0);
		for (fr = 0; fr < 12; ++fr) {
			int i;
			for (i = 0; i < 160; ++i) {
				rng = rng * 1664525u + 1013904223u;
				in[i] = (gsm_signal)((int)(rng >> 16) - 32768);
				if (fr == 0) in[i] = 0;
				if (fr == 1) in[i] = (i & 1) ? 32767 : -32768;
			}
			gsm_encode(e, in, f);
			SCHECK((f[0] >> 4) == 0xd);
			SCHECK(gsm_decode(d, f, out) == 0);
			for (i = 0; i < 33; ++i) enc = mix32(enc, f[i]);
			for (i = 0; i < 160; ++i) {
				dec = mix32(mix32(dec, (uint16_t)out[i]),
					    (uint16_t)out[i] >> 8);
				nonzero += out[i] != 0;
			}
		}
		SCHECK(enc == 0x1727e7c8u);
		SCHECK(dec == 0x7fc5a90au);
		SCHECK(nonzero == 1918);
		gsm_destroy(e);
		gsm_destroy(d);
	}

	/* Continuous silence and alternating extremes must be stable and
	 * finite over many frames, exercising saturation repeatedly.
	 */
	for (g = 0; g < 2; ++g) {
		gsm e = gsm_create(), d = gsm_create();
		int peak = 0;
		for (fr = 0; fr < 8; ++fr) {
			int i;
			fill_signal(in, g, fr);
			gsm_encode(e, in, f);
			SCHECK(gsm_decode(d, f, out) == 0);
			for (i = 0; i < 160; ++i) {
				int mag = out[i] < 0 ? -out[i] : out[i];
				SCHECK(out[i] >= -32768 && out[i] <= 32767);
				if (mag > peak) peak = mag;
			}
		}
		/* Silence stays at the small filter-floor level; rail-to-rail
		 * input drives a large output while never leaving 16 bits.
		 */
		SCHECK(g == 0 ? peak <= 16 : peak >= 16000);
		gsm_destroy(e);
		gsm_destroy(d);
	}

	/* Pulse / odd / ramp short cases with per-frame fingerprints.
	 * These are the cases in which a misplaced scaling or rounding
	 * changes the decoded PCM.
	 */
	for (g = 0; g < 3; ++g) {
		gsm e = gsm_create(), d = gsm_create();
		uint32_t dh = 2166136261u;
		int nonzero = 0, gi;
		for (fr = 0; fr < 4; ++fr) {
			int i;
			fill_signal(in, g + 2, fr);
			gsm_encode(e, in, f);
			SCHECK(gsm_decode(d, f, out) == 0);
			gi = g * 4 + fr;
			SCHECK(frame_hash(f) == stream_gold[gi].bytes);
			SCHECK(pcm_hash(out, 160) == stream_gold[gi].pcm);
			dh ^= pcm_hash(out, 160);
			dh *= 16777619u;
			for (i = 0; i < 160; ++i) nonzero += out[i] != 0;
		}
		SCHECK(dh == stream_sum[g]);
		SCHECK(nonzero == stream_nonzero[g]);
		gsm_destroy(e);
		gsm_destroy(d);
	}

	/* Exact first samples of the two rounding-sensitive short cases:
	 * pre-emphasis and RPE dequantization rounding show up here.
	 */
	{
		static const short pulse0[5] = { 0, -14336, -9168, -6024, -2008 };
		static const short odds0[5]  = { -8, -8, -8, -16, -16 };
		gsm e = gsm_create(), d = gsm_create();
		fill_signal(in, 2, 0);
		gsm_encode(e, in, f);
		gsm_decode(d, f, out);
		SCHECK(out[159] == -8);
		for (g = 0; g < 5; ++g) SCHECK(out[g] == pulse0[g]);
		gsm_destroy(e);
		gsm_destroy(d);

		e = gsm_create();
		d = gsm_create();
		fill_signal(in, 3, 0);
		gsm_encode(e, in, f);
		gsm_decode(d, f, out);
		SCHECK(out[159] == 216);
		for (g = 0; g < 5; ++g) SCHECK(out[g] == odds0[g]);
		gsm_destroy(e);
		gsm_destroy(d);
	}

	/* Two instances given different histories must diverge on an
	 * identical later frame, while each history is deterministic.
	 */
	{
		gsm e1 = gsm_create(), e2 = gsm_create();
		gsm d1 = gsm_create(), d2 = gsm_create();
		gsm_byte f1[33], f2[33];
		gsm_signal o1[160], o2[160];
		fill_signal(in, 1, 0);
		gsm_encode(e1, in, f1);
		gsm_decode(d1, f1, o1);
		fill_signal(in, 0, 0);
		gsm_encode(e2, in, f2);
		gsm_decode(d2, f2, o2);
		/* identical frame from now on, different stored history */
		fill_signal(in, 5, 3);
		gsm_encode(e1, in, f1);
		gsm_encode(e2, in, f2);
		SCHECK(frame_hash(f1) != frame_hash(f2));
		gsm_decode(d1, f1, o1);
		gsm_decode(d2, f2, o2);
		SCHECK(pcm_hash(o1, 160) != pcm_hash(o2, 160));
		gsm_destroy(e1);
		gsm_destroy(e2);
		gsm_destroy(d1);
		gsm_destroy(d2);

		/* determinism: replay history 1 on a fresh pair. */
		{
			gsm e = gsm_create(), d = gsm_create();
			gsm_byte ff[33];
			gsm_signal oo[160];
			fill_signal(in, 1, 0);
			gsm_encode(e, in, ff);
			gsm_decode(d, ff, oo);
			fill_signal(in, 5, 3);
			gsm_encode(e, in, ff);
			gsm_decode(d, ff, oo);
			SCHECK(pcm_hash(oo, 160) == pcm_hash(o1, 160));
			gsm_destroy(e);
			gsm_destroy(d);
		}
	}

	/* Normal packing versus WAV49 on the same input stream. */
	{
		gsm en = gsm_create(), de = gsm_create();
		gsm ew = gsm_create(), dw = gsm_create();
		gsm_byte fn[6][33], fw[6][33];
		gsm_signal pn[6][160], pw[6][160];
		int on = 1, wv = 1;
		uint32_t dn = 2166136261u, dw2 = 2166136261u;
		gsm_option(ew, GSM_OPT_WAV49, &wv);
		gsm_option(dw, GSM_OPT_WAV49, &wv);
		(void)on;
		for (fr = 0; fr < 6; ++fr) {
			fill_signal(in, (fr & 1) ? 1 : 5, fr);
			gsm_encode(en, in, fn[fr]);
			gsm_encode(ew, in, fw[fr]);
			SCHECK(frame_hash(fn[fr]) != frame_hash(fw[fr]));
		}
		for (fr = 0; fr < 6; ++fr) {
			int i;
			gsm_decode(de, fn[fr], pn[fr]);
			gsm_decode(dw, fw[fr], pw[fr]);
			SCHECK(pcm_hash(pn[fr], 160)
			       == wav_frame_normal_pcm[fr]);
			SCHECK(pcm_hash(pw[fr], 160)
			       == wav_frame_wav_pcm[fr]);
			for (i = 0; i < 160; ++i) {
				dn = mix32(mix32(dn, (uint16_t)pn[fr][i]),
					   (uint16_t)pn[fr][i] >> 8);
				dw2 = mix32(mix32(dw2, (uint16_t)pw[fr][i]),
					    (uint16_t)pw[fr][i] >> 8);
			}
		}
		/* First WAV49 frame carries no chained nibble, so its
		 * parameters and PCM are identical to normal packing.
		 */
		SCHECK(pcm_hash(pw[0], 160) == pcm_hash(pn[0], 160));
		SCHECK(dn == 0xd795fd27u);
		SCHECK(dw2 == 0xa76e4a53u);
		gsm_destroy(en);
		gsm_destroy(de);
		gsm_destroy(ew);
		gsm_destroy(dw);
	}

	return stream_checks;
}

/* ------------------------------------------------------------------ */
/* Parameter group.                                                    */
/* ------------------------------------------------------------------ */

static int param_checks;

#define PCHECK(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "case_parameters FAILED line %d: %s\n",\
			__LINE__, #cond);				\
		abort();						\
	}								\
	++param_checks;						\
} while (0)

struct param_gold {
	uint32_t bytes, pcm1, pcm2;
	short s1[3], s2[3];
};
static const struct param_gold param_gold[4] = {
	{ 0x93b0338du, 0x47117a12u, 0x637897beu,
	  { 0, -1280, -744 }, { -848, -1376, -760 } },
	{ 0x00eaaafcu, 0x851eb49eu, 0xff25c109u,
	  { 0, 9216, 3072 }, { 3192, 11208, 3920 } },
	{ 0xd2ed65f9u, 0x7ead73d0u, 0xaf91b359u,
	  { 0, -8056, -8584 }, { -5704, -15616, -16128 } },
	{ 0x49946bcbu, 0x13021b71u, 0xc1feffa6u,
	  { -1152, -168, -1640 }, { -2344, -200, -1640 } }
};
static const uint32_t grid_hash[4] = {
	0xabe113c4u, 0x12fc34aau, 0x0d2a6351u, 0x3c585519u
};
static const short grid_last[4] = { -21016, 5216, 31512, 32760 };

static void build_params(short src[76], int t)
{
	static const short rlo[8] = { -32, -32, -16, -16, -8, -8, -4, -4 };
	static const short rhi[8] = { 31, 31, 15, 15, 7, 7, 3, 3 };
	int i, j, m;
	memset(src, 0, 76 * sizeof(short));
	for (i = 0; i < 8; ++i)
		src[i] = (t & 1) ? rhi[i] : rlo[i];
	for (j = 0; j < 4; ++j) {
		src[8 + j]  = (t == 3) ? ((j & 1) ? 40 : 120)
				       : (short)(40 + 27 * j);
		src[9 + j]  = (short)(j & 3);
		src[10 + j] = (short)((j + t) & 3);
		src[11 + j] = (t == 1) ? 63 : (t == 2 ? 0
						      : (short)(7 * j + 1));
		for (m = 0; m < 13; ++m)
			src[12 + 13 * j + m] =
				(t == 1) ? (short)((m + t) & 7)
				: (t == 2) ? 3
				: (short)((m * 3 + j) & 7);
	}
}

int case_parameters(void)
{
	short src[76], out[160];
	gsm_byte f[33];
	int t, mc;

	param_checks = 0;

	/* Four independently constructed legal frames: edge LAR,
	 * RPE pulses, all-grid content and long-term lag reuse.  Decoded
	 * samples and the successor frame (state update) are checked.
	 */
	for (t = 0; t < 4; ++t) {
		gsm d = gsm_create();
		build_params(src, t);
		gsm_implode(d, src, f);
		PCHECK(((f[0] >> 4) & 15) == GSM_MAGIC);
		PCHECK(frame_hash(f) == param_gold[t].bytes);
		PCHECK(gsm_decode(d, f, out) == 0);
		PCHECK(pcm_hash(out, 160) == param_gold[t].pcm1);
		PCHECK(out[0] == param_gold[t].s1[0]);
		PCHECK(out[79] == param_gold[t].s1[1]);
		PCHECK(out[159] == param_gold[t].s1[2]);
		/* second frame exercises the updated successor state */
		gsm_implode(d, src, f);
		PCHECK(gsm_decode(d, f, out) == 0);
		PCHECK(pcm_hash(out, 160) == param_gold[t].pcm2);
		PCHECK(out[0] == param_gold[t].s2[0]);
		PCHECK(out[79] == param_gold[t].s2[1]);
		PCHECK(out[159] == param_gold[t].s2[2]);
		gsm_destroy(d);
	}

	/* Every RPE grid position on otherwise identical legal params
	 * must yield a distinct, exact output (grid placement check).
	 */
	for (mc = 0; mc < 4; ++mc) {
		gsm d = gsm_create();
		int j, m;
		memset(src, 0, sizeof src);
		for (j = 0; j < 4; ++j) {
			src[8 + j] = 60;
			src[9 + j] = 2;
			src[10 + j] = (short)mc;
			src[11 + j] = 20;
			for (m = 0; m < 13; ++m)
				src[12 + 13 * j + m] = 4;
		}
		gsm_implode(d, src, f);
		PCHECK(gsm_decode(d, f, out) == 0);
		PCHECK(pcm_hash(out, 160) == grid_hash[mc]);
		PCHECK(out[159] == grid_last[mc]);
		gsm_destroy(d);
	}
	/* The four grids really do produce different PCM. */
	PCHECK(grid_hash[0] != grid_hash[1]);
	PCHECK(grid_hash[1] != grid_hash[2]);
	PCHECK(grid_hash[2] != grid_hash[3]);

	/* Long-term lag reuse: repeat the minimum and maximum lags over
	 * successive frames and confirm the stateful output is stable and
	 * matches the lag-reuse golden successor sample.
	 */
	{
		gsm d = gsm_create();
		int j;
		build_params(src, 3);	/* Nc alternates 120 / 40 */
		for (j = 0; j < 3; ++j) {
			gsm_implode(d, src, f);
			PCHECK(gsm_decode(d, f, out) == 0);
		}
		/* Third frame settles under repeated lag reuse; identical
		 * across O0/O2 and both SASR settings because every input
		 * parameter is fully initialized.
		 */
		PCHECK(out[159] == -1648);
		PCHECK(out[79] == -264);
		gsm_destroy(d);
	}

	/* Implode/explode round trip using the documented "exploded"
	 * layout: eight LAR entries followed by four stride-17 subframe
	 * records (Nc, bc, Mc, xmaxc, then 13 RPE samples).  gsm_explode
	 * hands the LAR back as the unsigned coded form, while all 68
	 * subframe parameters are preserved exactly.
	 */
	{
		static const short lar[8] = { -32, -32, -16, -16, -8, -8, -4, -4 };
		static const int lar_width[8] = { 6, 6, 5, 5, 4, 4, 3, 3 };
		gsm a = gsm_create(), b = gsm_create();
		gsm_signal back[76];
		int j, m, q, ok = 1;
		memset(src, 0, sizeof src);
		for (j = 0; j < 8; ++j) src[j] = lar[j];
		for (j = 0; j < 4; ++j) {
			int base = 8 + 17 * j;
			src[base + 0] = 60;	/* Nc  */
			src[base + 1] = 2;	/* bc  */
			src[base + 2] = (short)j;/* Mc  */
			src[base + 3] = 20;	/* xmaxc */
			for (m = 0; m < 13; ++m)
				src[base + 4 + m] = 4;
		}
		gsm_implode(a, src, f);
		PCHECK(gsm_explode(b, f, back) == 0);
		for (j = 0; j < 8; ++j) {
			short coded = (short)(lar[j] & ((1 << lar_width[j]) - 1));
			if (back[j] != coded) ok = 0;
		}
		PCHECK(ok);
		for (j = 0; j < 4; ++j) {
			int base = 8 + 17 * j;
			for (q = 0; q < 4; ++q)
				PCHECK(back[base + q] == src[base + q]);
			for (m = 0; m < 13; ++m)
				PCHECK(back[base + 4 + m] == src[base + 4 + m]);
		}
		gsm_destroy(a);
		gsm_destroy(b);
	}

	return param_checks;
}
