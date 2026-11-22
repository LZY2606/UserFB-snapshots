/*
   Regression for the measured bit-grid decoder path and the
   clocked (grid-locked) renderer.

   The LTC waveforms and the ten-byte labels here are produced by
   hand (an independent biphase-mark builder), never by libltc's
   encoder, so the library cannot be checked against itself.
*/

#include "ltc.h"
#include "task_support/check.h"

#include <stdlib.h>
#include <string.h>

#define NFRAMES 8
#define MAX_SAMPLES 40000
#define LOW 38
#define HIGH 218

/* ------------------------------------------------------------------ */
/* Independent helpers                                                 */
/* ------------------------------------------------------------------ */

/* bit b of a hand-written ten-byte label, LSB of byte 0 first */
static unsigned char raw_bit(const unsigned char raw[10], int b) {
	return (raw[b >> 3] >> (b & 7)) & 1;
}

/* bit b of a decoded LTCFrame, read through the named fields.
 * Written as an if-chain (no grouped case labels) to stay strictly
 * on the documented bitfield semantics regardless of layout.
 */
static unsigned char field_bit(const LTCFrame *f, int b) {
	if (b <= 3)  return (f->frame_units >> (b - 0)) & 1;
	if (b <= 7)  return (f->user1 >> (b - 4)) & 1;
	if (b <= 9)  return (f->frame_tens >> (b - 8)) & 1;
	if (b == 10) return f->dfbit & 1;
	if (b == 11) return f->col_frame & 1;
	if (b <= 15) return (f->user2 >> (b - 12)) & 1;
	if (b <= 19) return (f->secs_units >> (b - 16)) & 1;
	if (b <= 23) return (f->user3 >> (b - 20)) & 1;
	if (b <= 26) return (f->secs_tens >> (b - 24)) & 1;
	if (b == 27) return f->biphase_mark_phase_correction & 1;
	if (b <= 31) return (f->user4 >> (b - 28)) & 1;
	if (b <= 35) return (f->mins_units >> (b - 32)) & 1;
	if (b <= 39) return (f->user5 >> (b - 36)) & 1;
	if (b <= 42) return (f->mins_tens >> (b - 40)) & 1;
	if (b == 43) return f->binary_group_flag_bit0 & 1;
	if (b <= 47) return (f->user6 >> (b - 44)) & 1;
	if (b <= 51) return (f->hours_units >> (b - 48)) & 1;
	if (b <= 55) return (f->user7 >> (b - 52)) & 1;
	if (b <= 57) return (f->hours_tens >> (b - 56)) & 1;
	if (b == 58) return f->binary_group_flag_bit1 & 1;
	if (b == 59) return f->binary_group_flag_bit2 & 1;
	if (b <= 63) return (f->user8 >> (b - 60)) & 1;
	return 0;
}

/* non-uniform but smooth widths: a slow triangle over 20..30 samples
 * (reaching a wide spread after lock-in), every adjacent pair -
 * including the frame wrap - differing by exactly 2. Staying inside
 * the decoder tracker's reliable range keeps the label decode exact
 * while the measured grid still captures every width variation.
 */
static void make_widths(int w[80]) {
	int t, b;
	for (b = 0; b < 80; ++b) {
		t = b % 10;
		w[b] = 20 + 2 * (t <= 5 ? t : 10 - t);
	}
}

/* constant tables at the two extremes of the allowed 16..32 range */
static void make_const_widths(int w[80], int width) {
	int b;
	for (b = 0; b < 80; ++b) w[b] = width;
}

/* build a canonical, even-parity, hand-written label for frame tag */
static void make_label(unsigned char raw[10], int tag) {
	static const unsigned char tmpl[10] =
		{0xa0,0x50,0x40,0x30,0x20,0x10,0x00,0xb0,0xfc,0xbf};
	int b, ones;
	memcpy(raw, tmpl, 10);
	raw[0] = (unsigned char)((raw[0] & 0xf0) | (tag & 0x0f));
	raw[3] &= (unsigned char)~(1 << 3); /* clear parity bit 27 */
	ones = 0;
	for (b = 0; b < 80; ++b) ones += raw_bit(raw, b);
	raw[3] |= (unsigned char)((ones & 1) << 3); /* even total number of ones */
}

