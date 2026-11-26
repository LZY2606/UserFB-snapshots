/*
 * Independent numeric regression for the bcg729 decoder excitation path.
 *
 * The expectations in this file are derived from the bit-field definitions,
 * the published b30 interpolation constants, wide integer products, and
 * explicit rounding.  Production pitch/fixed-codebook routines are invoked
 * only as the implementation under test; their results are never used to
 * construct expected values.  decodeGains is the documented exception for
 * frame-level tests: it is run on a separate context with reference fixed
 * vectors to obtain the gain state that is explicitly outside this proof.
 */

#include "task_support/check.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "typedef.h"
#include "codebooks.h"
#include "decodeAdaptativeCodeVector.h"
#include "decodeFixedCodeVector.h"
#include "decodeGains.h"

#define REF_SUBFRAME 40
#define REF_FRAME 80
#define REF_PAST 154
#define REF_BOUNDED_MIN 3277
#define REF_BOUNDED_MAX 13107
#define REF_MAX_PITCH 143

/* Record one actual comparison and print enough location to diagnose a drift. */
#define EXPECT_AT(phase, subframe, sample, expected, actual) do { \
	long long ref_e = (long long)(expected); \
	long long ref_a = (long long)(actual); \
	if (ref_e != ref_a) { \
		printf("CONTENT_MISMATCH phase=%s subframe=%d sample=%d expected=%lld actual=%lld\n", \
			(phase), (int)(subframe), (int)(sample), ref_e, ref_a); \
		TASK_CHECK(0); \
		return 1; \
	} \
	TASK_CHECK(1); \
} while (0)

#define EXPECT_VEC40(phase, subframe, expected, actual) do { \
	int ref_i; \
	for (ref_i = 0; ref_i < REF_SUBFRAME; ++ref_i) \
		EXPECT_AT((phase), (subframe), ref_i, (expected)[ref_i], (actual)[ref_i]); \
} while (0)

/* Explicit biased arithmetic shift, equivalent to floor((x + 2^(q-1))/2^q). */
static int32_t ref_round_shift(int64_t x, unsigned q)
{
	int64_t divisor = INT64_C(1) << q;
	int64_t biased = x + (divisor >> 1);
	if (biased >= 0) return (int32_t)(biased / divisor);
	return (int32_t)(-((-biased + divisor - 1) / divisor));
}

static int16_t ref_saturate16(int64_t x)
{
	if (x > 32767) return 32767;
	if (x < -32768) return -32768;
	return (int16_t)x;
}

static int16_t ref_wrap_add16(int32_t a, int32_t b)
{
	uint32_t bits = (uint32_t)((uint16_t)a + (uint16_t)b);
	bits &= 0xffffu;
	return (int16_t)(bits >= 0x8000u ? bits - 0x10000u : bits);
}

static int ref_clamp(int x, int lo, int hi)
{
	if (x < lo) return lo;
	if (x > hi) return hi;
	return x;
}

static uint16_t ref_parity(uint16_t p1)
{
	int bit;
	uint16_t parity = 1;
	unsigned v = (unsigned)(p1 >> 2);
	for (bit = 0; bit < 6; ++bit) {
		parity ^= (uint16_t)(v & 1u);
		v >>= 1;
	}
	return parity;
}

static void fill_small_history(int16_t history[REF_PAST], uint32_t seed)
{
	int i;
	for (i = 0; i < REF_PAST; ++i) {
		uint32_t x = seed + (uint32_t)i * 37u + (uint32_t)(i * i) * 3u;
		history[i] = (int16_t)((int)(x % 25u) - 12);
	}
}

struct pitch_desc {
	int integer_delay;
	int fraction;
	int t_min;
};

static struct pitch_desc ref_first_pitch(uint16_t p1)
{
	struct pitch_desc d;
	d.t_min = 0;
	if (p1 < 197) {
		d.integer_delay = (p1 + 2) / 3 + 19;
		d.fraction = p1 - 3 * d.integer_delay + 58;
	} else {
		d.integer_delay = p1 - 112;
		d.fraction = 0;
	}
	return d;
}

