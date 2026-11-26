/*
 * Pulse-index contract tests for the VisualOn AMR-WB algebraic codebook.
 *
 * The production encoder remains untouched apart from the read-only
 * PULSE_TRACE observation seam in amrwbenc/src/c4t64fx.c.  The inverse index
 * implementation below is independent of quant_1p/2p/3p: it follows the
 * bit layout of the one-, two-, and three-pulse codebook indices and rebuilds
 * the 64-sample, four-track interleaved excitation.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "enc_if.h"
#include "pulse_trace.h"

#include "voAMRWB.h"
#include "cmnMemory.h"
#include "typedef.h"
#include "acelp.h"

/* typedefs.h prefixes the production Copy() symbol, which also rewrites the
 * VO_MEM_OPERATOR struct member name in this test. */
#ifdef Copy
#undef Copy
#endif

/* ----------------------------------------------------------------------- */
/* Assertion/reporting helpers                                              */
/* ----------------------------------------------------------------------- */

static char assert_label[320];

static void check_real(int condition, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(assert_label, sizeof(assert_label), fmt, args);
	va_end(args);
	unit_check(condition, assert_label);
}

/* ----------------------------------------------------------------------- */
/* Independent inverse index implementation                                 */
/* ----------------------------------------------------------------------- */

typedef struct {
	int q;       /* position within a track, 0..15 */
	int negative;
} RefPulse;

typedef struct {
	int count;
	RefPulse p[3];
} RefTrack;

typedef struct {
	int acelp_bits;
	int parameter_bits;
	int index_width[4];
	int pulse_count[4];
} PulseMode;

static const PulseMode pulse_modes[4] = {
	{20, 177, {5, 5, 5, 5},       {1, 1, 1, 1}},
	{36, 253, {9, 9, 9, 9},       {2, 2, 2, 2}},
	{44, 285, {13, 13, 9, 9},     {3, 3, 2, 2}},
	{52, 317, {13, 13, 13, 13},   {3, 3, 3, 3}}
};

static const PulseMode *find_pulse_mode(int acelp_bits)
{
	unsigned i;
	for (i = 0; i < sizeof(pulse_modes) / sizeof(pulse_modes[0]); i++) {
		if (pulse_modes[i].acelp_bits == acelp_bits)
			return &pulse_modes[i];
	}
	return NULL;
}

/*
 * One pulse, N position bits plus one sign bit.  For the top-level 1-pulse
 * mode N is 4.
 */
static int inverse_1p(int index, int n, RefTrack *out)
{
	int position_mask;

	memset(out, 0, sizeof(*out));
	if (n < 1 || n > 4 || index < 0 || index >= (1 << (n + 1)))
		return 0;

	position_mask = (1 << n) - 1;
	out->count = 1;
	out->p[0].q = index & position_mask;
	out->p[0].negative = (index >> n) & 1;
	return 1;
}

/*
 * Two pulses, 2*N position/sign-bearing bits plus one shared relation bit.
 *
 * The high and low nibbles are ordered differently depending on whether the
 * pulse signs agree.  Therefore the top bit is not a pair of independent
 * signs: together with hi<=lo versus hi>lo it identifies both signs.
 */
static int inverse_2p(int index, int n, RefTrack *out)
{
	int mask, hi, lo, top_negative;

	memset(out, 0, sizeof(*out));
	if (n < 1 || n > 4 || index < 0 || index >= (1 << (2 * n + 1)))
		return 0;

	mask = (1 << n) - 1;
	hi = (index >> n) & mask;
	lo = index & mask;
	top_negative = (index >> (2 * n)) & 1;

	out->count = 2;
	if (hi <= lo) {
		/* Same signs; hi/lo are min/max local positions. */
		out->p[0].q = hi;
		out->p[1].q = lo;
		out->p[0].negative = top_negative;
		out->p[1].negative = top_negative;
	} else {
		/*
		 * Opposite signs; hi is the larger position and the top bit
		 * is that position's sign.  The lower position has the other
		 * sign.
		 */
		out->p[0].q = hi;
		out->p[1].q = lo;
		out->p[0].negative = top_negative;
		out->p[1].negative = !top_negative;
	}
	return 1;
}

/*
 * Three pulses, 3*4+1 = 13 bits.  The top five bits are the singleton pulse;
 * bit 7 is the half (q bit 3) occupied by the recursively encoded pair; the
 * low seven bits are a 2-pulse index with N=3.
 */