/* independent biphase-mark builder. records the true start edge of
 * every bit and the exclusive end in starts[tag][0..80]. polarity 1
 * inverts the physical high/low levels.
 */
static size_t build_bmc(unsigned char *audio,
		unsigned char raws[][10], const int w[80], int polarity,
		size_t starts[][81])
{
	size_t total = 0;
	int level = 0, f, b, j;
	for (f = 0; f < NFRAMES; ++f) {
		for (b = 0; b < 80; ++b) {
			int width = w[b];
			int one = raw_bit(raws[f], b);
			starts[f][b] = total;
			level = !level; /* edge at every bit start */
			for (j = 0; j < width; ++j) {
				if (one && j == width / 2) level = !level;
				audio[total++] = (unsigned char)
					(polarity ? (level ? LOW : HIGH) : (level ? HIGH : LOW));
			}
		}
	}
	for (f = 0; f < NFRAMES; ++f)
		starts[f][80] = (f < NFRAMES - 1) ? starts[f + 1][0] : total;
	/* one trailing edge so the final tag can terminate */
	level = !level;
	for (j = 0; j < 26; ++j)
		audio[total++] = (unsigned char)
			(polarity ? (level ? LOW : HIGH) : (level ? HIGH : LOW));
	return total;
}

/* true time-ordered edge k (0..80) of a tag inside the fed stream */
static ltc_off_t ref_edge(const size_t starts[][81], size_t total,
		int tag, int k, int reverse, ltc_off_t origin)
{
	if (!reverse) return origin + (ltc_off_t)starts[tag][k];
	return origin + (ltc_off_t)(total - starts[tag][80 - k]);
}

static void feed(LTCDecoder *d, const unsigned char *stream, size_t total,
		ltc_off_t origin, size_t chunk)
{
	size_t at = 0;
	while (at < total) {
		size_t n = total - at;
		if (n > chunk) n = chunk;
		ltc_decoder_write(d, (ltcsnd_sample_t *)(void *)(stream + at), n,
				origin + (ltc_off_t)at);
		at += n;
	}
}

/* the fixed forward sync word, bits 64..79 */
static const unsigned char SYNC[16] = {0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1};

/* ------------------------------------------------------------------ */
/* measured_grid group                                                 */
/* ------------------------------------------------------------------ */

static int check_grids(const int w[80], int reverse, int polarity,
		size_t chunk, ltc_off_t origin) {
	static unsigned char raws[NFRAMES][10];
	static unsigned char audio[MAX_SAMPLES], stream[MAX_SAMPLES];
	static size_t starts[NFRAMES][81];
	int f, internal = 0, total_frames = 0;
	size_t total, i;

	for (f = 0; f < NFRAMES; ++f) make_label(raws[f], f);
	total = build_bmc(audio, raws, w, polarity, starts);
	if (reverse)
		for (i = 0; i < total; ++i) stream[i] = audio[total - 1 - i];
	else
		memcpy(stream, audio, total);

	LTCDecoder *d = ltc_decoder_create(1920, 32);
	TASK_CHECK(d != NULL);
	feed(d, stream, total, origin, chunk);

	LTCFrameExt fe;
	LTCFrameClock clk;
	while (ltc_decoder_read_clocked(d, &fe, &clk) == 1) {
		int tag = fe.ltc.frame_units;
		int k;
		TASK_CHECK(tag >= 0 && tag < NFRAMES);
		TASK_CHECK(reverse ? fe.reverse != 0 : fe.reverse == 0);
		++total_frames;

		/* internal frames exclude the first and last tag only; these
		 * are the ones required to carry an exact label and grid.
		 * Boundary frames may still be settling after acquisition.
		 */
		if (tag >= 1 && tag <= NFRAMES - 2) {
			/* the 64 payload bits must match the hand-written bytes */
			for (k = 0; k < 64; ++k)
				TASK_CHECK(field_bit(&fe.ltc, k) == raw_bit(raws[tag], k));
			/* the 16 sync bits decode to a valid representation */
			TASK_CHECK(fe.ltc.sync_word == 0xBFFC || fe.ltc.sync_word == 0xFCBF);

			ltc_off_t e0 = ref_edge(starts, total, tag, 0, reverse, origin);
			TASK_CHECK(clk.valid == 1);
			TASK_CHECK(clk.start_high == (stream[(size_t)(e0 - origin)] >= 128 ? 1 : 0));
			for (k = 0; k <= 80; ++k)
				TASK_CHECK(clk.edge[k] ==
					ref_edge(starts, total, tag, k, reverse, origin));
			++internal;
		}
	}
	TASK_CHECK(internal == NFRAMES - 2);
	TASK_CHECK(total_frames >= NFRAMES - 2);
	ltc_decoder_free(d);
	return 0;
}

