/*
 * Portable fixed point conformance tests for the GSM 06.10 integer
 * codec.  These tests are compiled together with the real library and
 * run under the full UndefinedBehaviorSanitizer at -O0 and -O2, with
 * and without SASR defined.
 *
 * The three entry points each return the number of assertions that
 * actually ran and passed; a group that executes nothing or skips
 * returns 0 so the harness can flag it.
 *
 * Expected arithmetic values are derived from independent wide integer
 * rules (explicit quotient/remainder floor division and modular
 * truncation), never from the production helpers under test.  Expected
 * stream and parameter results are fixed literals captured from the
 * codec's documented bit-exact integer path.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gsm.h"
#include "private.h"

/* ------------------------------------------------------------------ */
/* Independent reference arithmetic (int64 based, no production use). */
/* ------------------------------------------------------------------ */

/*
 * Floor division towards negative infinity built explicitly from
 * C truncation-towards-zero quotient and remainder.  The production
 * code never uses this; it is only the oracle for the test.
 */
static int64_t ref_floor_div(int64_t x, int64_t d)
{
	int64_t q = x / d;
	int64_t r = x % d;

	if (r != 0 && ((r < 0) != (d < 0))) q--;
	return q;
}

/* Reference floor arithmetic shift right for a 16 bit word. */
static int16_t ref_asr16(int16_t x, int by)
{
	return (int16_t)ref_floor_div((int64_t)x, (int64_t)1 << by);
}

/* Reference floor arithmetic shift right for a 32 bit word. */
static int32_t ref_asr32(int32_t x, int by)
{
	return (int32_t)ref_floor_div((int64_t)x, (int64_t)1 << by);
}

/*
 * Reference low-bit-truncating left shift: multiply by the power of
 * two, reduce modulo 2**16 / 2**32, then reinterpret the bit pattern
 * as signed two's complement.  Deliberately not written as a shift so
 * that it is independent of gsm_word_asl / gsm_longword_asl.
 */
static int16_t ref_asl16(int16_t x, int by)
{
	int64_t v = (int64_t)x * ((int64_t)1 << by);
	uint16_t u = (uint16_t)((uint64_t)v & 0xFFFFu);

	return (int16_t)u;
}

static int32_t ref_asl32(int32_t x, int by)
{
	int64_t v = (int64_t)x * ((int64_t)1 << by);
	uint32_t u = (uint32_t)((uint64_t)v & 0xFFFFFFFFu);

	return (int32_t)u;
}

/* Independent Q15 multiplies, again via floor division. */
static int16_t ref_mult(int16_t a, int16_t b)
{
	if (a == -32768 && b == -32768) return 32767;
	return (int16_t)ref_floor_div((int64_t)a * (int64_t)b, 32768);
}

static int16_t ref_mult_r(int16_t a, int16_t b)
{
	if (a == -32768 && b == -32768) return 32767;
	return (int16_t)ref_floor_div((int64_t)a * (int64_t)b + 16384, 32768);
}

static int16_t ref_clamp16(int64_t v)
{
	if (v < -32768) return -32768;
	if (v > 32767) return 32767;
	return (int16_t)v;
}

static int32_t ref_clamp32(int64_t v)
{
	if (v < INT32_MIN) return INT32_MIN;
	if (v > INT32_MAX) return INT32_MAX;
	return (int32_t)v;
}

/* Independent normalization count, expressed as a plain loop.  The
 * target is bit 30 set (value in the 2**30 .. 2**31-1 band), matching
 * the production band documented above gsm_norm.
 */
static int ref_norm(int32_t a)
{
	int n;
	uint32_t u = (a < 0) ? ~(uint32_t)a : (uint32_t)a;

	if (a < 0 && a <= -1073741824) return 0;
	for (n = 0; n < 31; n++) {
		if (u & 0x40000000u) break;
		u <<= 1;
	}
	return n;
}

/* Independent replication of the historical shift edge rules. */
static int16_t ref_gsm_asl(int16_t a, int n)
{
	if (n >= 16) return 0;
	if (n <= -16) return -(a < 0);
	if (n < 0) return ref_asr16(a, -n);
	return ref_asl16(a, n);
}

static int16_t ref_gsm_asr(int16_t a, int n)
{
	if (n >= 16) return -(a < 0);
	if (n <= -16) return 0;
	if (n < 0) return ref_asl16(a, -n);
	return ref_asr16(a, n);
}