static struct pitch_desc ref_second_pitch(uint16_t p2, int first_integer_delay)
{
	struct pitch_desc d;
	d.t_min = ref_clamp(first_integer_delay - 5, 20, 134);
	int relative = (p2 + 2) / 3 - 1;
	d.integer_delay = d.t_min + relative;
	d.fraction = p2 - 3 * relative - 2;
	return d;
}

/*
 * Reconstruct 40 adaptive samples.  x[0..39] is both output and the causal
 * current-subframe storage; consequently short delays read values written by
 * earlier loop iterations.
 */
static void ref_compute_pitch(int16_t *x, int integer_delay, int fraction,
			      unsigned *causal_reads, unsigned *future_reads)
{
	int base_delay;
	int phase_a;
	int phase_b;
	int n, tap;

	if (fraction == 1) {
		base_delay = integer_delay + 1;
		phase_a = 2;
	} else {
		base_delay = integer_delay;
		phase_a = -fraction; /* -1 -> 1, 0 -> 0 */
	}
	phase_b = 3 - phase_a;

	for (n = 0; n < REF_SUBFRAME; ++n) {
		int64_t acc = 0;
		for (tap = 0; tap < 10; ++tap) {
			int j = 3 * tap;
			int index_a = n - base_delay - tap;
			int index_b = n - base_delay + 1 + tap;
			int16_t sample_a, sample_b;

			if (index_a >= 0) {
				if (causal_reads) ++*causal_reads;
				if (future_reads && index_a >= n) ++*future_reads;
			}
			if (index_b >= 0) {
				if (causal_reads) ++*causal_reads;
				if (future_reads && index_b >= n) ++*future_reads;
			}
			sample_a = x[index_a];
			sample_b = x[index_b];
			acc += (int64_t)sample_a * b30[phase_a + j];
			acc += (int64_t)sample_b * b30[phase_b + j];
		}
		x[n] = ref_saturate16(ref_round_shift(acc, 15));
	}
}

static int test_pitch_first_indices(void)
{
	static const uint16_t indices[] = {0, 1, 2, 3, 17, 195, 196, 197, 198, 255};
	unsigned idx;

	for (idx = 0; idx < sizeof(indices) / sizeof(indices[0]); ++idx) {
		uint16_t p1 = indices[idx];
		struct pitch_desc expected = ref_first_pitch(p1);
		int16_t ref_arena[REF_PAST + REF_SUBFRAME];
		int16_t actual_arena[REF_PAST + REF_SUBFRAME];
		int16_t *ref_x = ref_arena + REF_PAST;
		int16_t *actual_x = actual_arena + REF_PAST;
		bcg729DecoderChannelContextStruct ctx;
		int16_t actual_delay = -1;
		unsigned causal_reads = 0, future_reads = 0;
		char phase[32];

		snprintf(phase, sizeof(phase), "pitch-first-P%u", (unsigned)p1);
		fill_small_history(ref_arena, 0x51a7u + idx * 97u);
		memcpy(actual_arena, ref_arena, sizeof(ref_arena));
		memset(&ctx, 0, sizeof(ctx));
		initDecodeAdaptativeCodeVector(&ctx);

		ref_compute_pitch(ref_x, expected.integer_delay, expected.fraction,
				  &causal_reads, &future_reads);
		decodeAdaptativeCodeVector(&ctx, 0, p1, 0, 0, &actual_delay, actual_x);

		EXPECT_AT(phase, 0, -1, expected.integer_delay, actual_delay);
		EXPECT_AT(phase, 0, -2, expected.integer_delay, ctx.previousIntPitchDelay);
		EXPECT_VEC40(phase, 0, ref_x, actual_x);
		EXPECT_AT(phase, 0, -3, 0, future_reads);
		if (p1 <= 3) EXPECT_AT(phase, 0, -4, 1, causal_reads > 40);
	}
	return 0;
}