static int inverse_3p(int index, RefTrack *out)
{
	RefTrack pair, singleton;
	int half;

	memset(out, 0, sizeof(*out));
	if (index < 0 || index >= (1 << 13))
		return 0;
	if (!inverse_1p((index >> 8) & 0x1f, 4, &singleton))
		return 0;
	if (!inverse_2p(index & 0x7f, 3, &pair))
		return 0;

	half = (index >> 7) & 1;
	pair.p[0].q += half * 8;
	pair.p[1].q += half * 8;

	out->count = 3;
	out->p[0] = pair.p[0];
	out->p[1] = pair.p[1];
	out->p[2] = singleton.p[0];
	return 1;
}

static int decode_track(int pulse_count, int index, RefTrack *track)
{
	if (pulse_count == 1)
		return inverse_1p(index, 4, track);
	if (pulse_count == 2)
		return inverse_2p(index, 4, track);
	if (pulse_count == 3)
		return inverse_3p(index, track);
	return 0;
}

/* Four tracks are interleaved: absolute sample = track + 4 * q. */
static int inverse_excitation(const PulseMode *mode, const int indices[4],
			      int amplitude, int excitation[64])
{
	int t, k;

	memset(excitation, 0, 64 * sizeof(excitation[0]));
	for (t = 0; t < 4; t++) {
		RefTrack track;
		if (!decode_track(mode->pulse_count[t], indices[t], &track))
			return 0;
		for (k = 0; k < track.count; k++) {
			int sample = t + 4 * track.p[k].q;
			/* Deliberate +=: coincident same-sign pulses stack. */
			excitation[sample] += track.p[k].negative ?
				-amplitude : amplitude;
		}
	}
	return 1;
}

/* ----------------------------------------------------------------------- */
/* Actual observation storage and occurrence counters                       */
/* ----------------------------------------------------------------------- */

#define MAX_TRACES 512

static pulse_trace_sample trace_ring[MAX_TRACES];
static unsigned trace_count;
static unsigned trace_overflow;
static unsigned long long callback_count;

static unsigned long long stat_observed_subframes;
static unsigned long long stat_mode_subframes[4];
static unsigned long long stat_sample_comparisons;
static unsigned long long stat_index_comparisons;
static unsigned long long stat_decoded_tracks;
static unsigned long long stat_pulse_occurrences;
static unsigned long long stat_duplicate_position_tracks;
static unsigned long long stat_equal_2p;
static unsigned long long stat_same_order_2p;
static unsigned long long stat_opposite_2p;
static unsigned long long stat_head_positions;
static unsigned long long stat_tail_positions;
static unsigned long long stat_pair_half_low;
static unsigned long long stat_pair_half_high;
static unsigned long long stat_singleton_same_half;
static unsigned long long stat_singleton_other_half;

void pulse_trace_callback(const pulse_trace_sample *sample)
{
	callback_count++;
	if (trace_count < MAX_TRACES)
		trace_ring[trace_count++] = *sample;
	else
		trace_overflow++;
}

static void traces_reset(void)
{
	trace_count = 0;
	trace_overflow = 0;
}

static void classify_track(const PulseMode *mode, int track_no, int index)
{
	RefTrack track;
	int q_count[16];
	int k, half, singleton_half;

	if (!decode_track(mode->pulse_count[track_no], index, &track))
		return;

	stat_decoded_tracks++;
	stat_pulse_occurrences += track.count;
	memset(q_count, 0, sizeof(q_count));

	for (k = 0; k < track.count; k++) {
		int q = track.p[k].q;
		q_count[q]++;
		if (q == 0)
			stat_head_positions++;
		if (q == 15)
			stat_tail_positions++;
	}
	for (k = 0; k < 16; k++) {
		if (q_count[k] >= 2) {
			stat_duplicate_position_tracks++;
			break;
		}
	}

	if (mode->pulse_count[track_no] == 2) {
		int hi = (index >> 4) & 15;
		int lo = index & 15;
		if (hi == lo)
			stat_equal_2p++;
		else if (hi < lo)
			stat_same_order_2p++;
		else
			stat_opposite_2p++;
	}

	if (mode->pulse_count[track_no] == 3) {
		half = (index >> 7) & 1;
		singleton_half = (track.p[2].q >> 3) & 1;
		if (half == 0)
			stat_pair_half_low++;
		else
			stat_pair_half_high++;
		if (singleton_half == half)
			stat_singleton_same_half++;
		else
			stat_singleton_other_half++;
	}
}