static int case_grid_forward(void) {
	int w[80];
	make_widths(w);
	int rc = check_grids(w, 0, 0, 317, 0);
	if (rc) return rc;
	return check_grids(w, 0, 0, 1, ((ltc_off_t)1 << 33) + 317);
}

static int case_grid_reverse_inverted(void) {
	int w[80];
	make_widths(w);
	int rc = check_grids(w, 1, 1, 1025, 0);
	if (rc) return rc;
	return check_grids(w, 1, 0, 1, ((ltc_off_t)1 << 33) + 7);
}

static int case_grid_width_extremes(void) {
	/* uneven table across chunk sizes, plus the two allowed extreme
	 * constant widths, including polarity reversal and large offset.
	 */
	int w[80];
	make_widths(w);
	int rc = check_grids(w, 0, 1, 1, 0);
	if (rc) return rc;
	rc = check_grids(w, 0, 0, 1025, 0);
	if (rc) return rc;
	rc = check_grids(w, 1, 1, 317, ((ltc_off_t)1 << 33) + 11);
	if (rc) return rc;
	make_const_widths(w, 16);
	rc = check_grids(w, 0, 0, 317, 0);
	if (rc) return rc;
	make_const_widths(w, 32);
	return check_grids(w, 1, 1, 1025, ((ltc_off_t)1 << 33) + 13);
}

static int case_history_insufficient(void) {
	/* a single frame can never accumulate 81 observed edges: it must
	 * still be returned, with valid=0 and every other field zeroed.
	 */
	unsigned char raw[10];
	static unsigned char audio[4000];
	int w[80], b, j, level = 0;
	size_t total = 0;
	make_widths(w);
	make_label(raw, 0);
	for (b = 0; b < 80; ++b) {
		int width = w[b], one = raw_bit(raw, b);
		level = !level;
		for (j = 0; j < width; ++j) {
			if (one && j == width / 2) level = !level;
			audio[total++] = (unsigned char)(level ? HIGH : LOW);
		}
	}
	level = !level;
	for (j = 0; j < 26; ++j) audio[total++] = (unsigned char)(level ? HIGH : LOW);

	LTCDecoder *d = ltc_decoder_create(1920, 8);
	TASK_CHECK(d != NULL);
	ltc_decoder_write(d, audio, total, 0);
	LTCFrameExt fe;
	LTCFrameClock clk;
	TASK_CHECK(ltc_decoder_read_clocked(d, &fe, &clk) == 1);
	TASK_CHECK(clk.valid == 0);
	TASK_CHECK(clk.start_high == 0);
	for (b = 0; b <= 80; ++b) TASK_CHECK(clk.edge[b] == 0);
	TASK_CHECK(ltc_decoder_read_clocked(d, &fe, &clk) == 0);
	ltc_decoder_free(d);
	return 0;
}