static int test_pitch_second_window(void)
{
	struct window_case { uint16_t p1, p2; int t_min; const char *name; };
	static const struct window_case cases[] = {
		{  0, 0, 20, "lower-clamp-positive-fraction" },
		{ 17, 2, 20, "lower-edge" },
		{251, 2, 134, "upper-edge" },
		{255, 1, 134, "upper-clamp-negative-fraction" },
		{255, 30, 134, "upper-positive-fraction" }
	};
	unsigned c;

	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
		int16_t ref_arena[REF_PAST + 2 * REF_SUBFRAME];
		int16_t actual_arena[REF_PAST + 2 * REF_SUBFRAME];
		int16_t *ref_first = ref_arena + REF_PAST;
		int16_t *actual_first = actual_arena + REF_PAST;
		int16_t *ref_second = ref_first + REF_SUBFRAME;
		int16_t *actual_second = actual_first + REF_SUBFRAME;
		bcg729DecoderChannelContextStruct ctx;
		struct pitch_desc first = ref_first_pitch(cases[c].p1);
		struct pitch_desc second;
		int16_t actual_delay;
		unsigned causal_reads = 0, future_reads = 0;
		char phase[64];

		snprintf(phase, sizeof(phase), "pitch-window-%s", cases[c].name);
		fill_small_history(ref_arena, 0x7000u + c * 131u);
		memcpy(actual_arena, ref_arena, sizeof(ref_arena));
		memset(&ctx, 0, sizeof(ctx));
		initDecodeAdaptativeCodeVector(&ctx);
		actual_delay = 60;

		ref_compute_pitch(ref_first, first.integer_delay, first.fraction, NULL, NULL);
		decodeAdaptativeCodeVector(&ctx, 0, cases[c].p1, 0, 0,
					   &actual_delay, actual_first);
		EXPECT_VEC40(phase, 0, ref_first, actual_first);
		EXPECT_AT(phase, 0, -1, first.integer_delay, actual_delay);

		second = ref_second_pitch(cases[c].p2, first.integer_delay);
		EXPECT_AT(phase, 1, -10, cases[c].t_min, second.t_min);
		ref_compute_pitch(ref_second, second.integer_delay, second.fraction,
				  &causal_reads, &future_reads);
		decodeAdaptativeCodeVector(&ctx, REF_SUBFRAME, cases[c].p2, 0, 0,
					   &actual_delay, actual_second);

		EXPECT_AT(phase, 1, -1, second.integer_delay, actual_delay);
		EXPECT_AT(phase, 1, -2, second.integer_delay, ctx.previousIntPitchDelay);
		EXPECT_VEC40(phase, 1, ref_second, actual_second);
		EXPECT_AT(phase, 1, -3, 0, future_reads);
		if (second.integer_delay <= 25)
			EXPECT_AT(phase, 1, -4, 1, causal_reads > 20);
	}
	return 0;
}

static int test_pitch_parity_error(void)
{
	int16_t ref_arena[REF_PAST + 2 * REF_SUBFRAME];
	int16_t actual_arena[REF_PAST + 2 * REF_SUBFRAME];
	int16_t *ref_first = ref_arena + REF_PAST;
	int16_t *actual_first = actual_arena + REF_PAST;
	int16_t *ref_second = ref_first + REF_SUBFRAME;
	int16_t *actual_second = actual_first + REF_SUBFRAME;
	bcg729DecoderChannelContextStruct ctx;
	const uint16_t p1 = 40, p2 = 6;
	const int initial_previous = 77;
	int first_delay, second_delay;
	int16_t actual_delay;
	struct pitch_desc normal_second;

	fill_small_history(ref_arena, 0x9e37u);
	memcpy(actual_arena, ref_arena, sizeof(ref_arena));
	memset(&ctx, 0, sizeof(ctx));
	initDecodeAdaptativeCodeVector(&ctx);
	ctx.previousIntPitchDelay = initial_previous;
	actual_delay = (int16_t)initial_previous;

	/* A parity failure conceals only the first subframe. */
	first_delay = initial_previous;
	ref_compute_pitch(ref_first, first_delay, 0, NULL, NULL);
	decodeAdaptativeCodeVector(&ctx, 0, p1, 1, 0, &actual_delay, actual_first);
	EXPECT_AT("pitch-parity", 0, -1, initial_previous, actual_delay);
	EXPECT_AT("pitch-parity", 0, -2, initial_previous + 1, ctx.previousIntPitchDelay);
	EXPECT_VEC40("pitch-parity", 0, ref_first, actual_first);

	/* The second call still interprets P2 relative to the concealed delay. */
	normal_second = ref_second_pitch(p2, first_delay);
	second_delay = normal_second.integer_delay;
	ref_compute_pitch(ref_second, second_delay, normal_second.fraction, NULL, NULL);
	decodeAdaptativeCodeVector(&ctx, REF_SUBFRAME, p2, 1, 0,
				   &actual_delay, actual_second);
	EXPECT_AT("pitch-parity", 1, -1, second_delay, actual_delay);
	EXPECT_AT("pitch-parity", 1, -2, second_delay, ctx.previousIntPitchDelay);
	EXPECT_VEC40("pitch-parity", 1, ref_second, actual_second);
	return 0;
}