static void verify_indexed_code(const PulseMode *mode, const int indices[4],
				const short *code, int pulse_amplitude,
				const char *where)
{
	int expected[64];
	int abs_sum = 0;
	int t, i, total_pulses = 0;

	check_real(pulse_amplitude == 256 || pulse_amplitude == 512,
		   "%s: unexpected pulse amplitude=%d",
		   where, pulse_amplitude);

	for (t = 0; t < 4; t++) {
		int width = mode->index_width[t];
		stat_index_comparisons++;
		check_real(indices[t] >= 0 && indices[t] < (1 << width),
			   "%s: bits=%d track=%d index=%d outside %d bits",
			   where, mode->acelp_bits, t, indices[t], width);
		classify_track(mode, t, indices[t]);
		total_pulses += mode->pulse_count[t];
	}

	check_real(inverse_excitation(mode, indices, pulse_amplitude,
				      expected),
		   "%s: inverse rejected bits=%d indices=%d,%d,%d,%d",
		   where, mode->acelp_bits, indices[0], indices[1],
		   indices[2], indices[3]);

	for (i = 0; i < 64; i++) {
		stat_sample_comparisons++;
		check_real((int)code[i] == expected[i],
			   "%s sample=%d bits=%d idx=[%d,%d,%d,%d] "
			   "amplitude=%d actual=%d expected=%d",
			   where, i, mode->acelp_bits,
			   indices[0], indices[1], indices[2], indices[3],
			   pulse_amplitude, (int)code[i], expected[i]);
		abs_sum += abs((int)code[i]);
	}

	check_real((abs_sum % pulse_amplitude) == 0 &&
		   abs_sum / pulse_amplitude == total_pulses,
		   "%s: pulse multiplicity count mismatch "
		   "abs_sum=%d amplitude=%d decoded_pulses=%d",
		   where, abs_sum, pulse_amplitude, total_pulses);

	stat_observed_subframes++;
	stat_mode_subframes[mode - pulse_modes]++;
}

static void verify_observed(const pulse_trace_sample *obs, const char *where)
{
	const PulseMode *mode;
	int indices[4];
	int t;

	mode = find_pulse_mode(obs->nbbits);
	check_real(mode != NULL, "%s: unsupported observed nbbits=%d",
		   where, obs->nbbits);
	if (!mode)
		return;

	check_real(obs->magic == PULSE_TRACE_MAGIC,
		   "%s: bad trace magic=0x%08x", where, obs->magic);

	for (t = 0; t < 4; t++)
		indices[t] = obs->index[t];
	verify_indexed_code(mode, indices, obs->code,
			    obs->pulse_amplitude, where);
}

/* ----------------------------------------------------------------------- */
/* test_inverse: hand-built index contracts and exact encoder vectors       */
/* ----------------------------------------------------------------------- */

typedef struct {
	int q;
	int negative;
} WantPulse;

static void expect_ref_track(const char *name, int pulse_count, int index,
			     const WantPulse *wanted, int wanted_count)
{
	RefTrack got;
	int used[3] = {0, 0, 0};
	int i, j;

	check_real(decode_track(pulse_count, index, &got),
		   "inverse decode rejected %s index=0x%x", name, index);
	check_real(got.count == wanted_count,
		   "%s: decoded count=%d expected=%d index=0x%x",
		   name, got.count, wanted_count, index);

	for (i = 0; i < wanted_count; i++) {
		int found = 0;
		for (j = 0; j < got.count; j++) {
			if (!used[j] && got.p[j].q == wanted[i].q &&
			    got.p[j].negative == wanted[i].negative) {
				used[j] = 1;
				found = 1;
				break;
			}
		}
		check_real(found,
			   "%s: missing pulse q=%d sign=%s index=0x%x",
			   name, wanted[i].q,
			   wanted[i].negative ? "-" : "+", index);
	}
}

