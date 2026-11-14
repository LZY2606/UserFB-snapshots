/*
 ** Copyright 2003-2010, VisualOn, Inc.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

/*--------------------------------------------------------------------------*
 *                         PULSE_CONTRACT.C                                 *
 *--------------------------------------------------------------------------*
 * Algebraic-codebook (4 tracks x 16 positions, 64-sample subframe) contract*
 * test for the 20 / 36 / 44 / 52 bit modes of the VisualOn AMR-WB encoder. *
 *                                                                          *
 * The codebook index is only useful if it can be turned back into the      *
 * exact excitation the encoder used. This file never calls the production  *
 * quant_* functions to manufacture an "expected" result and never copies   *
 * an answer out of the code[] array. Instead it independently re-derives,  *
 * from the index alone, every pulse position (0..15), sign and multiplicity*
 * per track, interleaves the four tracks, accumulates coincident like-     *
 * signed pulses, and compares the resulting 64 samples against the raw      *
 * excitation observed at the PULSE_TRACE observation point.                *
 *                                                                          *
 * Three groups:                                                            *
 *   test_inverse - direct ACELP_4t64_fx() on frozen legal correlation /    *
 *                  impulse-response vectors plus a fixed-seed property run *
 *   test_serial  - VisualOn DEFAULT parameter stream (16-bit BIT_0/BIT_1   *
 *                  words); indices read bit-by-bit from the real output    *
 *   test_stream  - public E_IF_encode() RFC 3267 MIME byte stream; the     *
 *                  packed bytes are mapped back to canonical serial bits    *
 *                  through the sort permutation before reading the indices *
 *--------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "typedef.h"
#include "acelp.h"
#include "bits.h"
#include "voAMRWB.h"
#include "cmnMemory.h"
#include "enc_if.h"
#include "pulse_trace.h"
#include "mime_io.tab"

/* The codec typedefs define Copy as a macro (voAWB_Copy). The VO_MEM_OPERATOR
 * struct is parsed while that macro is live, so its member is named
 * voAWB_Copy; writing mop.Copy expands to the matching member, exactly as the
 * production sources do. Keep the macro defined rather than undefining it. */

#define PULSE_SUBFR      64
#define PULSE_TRACKS     4
#define BIT_WORD_0       (-127)          /* BIT_0 as emitted by PackBits()   */
#define BIT_WORD_1       (127)           /* BIT_1 as emitted by PackBits()   */

/* mode table index = AMR-WB mode-1 for the four codebook modes under test */
static const int MODE_SER_BITS[4] = {177, 253, 285, 317};
static const int MODE_NBBITS[4]    = {20, 36, 44, 52};
static const int MODE_CB_WIDTH[4][PULSE_TRACKS] = {
	{5, 5, 5, 5},          /* 20 bit: four 1-pulse, 5-bit tracks      */
	{9, 9, 9, 9},          /* 36 bit: four 2-pulse, 9-bit tracks      */
	{13, 13, 9, 9},        /* 44 bit: two 3-pulse then two 2-pulse    */
	{13, 13, 13, 13}       /* 52 bit: four 3-pulse, 13-bit tracks     */
};

/* A decoded pulse: position inside its track (0..15) and amplitude sign. */
typedef struct
{
	int pos;
	int sgn;
} Pulse;

/*--------------------------------------------------------------------------*
 * Independent inverse index interpreters. These mirror the bit packing of  *
 * quant_1p_N1 / quant_2p_2N1 / quant_3p_3N1 in q_pulse.c but are written    *
 * from the index format, not by calling those functions.                   *
 *--------------------------------------------------------------------------*/

/* 1 pulse, N+1 bits: N low position bits, top bit is the sign (1 => -ve). */
static int decode_1p(int idx, Pulse out[1])
{
	out[0].pos = idx & 0x0F;
	out[0].sgn = (idx & 0x10) ? -1 : +1;
	return 1;
}

/* 2 pulses, 2*N+1 bits.
 *
 * The top bit is NOT a second independent sign. The two N-bit position
 * fields are stored ordered, so their magnitude relation carries the sign
 * relation:
 *   hi <= lo -> equal signs; top bit selects the shared sign
 *   hi >  lo -> opposite signs; top bit selects the sign of the hi pulse,
 *               and the lo pulse takes the other sign.
 * Coincident equal-signed pulses (hi == lo) decode as a multiplicity of two. */
