/*
 *  audio_stream_test.c
 *
 *  Target tests for the 32 kHz / 12 bit four channel DV audio muxer and
 *  the dv_audio_stream_t continuous input interface.
 *
 *  The tests register three groups ("companding", "layout", "stream")
 *  through task_cases/task_case_count; task_support/driver.c provides
 *  main() and the TASK_CHECK accounting.
 *
 *  This file is part of libdv, a free DV (IEC 61834/SMPTE 314M)
 *  codec.  It is free software under the GNU Lesser Public License;
 *  see COPYING for details.
 */

#include "task_support/check.h"
#include "task_support/dv_fixture.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Independent reference for the 12 bit non-linear reconstruction curve
 * (IEC 61834-2, figure 16).  Written from the standard's segment view of
 * the curve, not copied from the production code.  Code 0x800 is the
 * reserved error code and is excluded from the legal code set.
 */

static int ref_reconstruct(int code)
{
	if (code < 0x800) {
		/* non-negative codes */
		int v = code;
		int t;
		if (v <= 511)
			return v;
		t = (v - 512) / 256 + 1;
		return (v - 256 * t) << t;
	} else {
		/* negative codes: u = distance below zero */
		int u = 4096 - code;
		int t;
		if (u <= 512)
			return -u;
		t = (u - 513) / 256 + 1;
		return -((u - 256 * t - 1) << t) - 1;
	}
}

static int ref_recon[4096];        /* reconstruction of every code      */
static uint16_t ref_code[65536];   /* nearest legal code per int16      */
static int ref_ready = 0;

static void ref_init(void)
{
	int code, s, i;
	if (ref_ready)
		return;
	for (code = 0; code < 4096; code++)
		ref_recon[code] = ref_reconstruct(code);
	for (s = -32768; s <= 32767; s++) {
		int best = 0, best_dist = 0, best_abs = 0;
		for (i = 0; i < 4095; i++) {
			int r, d, a;
			/* codes ordered by reconstruction value */
			code = (i < 2047) ? 0x801 + i : i - 2047;
			r = ref_recon[code];
			d = abs(s - r);
			a = abs(r);
			if (i == 0 || d < best_dist ||
			    (d == best_dist &&
			     (a < best_abs ||
			      (a == best_abs && code < best)))) {
				best = code;
				best_dist = d;
				best_abs = a;
			}
		}
		ref_code[(uint16_t) (int16_t) s] = (uint16_t) best;
	}
	ref_ready = 1;
}

static int ref_value(int16_t sample)
{
	return ref_recon[ref_code[(uint16_t) sample]];
}

/* ---------------------------------------------------------------------------
 * Four clearly distinct, deterministic, full-range channel signals.
 */

static int16_t sig_sample(int ch, long g)
{
	uint32_t u = (uint32_t) g;
	switch (ch & 3) {
	case 0:
		return (int16_t) (u * 181u + 3u);
	case 1:
		return (int16_t) (0x7fffu - u * 257u);
	case 2:
		return (int16_t) (((u * u) >> 3) + 0x1234u);
	default: {
		uint32_t h = u * 2654435761u;
		h ^= h >> 13;
		h *= 2246822519u;
		h ^= h >> 16;
		return (int16_t) (h >> 8);
	}
	}
}

/* Signals for the exhaustive companding sweep: channels 0 and 1 visit
 * every 16 bit value exactly once over the sweep. */
static int16_t sweep_sample(int ch, long g)
{
	uint32_t u = (uint32_t) g;
	switch (ch & 3) {
	case 0:
		return (int16_t) (u - 0x8000u);
	case 1:
		return (int16_t) (0x7fffu - u);
	case 2:
		return (int16_t) (u * 4099u + 7u);
	default:
		return sig_sample(3, g);
	}
}

/* ---------------------------------------------------------------------------
 * Frame geometry helpers (positions of AAUX packs and audio DIF blocks,
 * derived from the DIF structure, not from the production shuffle tables).
 */

#define AS0_OFF   (80*6 + 80*16*3 + 3)          /* seq 0, audio block 3 */
#define ASC0_OFF  (80*6 + 80*16*4 + 3)          /* seq 0, audio block 4 */
#define AS1_PAL   (80*150*6 + AS0_OFF)          /* PAL:   seq 6 blk 3   */
#define ASC1_PAL  (80*150*6 + ASC0_OFF)         /* PAL:   seq 6 blk 4   */
#define AS1_NTSC  (80*150*6 + 80*6 + 3)         /* NTSC:  seq 6 blk 0   */
#define ASC1_NTSC (80*150*6 + 80*6 + 80*16 + 3) /* NTSC:  seq 6 blk 1   */