static void test_inverse_tables(void)
{
	static const WantPulse p_1p_q0_pos[] = {{0, 0}};
	static const WantPulse p_1p_q0_neg[] = {{0, 1}};
	static const WantPulse p_1p_q15_pos[] = {{15, 0}};
	static const WantPulse p_1p_q15_neg[] = {{15, 1}};

	static const WantPulse same7_pos_x2[] = {{7, 0}, {7, 0}};
	static const WantPulse same0_neg_x2[] = {{0, 1}, {0, 1}};
	static const WantPulse opp_2_13[] = {{13, 1}, {2, 0}};
	static const WantPulse opp_0_15[] = {{15, 1}, {0, 0}};

	static const WantPulse three_A[] = {
		{0, 0}, {3, 1}, {15, 0}
	};
	static const WantPulse three_A_high[] = {
		{9, 1}, {14, 0}, {2, 1}
	};
	static const WantPulse three_B[] = {
		{1, 0}, {12, 1}, {5, 1}
	};
	static const WantPulse three_C[] = {
		{15, 1}, {2, 0}, {6, 1}
	};
	static const WantPulse three_edges[] = {
		{0, 1}, {8, 0}, {15, 1}
	};

	int index, q, negative;

	/* Exhaust the 32 one-pulse codewords. */
	for (index = 0; index < 32; index++) {
		RefTrack got;
		q = index & 15;
		negative = (index >> 4) & 1;
		check_real(inverse_1p(index, 4, &got) && got.count == 1 &&
			   got.p[0].q == q && got.p[0].negative == negative,
			   "exhaustive inverse_1p index=%d", index);
	}

	expect_ref_track("1p track head +", 1, 0, p_1p_q0_pos, 1);
	expect_ref_track("1p track head -", 1, 16, p_1p_q0_neg, 1);
	expect_ref_track("1p track tail +", 1, 15, p_1p_q15_pos, 1);
	expect_ref_track("1p track tail -", 1, 31, p_1p_q15_neg, 1);

	/* Two-pulse relation encoding, including coincident positions. */
	expect_ref_track("2p same + at q7", 2, 0x077, same7_pos_x2, 2);
	expect_ref_track("2p same - at q0", 2, 0x100, same0_neg_x2, 2);
	expect_ref_track("2p opposite q2+/q13-", 2, 0x1d2, opp_2_13, 2);
	expect_ref_track("2p opposite q0+/q15-", 2, 0x1f0, opp_0_15, 2);

	/* Three recursive branches with deliberately asymmetric halves. */
	expect_ref_track("3p branch A low pair", 3, 0xf58, three_A, 3);
	expect_ref_track("3p branch A high pair", 3, 0x12b1, three_A_high, 3);
	expect_ref_track("3p branch B", 3, 0x1c69, three_B, 3);
	expect_ref_track("3p branch C", 3, 0x1f72, three_C, 3);
	expect_ref_track("3p track ends split halves", 3, 0x10f8,
			 three_edges, 3);

	/* Check structural decoding of every two-pulse bit pattern. */
	for (index = 0; index < 512; index++) {
		RefTrack decoded;
		int hi = (index >> 4) & 15;
		int lo = index & 15;
		check_real(inverse_2p(index, 4, &decoded),
			   "exhaustive inverse_2p rejected 0x%x", index);
		check_real(decoded.count == 2 &&
			   decoded.p[0].q == hi && decoded.p[1].q == lo,
			   "exhaustive inverse_2p positions index=0x%x",
			   index);
	}
}

static void check_hand_interleaving(void)
{
	const PulseMode *m20 = &pulse_modes[0];
	const PulseMode *m44 = &pulse_modes[2];
	int idx20[4] = {16, 15, 0, 31};
	int idx44[4] = {0xf58, 0x1c69, 0x177, 0x1f0};
	int exc[64];
	int i;

	check_real(inverse_excitation(m20, idx20, 512, exc),
		   "hand 20-bit interleave decode");
	check_real(exc[0] == -512 && exc[2] == 512 &&
		   exc[61] == 512 && exc[63] == -512,
		   "20-bit track/head-tail interleave: "
		   "e0=%d e2=%d e61=%d e63=%d",
		   exc[0], exc[2], exc[61], exc[63]);
	for (i = 0; i < 64; i++) {
		if (i != 0 && i != 2 && i != 61 && i != 63)
			check_real(exc[i] == 0,
				   "20-bit unexpected sample %d=%d", i, exc[i]);
	}

	check_real(inverse_excitation(m44, idx44, 512, exc),
		   "hand 44-bit unequal-track decode");
	/* Track 2 has q7 twice with the same sign: sample 2+28=30 is -1024. */
	check_real(exc[30] == -1024,
		   "44-bit same-position accumulation exc[30]=%d", exc[30]);
	/* Last track is a two-pulse track, not a third recursive index. */
	check_real(exc[3] == 512 && exc[63] == -512,
		   "44-bit last two-pulse track exc[3]=%d exc[63]=%d",
		   exc[3], exc[63]);
}

void test_inverse(void)
{
	test_inverse_tables();
	check_hand_interleaving();
	printf("PULSE_STAT inverse callback_calls=%llu\n", callback_count);
}

/* ----------------------------------------------------------------------- */
/* Deterministic PCM                                                        */
/* ----------------------------------------------------------------------- */

static short clamp_pcm(int x)
{
	if (x > 32767)
		x = 32767;
	if (x < -32768)
		x = -32768;
	return (short)x;
}