/* ------------------------------------------------------------------ */
/* relabel group                                                       */
/* ------------------------------------------------------------------ */

static void new_label(LTCFrame *nl, int tag) {
	memset(nl, 0, sizeof(*nl));
	nl->frame_units = (unsigned)((7 - tag) % 10);
	nl->frame_tens = 2;
	nl->secs_units = 3;
	nl->secs_tens = 4;
	nl->mins_units = 5;
	nl->mins_tens = 1;
	nl->hours_units = 9;
	nl->hours_tens = 1;
	nl->user1 = 0xA; nl->user2 = 0xB; nl->user3 = 0xC; nl->user4 = 0xD;
	nl->user5 = 0xE; nl->user6 = 0xF; nl->user7 = 0x2; nl->user8 = 0x7;
	nl->sync_word = 0xBFFC;
}

/* independent expected logical bit of a rendered new label, including
 * the fixed sync word and an independently computed even parity bit.
 */
static unsigned char expected_bit(const LTCFrame *nl, int b) {
	if (b >= 64) return SYNC[b - 64];
	if (b == 27) {
		int ones = 0, k;
		for (k = 0; k < 64; ++k) {
			if (k == 27) continue;
			ones += field_bit(nl, k);
		}
		for (k = 0; k < 16; ++k) ones += SYNC[k];
		return (unsigned char)(ones & 1);
	}
	return field_bit(nl, b);
}

static int structural_check(const LTCFrame *nl, const LTCFrameClock *clk,
		const unsigned char *out, size_t len, int reverse)
{
	unsigned char entry = clk->start_high ? 1 : 0;
	int b;
	TASK_CHECK(len == (size_t)(clk->edge[80] - clk->edge[0]));
	for (b = 0; b < 80; ++b) {
		size_t s0 = (size_t)(clk->edge[b] - clk->edge[0]);
		size_t s1 = (size_t)(clk->edge[b + 1] - clk->edge[0]);
		int width = (int)(s1 - s0);
		int lb = reverse ? (79 - b) : b;
		unsigned char val = expected_bit(nl, lb);
		unsigned char lvl = entry;
		size_t x;
		TASK_CHECK(out[s0] == (lvl ? HIGH : LOW));
		if (b > 0) TASK_CHECK(out[s0] != out[s0 - 1]); /* boundary toggle */
		for (x = s0; x < s1; ++x) {
			unsigned char cur = (val && (x - s0) >= (size_t)(width / 2))
				? (unsigned char)(lvl ^ 1) : lvl;
			TASK_CHECK(out[x] == (cur ? HIGH : LOW));
		}
		entry = (unsigned char)(entry ^ 1 ^ val);
	}
	return 0;
}