static int decode_2p(int idx, int N, Pulse out[2])
{
	int mask = (1 << N) - 1;
	int hi = (idx >> N) & mask;
	int lo = idx & mask;
	int sign_bit = (idx >> (2 * N)) & 1;

	if (hi <= lo)
	{
		int s = sign_bit ? -1 : +1;
		out[0].pos = hi;
		out[0].sgn = s;
		out[1].pos = lo;
		out[1].sgn = s;
	} else
	{
		out[0].pos = hi;
		out[0].sgn = sign_bit ? -1 : +1;
		out[1].pos = lo;
		out[1].sgn = sign_bit ? +1 : -1;
	}
	return 2;
}

/* 3 pulses, 3*N+1 bits with N = 4 (13 bits).
 *
 * Layout (MSB->LSB): 5-bit signed single pulse | 1 half flag | 7-bit pair.
 * The single pulse is a 1-pulse code (4 position bits + sign bit); its
 * position's high half bit (bit 3) and the separate pair half flag choose
 * which 8-position half each group occupies, so the two halves can differ.
 * The 7-bit pair is itself a 2-pulse code over N-1 = 3 position bits. */
static int decode_3p(int idx, Pulse out[3])
{
	int single = idx >> 8;                 /* 5 bits: 4 position + sign       */
	int single_pos = single & 0x0F;
	int single_sgn = (single & 0x10) ? -1 : +1;
	int pair_half = (idx >> 7) & 1;        /* high half select for the pair   */
	int pair_code = idx & 0x7F;            /* quant_2p_2N1(...,3) = 7 bits     */
	Pulse pair[2];

	decode_2p(pair_code, 3, pair);

	out[0].pos = single_pos;
	out[0].sgn = single_sgn;
	out[1].pos = pair[0].pos | (pair_half << 3);
	out[1].sgn = pair[0].sgn;
	out[2].pos = pair[1].pos | (pair_half << 3);
	out[2].sgn = pair[1].sgn;
	return 3;
}

/* Number of pulses carried by each track for a codebook mode. */
static int pulses_per_track(int nbbits, int track)
{
	if (nbbits == 20)
		return 1;
	if (nbbits == 36)
		return 2;
	if (nbbits == 44)
		return track < 2 ? 3 : 2;          /* 3,3,2,2 - not four equal tracks */
	return 3;                              /* 52 bit                          */
}

/* Rebuild the 64-sample raw excitation purely from the four track indices.
 * Tracks interleave as sample = 4*position + track; coincident like-signed
 * pulses are accumulated, so a multiplicity of n yields n unit amplitudes
 * rather than collapsing the array into a set. */
static void rebuild_excitation(int nbbits, const int track_idx[PULSE_TRACKS],
		int unit_amp, int exc[PULSE_SUBFR])
{
	Pulse pulses[3];
	int t, k, n, sample;

	for (sample = 0; sample < PULSE_SUBFR; sample++)
		exc[sample] = 0;

	for (t = 0; t < PULSE_TRACKS; t++)
	{
		if (nbbits == 20)
			n = decode_1p(track_idx[t], pulses);
		else if (nbbits == 36)
			n = decode_2p(track_idx[t], 4, pulses);
		else if (nbbits == 44)
			n = (t < 2) ? decode_3p(track_idx[t], pulses)
			            : decode_2p(track_idx[t], 4, pulses);
		else
			n = decode_3p(track_idx[t], pulses);

		for (k = 0; k < n; k++)
			exc[4 * pulses[k].pos + t] += pulses[k].sgn * unit_amp;
	}
}

static int vectors_equal(const int a[PULSE_SUBFR], const Word16 b[PULSE_SUBFR])
{
	int i;
	for (i = 0; i < PULSE_SUBFR; i++)
		if (a[i] != (int) b[i])
			return 0;
	return 1;
}

/*--------------------------------------------------------------------------*
 * Observation recorder fed by the PULSE_TRACE hook in ACELP_4t64_fx().     *
 *--------------------------------------------------------------------------*/