static void make_pcm(short *pcm, unsigned frame, unsigned salt)
{
	unsigned i;
	for (i = 0; i < 320; i++) {
		unsigned pseudo = ((frame + salt + 1) * 320u + i) * 7919u;
		int noise = (int)(pseudo % 24001u) - 12000;
		int phase = (int)((i * (5u + (frame % 11u)) +
				   frame * 97u + salt * 31u) % 360u) - 180;
		int impulse_pos = (int)((frame * 53u + salt * 17u) % 320u);
		int impulse = (i == (unsigned)impulse_pos) ?
			(((frame & 1u) ? -1 : 1) * 11000) : 0;
		int second = (i == (unsigned)((impulse_pos + 61) % 320)) ?
			(((frame & 2u) ? -7000 : 7000)) : 0;
		pcm[i] = clamp_pcm(noise / 3 + phase * 35 +
				   impulse + second);
	}
}

/* ----------------------------------------------------------------------- */
/* test_serial: VisualOn VOAMRWB_DEFAULT one-word-per-BIT parameter stream  */
/* ----------------------------------------------------------------------- */

typedef struct {
	VO_AUDIO_CODECAPI api;
	VO_HANDLE handle;
	VO_MEM_OPERATOR mem;
	VO_CODEC_INIT_USERDATA user_data;
} DefaultFileEncoder;

static void default_encoder_init(DefaultFileEncoder *enc)
{
	int frame_type = VOAMRWB_DEFAULT;
	VO_U32 result;

	memset(enc, 0, sizeof(*enc));
	voGetAMRWBEncAPI(&enc->api);

	enc->mem.Alloc = cmnMemAlloc;
	enc->mem.Copy = cmnMemCopy;
	enc->mem.Free = cmnMemFree;
	enc->mem.Set = cmnMemSet;
	enc->mem.Check = cmnMemCheck;

	enc->user_data.memflag = VO_IMF_USERMEMOPERATOR;
	enc->user_data.memData = (VO_PTR)&enc->mem;
	result = enc->api.Init(&enc->handle, VO_AUDIO_CodingAMRWB,
			       &enc->user_data);
	check_real(result == VO_ERR_NONE && enc->handle != NULL,
		   "default serial encoder init failed=%u", result);
	result = enc->api.SetParam(enc->handle, VO_PID_AMRWB_FRAMETYPE,
				   &frame_type);
	check_real(result == VO_ERR_NONE,
		   "set VOAMRWB_DEFAULT failed=%u", result);
}

static void default_encoder_exit(DefaultFileEncoder *enc)
{
	if (enc->handle)
		enc->api.Uninit(enc->handle);
}

static int default_encode(DefaultFileEncoder *enc, int mode,
			  const short *pcm, unsigned short *words,
			  unsigned word_capacity, unsigned *word_count)
{
	VO_CODECBUFFER in, out;
	VO_AUDIO_OUTPUTINFO info;
	int dtx = 0;
	VO_U32 result;

	result = enc->api.SetParam(enc->handle, VO_PID_AMRWB_MODE, &mode);
	check_real(result == VO_ERR_NONE, "serial set mode %d failed=%u",
		   mode, result);
	result = enc->api.SetParam(enc->handle, VO_PID_AMRWB_DTX, &dtx);
	check_real(result == VO_ERR_NONE, "serial set dtx failed=%u", result);

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	memset(&info, 0, sizeof(info));
	in.Buffer = (unsigned char *)(void *)pcm;
	in.Length = 640;
	out.Buffer = (unsigned char *)words;
	out.Length = word_capacity * sizeof(unsigned short);

	result = enc->api.SetInputData(enc->handle, &in);
	if (result != VO_ERR_NONE)
		return (int)result;
	result = enc->api.GetOutputData(enc->handle, &out, &info);
	if (result != VO_ERR_NONE)
		return (int)result;
	*word_count = out.Length / (unsigned)sizeof(unsigned short);
	return 0;
}

typedef struct {
	const unsigned short *parameter_words;
	unsigned position;
	int broken_symbol;
} SerialBitReader;

static int serial_read_bit(SerialBitReader *reader)
{
	unsigned short symbol = reader->parameter_words[reader->position++];
	if (symbol == (unsigned short)-127) /* BIT_0 */
		return 0;
	if (symbol == 127)                  /* BIT_1 */
		return 1;
	reader->broken_symbol = 1;
	return 0;
}

static unsigned serial_read_bits(SerialBitReader *reader, unsigned width)
{
	unsigned value = 0;
	while (width--)
		value = (value << 1) | (unsigned)serial_read_bit(reader);
	return value;
}