static int case_relabel_forward(void) {
	static unsigned char raws[NFRAMES][10];
	static unsigned char audio[MAX_SAMPLES], spliced[MAX_SAMPLES], out[MAX_SAMPLES];
	static size_t starts[NFRAMES][81];
	int w[80], f, relabeled[NFRAMES] = {0}, tested = 0;
	size_t total;
	make_widths(w);
	for (f = 0; f < NFRAMES; ++f) make_label(raws[f], f);
	total = build_bmc(audio, raws, w, 0, starts);
	memcpy(spliced, audio, total);

	LTCDecoder *d = ltc_decoder_create(1920, 32);
	TASK_CHECK(d != NULL);
	feed(d, audio, total, 0, 317);

	LTCFrameExt fe;
	LTCFrameClock clk;
	while (ltc_decoder_read_clocked(d, &fe, &clk) == 1) {
		int tag = fe.ltc.frame_units;
		size_t len;
		LTCFrame nl, saved;
		if (!clk.valid) continue;
		new_label(&nl, tag);
		saved = nl;
		len = (size_t)(clk.edge[80] - clk.edge[0]);
		TASK_CHECK(ltc_encoder_render_clocked(&nl, &clk, LTC_TV_525_60, 0,
				LOW, HIGH, out, len) == 0);
		/* caller's object is untouched */
		TASK_CHECK(memcmp(&nl, &saved, sizeof(LTCFrame)) == 0);
		TASK_CHECK(structural_check(&nl, &clk, out, len, 0) == 0);
		/* entering polarity preserved against the source recording */
		TASK_CHECK(out[0] == audio[(size_t)clk.edge[0]]);
		memcpy(spliced + (size_t)clk.edge[0], out, len);
		relabeled[tag] = 1;
		++tested;
	}
	TASK_CHECK(tested == NFRAMES - 1); /* tag 0 lacks history */
	ltc_decoder_free(d);

	/* production decoder reads the spliced stream back */
	LTCDecoder *d2 = ltc_decoder_create(1920, 32);
	TASK_CHECK(d2 != NULL);
	feed(d2, spliced, total, 0, 1025);
	LTCFrameExt back;
	int order = 0;
	while (ltc_decoder_read(d2, &back) == 1) {
		int orig = order++;
		if (relabeled[orig]) {
			TASK_CHECK(back.ltc.frame_units == (unsigned)((7 - orig) % 10));
			TASK_CHECK(back.ltc.user1 == 0xA && back.ltc.user8 == 0x7);
			TASK_CHECK(back.ltc.secs_units == 3 && back.ltc.hours_units == 9);
		} else {
			/* untouched neighbor keeps its original label */
			TASK_CHECK(back.ltc.frame_units == (unsigned)orig);
			TASK_CHECK(back.ltc.user1 == 10 && back.ltc.user8 == 11);
		}
	}
	TASK_CHECK(order == NFRAMES);
	ltc_decoder_free(d2);
	return 0;
}

static int case_relabel_reverse(void) {
	static unsigned char raws[NFRAMES][10];
	static unsigned char audio[MAX_SAMPLES], rev[MAX_SAMPLES], spliced[MAX_SAMPLES], out[MAX_SAMPLES];
	static size_t starts[NFRAMES][81];
	const ltc_off_t origin = ((ltc_off_t)1 << 33) + 317;
	int w[80], f, done[NFRAMES] = {0}, tested = 0, errs_seen = 0;
	size_t total, i;
	make_widths(w);
	for (f = 0; f < NFRAMES; ++f) make_label(raws[f], f);
	total = build_bmc(audio, raws, w, 1, starts); /* inverted */
	for (i = 0; i < total; ++i) rev[i] = audio[total - 1 - i]; /* reversed */
	memcpy(spliced, rev, total);

	LTCDecoder *d = ltc_decoder_create(1920, 32);
	TASK_CHECK(d != NULL);
	feed(d, rev, total, origin, 1);

	LTCFrameExt fe;
	LTCFrameClock clk;
	while (ltc_decoder_read_clocked(d, &fe, &clk) == 1) {
		int tag = fe.ltc.frame_units;
		size_t local0, len;
		LTCFrame nl;
		if (!clk.valid || done[tag]) continue;
		done[tag] = 1;
		TASK_CHECK(fe.reverse != 0);
		memset(&nl, 0, sizeof(nl));
		nl.frame_units = (unsigned)((9 - tag) % 10);
		nl.frame_tens = 3;
		nl.secs_units = 2;
		nl.user1 = 0x5;
		nl.user8 = 0xC;
		nl.sync_word = 0xFCBF; /* reverse-decoded representation is allowed */
		local0 = (size_t)(clk.edge[0] - origin);
		len = (size_t)(clk.edge[80] - clk.edge[0]);
		TASK_CHECK(ltc_encoder_render_clocked(&nl, &clk, LTC_TV_525_60, 1,
				LOW, HIGH, out, len) == 0);
		TASK_CHECK(structural_check(&nl, &clk, out, len, 1) == 0);
		TASK_CHECK(out[0] == rev[local0]); /* boundary polarity continuity */
		memcpy(spliced + local0, out, len);
		++tested;
	}
	TASK_CHECK(tested >= NFRAMES - 2);
	ltc_decoder_free(d);

	LTCDecoder *d2 = ltc_decoder_create(1920, 32);
	TASK_CHECK(d2 != NULL);
	feed(d2, spliced, total, origin, 317);
	LTCFrameExt back;
	int ok = 0, frames = 0;
	while (ltc_decoder_read(d2, &back) == 1) {
		++frames;
		if (back.ltc.frame_tens == 3 && back.ltc.secs_units == 2
				&& back.ltc.user1 == 5 && back.ltc.user8 == 0xC)
			++ok;
	}
	TASK_CHECK(frames >= tested);
	TASK_CHECK(ok == tested);
	(void)errs_seen;
	ltc_decoder_free(d2);
	return 0;
}