static int is_audio_block_byte(long pos)
{
	int block = (int) (pos / 80);
	int off   = (int) (pos % 80);
	int b     = block % 150;
	if (off < 3)
		return 0; /* DIF id bytes are not audio payload */
	return (b >= 6 && b <= 134 && ((b - 6) % 16) == 0);
}

/* ===========================================================================
 * Group: companding
 */

static int case_companding_curve(void)
{
	int i;

	ref_init();

	/* landmarks of the reconstruction curve */
	TASK_CHECK(ref_recon[0x000] == 0);
	TASK_CHECK(ref_recon[0x001] == 1);
	TASK_CHECK(ref_recon[0x1ff] == 511);
	TASK_CHECK(ref_recon[0x200] == 512);
	TASK_CHECK(ref_recon[0x2ff] == 1022);
	TASK_CHECK(ref_recon[0x300] == 1024);
	TASK_CHECK(ref_recon[0x400] == 2048);
	TASK_CHECK(ref_recon[0x7ff] == 32704);
	TASK_CHECK(ref_recon[0xfff] == -1);
	TASK_CHECK(ref_recon[0xf00] == -256);
	TASK_CHECK(ref_recon[0xe00] == -512);
	TASK_CHECK(ref_recon[0xdff] == -513);
	TASK_CHECK(ref_recon[0xd00] == -1023);
	TASK_CHECK(ref_recon[0x801] == -32641);

	/* strictly increasing over the legal codes in reconstruction
	 * order (0x800 excluded), so the nearest code is unique except
	 * for exact midpoints */
	for (i = 1; i < 4095; i++) {
		int prev = (i - 1 < 2047) ? 0x801 + i - 1 : i - 1 - 2047;
		int cur  = (i < 2047) ? 0x801 + i : i - 2047;
		TASK_CHECK(ref_recon[cur] > ref_recon[prev]);
	}

	/* tie and boundary decisions of the nearest-value reference */
	TASK_CHECK(ref_code[0] == 0x000);
	TASK_CHECK(ref_code[513] == 0x200);  /* 512 vs 514: smaller |r|  */
	TASK_CHECK(ref_code[1023] == 0x2ff); /* 1022 vs 1024             */
	TASK_CHECK(ref_code[1025] == 0x300); /* 1024 vs 1026             */
	TASK_CHECK(ref_code[(uint16_t) (int16_t) -514] == 0xdff);
	TASK_CHECK(ref_code[(uint16_t) (int16_t) -32768] == 0x801);
	TASK_CHECK(ref_code[32767] == 0x7ff);
	TASK_CHECK(ref_code[(uint16_t) (int16_t) -32641] == 0x801);
	TASK_CHECK(ref_code[32704] == 0x7ff);
	return 0;
}

static int case_companding_sweep(void)
{
	enum { PER = 1296, FRAMES = 51 }; /* 51*1296 = 66096 >= 65536 */
	dv_encoder_t *enc;
	dv_decoder_t *dec;
	long g = 0;
	int f, i, c;

	ref_init();
	enc = dv_encoder_new(0, 0, 0);
	dec = dv_decoder_new(0, 0, 0);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(dec != NULL);
	enc->samples_this_frame = PER;
	dec->audio->arg_mixing_level = 16; /* keep all four channels */
	dv_set_audio_correction(dec, DV_AUDIO_CORRECT_NONE);

	for (f = 0; f < FRAMES; f++) {
		uint8_t frame[144000];
		static int16_t pcm[4][PER];
		static int16_t out[4][DV_AUDIO_MAX_SAMPLES];
		int16_t *planes[4] = { pcm[0], pcm[1], pcm[2], pcm[3] };
		int16_t *outs[4]   = { out[0], out[1], out[2], out[3] };

		task_make_dv(frame, 1, f);
		for (i = 0; i < PER; i++, g++)
			for (c = 0; c < 4; c++)
				pcm[c][i] = sweep_sample(c, g);

		TASK_CHECK(dv_encode_full_audio(enc, planes, 4, 32000,
						frame) == 0);
		TASK_CHECK(dv_parse_header(dec, frame) >= 0);
		TASK_CHECK(dv_decode_full_audio(dec, frame, outs) == 1);
		/* the reserved 0x800 error code must never appear */
		TASK_CHECK(dec->audio->sample_failure == 0);
		for (i = 0; i < PER; i++)
			for (c = 0; c < 4; c++)
				TASK_CHECK(out[c][i] == ref_value(pcm[c][i]));
	}
	/* the sweep really covered the whole 16 bit range on ch 0/1 */
	TASK_CHECK(g >= 65536);

	dv_encoder_free(enc);
	dv_decoder_free(dec);
	return 0;
}