static int serial_pitch_width(int codec_mode, int subframe)
{
	if (codec_mode == 1)
		return (subframe & 1) ? 5 : 8;
	return (subframe & 1) ? 6 : 9;
}

static int serial_gain_width(int codec_mode)
{
	return codec_mode == 1 ? 6 : 7;
}

static unsigned serial_layout_bits(const PulseMode *mode)
{
	unsigned subframe, total = 1 + 46; /* VAD flag + 46 ISF bits */
	for (subframe = 0; subframe < 4; subframe++) {
		unsigned t;
		total += (unsigned)serial_pitch_width(mode->parameter_bits ==
						      177 ? 1 : 2, subframe);
		if (mode->parameter_bits > 177)
			total += 1; /* select between pitch filters */
		for (t = 0; t < 4; t++)
			total += (unsigned)mode->index_width[t];
		total += (unsigned)serial_gain_width(
			mode->parameter_bits == 177 ? 1 : 2);
	}
	return total;
}

static void verify_serial_frame(unsigned frame_no, int codec_mode,
				const unsigned short *words,
				unsigned word_count)
{
	const PulseMode *mode = &pulse_modes[codec_mode - 1];
	SerialBitReader reader_storage;
	SerialBitReader *reader = &reader_storage;
	int serial_indices[4][4];
	unsigned expected_bits, sf, t, w;
	char where[160];

	expected_bits = serial_layout_bits(mode);
	check_real(expected_bits == (unsigned)mode->parameter_bits,
		   "serial layout table mismatch mode=%d calculated=%u table=%d",
		   codec_mode, expected_bits, mode->parameter_bits);
	check_real(word_count == 3u + expected_bits,
		   "serial frame=%u mode=%d word_count=%u expected=%u",
		   frame_no, codec_mode, word_count, 3u + expected_bits);
	check_real(words[0] == 0x6b21,
		   "serial frame=%u bad sync=0x%04x", frame_no, words[0]);
	check_real(words[1] == 0,
		   "serial frame=%u bad TX_SPEECH word=%u",
		   frame_no, words[1]);
	check_real(words[2] == (unsigned short)codec_mode,
		   "serial frame=%u header mode=%u expected=%d",
		   frame_no, words[2], codec_mode);

	check_real(trace_count == 4 && trace_overflow == 0,
		   "serial frame=%u trace_count=%u overflow=%u",
		   frame_no, trace_count, trace_overflow);

	for (w = 0; w < expected_bits; w++) {
		unsigned short raw = words[3 + w];
		check_real(raw == (unsigned short)-127 || raw == 127,
			   "serial frame=%u parameter word %u is not "
			   "BIT_0/BIT_1: 0x%04x", frame_no, w, raw);
	}

	memset(reader, 0, sizeof(*reader));
	reader->parameter_words = words + 3;

	(void)serial_read_bit(reader); /* VAD flag */
	(void)serial_read_bits(reader, 8);
	(void)serial_read_bits(reader, 8);
	(void)serial_read_bits(reader, 6);
	(void)serial_read_bits(reader, 7);
	(void)serial_read_bits(reader, 7);
	(void)serial_read_bits(reader, 5);
	(void)serial_read_bits(reader, 5);

	for (sf = 0; sf < 4; sf++) {
		int reader_codec_mode = (mode->parameter_bits == 177) ? 1 : 2;
		(void)serial_read_bits(
			reader, (unsigned)serial_pitch_width(reader_codec_mode,
							      (int)sf));
		if (mode->parameter_bits > 177)
			(void)serial_read_bit(reader);
		for (t = 0; t < 4; t++) {
			serial_indices[sf][t] = (int)serial_read_bits(
				reader, (unsigned)mode->index_width[t]);
		}
		(void)serial_read_bits(
			reader, (unsigned)serial_gain_width(reader_codec_mode));
	}

	check_real(!reader->broken_symbol,
		   "serial frame=%u encountered a non-BIT symbol", frame_no);
	check_real(reader->position == expected_bits,
		   "serial frame=%u layout consumed %u bits, expected %u",
		   frame_no, reader->position, expected_bits);

	for (sf = 0; sf < 4; sf++) {
		const pulse_trace_sample *obs = &trace_ring[sf];
		int extracted[4];
		snprintf(where, sizeof(where),
			 "serial frame=%u subframe=%u mode=%d",
			 frame_no, sf, codec_mode);
		check_real(obs->magic == PULSE_TRACE_MAGIC &&
			   obs->nbbits == mode->acelp_bits,
			   "%s: bad observation magic/bits magic=0x%x bits=%d",
			   where, obs->magic, obs->nbbits);
		for (t = 0; t < 4; t++) {
			extracted[t] = serial_indices[sf][t];
			check_real(extracted[t] == obs->index[t],
				   "serial extracted index differs from "
				   "observed index %s track=%u serial=0x%x "
				   "observed=0x%x",
				   where, t, extracted[t],
				   (unsigned)obs->index[t]);
		}
		/* The excitation comparison is driven by the bits parsed from
		 * the real 16-bit parameter stream, not by obs->index[]. */
		verify_indexed_code(mode, extracted, obs->code,
				    obs->pulse_amplitude, where);
	}
}