static int case_render_rejects(void) {
	LTCFrameClock c, bad;
	LTCFrame f, fb;
	static unsigned char buf[MAX_SAMPLES];
	const unsigned char SENT = 0x5A;
	size_t len, i;
	int dirty;

	memset(&c, 0, sizeof(c));
	c.valid = 1;
	c.start_high = 0;
	{
		ltc_off_t p = 0;
		int k;
		for (k = 0; k < 80; ++k) { c.edge[k] = p; p += 22; }
		c.edge[80] = p;
	}
	len = (size_t)(c.edge[80] - c.edge[0]);
	memset(&f, 0, sizeof(f));
	f.sync_word = 0xBFFC;

	#define EXPECT_REJECT(name, call) do { \
		memset(buf, SENT, sizeof(buf)); \
		int _r = (call); \
		dirty = 0; \
		for (i = 0; i < sizeof(buf); ++i) if (buf[i] != SENT) { dirty = 1; break; } \
		TASK_CHECK(_r == -1); \
		TASK_CHECK(!dirty); \
		(void)name; \
	} while (0)

	EXPECT_REJECT("null label",
		ltc_encoder_render_clocked(NULL, &c, LTC_TV_525_60, 0, LOW, HIGH, buf, len));
	EXPECT_REJECT("null clock",
		ltc_encoder_render_clocked(&f, NULL, LTC_TV_525_60, 0, LOW, HIGH, buf, len));
	EXPECT_REJECT("null output",
		ltc_encoder_render_clocked(&f, &c, LTC_TV_525_60, 0, LOW, HIGH, NULL, len));

	bad = c; bad.valid = 0;
	EXPECT_REJECT("invalid clock",
		ltc_encoder_render_clocked(&f, &bad, LTC_TV_525_60, 0, LOW, HIGH, buf, len));
	bad = c; bad.start_high = 2;
	EXPECT_REJECT("bad start_high",
		ltc_encoder_render_clocked(&f, &bad, LTC_TV_525_60, 0, LOW, HIGH, buf, len));
	EXPECT_REJECT("bad reverse",
		ltc_encoder_render_clocked(&f, &c, LTC_TV_525_60, 2, LOW, HIGH, buf, len));
	EXPECT_REJECT("bad standard",
		ltc_encoder_render_clocked(&f, &c, (enum LTC_TV_STANDARD)99, 0, LOW, HIGH, buf, len));
	EXPECT_REJECT("levels",
		ltc_encoder_render_clocked(&f, &c, LTC_TV_525_60, 0, 200, 210, buf, len));
	EXPECT_REJECT("low not below center",
		ltc_encoder_render_clocked(&f, &c, LTC_TV_525_60, 0, 128, HIGH, buf, len));

	fb = f; fb.sync_word = 0x1234;
	EXPECT_REJECT("bad sync word",
		ltc_encoder_render_clocked(&fb, &c, LTC_TV_525_60, 0, LOW, HIGH, buf, len));

	bad = c; bad.edge[10] = bad.edge[9];
	EXPECT_REJECT("non increasing",
		ltc_encoder_render_clocked(&f, &bad, LTC_TV_525_60, 0, LOW, HIGH, buf, len));
	bad = c; bad.edge[40] = bad.edge[39] + 1;
	EXPECT_REJECT("one sample bit",
		ltc_encoder_render_clocked(&f, &bad, LTC_TV_525_60, 0, LOW, HIGH, buf, len));
	bad = c; bad.edge[0] = -5;
	EXPECT_REJECT("negative edge",
		ltc_encoder_render_clocked(&f, &bad, LTC_TV_525_60, 0, LOW, HIGH, buf, len));
	EXPECT_REJECT("short buffer",
		ltc_encoder_render_clocked(&f, &c, LTC_TV_525_60, 0, LOW, HIGH, buf, len - 1));
	EXPECT_REJECT("long buffer",
		ltc_encoder_render_clocked(&f, &c, LTC_TV_525_60, 0, LOW, HIGH, buf, len + 1));

	/* absolute positions above 2^33 must still work, spans checked */
	bad = c;
	{
		ltc_off_t shift = ((ltc_off_t)1 << 33) + 1234567;
		int k;
		for (k = 0; k <= 80; ++k) bad.edge[k] += shift;
	}
	memset(buf, SENT, sizeof(buf));
	TASK_CHECK(ltc_encoder_render_clocked(&f, &bad, LTC_TV_625_50, 0,
			LOW, HIGH, buf, len) == 0);
	TASK_CHECK(buf[0] != SENT && buf[len - 1] != SENT && buf[len] == SENT);

	/* a deliberately wrong parity bit is corrected on the internal copy */
	{
		LTCFrame p;
		new_label(&p, 3);
		p.biphase_mark_phase_correction = (unsigned char)(!p.biphase_mark_phase_correction);
		memset(buf, SENT, sizeof(buf));
		TASK_CHECK(ltc_encoder_render_clocked(&p, &c, LTC_TV_525_60, 0,
				LOW, HIGH, buf, len) == 0);
	}
	#undef EXPECT_REJECT
	return 0;
}