/* ===========================================================================
 * Group: layout
 */

static int layout_run(int pal, int n)
{
	const long size = pal ? 144000 : 120000;
	const int seqs = pal ? 12 : 10;
	const int max_slots = pal ? 1296 : 1080;
	static uint8_t frame[144000], orig[144000];
	static int16_t pcm[4][1296];
	int16_t *planes[4] = { pcm[0], pcm[1], pcm[2], pcm[3] };
	static int16_t out[4][DV_AUDIO_MAX_SAMPLES];
	int16_t *outs[4] = { out[0], out[1], out[2], out[3] };
	static unsigned int counts[4][4096], expect[4][4096];
	dv_encoder_t *enc;
	dv_decoder_t *dec;
	const uint8_t *as0, *asc0, *as1, *asc1;
	long p;
	int s, a, j, c, i, half;
	long mismatches;

	ref_init();
	task_make_dv(frame, pal, 5);
	memcpy(orig, frame, size);

	for (i = 0; i < n; i++)
		for (c = 0; c < 4; c++)
			pcm[c][i] = sig_sample(c, 777 + i);

	enc = dv_encoder_new(0, 0, 0);
	TASK_CHECK(enc != NULL);
	enc->samples_this_frame = n;
	TASK_CHECK(dv_encode_full_audio(enc, planes, 4, 32000, frame) == 0);
	dv_encoder_free(enc);

	/* --- AAUX packs, read back independently --- */
	as0  = frame + AS0_OFF;
	asc0 = frame + ASC0_OFF;
	as1  = frame + (pal ? AS1_PAL : AS1_NTSC);
	asc1 = frame + (pal ? ASC1_PAL : ASC1_NTSC);

	TASK_CHECK(as0[0] == 0x50);
	TASK_CHECK(asc0[0] == 0x51);
	TASK_CHECK(as1[0] == 0x50);
	TASK_CHECK(asc1[0] == 0x51);
	/* actual sample count, identical for both channel pairs */
	TASK_CHECK((as0[1] & 0x3f) == n - (pal ? 1264 : 1053));
	TASK_CHECK((as1[1] & 0x3f) == n - (pal ? 1264 : 1053));
	/* channel pair identity */
	TASK_CHECK((as0[2] & 0x0f) == 0);
	TASK_CHECK((as1[2] & 0x0f) == 1);
	TASK_CHECK(((as0[2] >> 4) & 1) == 1);  /* pa  */
	TASK_CHECK(((as0[2] >> 5) & 3) == 1);  /* chn */
	/* video standard */
	TASK_CHECK((as0[3] & 0x20) == (pal ? 0x20 : 0));
	TASK_CHECK((as1[3] & 0x20) == (pal ? 0x20 : 0));
	/* quantization and sampling rate */
	TASK_CHECK((as0[4] & 7) == 1);
	TASK_CHECK(((as0[4] >> 3) & 7) == 2);
	TASK_CHECK((as1[4] & 7) == 1);
	TASK_CHECK(((as1[4] >> 3) & 7) == 2);

	/* --- payload: unpack every 12 bit code independently and
	 * compare the per-channel code multiset; this pins down the
	 * channel-pair routing and the zero tail without borrowing the
	 * production shuffle tables --- */
	memset(counts, 0, sizeof(counts));
	memset(expect, 0, sizeof(expect));
	for (s = 0; s < seqs; s++) {
		half = (s >= seqs / 2);
		for (a = 0; a < 9; a++) {
			const uint8_t *blk =
				frame + (s * 150 + 6 + a * 16) * 80;
			for (j = 0; j < 24; j++) {
				unsigned b0 = blk[8 + 3*j];
				unsigned b1 = blk[9 + 3*j];
				unsigned b2 = blk[10 + 3*j];
				counts[half*2][(b0 << 4) | (b2 >> 4)]++;
				counts[half*2+1][(b1 << 4) | (b2 & 15)]++;
			}
		}
	}
	for (c = 0; c < 4; c++) {
		unsigned int total_codes = 0;
		for (i = 0; i < n; i++)
			expect[c][ref_code[(uint16_t) pcm[c][i]]]++;
		expect[c][0] += (unsigned) (max_slots - n); /* zero tail */
		TASK_CHECK(memcmp(counts[c], expect[c],
				  sizeof(counts[c])) == 0);
		TASK_CHECK(counts[c][0x800] == 0); /* reserved code */
		for (i = 0; i < 4096; i++)
			total_codes += counts[c][i];
		TASK_CHECK(total_codes == (unsigned) max_slots);
	}

	/* --- every non-audio byte must be untouched --- */
	mismatches = 0;
	for (p = 0; p < size; p++) {
		if (is_audio_block_byte(p))
			continue;
		if (frame[p] != orig[p])
			mismatches++;
	}
	TASK_CHECK(mismatches == 0);

	/* --- the public decoder reads all four channels back --- */
	dec = dv_decoder_new(0, 0, 0);
	TASK_CHECK(dec != NULL);
	dec->audio->arg_mixing_level = 16;
	dv_set_audio_correction(dec, DV_AUDIO_CORRECT_NONE);
	TASK_CHECK(dv_parse_header(dec, frame) >= 0);
	TASK_CHECK(dv_decode_full_audio(dec, frame, outs) == 1);
	TASK_CHECK(dv_is_4ch(dec) == 1);
	TASK_CHECK(dv_get_frequency(dec) == 32000);
	TASK_CHECK(dv_get_num_samples(dec) == n);
	TASK_CHECK(dv_get_raw_samples(dec, 0) == n);
	TASK_CHECK(dv_get_raw_samples(dec, 1) == n);
	for (c = 0; c < 4; c++) {
		for (i = 0; i < n; i++)
			TASK_CHECK(out[c][i] == ref_value(pcm[c][i]));
		for (i = n; i < max_slots; i++)
			TASK_CHECK(out[c][i] == 0); /* zero tail */
	}
	dv_decoder_free(dec);
	return 0;
}