static int test_pitch_explicit_loss(void)
{
	static const int initial_values[] = {60, 141, 143};
	unsigned i;

	for (i = 0; i < sizeof(initial_values) / sizeof(initial_values[0]); ++i) {
		int initial_previous = initial_values[i];
		int16_t ref_arena[REF_PAST + 2 * REF_SUBFRAME];
		int16_t actual_arena[REF_PAST + 2 * REF_SUBFRAME];
		int16_t *ref_first = ref_arena + REF_PAST;
		int16_t *actual_first = actual_arena + REF_PAST;
		int16_t *ref_second = ref_first + REF_SUBFRAME;
		int16_t *actual_second = actual_first + REF_SUBFRAME;
		bcg729DecoderChannelContextStruct ctx;
		int first_delay = initial_previous;
		int previous_after_first = ref_clamp(initial_previous + 1, 0, REF_MAX_PITCH);
		int second_delay = previous_after_first;
		int previous_after_second = ref_clamp(previous_after_first + 1, 0, REF_MAX_PITCH);
		int16_t actual_delay;
		char phase[48];

		snprintf(phase, sizeof(phase), "pitch-loss-old%d", initial_previous);
		fill_small_history(ref_arena, 0x1234u + i * 19u);
		memcpy(actual_arena, ref_arena, sizeof(ref_arena));
		memset(&ctx, 0, sizeof(ctx));
		initDecodeAdaptativeCodeVector(&ctx);
		ctx.previousIntPitchDelay = (uint16_t)initial_previous;
		actual_delay = (int16_t)initial_previous;

		ref_compute_pitch(ref_first, first_delay, 0, NULL, NULL);
		decodeAdaptativeCodeVector(&ctx, 0, 0, 0, 1, &actual_delay, actual_first);
		EXPECT_AT(phase, 0, -1, first_delay, actual_delay);
		EXPECT_AT(phase, 0, -2, previous_after_first, ctx.previousIntPitchDelay);
		EXPECT_VEC40(phase, 0, ref_first, actual_first);

		ref_compute_pitch(ref_second, second_delay, 0, NULL, NULL);
		decodeAdaptativeCodeVector(&ctx, REF_SUBFRAME, 0, 0, 1,
					   &actual_delay, actual_second);
		EXPECT_AT(phase, 1, -1, second_delay, actual_delay);
		EXPECT_AT(phase, 1, -2, previous_after_second, ctx.previousIntPitchDelay);
		EXPECT_VEC40(phase, 1, ref_second, actual_second);
	}
	return 0;
}

#define REF_PACK_C(c0,c1,c2,c3,jx) \
	((uint16_t)((c0) | ((c1) << 3) | ((c2) << 6) | ((jx) << 9) | ((c3) << 10)))

struct ref_fixed {
	int16_t vector[REF_SUBFRAME];
	unsigned low_remainder_rounds;
	unsigned high_remainder_rounds;
	unsigned propagated_samples;
};