/* ------------------------------------------------------------------ */
/* queue_pairing group                                                 */
/* ------------------------------------------------------------------ */

static int case_alternate_wrap(void) {
	static unsigned char raws[NFRAMES][10];
	static unsigned char audio[MAX_SAMPLES];
	static size_t starts[NFRAMES][81];
	int w[80], f, seen = 0, use_clock = 0;
	ltc_off_t fed = 0;
	size_t total;
	make_widths(w);
	for (f = 0; f < NFRAMES; ++f) make_label(raws[f], f);
	total = build_bmc(audio, raws, w, 0, starts);

	/* queue of three forces both offsets to wrap; drain between feeds */
	LTCDecoder *d = ltc_decoder_create(1920, 3);
	TASK_CHECK(d != NULL);
	while (fed < (ltc_off_t)total) {
		size_t step = 1500;
		if ((size_t)(total - fed) < step) step = (size_t)(total - fed);
		ltc_decoder_write(d, audio + fed, step, fed);
		fed += (ltc_off_t)step;
		for (;;) {
			int r;
			if (use_clock) {
				LTCFrameExt b;
				LTCFrameClock cb;
				r = ltc_decoder_read_clocked(d, &b, &cb);
				if (r == 1) {
					int k;
					TASK_CHECK(b.ltc.frame_units == (unsigned)seen);
					if (cb.valid)
						for (k = 0; k < 80; ++k)
							TASK_CHECK(cb.edge[k + 1] > cb.edge[k]);
					++seen;
				}
			} else {
				LTCFrameExt a;
				r = ltc_decoder_read(d, &a);
				if (r == 1) {
					TASK_CHECK(a.ltc.frame_units == (unsigned)seen);
					++seen;
				}
			}
			if (r != 1) break;
			use_clock ^= 1;
		}
	}
	TASK_CHECK(seen == NFRAMES);

	/* empty clocked read returns 0 and leaves outputs untouched */
	{
		LTCFrameExt b;
		LTCFrameClock cb, zc;
		memset(&b, 0xAB, sizeof(b));
		memset(&cb, 0xCD, sizeof(cb));
		memset(&zc, 0xCD, sizeof(zc));
		TASK_CHECK(ltc_decoder_read_clocked(d, &b, &cb) == 0);
		TASK_CHECK(memcmp(&cb, &zc, sizeof(cb)) == 0);
		TASK_CHECK(ltc_decoder_read(d, &b) == 0);
	}
	ltc_decoder_free(d);
	return 0;
}