void test_serial(void)
{
	DefaultFileEncoder enc;
	short pcm[320];
	unsigned short words[600];
	unsigned frame, word_count = 0;

	default_encoder_init(&enc);
	for (frame = 0; frame < 24; frame++) {
		int mode = 1 + (int)(frame & 3u);
		make_pcm(pcm, frame, 100u);
		traces_reset();
		check_real(default_encode(&enc, mode, pcm, words,
					  sizeof(words) / sizeof(words[0]),
					  &word_count) == 0,
			   "default serial encode failed frame=%u mode=%d",
			   frame, mode);
		verify_serial_frame(frame, mode, words, word_count);
	}
	default_encoder_exit(&enc);
	printf("PULSE_STAT serial_frames=24 callback_calls=%llu\n",
	       callback_count);
}

/* ----------------------------------------------------------------------- */
/* test_stream: E_IF_encode frames plus constructed ACELP vectors           */
/* ----------------------------------------------------------------------- */

static void verify_eif_stream(void)
{
	void *encoder = E_IF_init();
	static const int byte_size[5] = {0, 24, 33, 37, 41};
	short pcm[320];
	unsigned char out[128];
	unsigned frame, sf;
	int n;

	check_real(encoder != NULL, "E_IF_init returned NULL");
	for (frame = 0; frame < 32; frame++) {
		int mode = 1 + (int)(frame & 3u);
		make_pcm(pcm, frame, 200u);
		traces_reset();
		n = E_IF_encode(encoder, mode, pcm, out, 0);
		check_real(n == byte_size[mode],
			   "E_IF frame=%u mode=%d bytes=%d expected=%d",
			   frame, mode, n, byte_size[mode]);
		check_real(((out[0] >> 3) & 15) == mode,
			   "E_IF frame=%u ToC mode=%d expected=%d",
			   frame, (out[0] >> 3) & 15, mode);
		check_real(trace_count == 4 && trace_overflow == 0,
			   "E_IF frame=%u trace_count=%u overflow=%u",
			   frame, trace_count, trace_overflow);
		for (sf = 0; sf < trace_count; sf++) {
			char where[128];
			snprintf(where, sizeof(where),
				 "E_IF frame=%u subframe=%u mode=%d",
				 frame, sf, mode);
			verify_observed(&trace_ring[sf], where);
		}
	}
	E_IF_exit(encoder);
}

static Word16 pseudo_word(unsigned seed, unsigned iteration, int span)
{
	unsigned x = (seed * 1103515245u + iteration * 12345u + 7u) & 0xffffu;
	return (Word16)((int)(x % (unsigned)span) - span / 2);
}