/* Independent 13-bit position/4-bit sign pulse description and Q14 sharpening. */
static void ref_fixed_code(uint16_t positions, uint16_t signs,
			   int pitch_delay, int16_t bounded_gain,
			   struct ref_fixed *out)
{
	int code[4], pulse_position[4], pulse_sign[4];
	int pulse, i;

	memset(out, 0, sizeof(*out));
	code[0] = (positions >> 0) & 7;
	code[1] = (positions >> 3) & 7;
	code[2] = (positions >> 6) & 7;
	code[3] = (positions >> 10) & 7;
	pulse_position[0] = 5 * code[0];
	pulse_position[1] = 5 * code[1] + 1;
	pulse_position[2] = 5 * code[2] + 2;
	pulse_position[3] = 5 * code[3] + 3 + ((positions >> 9) & 1);

	for (pulse = 0; pulse < 4; ++pulse) {
		pulse_sign[pulse] = ((signs >> pulse) & 1u) ? 8192 : -8192;
		out->vector[pulse_position[pulse]] = (int16_t)pulse_sign[pulse];
	}

	for (i = pitch_delay; i < REF_SUBFRAME; ++i) {
		int64_t product = (int64_t)out->vector[i - pitch_delay] * bounded_gain;
		int64_t remainder = product % 16384;
		int32_t sharpened;
		if (remainder < 0) remainder += 16384;
		if (remainder < 8192) ++out->low_remainder_rounds;
		else ++out->high_remainder_rounds;
		sharpened = ref_round_shift(product, 14);
		if (sharpened != 0) ++out->propagated_samples;
		out->vector[i] = ref_wrap_add16(out->vector[i], sharpened);
	}
}

static int expect_fixed_case(const char *phase, uint16_t positions, uint16_t signs,
			     int pitch_delay, int16_t bounded_gain,
			     unsigned *round_sides_seen)
{
	struct ref_fixed expected;
	int16_t actual[REF_SUBFRAME];

	ref_fixed_code(positions, signs, pitch_delay, bounded_gain, &expected);
	decodeFixedCodeVector(signs, positions, (int16_t)pitch_delay,
			      bounded_gain, actual);
	EXPECT_VEC40(phase, 0, expected.vector, actual);
	EXPECT_AT(phase, 0, -5, 1, expected.propagated_samples > 0);
	if (round_sides_seen) {
		if (expected.low_remainder_rounds) *round_sides_seen |= 1u;
		if (expected.high_remainder_rounds) *round_sides_seen |= 2u;
	}
	return 0;
}

static int test_innovation_last_tracks(void)
{
	uint16_t c_jx0 = REF_PACK_C(1, 2, 0, 2, 0); /* 5, 11, 2, 13 */
	uint16_t c_jx1 = REF_PACK_C(1, 2, 0, 2, 1); /* 5, 11, 2, 14 */
	unsigned sides = 0;

	if (expect_fixed_case("innovation-jx0", c_jx0, 0xau, 20, 9830, &sides))
		return 1;
	if (expect_fixed_case("innovation-jx1", c_jx1, 0x5u, 20, 9830, &sides))
		return 1;
	EXPECT_AT("innovation-tracks", 0, -1, 1, c_jx0 != c_jx1);
	return 0;
}