static int case_layout_pal_min(void)
{
	return layout_run(1, 1264);
}

static int case_layout_pal(void)
{
	return layout_run(1, 1280);
}

static int case_layout_ntsc_min(void)
{
	return layout_run(0, 1053);
}

static int case_layout_ntsc(void)
{
	return layout_run(0, 1067);
}

static int case_layout_reject(void)
{
	static uint8_t frame[144000], orig[144000];
	static int16_t pcm[4][1296];
	int16_t *planes[4] = { pcm[0], pcm[1], pcm[2], pcm[3] };
	dv_encoder_t *enc;
	int pal, t, i, c;

	for (i = 0; i < 1296; i++)
		for (c = 0; c < 4; c++)
			pcm[c][i] = sig_sample(c, i);

	for (pal = 0; pal < 2; pal++) {
		const long size = pal ? 144000 : 120000;
		const int lo = pal ? 1264 : 1053;
		const int hi = pal ? 1296 : 1080;
		int bad[4];

		bad[0] = lo - 1;
		bad[1] = hi + 1;
		bad[2] = -1;
		bad[3] = 100000;

		enc = dv_encoder_new(0, 0, 0);
		TASK_CHECK(enc != NULL);
		for (t = 0; t < 4; t++) {
			task_make_dv(frame, pal, 2);
			memcpy(orig, frame, size);
			enc->samples_this_frame = bad[t];
			TASK_CHECK(dv_encode_full_audio(enc, planes, 4, 32000,
							frame) == -1);
			TASK_CHECK(memcmp(frame, orig, size) == 0);
		}
		/* four channels only exist at 32 kHz */
		task_make_dv(frame, pal, 2);
		memcpy(orig, frame, size);
		enc->samples_this_frame = pal ? 1280 : 1067;
		TASK_CHECK(dv_encode_full_audio(enc, planes, 4, 44100,
						frame) == -1);
		TASK_CHECK(dv_encode_full_audio(enc, planes, 4, 48000,
						frame) == -1);
		TASK_CHECK(memcmp(frame, orig, size) == 0);

		/* the window boundaries themselves are legal */
		task_make_dv(frame, pal, 2);
		enc->samples_this_frame = lo;
		TASK_CHECK(dv_encode_full_audio(enc, planes, 4, 32000,
						frame) == 0);
		task_make_dv(frame, pal, 2);
		enc->samples_this_frame = hi;
		TASK_CHECK(dv_encode_full_audio(enc, planes, 4, 32000,
						frame) == 0);
		dv_encoder_free(enc);
	}
	return 0;
}