static int32_t ref_gsm_L_asl(int32_t a, int n)
{
	if (n >= 32) return 0;
	if (n <= -32) return -(a < 0);
	if (n < 0) return ref_asr32(a, -n);
	return ref_asl32(a, n);
}

static int32_t ref_gsm_L_asr(int32_t a, int n)
{
	if (n >= 32) return -(a < 0);
	if (n <= -32) return 0;
	if (n < 0) return ref_asl32(a, -n);
	return ref_asr32(a, n);
}

static int checks;

static void check(int condition, const char *what)
{
	if (condition) {
		checks++;
	} else {
		fprintf(stderr, "ASSERTION FAILED: %s\n", what);
	}
}

/* ------------------------------------------------------------------ */
/* case_arithmetic: shift, rounding, saturation and width boundaries. */
/* ------------------------------------------------------------------ */

int case_arithmetic(void)
{
	static const int16_t wvals[] = {
		-32768, -32767, -16384, -16383, -257, -3, -2, -1,
		0, 1, 2, 3, 257, 16383, 16384, 32767
	};
	static const int32_t lvals[] = {
		INT32_MIN, INT32_MIN + 1, -1073741825, -1073741824,
		-65537, -65536, -3, -2, -1, 0, 1, 2, 3,
		65535, 65536, 1073741823, 1073741824, INT32_MAX
	};
	size_t i;
	int by;

	checks = 0;

	/* Negative odd numbers must floor, not truncate towards zero. */
	check(gsm_asr(-3, 1) == -2, "gsm_asr(-3,1) floors to -2");
	check(gsm_asr(-1, 1) == -1, "gsm_asr(-1,1) is -1");
	check(gsm_L_asr(-3, 1) == -2, "gsm_L_asr(-3,1) floors to -2");
	check(gsm_L_asr(-1, 1) == -1, "gsm_L_asr(-1,1) is -1");
	check(gsm_asr(-5, 2) == -2, "gsm_asr(-5,2) floors");
	check(gsm_L_asr(-5, 2) == -2, "gsm_L_asr(-5,2) floors");

	/* Exhaustive in-range right shifts against the floor oracle. */
	for (i = 0; i < sizeof(wvals) / sizeof(wvals[0]); i++) {
		for (by = 0; by < 16; by++) {
			check(gsm_asr(wvals[i], by) == ref_asr16(wvals[i], by),
			      "word floor right shift");
		}
	}
	for (i = 0; i < sizeof(lvals) / sizeof(lvals[0]); i++) {
		static const int lby[] = {0, 1, 2, 3, 7, 15, 16, 31};
		for (unsigned k = 0; k < sizeof(lby) / sizeof(lby[0]); k++) {
			check(gsm_L_asr(lvals[i], lby[k]) ==
			      ref_asr32(lvals[i], lby[k]),
			      "longword floor right shift");
		}
	}

	/* In-range low-bit truncating left shifts, including negatives. */
	for (i = 0; i < sizeof(wvals) / sizeof(wvals[0]); i++) {
		for (by = 0; by < 16; by++) {
			check(gsm_asl(wvals[i], by) == ref_asl16(wvals[i], by),
			      "word truncating left shift");
		}
	}
	for (i = 0; i < sizeof(lvals) / sizeof(lvals[0]); i++) {
		static const int lby[] = {0, 1, 2, 15, 16, 31};
		for (unsigned k = 0; k < sizeof(lby) / sizeof(lby[0]); k++) {
			check(gsm_L_asl(lvals[i], lby[k]) ==
			      ref_asl32(lvals[i], lby[k]),
			      "longword truncating left shift");
		}
	}

	/* Minimum negative at the width edges. */
	check(gsm_asl(MIN_WORD, 1) == 0, "MIN_WORD << 1 truncates to 0");
	check(gsm_asl(MIN_WORD, 15) == 0, "MIN_WORD << 15 truncates to 0");
	check(gsm_asr(MIN_WORD, 15) == -1, "MIN_WORD >> 15 is -1");
	check(gsm_L_asl(MIN_LONGWORD, 1) == 0, "MIN_LONGWORD << 1 truncates");
	check(gsm_L_asr(MIN_LONGWORD, 31) == -1, "MIN_LONGWORD >> 31 is -1");

	/* Out-of-range and negative-count edge behaviour, kept historically. */
	check(gsm_asl(123, 16) == 0, "gsm_asl count >= 16 gives 0");
	check(gsm_asl(-123, 16) == 0, "negative gsm_asl count >= 16 gives 0");
	check(gsm_asl(123, 17) == 0, "gsm_asl count > 16 gives 0");
	check(gsm_asl(123, -16) == 0, "gsm_asl count <= -16, positive input");
	check(gsm_asl(-123, -16) == -1, "gsm_asl count <= -16, negative input");
	check(gsm_asr(123, 16) == 0, "gsm_asr count >= 16, positive input");
	check(gsm_asr(-123, 16) == -1, "gsm_asr count >= 16, negative input");
	check(gsm_L_asl(123, 32) == 0, "gsm_L_asl count >= 32 gives 0");
	check(gsm_L_asl(-123, 32) == 0, "negative gsm_L_asl count >= 32 gives 0");
	check(gsm_L_asl(123, -32) == 0, "gsm_L_asl count <= -32, positive");
	check(gsm_L_asl(-123, -32) == -1, "gsm_L_asl count <= -32, negative");
	check(gsm_L_asr(123, 32) == 0, "gsm_L_asr count >= 32, positive");
	check(gsm_L_asr(-123, 32) == -1, "gsm_L_asr count >= 32, negative");

	/* Negative counts reverse direction, matching the opposite helper. */
	for (i = 0; i < sizeof(wvals) / sizeof(wvals[0]); i++) {
		check(gsm_asl(wvals[i], -5) == gsm_asr(wvals[i], 5),
		      "word negative shift count reverses");
		check(gsm_asr(wvals[i], -5) == gsm_asl(wvals[i], 5),
		      "word reversed negative count");
		check(gsm_asl(wvals[i], 5) == ref_gsm_asl(wvals[i], 5),
		      "word asl full edge rule");
		check(gsm_asr(wvals[i], 5) == ref_gsm_asr(wvals[i], 5),
		      "word asr full edge rule");
	}
	for (i = 0; i < sizeof(lvals) / sizeof(lvals[0]); i++) {
		check(gsm_L_asl(lvals[i], -7) == gsm_L_asr(lvals[i], 7),
		      "longword negative shift count reverses");
		check(gsm_L_asr(lvals[i], -7) == gsm_L_asl(lvals[i], 7),
		      "longword reversed negative count");
		check(gsm_L_asl(lvals[i], 7) == ref_gsm_L_asl(lvals[i], 7),
		      "longword asl full edge rule");
		check(gsm_L_asr(lvals[i], 7) == ref_gsm_L_asr(lvals[i], 7),
		      "longword asr full edge rule");
	}

	/* Q15 multiplies against the independent floor oracle. */
	for (i = 0; i < sizeof(wvals) / sizeof(wvals[0]); i++) {
		size_t j;
		for (j = 0; j < sizeof(wvals) / sizeof(wvals[0]); j++) {
			int16_t a = wvals[i], b = (int16_t)wvals[j];
			check(gsm_mult(a, b) == ref_mult(a, b), "gsm_mult Q15");
			check(gsm_mult_r(a, b) == ref_mult_r(a, b),
			      "gsm_mult_r rounded Q15");
		}
	}

	/* Specific rounding boundaries around the 16384 bias. */
	check(gsm_mult_r(1, 16383) == 0, "mult_r below rounding threshold");
	check(gsm_mult_r(1, 16384) == 1, "mult_r at rounding threshold");
	check(gsm_mult_r(-1, 16384) == 0, "mult_r negative exact half");
	check(gsm_mult_r(-1, 16383) == 0, "mult_r negative below half");
	check(gsm_mult(MIN_WORD, MIN_WORD) == MAX_WORD, "mult MIN/MIN saturates");
	check(gsm_mult_r(MIN_WORD, MIN_WORD) == MAX_WORD,
	      "mult_r MIN/MIN saturates");
	check(gsm_L_mult(32767, 32767) ==
	      (longword)ref_asl32((int32_t)(32767 * 32767), 1),
	      "L_mult truncates the doubled product");
	check(gsm_L_mult(-32767, 32767) ==
	      (longword)ref_asl32((int32_t)(-32767 * 32767), 1),
	      "L_mult negative truncation");

	/* Saturating word add/sub against plain clamped arithmetic. */
	for (i = 0; i < sizeof(wvals) / sizeof(wvals[0]); i++) {
		size_t j;
		for (j = 0; j < sizeof(wvals) / sizeof(wvals[0]); j++) {
			int16_t a = wvals[i], b = (int16_t)wvals[j];
			check(gsm_add(a, b) == ref_clamp16((int64_t)a + b),
			      "gsm_add saturation");
			check(gsm_sub(a, b) == ref_clamp16((int64_t)a - b),
			      "gsm_sub saturation");
		}
	}
	check(gsm_add(MAX_WORD, 1) == MAX_WORD, "positive word add saturation");
	check(gsm_add(MIN_WORD, -1) == MIN_WORD, "negative word add saturation");
	check(gsm_sub(MIN_WORD, 1) == MIN_WORD, "word underflow saturation");
	check(gsm_abs(MIN_WORD) == MAX_WORD, "gsm_abs MIN_WORD rule");
	check(gsm_abs(-1234) == 1234, "gsm_abs ordinary negative");

	/* Saturating longword add/sub at the 32 bit boundaries. */
	check(gsm_L_add(MAX_LONGWORD, 1) == MAX_LONGWORD, "L_add positive sat");
	check(gsm_L_add(MIN_LONGWORD, MIN_LONGWORD) == MIN_LONGWORD,
	      "L_add negative sat");
	check(gsm_L_sub(MIN_LONGWORD, 1) == MIN_LONGWORD, "L_sub negative sat");
	check(gsm_L_sub(MAX_LONGWORD, MIN_LONGWORD) == MAX_LONGWORD,
	      "L_sub span saturation");
	check(gsm_L_add(MAX_LONGWORD, MIN_LONGWORD) == -1,
	      "L_add opposite extrema cancel");
	check(gsm_L_add(MAX_LONGWORD - 1, 2) == MAX_LONGWORD,
	      "L_add boundary clamp");
	check(gsm_L_sub(MIN_LONGWORD + 1, 2) == MIN_LONGWORD,
	      "L_sub boundary clamp");
	check(gsm_L_add(MAX_LONGWORD - 5, 3) ==
	      ref_clamp32((int64_t)MAX_LONGWORD - 5 + 3), "L_add near max");
	check(gsm_L_add(MIN_LONGWORD + 5, -3) ==
	      ref_clamp32((int64_t)MIN_LONGWORD + 5 - 3), "L_add near min");

	/* gsm_norm against an independent leading-bit loop. */
	for (i = 0; i < sizeof(lvals) / sizeof(lvals[0]); i++) {
		if (lvals[i] == 0) continue;
		check(gsm_norm(lvals[i]) == ref_norm(lvals[i]), "gsm_norm");
	}
	check(gsm_norm(1) == 30, "gsm_norm(1)");
	check(gsm_norm(-1) == 31, "gsm_norm(-1)");
	check(gsm_norm(0x40000000) == 0, "gsm_norm(0x40000000)");
	check(gsm_norm(MIN_LONGWORD) == 0, "gsm_norm(MIN_LONGWORD)");

	/* gsm_div on its legal non-negative domain: the Q15 fractional
	 * quotient floor(num * 32768 / denum), clamped to the 15 bit max.
	 */
	static const int16_t nums[] = {0, 1, 5, 100, 16384, 32767};
	for (i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
		int64_t q = (int64_t)nums[i] * 32768 / 32767;
		if (q > 32767) q = 32767;
		check(gsm_div(nums[i], 32767) == (int16_t)q, "gsm_div domain");
	}
	check(gsm_div(0, 32767) == 0, "gsm_div zero numerator");
	check(gsm_div(32767, 32767) == 32767, "gsm_div equal operands");
	check(gsm_div(5, 10) == 16384, "gsm_div half scale fraction");
	check(gsm_div(5, 40) == 4096, "gsm_div eighth scale fraction");
	check(gsm_div(1, 1) == 32767, "gsm_div unit fraction clamps");

	return checks;
}