static pulse_obs_t g_obs[256];
static pulse_obs_t g_last;            /* most recent observation, always kept */
static int g_nobs;                    /* observations retained in g_obs[]      */
static int g_seen_double;             /* a 2x coincident-pulse sample seen     */

static void trace_callback(const pulse_obs_t * obs)
{
	int i;
	g_last = *obs;
	if (g_nobs < (int) (sizeof(g_obs) / sizeof(g_obs[0])))
		g_obs[g_nobs] = *obs;
	g_nobs++;
	for (i = 0; i < PULSE_SUBFR; i++)
		if (abs(obs->code[i]) == 2 * obs->pulse_amp)
			g_seen_double = 1;
}

/*--------------------------------------------------------------------------*
 * Frozen, legal codebook inputs. Generated once with a fixed search and    *
 * embedded verbatim so the run is deterministic and needs no RNG state.    *
 * H is a decaying weighted-synthesis impulse response (Q12); dn/cn are the  *
 * target correlation and residual vectors. This single vector exercises     *
 * track head/tail, both 2-pulse sign orderings, multiplicity and both       *
 * asymmetric 3-pulse half assignments across all four codebook modes.       *
 *--------------------------------------------------------------------------*/
static const Word16 EDGE_DN[64] = {
	-4, -18, -1538, -1450, 2, -20, 12, 24, 13, -11, 15, 16, 5, 6, 6, -14,
	-24, 12, 1, -4, -25, -18, 13, 14, 19, -20, -9, -1059, -1554, 8, -664, -24,
	-20, 24, 8, -9, 0, 7, -1049, -18, 4, 3, -23, 4, -11, -2, 5, -19,
	-20, 14, -14, 1306, 13, -24, -3, -21, -23, 3, 25, 5, -5, -1, 1565, 5
};
static const Word16 EDGE_CN[64] = {
	-76, -39, -384, -362, -75, -75, -23, -63, -72, -43, -16, -75, -77, 8, 73, -66,
	-34, 79, -51, -28, -79, 46, -29, 39, -17, -32, -80, -264, -388, -11, -166, 2,
	-28, 68, 78, 53, 73, -21, -262, 14, -18, -66, -13, -60, -53, 8, -23, 0,
	-56, 50, -1, 326, -51, -43, 71, 28, -10, -66, -75, -66, -56, 38, 391, 1
};
static const Word16 EDGE_H[64] = {
	2048, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* Deterministic LCG used only to build additional legal codebook inputs. */
static unsigned int g_seed;
static unsigned int next_rand(void)
{
	g_seed ^= g_seed << 13;
	g_seed ^= g_seed >> 17;
	g_seed ^= g_seed << 5;
	return g_seed;
}
static int rand_between(int lo, int hi)
{
	return lo + (int) (next_rand() % (unsigned) (hi - lo + 1));
}

/*
 * Coverage flags actually observed while decoding real indices. Every flag
 * is asserted after the frozen-vector runs, proving the edge cases occur in
 * generated data rather than being named in a label.
 */
enum {
	COV_1P_HEAD = 0, COV_1P_TAIL, COV_1P_NEG, COV_1P_POS,
	COV_2P_REV_SB0, COV_2P_REV_SB1, COV_2P_MULTI, COV_2P_HEAD, COV_2P_TAIL,
	COV_3P_HALF0_ASYM, COV_3P_HALF1_ASYM, COV_3P_MULTI, COV_3P_HEAD,
	COV_3P_TAIL,
	COV_COUNT
};

static void note_coverage(int cov[COV_COUNT], int nbbits, int track,
		const int track_idx[PULSE_TRACKS])
{
	Pulse p[3];
	int idx = track_idx[track];

	if (nbbits == 20)
	{
		decode_1p(idx, p);
		if (p[0].pos == 0)
			cov[COV_1P_HEAD] = 1;
		if (p[0].pos == 15)
			cov[COV_1P_TAIL] = 1;
		if (p[0].sgn < 0)
			cov[COV_1P_NEG] = 1;
		else
			cov[COV_1P_POS] = 1;
	} else if (pulses_per_track(nbbits, track) == 2)
	{
		int hi = (idx >> 4) & 15;
		int lo = idx & 15;
		int sign_bit = (idx >> 8) & 1;
		decode_2p(idx, 4, p);
		if (hi > lo && p[0].sgn != p[1].sgn)
			cov[sign_bit ? COV_2P_REV_SB1 : COV_2P_REV_SB0] = 1;
		if (p[0].pos == p[1].pos)
			cov[COV_2P_MULTI] = 1;
		if (p[0].pos == 0 || p[1].pos == 0)
			cov[COV_2P_HEAD] = 1;
		if (p[0].pos == 15 || p[1].pos == 15)
			cov[COV_2P_TAIL] = 1;
	} else
	{
		int pair_half = (idx >> 7) & 1;
		int single_high = (idx >> 8) & 8;     /* bit 3 of the single position */
		decode_3p(idx, p);
		if (single_high != (pair_half << 3))
			cov[pair_half ? COV_3P_HALF1_ASYM : COV_3P_HALF0_ASYM] = 1;
		if (p[0].pos == p[1].pos || p[0].pos == p[2].pos || p[1].pos == p[2].pos)
			cov[COV_3P_MULTI] = 1;
		if (p[0].pos == 0 || p[1].pos == 0 || p[2].pos == 0)
			cov[COV_3P_HEAD] = 1;
		if (p[0].pos == 15 || p[1].pos == 15 || p[2].pos == 15)
			cov[COV_3P_TAIL] = 1;
	}
}

/* Run one ACELP_4t64_fx() call, verify the trace observation agrees with the
 * returned arrays, then independently rebuild the excitation from the index
 * and compare all 64 samples. Returns the observed subframe record. */
static const pulse_obs_t * run_one_acelp(int nbbits, const Word16 * dn,
		const Word16 * cn, const Word16 * H, int cov[COV_COUNT],
		const char *label)
{
	Word16 dn_local[64], cn_local[64], H_local[64];
	Word16 code[64], y[64], index[8];
	int rebuilt[64], track_idx[PULSE_TRACKS], before, t;

	memcpy(dn_local, dn, sizeof(dn_local));
	memcpy(cn_local, cn, sizeof(cn_local));
	memcpy(H_local, H, sizeof(H_local));
	memset(code, 0, sizeof(code));
	memset(y, 0, sizeof(y));
	memset(index, 0, sizeof(index));

	before = g_nobs;
	ACELP_4t64_fx(dn_local, cn_local, H_local, code, y, nbbits, 317, index);

	/* The observation hook must have fired exactly once and must expose the
	 * same raw values the production function returned. */
	unit_check(g_nobs == before + 1, "trace: one observation per subframe");
	{
		const pulse_obs_t *obs = &g_obs[g_nobs - 1];
		unit_check(obs->nbbits == nbbits, "trace: observed mode matches");
		unit_check(obs->pulse_amp == 512, "trace: unit pulse amplitude recorded");
		for (t = 0; t < PULSE_TRACKS; t++)
		{
			track_idx[t] = obs->index[t];
			unit_check(obs->index[t] == index[t], "trace: observed index matches");
		}
		unit_check(memcmp(code, obs->code, sizeof(code)) == 0,
				"trace: observed raw code matches");

		/* Independent inverse: index alone -> 64-sample excitation. */
		rebuild_excitation(nbbits, track_idx, obs->pulse_amp, rebuilt);
		unit_check(vectors_equal(rebuilt, obs->code) != 0, label);

		for (t = 0; t < PULSE_TRACKS; t++)
			note_coverage(cov, nbbits, t, track_idx);
		return obs;
	}
}

void test_inverse(void)
{
	static const int nbbits[4] = {20, 36, 44, 52};
	int cov[COV_COUNT];
	int m, t, c, subframes;

	memset(cov, 0, sizeof(cov));
	g_nobs = 0;
	pulse_trace_cb = trace_callback;

	/* ---- Frozen legal vectors over all four codebook modes ---- */
	for (m = 0; m < 4; m++)
	{
		const pulse_obs_t *obs =
			run_one_acelp(nbbits[m], EDGE_DN, EDGE_CN, EDGE_H, cov,
					"inverse: frozen index rebuilds raw excitation");

		/* 44 bit mode must be 3,3,2,2, never four equal-length tracks. */
		if (nbbits[m] == 44)
		{
			unit_check(pulses_per_track(44, 0) == 3, "44bit: track0 has 3 pulses");
			unit_check(pulses_per_track(44, 1) == 3, "44bit: track1 has 3 pulses");
			unit_check(pulses_per_track(44, 2) == 2, "44bit: track2 has 2 pulses");
			unit_check(pulses_per_track(44, 3) == 2, "44bit: track3 has 2 pulses");
			unit_check(obs->index[0] < (1 << 13) && obs->index[1] < (1 << 13),
					"44bit: first two indices are 13 bit");
			unit_check(obs->index[2] < (1 << 9) && obs->index[3] < (1 << 9),
					"44bit: last two indices are 9 bit");
		}
	}

	/* Every required edge case must have occurred in real generated data. */
	for (c = 0; c < COV_COUNT; c++)
		unit_check(cov[c] == 1, "inverse: required edge case observed");

	/* Multiplicity means accumulation, not a set: a coincident equal-signed
	 * pair must leave 2 unit amplitudes in the observed raw excitation. */
	unit_check(g_seen_double, "inverse: coincident pulses accumulate to 2x");

	/* ---- Fixed-seed property run over many additional legal inputs ---- */
	g_seed = 0x1234ABCDu;
	subframes = 0;
	{
		int trial;
		for (trial = 0; trial < 1500; trial++)
		{
			Word16 dn[64], cn[64], H[64];
			int i, peaks;
			for (i = 0; i < 64; i++)
			{
				H[i] = 0;
				dn[i] = (Word16) rand_between(-1000, 1000);
				cn[i] = (Word16) rand_between(-250, 250);
			}
			H[0] = (Word16) rand_between(1024, 2048);
			if (next_rand() & 1)
				for (i = 1; i < 8; i++)
					H[i] = (Word16) (H[0] >> (i < 5 ? i : 5));
			peaks = rand_between(4, 10);
			for (i = 0; i < peaks; i++)
			{
				int tr = next_rand() & 3;
				int pos = next_rand() & 15;
				int val = (next_rand() & 1) ? -rand_between(500, 2000)
				                            : rand_between(500, 2000);
				dn[4 * pos + tr] = (Word16) val;
				cn[4 * pos + tr] = (Word16) (val / 4);
			}
			for (m = 0; m < 4; m++)
			{
				/* Fresh copy per mode: ACELP_4t64_fx() negates entries in
				 * dn[], so each mode must see the pristine generated input. */
				Word16 d[64], c[64], hh[64];
				Word16 code[64], y[64], index[8];
				int rebuilt[64], ti[PULSE_TRACKS];
				int b;
				memcpy(d, dn, sizeof(d));
				memcpy(c, cn, sizeof(c));
				memcpy(hh, H, sizeof(hh));
				memset(code, 0, sizeof(code));
				memset(y, 0, sizeof(y));
				memset(index, 0, sizeof(index));
				b = g_nobs;
				ACELP_4t64_fx(d, c, hh, code, y, nbbits[m], 317, index);
				unit_check(g_nobs == b + 1, "property: observation fired");
				for (t = 0; t < PULSE_TRACKS; t++)
					ti[t] = g_last.index[t];
				rebuild_excitation(nbbits[m], ti, g_last.pulse_amp, rebuilt);
				unit_check(vectors_equal(rebuilt, code) != 0,
						"property: inverse index matches raw excitation");
				subframes++;
			}
		}
	}
	unit_check(subframes == 1500 * 4, "property: deterministic subframe count");

	pulse_trace_cb = (void *) 0;
}

/*--------------------------------------------------------------------------*
 * Bit reader over a canonical serial bit array (0/1 values).               *
 *--------------------------------------------------------------------------*/
typedef struct
{
	const int *bits;
	int pos;
} SerialReader;

static int sr_read(SerialReader * r, int width)
{
	int v = 0, k;
	for (k = 0; k < width; k++)
		v = (v << 1) | r->bits[r->pos++];
	return v;
}

/* Extract the four subframes' four track indices from a canonical serial
 * bit array, following the parameter layout of coder(): 47 header bits
 * (1 VAD + 46 ISF), then per subframe pitch, optional select, codebook and
 * gain. The reader ends exactly on the mode's declared serial size. */
static void extract_indices(int mode, const int *bits,
		int idx[4][PULSE_TRACKS])
{
	SerialReader r;
	int mi = mode - 1;
	int s, t;
	r.bits = bits;
	r.pos = 0;

	(void) sr_read(&r, 47);                 /* VAD (1) + ISF (46) */

	for (s = 0; s < 4; s++)
	{
		int pitch_width = (mode == 1) ? (s % 2 ? 5 : 8) : (s % 2 ? 6 : 9);
		int select_width = (mode == 1) ? 0 : 1;
		int gain_width = (mode == 1) ? 6 : 7;

		(void) sr_read(&r, pitch_width);
		if (select_width)
			(void) sr_read(&r, select_width);
		for (t = 0; t < PULSE_TRACKS; t++)
			idx[s][t] = sr_read(&r, MODE_CB_WIDTH[mi][t]);
		(void) sr_read(&r, gain_width);
	}
	unit_check(r.pos == MODE_SER_BITS[mi], "serial: layout consumes all bits");
}

/* Decode stream-extracted indices back to an excitation and compare every
 * sample to the independently observed raw excitation. */
static void compare_stream_to_obs(int mode, int idx[4][PULSE_TRACKS],
		int first_obs, int *sample_checks, int *index_checks)
{
	int mi = mode - 1;
	int nbbits = MODE_NBBITS[mi];
	int s;
	for (s = 0; s < 4; s++)
	{
		const pulse_obs_t *obs = &g_obs[first_obs + s];
		int rebuilt[64], ti[PULSE_TRACKS], t;
		unit_check(obs->nbbits == nbbits, "stream: observed mode per subframe");
		for (t = 0; t < PULSE_TRACKS; t++)
		{
			ti[t] = idx[s][t];
			unit_check(idx[s][t] == obs->index[t],
					"stream: extracted index equals observed index");
			(*index_checks)++;
		}
		rebuild_excitation(nbbits, ti, obs->pulse_amp, rebuilt);
		unit_check(vectors_equal(rebuilt, obs->code) != 0,
				"stream: bit-stream index rebuilds observed excitation");
		(*sample_checks) += PULSE_SUBFR;
	}
}

/* Deterministic, frame-distinct 16 kHz PCM (same generator as baseline). */
static void make_pcm(int frame, short pcm[320])
{
	int i;
	for (i = 0; i < 320; i++)
		pcm[i] = (short) ((((frame * 320u + i) * 7919u) % 24001u) - 12000);
}

/*
 * DEFAULT parameter stream: 3 header words followed by one 16-bit word per
 * serial bit (BIT_0 / BIT_1). Driven through the public VisualOn codec API
 * with the frame type set to VOAMRWB_DEFAULT.
 */
void test_serial(void)
{
	VO_AUDIO_CODECAPI api;
	VO_MEM_OPERATOR mop;
	VO_CODEC_INIT_USERDATA user;
	VO_HANDLE handle = (void *) 0;
	int frame_type = VOAMRWB_DEFAULT;
	int dtx = 0, f;
	short pcm[320];
	unsigned short out[400];
	int sample_checks = 0, index_checks = 0, frames = 8;

	voGetAMRWBEncAPI(&api);
	mop.Alloc = cmnMemAlloc;
	mop.Copy = cmnMemCopy;
	mop.Free = cmnMemFree;
	mop.Set = cmnMemSet;
	mop.Check = cmnMemCheck;
	user.memflag = VO_IMF_USERMEMOPERATOR;
	user.memData = &mop;
	unit_check(api.Init(&handle, VO_AUDIO_CodingAMRWB, &user) == VO_ERR_NONE,
			"serial: encoder init");
	api.SetParam(handle, VO_PID_AMRWB_FRAMETYPE, &frame_type);
	api.SetParam(handle, VO_PID_AMRWB_DTX, &dtx);

	g_nobs = 0;
	pulse_trace_cb = trace_callback;

	for (f = 0; f < frames; f++)
	{
		int mode = 1 + (f % 4);             /* rotate 8.85/12.65/14.25/15.85k   */
		int mi = mode - 1;
		VO_CODECBUFFER in, outbuf;
		VO_AUDIO_OUTPUTINFO info;
		int bits[NB_BITS_MAX];
		int idx[4][PULSE_TRACKS];
		int first, p;

		make_pcm(f, pcm);
		api.SetParam(handle, VO_PID_AMRWB_MODE, &mode);
		in.Buffer = (unsigned char *) pcm;
		in.Length = 640;
		outbuf.Buffer = (unsigned char *) out;
		api.SetInputData(handle, &in);
		first = g_nobs;
		unit_check(api.GetOutputData(handle, &outbuf, &info) == VO_ERR_NONE,
				"serial: frame encoded");

		/* DEFAULT framing: sync word, frame type, mode, then BIT words. */
		unit_check(out[0] == 0x6b21, "serial: sync word");
		unit_check(out[1] == TX_SPEECH, "serial: speech frame type");
		unit_check(out[2] == mode, "serial: mode word");
		unit_check((int) outbuf.Length == (3 + MODE_SER_BITS[mi]) * 2,
				"serial: output length matches serial size");

		/* Translate the real 16-bit BIT_0/BIT_1 words to canonical bits,
		 * checking each word is a genuine bit marker. */
		for (p = 0; p < MODE_SER_BITS[mi]; p++)
		{
			unsigned short w = out[3 + p];
			unit_check(w == (unsigned short) BIT_WORD_0
					|| w == (unsigned short) BIT_WORD_1,
					"serial: word is BIT_0 or BIT_1");
			bits[p] = (w == (unsigned short) BIT_WORD_1) ? 1 : 0;
		}

		extract_indices(mode, bits, idx);
		unit_check(g_nobs == first + 4, "serial: four subframes observed");
		compare_stream_to_obs(mode, idx, first, &sample_checks, &index_checks);
	}

	unit_check(index_checks == frames * 4 * PULSE_TRACKS,
			"serial: all track indices compared");
	unit_check(sample_checks == frames * 4 * PULSE_SUBFR,
			"serial: all excitation samples compared");
	api.Uninit(handle);
	pulse_trace_cb = (void *) 0;
}

/*
 * RFC 3267 MIME storage stream as produced by the public E_IF_encode()
 * wrapper: one table-of-contents byte followed by packed payload bytes.
 * PackBits() emits canonical serial bits in sort_ptr order, MSB first. The
 * inverse permutation is built from that table (no byte scanning for a
 * matching index) so the canonical serial sequence is reconstructed first.
 */
void test_stream(void)
{
	void *enc = E_IF_init();
	short pcm[320];
	unsigned char out[128];
	int sample_checks = 0, index_checks = 0, frames = 8;
	int f;

	unit_check(enc != (void *) 0, "stream: E_IF init");
	g_nobs = 0;
	pulse_trace_cb = trace_callback;

	for (f = 0; f < frames; f++)
	{
		int mode = 1 + (f % 4);
		int mi = mode - 1;
		int ser = MODE_SER_BITS[mi];
		int bits[NB_BITS_MAX];
		int idx[4][PULSE_TRACKS];
		int first, k, ret;

		make_pcm(f, pcm);
		first = g_nobs;
		ret = E_IF_encode(enc, mode, pcm, out, 0);

		/* ToC byte and packed length come straight from the mode tables. */
		unit_check(ret == 1 + (int) packed_size[mode],
				"stream: MIME packed length");
		unit_check(out[0] == toc_byte[mode], "stream: ToC byte");

		/* Map each packed payload bit back to its canonical serial position.
		 * Sequence index k holds serial bit sort_ptr[mode][k], packed with
		 * the first bit of each byte at the MSB. */
		memset(bits, 0, sizeof(bits));
		for (k = 0; k < ser; k++)
		{
			int byte = k / 8;
			unsigned char mask = (unsigned char) (0x80 >> (k & 7));
			int serial_pos = sort_ptr[mode][k];
			bits[serial_pos] = (out[1 + byte] & mask) ? 1 : 0;
		}

		extract_indices(mode, bits, idx);
		unit_check(g_nobs == first + 4, "stream: four subframes observed");
		compare_stream_to_obs(mode, idx, first, &sample_checks, &index_checks);
	}

	unit_check(index_checks == frames * 4 * PULSE_TRACKS,
			"stream: all track indices compared");
	unit_check(sample_checks == frames * 4 * PULSE_SUBFR,
			"stream: all excitation samples compared");
	E_IF_exit(enc);
	pulse_trace_cb = (void *) 0;
}