static int test_innovation_mixed_signs_rounding(void)
{
	struct fixed_case {
		uint16_t positions, signs;
		int pitch_delay;
		int16_t gain;
		const char *name;
	};
	static const struct fixed_case cases[] = {
		{ REF_PACK_C(0, 0, 0, 0, 0), 0x5u, 20, 3277, "min-gain-alternating" },
		{ REF_PACK_C(3, 1, 4, 1, 1), 0x9u, 29, 4097, "small-remainders" },
		{ REF_PACK_C(2, 5, 1, 3, 0), 0x6u, 23, 8191, "mixed-sign-rounding" },
		{ REF_PACK_C(1, 3, 6, 2, 1), 0xfu, 19, 9831, "short-recursive" },
		{ REF_PACK_C(4, 2, 3, 5, 0), 0x0u, 21, 12289, "all-negative-high-gain" },
		{ REF_PACK_C(6, 4, 2, 4, 1), 0xau, 25, 13107, "max-bounded-gain" },
		{ REF_PACK_C(7, 0, 6, 1, 1), 0x7u, 19, 6553, "mixed-dense-codes" },
		{ REF_PACK_C(2, 1, 5, 3, 0), 0x8u, 27, 4099, "single-positive-sign" }
	};
	unsigned c, sides = 0;

	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
		char phase[64];
		snprintf(phase, sizeof(phase), "innovation-%s", cases[c].name);
		if (expect_fixed_case(phase, cases[c].positions, cases[c].signs,
				      cases[c].pitch_delay, cases[c].gain, &sides))
			return 1;
	}
	/* The table must actually exercise remainders on both sides of 1/2 Q14. */
	EXPECT_AT("innovation-rounding-coverage", 0, -1, 3, sides);
	return 0;
}

static void pack_frame_parameters(const uint16_t parameters[15], uint8_t bytes[10])
{
	static const uint8_t widths[15] = {1,7,5,5,8,1,13,4,3,4,5,13,4,3,4};
	int field, bit;
	int bit_position = 0;

	memset(bytes, 0, 10);
	for (field = 0; field < 15; ++field) {
		for (bit = widths[field] - 1; bit >= 0; --bit) {
			if ((parameters[field] >> bit) & 1u)
				bytes[bit_position >> 3] |=
					(uint8_t)(1u << (7 - (bit_position & 7)));
			++bit_position;
		}
	}
}

struct frame_harness {
	bcg729DecoderChannelContextStruct *decoder;
	bcg729DecoderChannelContextStruct *gain_context;
	int16_t reference_excitation[REF_PAST + REF_FRAME];
	int previous_pitch;
	int bounded_gain;
	int bounded_changes;
};

static void harness_init(struct frame_harness *h)
{
	memset(h, 0, sizeof(*h));
	h->decoder = initBcg729DecoderChannel();
	h->gain_context = initBcg729DecoderChannel();
	memset(h->reference_excitation, 0, sizeof(h->reference_excitation));
	h->previous_pitch = 60;
	h->bounded_gain = REF_BOUNDED_MIN;
	h->bounded_changes = 0;
}

static void harness_destroy(struct frame_harness *h)
{
	if (h->decoder) closeBcg729DecoderChannel(h->decoder);
	if (h->gain_context) closeBcg729DecoderChannel(h->gain_context);
}

/* Synthesize one frame's two excitation subframes using the independent refs. */
static int harness_reference_frame(struct frame_harness *h, const uint16_t p[15])
{
	int subframe;
	int first_integer_delay = 0;

	for (subframe = 0; subframe < 2; ++subframe) {
		int offset = subframe * REF_SUBFRAME;
		int pitch_parameter = subframe == 0 ? p[4] : p[10];
		int positions_parameter = subframe == 0 ? p[6] : p[11];
		int signs_parameter = subframe == 0 ? p[7] : p[12];
		int ga = subframe == 0 ? p[8] : p[13];
		int gb = subframe == 0 ? p[9] : p[14];
		struct pitch_desc pitch;
		struct ref_fixed fixed;
		int16_t adaptive_gain = 0, fixed_gain = 0;
		int16_t old_bounded_gain = (int16_t)h->bounded_gain;
		int i;

		if (subframe == 0) {
			pitch = ref_first_pitch((uint16_t)pitch_parameter);
			first_integer_delay = pitch.integer_delay;
		} else {
			pitch = ref_second_pitch((uint16_t)pitch_parameter,
						 first_integer_delay);
		}

		ref_compute_pitch(&h->reference_excitation[REF_PAST + offset],
				  pitch.integer_delay, pitch.fraction, NULL, NULL);
		ref_fixed_code((uint16_t)positions_parameter,
			       (uint16_t)signs_parameter,
			       pitch.integer_delay, old_bounded_gain, &fixed);

		/* Explicitly permitted: original gain module on a separate context. */
		decodeGains(h->gain_context, (uint16_t)ga, (uint16_t)gb,
			    fixed.vector, 0, &adaptive_gain, &fixed_gain);

		for (i = 0; i < REF_SUBFRAME; ++i) {
			int excitation_index = REF_PAST + offset + i;
			int64_t sum =
				(int64_t)h->reference_excitation[excitation_index] * adaptive_gain +
				(int64_t)fixed.vector[i] * fixed_gain;
			h->reference_excitation[excitation_index] =
				ref_saturate16(ref_round_shift(sum, 14));
		}

		h->previous_pitch = pitch.integer_delay;
		int new_bounded = ref_clamp(adaptive_gain,
					    REF_BOUNDED_MIN, REF_BOUNDED_MAX);
		if (new_bounded != h->bounded_gain) ++h->bounded_changes;
		h->bounded_gain = new_bounded;
	}

	memmove(h->reference_excitation,
		h->reference_excitation + REF_FRAME,
		REF_PAST * sizeof(int16_t));
	return 0;
}