/* ===========================================================================
 * Group: stream
 */

static long ref_budget(int pal, long k)
{
	if (pal)
		return 1280;
	return (long) (((long long) (k + 1) * 32000LL * 1001LL) / 30000LL
		       - ((long long) k * 32000LL * 1001LL) / 30000LL);
}

#define STREAM_NTSC_FRAMES 300

static int case_stream_ntsc(void)
{
	const long total =
		(long) ((300LL * 32000LL * 1001LL) / 30000LL); /* 320320 */
	static const int chunks[3] = { 1, 7, 113 };
	uint8_t *stored, *templ;
	int16_t *pcm;
	uint8_t fbuf[120000];
	dv_audio_stream_t *stream;
	long sum, pos, g;
	int k, c, ci;
	long used;
	int r;

	ref_init();

	/* rational total: the per-frame budgets sum to the exact
	 * 30000/1001 cadence */
	sum = 0;
	for (k = 0; k < STREAM_NTSC_FRAMES; k++)
		sum += ref_budget(0, k);
	TASK_CHECK(sum == total);
	TASK_CHECK(ref_budget(0, 0) == 1067);
	TASK_CHECK(ref_budget(0, 1) == 1068);

	stored = (uint8_t *) malloc((size_t) STREAM_NTSC_FRAMES * 120000);
	templ  = (uint8_t *) malloc((size_t) STREAM_NTSC_FRAMES * 120000);
	pcm = (int16_t *) malloc((size_t) total * 4 * sizeof(int16_t));
	TASK_CHECK(stored != NULL);
	TASK_CHECK(templ != NULL);
	TASK_CHECK(pcm != NULL);
	for (g = 0; g < total; g++)
		for (c = 0; c < 4; c++)
			pcm[g*4 + c] = sig_sample(c, g);
	/* one shared set of frame templates for both runs, so the
	 * comparison isolates the audio path */
	for (k = 0; k < STREAM_NTSC_FRAMES; k++)
		task_make_dv(templ + (size_t) k * 120000, 0, k);

	/* --- bulk run: offer everything, one frame per call --- */
	stream = dv_audio_stream_new(0);
	TASK_CHECK(stream != NULL);
	pos = 0;
	for (k = 0; k < STREAM_NTSC_FRAMES; k++) {
		used = 0;
		memcpy(fbuf, templ + (size_t) k * 120000, sizeof(fbuf));
		r = dv_audio_stream_feed(stream, pcm + pos*4, total - pos,
					 &used, fbuf, sizeof(fbuf));
		TASK_CHECK(r == 1);
		TASK_CHECK(used == ref_budget(0, k));
		pos += used;
		memcpy(stored + (size_t) k * 120000, fbuf, sizeof(fbuf));
	}
	TASK_CHECK(pos == total);
	dv_audio_stream_free(stream);

	/* --- every committed frame advertises its exact budget in
	 * both channel pairs --- */
	for (k = 0; k < STREAM_NTSC_FRAMES; k++) {
		const uint8_t *f = stored + (size_t) k * 120000;
		const uint8_t *as0 = f + AS0_OFF;
		const uint8_t *as1 = f + AS1_NTSC;
		TASK_CHECK(as0[0] == 0x50);
		TASK_CHECK(as1[0] == 0x50);
		TASK_CHECK((as0[1] & 0x3f) + 1053 == ref_budget(0, k));
		TASK_CHECK((as1[1] & 0x3f) + 1053 == ref_budget(0, k));
	}

	/* --- chunked run: 1/7/113 frames at a time, same DV bytes --- */
	stream = dv_audio_stream_new(0);
	TASK_CHECK(stream != NULL);
	pos = 0;
	ci = 0;
	for (k = 0; k < STREAM_NTSC_FRAMES; k++) {
		memcpy(fbuf, templ + (size_t) k * 120000, sizeof(fbuf));
		for (;;) {
			long offer = chunks[ci++ % 3];
			if (offer > total - pos)
				offer = total - pos;
			used = 0;
			r = dv_audio_stream_feed(stream, pcm + pos*4, offer,
						 &used, fbuf, sizeof(fbuf));
			TASK_CHECK(r == 0 || r == 1);
			TASK_CHECK(used >= 0 && used <= offer);
			if (r == 0)
				TASK_CHECK(used == offer);
			pos += used;
			if (r == 1)
				break;
		}
		TASK_CHECK(memcmp(fbuf, stored + (size_t) k * 120000,
				  sizeof(fbuf)) == 0);
	}
	TASK_CHECK(pos == total);
	dv_audio_stream_free(stream);

	free(stored);
	free(templ);
	free(pcm);
	return 0;
}