/* ------------------------------------------------------------------ */
/* Shared stream helpers.                                             */
/* ------------------------------------------------------------------ */

static uint32_t mix(uint32_t h, unsigned v)
{
	return (h ^ (v & 255u)) * 16777619u;
}

static uint32_t pcm_hash(const gsm_signal *p, int *nonzero_out)
{
	uint32_t h = 2166136261u;
	int i, nz = 0;

	for (i = 0; i < 160; i++) {
		h = mix(mix(h, (uint16_t)p[i]), (uint16_t)p[i] >> 8);
		if (p[i] != 0) nz++;
	}
	if (nonzero_out) *nonzero_out += nz;
	return h;
}

static int in_word_range(const gsm_signal *p)
{
	int i;

	for (i = 0; i < 160; i++) {
		if (p[i] < -32768 || p[i] > 32767) return 0;
	}
	return 1;
}

/* Pack 76 codec parameters into a plain 33 byte frame. */
static void pack_params(gsm s, const gsm_signal *params, gsm_frame f)
{
	gsm_implode(s, (gsm_signal *)params, f);
}

/* ------------------------------------------------------------------ */
/* case_stream: fingerprints, extrema, noise, history and WAV49.      */
/* ------------------------------------------------------------------ */

int case_stream(void)
{
	gsm e, d;
	gsm_signal in[160], out[160];
	gsm_frame f;
	uint32_t enc, dec, rng;
	int nonzero, frame, i;

	checks = 0;

	/* Reproduce the original baseline fingerprints exactly. */
	e = gsm_create();
	d = gsm_create();
	check(e != 0 && d != 0, "create baseline codecs");
	enc = 2166136261u;
	dec = enc;
	rng = 7;
	nonzero = 0;
	for (frame = 0; frame < 12; frame++) {
		for (i = 0; i < 160; i++) {
			rng = rng * 1664525u + 1013904223u;
			in[i] = (gsm_signal)((int)(rng >> 16) - 32768);
			if (frame == 0) in[i] = 0;
			if (frame == 1) in[i] = i % 2 ? 32767 : -32768;
		}
		gsm_encode(e, in, f);
		check((f[0] >> 4) == GSM_MAGIC, "frame keeps GSM magic nibble");
		check(gsm_decode(d, f, out) == 0, "baseline decode succeeds");
		for (i = 0; i < 33; i++) enc = mix(enc, f[i]);
		/* thread the PCM fold across frames exactly like the baseline */
		for (i = 0; i < 160; i++) {
			dec = mix(mix(dec, (uint16_t)out[i]),
				  (uint16_t)out[i] >> 8);
			if (out[i] != 0) nonzero++;
		}
	}
	check(enc == 0x1727e7c8u, "baseline encoded frame fingerprint");
	check(dec == 0x7fc5a90au, "baseline PCM fingerprint");
	check(nonzero == 1918, "baseline nonzero sample count");
	gsm_destroy(e);
	gsm_destroy(d);

	/* Continuous silence, positive and negative extrema. */
	e = gsm_create();
	d = gsm_create();
	for (frame = 0; frame < 8; frame++) {
		for (i = 0; i < 160; i++) in[i] = 0;
		gsm_encode(e, in, f);
		check(gsm_decode(d, f, out) == 0, "silence decode");
		check(in_word_range(out), "silence output in word range");
	}
	for (frame = 0; frame < 4; frame++) {
		for (i = 0; i < 160; i++) in[i] = (frame & 1) ? -32768 : 32767;
		gsm_encode(e, in, f);
		check(gsm_decode(d, f, out) == 0, "extrema decode");
		check(in_word_range(out), "extrema output in word range");
	}

	/* Deterministic impulse and noise streams over many frames. */
	rng = 99;
	for (frame = 0; frame < 40; frame++) {
		for (i = 0; i < 160; i++) {
			rng = rng * 1103515245u + 12345u;
			in[i] = (gsm_signal)((int)(rng >> 16) - 32768);
			if (i == (frame * 7) % 160) in[i] = (frame & 1) ?
				(gsm_signal)32767 : (gsm_signal)-32768;
		}
		gsm_encode(e, in, f);
		check(gsm_decode(d, f, out) == 0, "noise/impulse decode");
		check(in_word_range(out), "noise output in word range");
	}
	gsm_destroy(e);
	gsm_destroy(d);

	/* Two decoders with different history receiving identical frames. */
	{
		gsm d1 = gsm_create(), d2 = gsm_create(), en = gsm_create();
		gsm_signal o1[160], o2[160];
		int differed = 0, fr;

		for (i = 0; i < 160; i++) in[i] = 0;
		gsm_encode(en, in, f);
		gsm_decode(d1, f, o1);
		for (i = 0; i < 160; i++)
			in[i] = (gsm_signal)((i * 131 + 17) % 1024 - 512);
		gsm_encode(en, in, f);
		gsm_decode(d2, f, o2);

		for (fr = 0; fr < 24; fr++) {
			for (i = 0; i < 160; i++) {
				rng = rng * 1664525u + 1013904223u;
				in[i] = (gsm_signal)((int)(rng >> 16) - 32768);
			}
			gsm_encode(en, in, f);
			gsm_decode(d1, f, o1);
			gsm_decode(d2, f, o2);
			check(in_word_range(o1) && in_word_range(o2),
			      "two-history outputs in range");
			for (i = 0; i < 160; i++)
				if (o1[i] != o2[i]) differed = 1;
		}
		check(differed, "different history changes early outputs");
		gsm_destroy(d1);
		gsm_destroy(d2);
		gsm_destroy(en);
	}

	/* WAV49 packing.  The WAV #49 format emits alternating 32/33
	 * byte packets and even splits one frame's parameters across a
	 * pair, so its byte framing is deliberately incompatible with
	 * the native 33 byte stream (documented in gsm_option(3)); the
	 * meaningful invariant is therefore the self-consistent round
	 * trip: two independent WAV49 decoders consuming the same
	 * chained packets must produce identical, build-independent
	 * PCM with a fixed fingerprint.
	 */
	{
		enum { WFRAMES = 12 };
		gsm ew = gsm_create(), da = gsm_create(), db = gsm_create();
		gsm_signal wseq[WFRAMES][160], oa[160], ob[160];
		gsm_frame fw;
		uint32_t ha = 2166136261u, hb = 2166136261u;
		int on = 1, fr, identical = 1;

		gsm_option(ew, GSM_OPT_WAV49, &on);
		gsm_option(da, GSM_OPT_WAV49, &on);
		gsm_option(db, GSM_OPT_WAV49, &on);
		rng = 4242;
		for (fr = 0; fr < WFRAMES; fr++)
			for (i = 0; i < 160; i++) {
				rng = rng * 1664525u + 1013904223u;
				wseq[fr][i] =
					(gsm_signal)((int)(rng >> 16) - 32768);
			}
		for (fr = 0; fr < WFRAMES; fr++) {
			gsm_encode(ew, wseq[fr], fw);
			gsm_decode(da, fw, oa);
			gsm_decode(db, fw, ob);
			check(in_word_range(oa) && in_word_range(ob),
			      "wav49 output in word range");
			if (memcmp(oa, ob, sizeof(oa)) != 0) identical = 0;
			for (i = 0; i < 160; i++) {
				ha = mix(mix(ha, (uint16_t)oa[i]),
					 (uint16_t)oa[i] >> 8);
				hb = mix(mix(hb, (uint16_t)ob[i]),
					 (uint16_t)ob[i] >> 8);
			}
		}
		check(identical, "two WAV49 decoders agree on chained packets");
		check(ha == hb, "WAV49 decoder hashes agree");
		check(ha == 0x819a4a52u, "WAV49 round trip PCM fingerprint");
		gsm_destroy(ew);
		gsm_destroy(da);
		gsm_destroy(db);
	}

	return checks;
}