static void make_frame_plan(uint16_t p[15], int frame, int instance)
{
	static const uint16_t p1_values[8] = {32, 40, 51, 63, 80, 110, 196, 220};
	static const uint8_t ga_schedule[12] = {0, 5, 1, 0, 5, 0, 1, 5, 0, 1, 5, 0};
	static const uint8_t gb_schedule[12] = {0, 4, 0, 8, 4, 8, 0, 4, 8, 4, 0, 4};
	int gain_slot0 = (2 * frame + 3 * instance) % 12;
	int gain_slot1 = (gain_slot0 + 1) % 12;
	int f = frame & 7;

	memset(p, 0, 15 * sizeof(uint16_t));
	p[0] = (uint16_t)((frame + instance) & 1);
	p[1] = (uint16_t)((13 * frame + 7 * instance + 5) & 0x7f);
	p[2] = (uint16_t)((3 * frame + 2 * instance + 1) & 0x1f);
	p[3] = (uint16_t)((5 * frame + 7 * instance + 4) & 0x1f);
	p[4] = p1_values[(f + 3 * instance) & 7];
	p[5] = ref_parity(p[4]);
	p[6] = REF_PACK_C((frame + instance) & 7,
		      (2 * frame + 1) & 7,
		      (3 * frame + 2 * instance + 2) & 7,
		      (frame + 4) & 7,
		      (frame + instance) & 1);
	p[7] = (uint16_t)((0x9 + 5 * frame + 3 * instance) & 0xf);
	p[8] = ga_schedule[gain_slot0];
	p[9] = gb_schedule[gain_slot0];
	p[10] = (uint16_t)((3 * frame + 7 * instance + 1) & 0x1f);
	p[11] = REF_PACK_C((2 * frame + 3) & 7,
		       (frame + instance + 2) & 7,
		       (5 * frame + 1) & 7,
		       (2 * instance + frame + 3) & 7,
		       (frame + 2 * instance) & 1);
	p[12] = (uint16_t)((0x5 + 7 * frame + instance * 3) & 0xf);
	p[13] = ga_schedule[gain_slot1];
	p[14] = gb_schedule[gain_slot1];
}