static void verify_crafted_acelp_vectors(void)
{
	static const int bits_list[4] = {20, 36, 44, 52};
	static const int ser_for_bits[4] = {177, 253, 285, 317};
	unsigned bi, seed;
	unsigned crafted_calls = 0;

	for (bi = 0; bi < 4; bi++) {
		for (seed = 0; seed < 96; seed++) {
			Word16 dn[64], cn[64], xn[64], h[64];
			Word16 code[64], y[64], idx[8];
			unsigned i, p;
			const pulse_trace_sample *obs;
			char where[160];

			for (i = 0; i < 64; i++) {
				xn[i] = pseudo_word(seed + bi * 1009u,
						     i + 1u, 1500);
				cn[i] = (seed & 1u) ?
					pseudo_word(seed + 71u, i + 3u, 900) :
					0;
				if (seed % 3u == 0) {
					h[i] = (i == 0) ? 2048 :
						(i < 10 && (i & 1) == 0) ?
						(Word16)(900 / (1 + i / 2)) : 0;
				} else {
					h[i] = (i == 0) ?
						(Word16)(1024 + (seed & 3u) *
							 256u) :
						(i < 16) ?
						(Word16)(1400 / (i + 2)) : 0;
				}
				code[i] = 0;
				y[i] = 0;
			}

			/*
			 * Put strong target impulses at deterministic track/q
			 * combinations, then derive dn[] with the production
			 * correlation routine.  Seeds 0..15 explicitly walk
			 * every track position, including q=0 and q=15.
			 */
			for (p = 0; p < 3; p++) {
				unsigned q = (seed < 16) ? seed :
					(seed * 5u + p * 7u + bi) & 15u;
				unsigned track = (seed + p + bi) & 3u;
				unsigned pos = track + 4u * q;
				int magnitude = 8200 + (int)(p * 900);
				xn[pos] = (Word16)(((seed + p) & 1u) ?
						   -magnitude : magnitude);
			}

			/* A few extra coincident peaks encourage multiplicity. */
			if ((seed % 5u) == 0) {
				unsigned q = (seed / 5u) & 15u;
				unsigned track = bi;
				xn[track + 4u * q] =
					(Word16)(((seed & 1u) ? -11000 : 11000));
			}
			cor_h_x(h, xn, dn);

			for (i = 0; i < 8; i++)
				idx[i] = 0;
			traces_reset();
			ACELP_4t64_fx(dn, cn, h, code, y,
				      (Word16)bits_list[bi],
				      (Word16)ser_for_bits[bi], idx);
			crafted_calls++;
			check_real(trace_count == 1 && trace_overflow == 0,
				   "crafted ACELP bits=%d seed=%u trace=%u",
				   bits_list[bi], seed, trace_count);
			if (trace_count != 1)
				continue;
			obs = &trace_ring[0];
			snprintf(where, sizeof(where),
				 "crafted bits=%d seed=%u",
				 bits_list[bi], seed);
			verify_observed(obs, where);
		}
	}
	check_real(trace_overflow == 0, "crafted trace overflow");
	printf("PULSE_STAT crafted_acelp_calls=%u\n", crafted_calls);
}

static void check_required_coverage(void)
{
	/*
	 * These are occurrence floors from the deterministic E_IF, serial,
	 * and constructed-vector corpus.  They intentionally count observed
	 * events rather than merely mode names.
	 */
	check_real(stat_observed_subframes >= 400,
		   "too few observed subframes=%llu",
		   stat_observed_subframes);
	for (unsigned mode = 0; mode < 4; mode++) {
		check_real(stat_mode_subframes[mode] == 152,
			   "mode %d observed subframes=%llu expected 152",
			   pulse_modes[mode].acelp_bits,
			   stat_mode_subframes[mode]);
	}
	check_real(stat_sample_comparisons == stat_observed_subframes * 64,
		   "sample comparison count %llu != 64*subframes",
		   stat_sample_comparisons);
	check_real(stat_duplicate_position_tracks > 0,
		   "no same-position accumulated pulses observed");
	check_real(stat_equal_2p > 0,
		   "no equal-position two-pulse index observed");
	check_real(stat_opposite_2p > 0,
		   "no opposite-sign/order-reversed two-pulse index observed");
	check_real(stat_head_positions > 0 && stat_tail_positions > 0,
		   "missing track head/tail pulses head=%llu tail=%llu",
		   stat_head_positions, stat_tail_positions);
	check_real(stat_pair_half_low > 0 && stat_pair_half_high > 0,
		   "three-pulse pair halves not both covered low=%llu high=%llu",
		   stat_pair_half_low, stat_pair_half_high);
	check_real(stat_singleton_other_half > 0,
		   "no asymmetric three-pulse half decomposition observed");

	printf("PULSE_COVERAGE subframes=%llu mode_subframes=%llu/%llu/%llu/%llu "
	       "samples=%llu indices=%llu "
	       "tracks=%llu pulses=%llu duplicate_tracks=%llu "
	       "equal2=%llu same_order2=%llu opposite2=%llu "
	       "head=%llu tail=%llu pair_low=%llu pair_high=%llu "
	       "singleton_same=%llu singleton_other=%llu callbacks=%llu\n",
	       stat_observed_subframes,
	       stat_mode_subframes[0], stat_mode_subframes[1],
	       stat_mode_subframes[2], stat_mode_subframes[3],
	       stat_sample_comparisons,
	       stat_index_comparisons, stat_decoded_tracks,
	       stat_pulse_occurrences, stat_duplicate_position_tracks,
	       stat_equal_2p, stat_same_order_2p, stat_opposite_2p,
	       stat_head_positions, stat_tail_positions,
	       stat_pair_half_low, stat_pair_half_high,
	       stat_singleton_same_half, stat_singleton_other_half,
	       callback_count);
}

void test_stream(void)
{
	verify_eif_stream();
	verify_crafted_acelp_vectors();
	check_required_coverage();
}