/* ------------------------------------------------------------------ */
/* case_parameters: legal edge frames and real decoded output.        */
/* ------------------------------------------------------------------ */

static void make_frame(gsm s, const word *LARc, const word *Nc,
		       const word *bc, const word *Mc, const word *xmaxc,
		       const word *xMc, gsm_frame f)
{
	gsm_signal params[76];
	int k, i;

	memcpy(params, LARc, 8 * sizeof(gsm_signal));
	for (k = 0; k < 4; k++) {
		params[8 + k * 17 + 0] = Nc[k];
		params[8 + k * 17 + 1] = bc[k];
		params[8 + k * 17 + 2] = Mc[k];
		params[8 + k * 17 + 3] = xmaxc[k];
		for (i = 0; i < 13; i++)
			params[12 + k * 17 + i] = xMc[k * 13 + i];
	}
	pack_params(s, params, f);
}

static uint32_t pcm_hash_only(const gsm_signal *p)
{
	return pcm_hash(p, 0);
}

int case_parameters(void)
{
	gsm d;
	gsm_signal out[160];
	gsm_frame f;
	word LARc[8], Nc[4], bc[4], Mc[4], xmaxc[4], xMc[52];
	static const word lar_max[8] = {63, 63, 31, 31, 15, 15, 7, 7};
	int k, i;

	checks = 0;
	d = gsm_create();

	/* Edge LAR, all RPE pulses, grid 0, lag 40, full scale xmaxc. */
	memcpy(LARc, lar_max, sizeof(lar_max));
	for (k = 0; k < 4; k++) {
		Nc[k] = 40;
		bc[k] = 3;
		Mc[k] = 0;
		xmaxc[k] = 63;
		for (i = 0; i < 13; i++) xMc[k * 13 + i] = 7;
	}
	make_frame(d, LARc, Nc, bc, Mc, xmaxc, xMc, f);
	check(gsm_decode(d, f, out) == 0, "edge frame A decodes");
	check(pcm_hash_only(out) == 0x0a09d8b0u, "edge frame A PCM hash");
	check(out[0] == 32760 && out[1] == 8376 && out[2] == -5616,
	      "edge frame A first samples");
	check(out[159] == -12392 && out[158] == 26392,
	      "edge frame A tail samples");

	/* Zero LAR, alternating pulses, grid 3, lag 120, zero gain. */
	memset(LARc, 0, sizeof(LARc));
	for (k = 0; k < 4; k++) {
		Nc[k] = 120;
		bc[k] = 0;
		Mc[k] = 3;
		xmaxc[k] = 1;
		for (i = 0; i < 13; i++) xMc[k * 13 + i] = (i & 1) ? 7 : 0;
	}
	gsm_destroy(d);
	d = gsm_create();
	make_frame(d, LARc, Nc, bc, Mc, xmaxc, xMc, f);
	check(gsm_decode(d, f, out) == 0, "edge frame B decodes");
	check(pcm_hash_only(out) == 0x1a8837b5u, "edge frame B PCM hash");
	check(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == -112,
	      "edge frame B zero lead then pulse");
	check(out[159] == -3360, "edge frame B tail sample");

	/* Mixed grids and lags, mid-range LAR. */
	{
		static const word lar_mid[8] = {63, 0, 31, 0, 15, 0, 7, 0};
		memcpy(LARc, lar_mid, sizeof(lar_mid));
		for (k = 0; k < 4; k++) {
			Nc[k] = (k & 1) ? 120 : 40;
			bc[k] = (word)k;
			Mc[k] = (word)k;
			xmaxc[k] = (k & 1) ? 63 : 0;
			for (i = 0; i < 13; i++) xMc[k * 13 + i] = (word)(i % 8);
		}
		gsm_destroy(d);
		d = gsm_create();
		make_frame(d, LARc, Nc, bc, Mc, xmaxc, xMc, f);
		check(gsm_decode(d, f, out) == 0, "edge frame C decodes");
		check(pcm_hash_only(out) == 0xacbb30eeu, "edge frame C PCM hash");
		check(out[0] == -56 && out[3] == -80, "edge frame C head samples");
		check(out[157] == -32768 && out[159] == -32768,
		      "edge frame C saturating tail");
	}

	/* Every RPE grid position produces a distinct, fixed result. */
	{
		static const uint32_t grid_hash[4] = {
			0x4612be43u, 0x540e83d4u, 0xb4588842u, 0x66231493u
		};
		static const int16_t grid_s0[4] = {1456, 0, 0, 0};
		memset(LARc, 0, sizeof(LARc));
		for (k = 0; k < 4; k++) {
			Nc[k] = 80;
			bc[k] = 1;
			xmaxc[k] = 20;
			for (i = 0; i < 13; i++)
				xMc[k * 13 + i] = (i % 3 == 0) ? 7 : 1;
		}
		for (int g = 0; g < 4; g++) {
			for (k = 0; k < 4; k++) Mc[k] = (word)g;
			gsm_destroy(d);
			d = gsm_create();
			make_frame(d, LARc, Nc, bc, Mc, xmaxc, xMc, f);
			check(gsm_decode(d, f, out) == 0, "grid frame decodes");
			check(pcm_hash_only(out) == grid_hash[g],
			      "per-grid PCM fingerprint");
			check(out[0] == grid_s0[g], "grid offset first sample");
		}
	}

	/* Long term lag reuse: repeated Nc keeps the decoder stateful. */
	{
		gsm_signal a[160], b[160], c[160];
		int changed = 0;

		memset(LARc, 0, sizeof(LARc));
		for (k = 0; k < 4; k++) {
			Nc[k] = 120;
			bc[k] = 0;
			Mc[k] = 0;
			xmaxc[k] = 0;
		}
		memset(xMc, 0, sizeof(xMc));
		gsm_destroy(d);
		d = gsm_create();
		make_frame(d, LARc, Nc, bc, Mc, xmaxc, xMc, f);
		gsm_decode(d, f, a);
		gsm_decode(d, f, b);
		gsm_decode(d, f, c);
		for (i = 0; i < 160; i++) if (a[i] != b[i]) changed = 1;
		check(changed, "reused lag frame advances decoder state");
		check(pcm_hash_only(a) == 0xbe9780b6u, "first repeated-lag frame");
		check(pcm_hash_only(b) == 0xe295c817u, "second repeated-lag frame");
		check(pcm_hash_only(c) == 0x8b27b16a, "third repeated-lag frame");
	}

	/* An out-of-range Nc falls back to the previous lag (nrp). */
	{
		gsm d1 = gsm_create(), d2 = gsm_create();
		gsm_signal o1[160], o2[160];
		word Nz[4] = {0, 0, 0, 0};
		word Ng[4] = {120, 120, 120, 120};

		memset(LARc, 0, sizeof(LARc));
		for (k = 0; k < 4; k++) {
			bc[k] = 0;
			Mc[k] = 0;
			xmaxc[k] = 0;
		}
		memset(xMc, 0, sizeof(xMc));

		/* Prime both decoders with a valid Nc=120 frame. */
		make_frame(d1, LARc, Ng, bc, Mc, xmaxc, xMc, f);
		gsm_decode(d1, f, o1);
		make_frame(d2, LARc, Ng, bc, Mc, xmaxc, xMc, f);
		gsm_decode(d2, f, o2);

		/* Nc=0 must be replaced by the stored nrp=120. */
		make_frame(d1, LARc, Nz, bc, Mc, xmaxc, xMc, f);
		gsm_decode(d1, f, o1);
		make_frame(d2, LARc, Ng, bc, Mc, xmaxc, xMc, f);
		gsm_decode(d2, f, o2);
		check(memcmp(o1, o2, sizeof(o1)) == 0,
		      "illegal Nc falls back to previous lag");
		check(pcm_hash_only(o1) == 0xe295c817u, "lag fallback PCM hash");
		gsm_destroy(d1);
		gsm_destroy(d2);
	}

	/*
	 * Two short scaling/rounding sensitive cases.  Moving the RPE
	 * inverse quantization scale or the Q15 rounding point changes
	 * these decoded samples, so they pin the scale placement.
	 */
	{
		gsm_signal s[160];

		memset(LARc, 0, sizeof(LARc));
		for (k = 0; k < 4; k++) {
			Nc[k] = 40;
			bc[k] = 0;
			Mc[k] = 1;
			xmaxc[k] = 0;
			for (i = 0; i < 13; i++) xMc[k * 13 + i] = 7;
		}
		gsm_destroy(d);
		d = gsm_create();
		make_frame(d, LARc, Nc, bc, Mc, xmaxc, xMc, f);
		gsm_decode(d, f, s);
		check(pcm_hash_only(s) == 0xa88697cbu, "short case 1 hash");
		check(s[0] == 0 && s[1] == 56 && s[2] == 48 && s[3] == 56 &&
		      s[4] == 112 && s[5] == 112, "short case 1 scaled samples");

		memcpy(LARc, lar_max, sizeof(lar_max));
		for (k = 0; k < 4; k++) {
			Nc[k] = 120;
			bc[k] = 2;
			Mc[k] = 2;
			xmaxc[k] = 63;
			for (i = 0; i < 13; i++)
				xMc[k * 13 + i] = (i & 1) ? 7 : 0;
		}
		gsm_destroy(d);
		d = gsm_create();
		make_frame(d, LARc, Nc, bc, Mc, xmaxc, xMc, f);
		gsm_decode(d, f, s);
		check(pcm_hash_only(s) == 0x4ee32f2cu, "short case 2 hash");
		check(s[0] == 0 && s[1] == 0 && s[2] == -32768 && s[3] == -8384 &&
		      s[4] == 5608 && s[5] == 32760, "short case 2 scaled samples");
	}

	gsm_destroy(d);
	return checks;
}