static int case_stream_pal(void)
{
	enum { FRAMES = 30 };
	const long total = FRAMES * 1280;
	static const int chunks[3] = { 1, 7, 113 };
	static uint8_t stored[FRAMES][144000];
	static uint8_t templ[FRAMES][144000];
	static int16_t pcm[30 * 1280 * 4];
	uint8_t fbuf[144000], copy[144000];
	dv_audio_stream_t *stream;
	long pos, g;
	int k, c, ci;
	long used;
	int r;

	for (g = 0; g < total; g++)
		for (c = 0; c < 4; c++)
			pcm[g*4 + c] = sig_sample(c, g + 4242);
	for (k = 0; k < FRAMES; k++)
		task_make_dv(templ[k], 1, k);

	/* bulk run */
	stream = dv_audio_stream_new(1);
	TASK_CHECK(stream != NULL);
	pos = 0;
	for (k = 0; k < FRAMES; k++) {
		used = 0;
		memcpy(fbuf, templ[k], sizeof(fbuf));
		r = dv_audio_stream_feed(stream, pcm + pos*4, total - pos,
					 &used, fbuf, sizeof(fbuf));
		TASK_CHECK(r == 1);
		TASK_CHECK(used == 1280);
		pos += used;
		memcpy(stored[k], fbuf, sizeof(fbuf));
		/* PAL budget is fixed: af_size == 1280 - 1264 */
		TASK_CHECK((fbuf[AS0_OFF + 1] & 0x3f) == 1280 - 1264);
		TASK_CHECK((fbuf[AS1_PAL + 1] & 0x3f) == 1280 - 1264);
	}
	TASK_CHECK(pos == total);
	dv_audio_stream_free(stream);

	/* chunked run must reproduce the identical DV bytes */
	stream = dv_audio_stream_new(1);
	TASK_CHECK(stream != NULL);
	pos = 0;
	ci = 0;
	for (k = 0; k < FRAMES; k++) {
		memcpy(fbuf, templ[k], sizeof(fbuf));
		for (;;) {
			long offer = chunks[ci++ % 3];
			if (offer > total - pos)
				offer = total - pos;
			used = 0;
			r = dv_audio_stream_feed(stream, pcm + pos*4, offer,
						 &used, fbuf, sizeof(fbuf));
			TASK_CHECK(r == 0 || r == 1);
			pos += used;
			if (r == 1)
				break;
		}
		TASK_CHECK(memcmp(fbuf, stored[k], sizeof(fbuf)) == 0);
	}
	TASK_CHECK(pos == total);
	dv_audio_stream_free(stream);

	/* a waiting stream leaves the caller's frame completely alone,
	 * and zero input neither commits nor pads */
	stream = dv_audio_stream_new(1);
	TASK_CHECK(stream != NULL);
	task_make_dv(fbuf, 1, 0);
	memcpy(copy, fbuf, sizeof(fbuf));
	used = 999;
	r = dv_audio_stream_feed(stream, pcm, 100, &used, fbuf, sizeof(fbuf));
	TASK_CHECK(r == 0);
	TASK_CHECK(used == 100);
	TASK_CHECK(memcmp(fbuf, copy, sizeof(fbuf)) == 0);
	used = 999;
	r = dv_audio_stream_feed(stream, NULL, 0, &used, fbuf, sizeof(fbuf));
	TASK_CHECK(r == 0);
	TASK_CHECK(used == 0);
	TASK_CHECK(memcmp(fbuf, copy, sizeof(fbuf)) == 0);
	used = 999;
	r = dv_audio_stream_feed(stream, pcm + 100*4, 1180, &used,
				 fbuf, sizeof(fbuf));
	TASK_CHECK(r == 1);
	TASK_CHECK(used == 1180);
	dv_audio_stream_free(stream);

	/* the caller may hand in a different frame of the same standard
	 * while the stream waits; the pending audio belongs to whichever
	 * frame is committed next */
	{
		uint8_t expect[144000];
		int16_t planar[4][1280];
		int16_t *planes[4] =
			{ planar[0], planar[1], planar[2], planar[3] };
		dv_encoder_t *enc;
		int i;

		stream = dv_audio_stream_new(1);
		TASK_CHECK(stream != NULL);
		memcpy(fbuf, templ[0], sizeof(fbuf));
		used = 999;
		r = dv_audio_stream_feed(stream, pcm, 100, &used,
					 fbuf, sizeof(fbuf));
		TASK_CHECK(r == 0);
		TASK_CHECK(used == 100);
		memcpy(copy, templ[1], sizeof(copy)); /* swap frames */
		used = 999;
		r = dv_audio_stream_feed(stream, pcm + 100*4, 1180, &used,
					 copy, sizeof(copy));
		TASK_CHECK(r == 1);
		TASK_CHECK(used == 1180);
		/* the first frame was never written */
		TASK_CHECK(memcmp(fbuf, templ[0], sizeof(fbuf)) == 0);
		/* the second frame carries the audio, exactly as a
		 * direct encode of the same PCM into the same template */
		for (i = 0; i < 1280; i++)
			for (c = 0; c < 4; c++)
				planar[c][i] = pcm[i*4 + c];
		memcpy(expect, templ[1], sizeof(expect));
		enc = dv_encoder_new(0, 0, 0);
		TASK_CHECK(enc != NULL);
		enc->samples_this_frame = 1280;
		TASK_CHECK(dv_encode_full_audio(enc, planes, 4, 32000,
						expect) == 0);
		dv_encoder_free(enc);
		TASK_CHECK(memcmp(copy, expect, sizeof(copy)) == 0);
		dv_audio_stream_free(stream);
	}
	return 0;
}