static int case_flush_half_frame(void) {
	static unsigned char raws[NFRAMES][10];
	static unsigned char audio[MAX_SAMPLES];
	static size_t starts[NFRAMES][81];
	int w[80], f, got = 0, valid = 0;
	size_t total, mid;
	make_widths(w);
	for (f = 0; f < NFRAMES; ++f) make_label(raws[f], f);
	total = build_bmc(audio, raws, w, 0, starts);
	mid = starts[3][40]; /* halfway through frame 3 */

	LTCDecoder *d = ltc_decoder_create(1920, 8);
	TASK_CHECK(d != NULL);
	ltc_decoder_write(d, audio, mid, 0);
	TASK_CHECK(ltc_decoder_queue_length(d) > 0);
	ltc_decoder_queue_flush(d); /* drops only completed outputs */
	TASK_CHECK(ltc_decoder_queue_length(d) == 0);
	/* edges still being collected survive: feed the rest */
	ltc_decoder_write(d, audio + mid, total - mid, (ltc_off_t)mid);

	LTCFrameExt fe;
	LTCFrameClock clk;
	while (ltc_decoder_read_clocked(d, &fe, &clk) == 1) {
		++got;
		if (clk.valid) {
			int k;
			for (k = 0; k < 80; ++k)
				TASK_CHECK(clk.edge[k + 1] > clk.edge[k]);
			++valid;
		}
	}
	/* frames 3..7 complete after the flush */
	TASK_CHECK(got >= NFRAMES - 3);
	TASK_CHECK(valid >= NFRAMES - 4);
	ltc_decoder_free(d);
	return 0;
}

static int case_clock_matches_frame(void) {
	/* every clock stays paired with its frame across the parallel
	 * queue slots, including after wrap-around.
	 */
	static unsigned char raws[NFRAMES][10];
	static unsigned char audio[MAX_SAMPLES];
	static size_t starts[NFRAMES][81];
	int w[80], f;
	size_t total;
	make_widths(w);
	for (f = 0; f < NFRAMES; ++f) make_label(raws[f], f);
	total = build_bmc(audio, raws, w, 0, starts);

	LTCDecoder *d = ltc_decoder_create(1920, 4);
	TASK_CHECK(d != NULL);
	feed(d, audio, total, 0, 777);
	LTCFrameExt fe;
	LTCFrameClock clk;
	while (ltc_decoder_read_clocked(d, &fe, &clk) == 1) {
		int tag = fe.ltc.frame_units;
		if (clk.valid) {
			TASK_CHECK(clk.edge[0] == (ltc_off_t)starts[tag][0]);
			TASK_CHECK(clk.edge[80] == (ltc_off_t)starts[tag][80]);
		}
	}
	ltc_decoder_free(d);
	return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{ "measured_grid", "forward_chunks",        case_grid_forward },
	{ "measured_grid", "reverse_inverted",      case_grid_reverse_inverted },
	{ "measured_grid", "width_extremes",        case_grid_width_extremes },
	{ "measured_grid", "history_insufficient",  case_history_insufficient },

	{ "relabel", "forward_splice_readback",     case_relabel_forward },
	{ "relabel", "reverse_splice_readback",     case_relabel_reverse },
	{ "relabel", "parameter_rejection",         case_render_rejects },

	{ "queue_pairing", "alternate_wrap",        case_alternate_wrap },
	{ "queue_pairing", "half_frame_flush",      case_flush_half_frame },
	{ "queue_pairing", "clock_frame_pairing",   case_clock_matches_frame },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