static int verify_decoded_frame(struct frame_harness *h, const int16_t pcm[REF_FRAME],
				int frame, int instance, unsigned *frame_nonzero)
{
	int i;
	int gain_error;
	unsigned nonzero = 0, max_run = 0, run = 0;
	char phase[48];

	snprintf(phase, sizeof(phase), "frames-instance%d-frame%d", instance, frame);
	if (frame_nonzero) *frame_nonzero = 0;
	for (i = 0; i < REF_FRAME; ++i) {
		/* After the decoder's 80-word shift, this frame starts at 154-80=74. */
		EXPECT_AT("frames-excitation", i / 40, i % 40,
			  h->reference_excitation[REF_PAST - REF_FRAME + i],
			  h->decoder->excitationVector[REF_PAST - REF_FRAME + i]);
	}
	EXPECT_AT("frames-pitch-history", 1, -1,
		  h->previous_pitch, h->decoder->previousIntPitchDelay);
	EXPECT_AT("frames-bounded-gain", 1, -1,
		  h->bounded_gain, h->decoder->boundedAdaptativeCodebookGain);
	for (gain_error = 0; gain_error < 4; ++gain_error)
		EXPECT_AT("frames-gain-context", 1, gain_error,
			  h->gain_context->previousGainPredictionError[gain_error],
			  h->decoder->previousGainPredictionError[gain_error]);

	for (i = 0; i < REF_FRAME; ++i) {
		if (pcm[i] != 0) {
			++nonzero;
			++run;
			if (run > max_run) max_run = run;
		} else {
			run = 0;
		}
	}
	if (frame_nonzero) *frame_nonzero = nonzero;
	if (nonzero < 32 || max_run < 6) {
		printf("CONTENT_MISMATCH phase=frames-pcm subframe=-1 sample=%d expected_nonzero_min=32 actual_nonzero=%u expected_run_min=6 actual_run=%u\n",
		       frame, nonzero, max_run);
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(1);
	return 0;
}

static int test_frames_two_interleaved_instances(void)
{
	struct frame_harness a, b;
	int frame;
	unsigned total_pcm_nonzero = 0;
	unsigned frame_nonzero = 0;

	harness_init(&a);
	harness_init(&b);
	if (!a.decoder || !a.gain_context || !b.decoder || !b.gain_context) {
		harness_destroy(&a);
		harness_destroy(&b);
		return 1;
	}

	for (frame = 0; frame < 8; ++frame) {
		uint16_t plan_a[15], plan_b[15];
		uint8_t bytes_a[10], bytes_b[10];
		int16_t pcm_a[80], pcm_b[80];

		make_frame_plan(plan_a, frame, 0);
		make_frame_plan(plan_b, frame, 1);
		pack_frame_parameters(plan_a, bytes_a);
		pack_frame_parameters(plan_b, bytes_b);

		bcg729Decoder(a.decoder, bytes_a, 10, 0, 0, 0, pcm_a);
		harness_reference_frame(&a, plan_a);
		if (verify_decoded_frame(&a, pcm_a, frame, 0, &frame_nonzero)) {
			harness_destroy(&a); harness_destroy(&b); return 1;
		}
		total_pcm_nonzero += frame_nonzero;

		bcg729Decoder(b.decoder, bytes_b, 10, 0, 0, 0, pcm_b);
		harness_reference_frame(&b, plan_b);
		if (verify_decoded_frame(&b, pcm_b, frame, 1, &frame_nonzero)) {
			harness_destroy(&a); harness_destroy(&b); return 1;
		}
		total_pcm_nonzero += frame_nonzero;
	}

	/* The scheduled gain pairs must really create old/new bounded differences. */
	EXPECT_AT("frames-gain-variation-A", 1, -1, 1, a.bounded_changes >= 8);
	EXPECT_AT("frames-gain-variation-B", 1, -1, 1, b.bounded_changes >= 8);
	EXPECT_AT("frames-continuous-nonzero-pcm", -1, -1, 1, total_pcm_nonzero >= 512);
	harness_destroy(&a);
	harness_destroy(&b);
	return 0;
}

const struct task_case task_cases[] = {
	{"pitch", "first-indices-and-three-phases", test_pitch_first_indices},
	{"pitch", "second-relative-window-edges", test_pitch_second_window},
	{"pitch", "parity-error-first-subframe-only", test_pitch_parity_error},
	{"pitch", "explicit-frame-loss-advance-and-cap", test_pitch_explicit_loss},
	{"innovation", "last-pulse-jx-two-tracks", test_innovation_last_tracks},
	{"innovation", "mixed-signs-and-rounding-sides", test_innovation_mixed_signs_rounding},
	{"frames", "eight-frames-two-interleaved-instances", test_frames_two_interleaved_instances}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