static int case_stream_reset_reject(void)
{
	static int16_t pcm[4 * 2048];
	int16_t planar[4][1067];
	int16_t *planes[4] = { planar[0], planar[1], planar[2], planar[3] };
	uint8_t tmpl0[120000], f0[120000], ref[120000], other[144000];
	dv_audio_stream_t *stream;
	dv_encoder_t *enc;
	long used;
	long g;
	int c, i, r;

	for (g = 0; g < 2048; g++)
		for (c = 0; c < 4; c++)
			pcm[g*4 + c] = sig_sample(c, g + 99);

	/* constructor validation */
	TASK_CHECK(dv_audio_stream_new(2) == NULL);
	TASK_CHECK(dv_audio_stream_new(-1) == NULL);
	dv_audio_stream_free(NULL);  /* must be safe */
	dv_audio_stream_reset(NULL); /* must be safe */

	stream = dv_audio_stream_new(0);
	TASK_CHECK(stream != NULL);

	/* one template instance shared by every comparison below */
	task_make_dv(tmpl0, 0, 0);

	/* partial fill, then a batch of invalid calls */
	memcpy(f0, tmpl0, sizeof(f0));
	used = 999;
	r = dv_audio_stream_feed(stream, pcm, 500, &used, f0, sizeof(f0));
	TASK_CHECK(r == 0);
	TASK_CHECK(used == 500);

	used = 999;
	TASK_CHECK(dv_audio_stream_feed(stream, pcm, -1, &used,
					f0, sizeof(f0)) == -1);
	TASK_CHECK(used == 0);
	used = 999;
	TASK_CHECK(dv_audio_stream_feed(stream, NULL, 10, &used,
					f0, sizeof(f0)) == -1);
	TASK_CHECK(used == 0);
	TASK_CHECK(dv_audio_stream_feed(stream, pcm, 10, NULL,
					f0, sizeof(f0)) == -1);
	used = 999;
	TASK_CHECK(dv_audio_stream_feed(stream, pcm, 10, &used,
					NULL, sizeof(f0)) == -1);
	TASK_CHECK(used == 0);
	used = 999;
	TASK_CHECK(dv_audio_stream_feed(stream, pcm, 10, &used,
					f0, sizeof(f0) - 1) == -1);
	TASK_CHECK(used == 0);
	used = 999;
	TASK_CHECK(dv_audio_stream_feed(stream, pcm, 10, &used,
					f0, 144000) == -1);
	TASK_CHECK(used == 0);
	used = 999;
	TASK_CHECK(dv_audio_stream_feed(NULL, pcm, 10, &used,
					f0, sizeof(f0)) == -1);
	TASK_CHECK(used == 0);
	/* standard mismatch: PAL-sized frame on an NTSC stream */
	task_make_dv(other, 1, 0);
	used = 999;
	TASK_CHECK(dv_audio_stream_feed(stream, pcm, 10, &used,
					other, sizeof(other)) == -1);
	TASK_CHECK(used == 0);
	/* standard mismatch: NTSC-sized frame carrying a PAL marker */
	memcpy(other, f0, sizeof(f0));
	other[3] |= 0x80;
	used = 999;
	TASK_CHECK(dv_audio_stream_feed(stream, pcm, 10, &used,
					other, sizeof(f0)) == -1);
	TASK_CHECK(used == 0);

	/* the stream survived all rejections: frame k=0 completes with
	 * exactly the remaining 1067 - 500 samples */
	used = 999;
	r = dv_audio_stream_feed(stream, pcm + 500*4, 567, &used,
				 f0, sizeof(f0));
	TASK_CHECK(r == 1);
	TASK_CHECK(used == 567);

	/* reference: the same 1067 samples muxed directly into a copy
	 * of the same template */
	memcpy(ref, tmpl0, sizeof(ref));
	for (i = 0; i < 1067; i++)
		for (c = 0; c < 4; c++)
			planar[c][i] = pcm[i*4 + c];
	enc = dv_encoder_new(0, 0, 0);
	TASK_CHECK(enc != NULL);
	enc->samples_this_frame = 1067;
	TASK_CHECK(dv_encode_full_audio(enc, planes, 4, 32000, ref) == 0);
	dv_encoder_free(enc);
	TASK_CHECK(memcmp(f0, ref, sizeof(f0)) == 0);

	/* reset discards a pending tail and restarts the frame counter */
	used = 999;
	r = dv_audio_stream_feed(stream, pcm, 100, &used, f0, sizeof(f0));
	TASK_CHECK(r == 0);
	TASK_CHECK(used == 100);
	dv_audio_stream_reset(stream);
	memcpy(f0, tmpl0, sizeof(f0));
	used = 999;
	r = dv_audio_stream_feed(stream, pcm, 1067, &used, f0, sizeof(f0));
	TASK_CHECK(r == 1);
	TASK_CHECK(used == 1067); /* budget of k=0 again, not 1068-100 */
	TASK_CHECK(memcmp(f0, ref, sizeof(f0)) == 0);
	dv_audio_stream_free(stream);

	/* PAL stream rejects a PAL-sized frame with an NTSC marker */
	stream = dv_audio_stream_new(1);
	TASK_CHECK(stream != NULL);
	task_make_dv(other, 1, 0);
	other[3] &= 0x7f;
	used = 999;
	TASK_CHECK(dv_audio_stream_feed(stream, pcm, 1, &used,
					other, sizeof(other)) == -1);
	TASK_CHECK(used == 0);
	dv_audio_stream_free(stream);
	return 0;
}

/* ===========================================================================
 */

const struct task_case task_cases[] = {
	{ "companding", "reference_curve",   case_companding_curve   },
	{ "companding", "exhaustive_sweep",  case_companding_sweep   },
	{ "layout",     "pal_min",           case_layout_pal_min     },
	{ "layout",     "pal",               case_layout_pal         },
	{ "layout",     "ntsc_min",          case_layout_ntsc_min    },
	{ "layout",     "ntsc",              case_layout_ntsc        },
	{ "layout",     "reject_invalid",    case_layout_reject      },
	{ "stream",     "ntsc_bulk_vs_chunked", case_stream_ntsc     },
	{ "stream",     "pal_budget_and_wait",  case_stream_pal      },
	{ "stream",     "reset_and_reject",     case_stream_reset_reject },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
