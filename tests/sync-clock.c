/*
   tests/sync-clock.c

   Independent regression tests for libltc sample-position synchronization.

   The reference LTC signal in this file is built from scratch:
     - the ten raw LTC bytes are assembled from the bit layout documented in
       src/ltc.h (BCD nibbles, user bits, sync word), without calling the
       library encoder or the library timecode conversion helpers;
     - the biphase-mark audio waveform is generated from a known edge table:
       every bit starts with a level transition, a 1-bit adds a second
       transition at the half-bit position. Edge positions use cumulative
       rational positions so non-integer bit widths (44.1 kHz / 25 fps) do
       not truncate and drift.

   Decoder output (time fields, user bits, reverse flag, off_start/off_end
   and the 80 biphase tics) is then checked against that independent edge
   table. A conventional library encode/decode round-trip is kept only as a
   secondary cross-check; it can never replace the independent waveform.

   Public library behavior is not exercised through any private header.
*/

#include <ltc.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test parameters                                                     */
/* ------------------------------------------------------------------ */

#define FPS 25
#define NBITS LTC_FRAME_BIT_COUNT            /* 80 */
#define NFRAMES 7                            /* >= 3 so internal frames exist */
#define QUEUE_SMALL 3                        /* ring buffer that must wrap */
#define QUEUE_BIG 16

/* origin above 2^33: proves 64 bit posinfo accumulation survives */
#define ORIGIN ((ltc_off_t) 8589934592LL)    /* 2^33 */

/* logical two-level signal; every public entry point quantizes these
 * exactly to internal u8 levels 1 and 255:
 *   u8     : 1 / 255
 *   u16    : 0x0100 / 0xff00  (>> 8 -> 1 / 255)
 *   s16    : -32512 / +32512  (arithmetic >> 8 -> -127 / 127)
 *   float  : -1.f / +1.f      (128 +/- 127 -> 1 / 255)
 *   double : -1.0 / +1.0
 */
#define LEVEL_LO 1
#define LEVEL_HI 255

enum fmt { FMT_U8 = 0, FMT_U16, FMT_S16, FMT_FLOAT, FMT_DOUBLE, NFMT };

static const char *fmt_name[NFMT] = { "u8", "u16", "s16", "float", "double" };

/* ------------------------------------------------------------------ */
/* Assertion infrastructure                                            */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

static void check(const char *name, int ok, const char *detail) {
	++g_checks;
	if (!ok) {
		++g_failures;
		if (detail)
			printf("ASSERT_FAIL %-34s %s\n", name, detail);
		else
			printf("ASSERT_FAIL %s\n", name);
	}
}

static char detail_buf[256];
#define DETAIL(...) (snprintf(detail_buf, sizeof(detail_buf), __VA_ARGS__), detail_buf)

/* ------------------------------------------------------------------ */
/* Independent reference timecode and LTC bit stream                  */
/* ------------------------------------------------------------------ */

typedef struct {
	int hh, mm, ss, ff;
	unsigned char user[8];   /* user1..user8 nibbles */
} RefTC;

static RefTC ref_tc[NFRAMES];
static unsigned char ref_bits[NFRAMES][NBITS];

/* BCD field positions per src/ltc.h, little-endian bit ordering */
static const int bcd_lowbit[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
static const int bcd_width[8]  = { 4, 2, 4, 3, 4, 3, 4, 2 };
/* frame_units, frame_tens, secs_units, secs_tens,
 * mins_units, mins_tens, hours_units, hours_tens */

static void put_nibble(unsigned char *bits, int lowbit, int width, int value) {
	int k;
	for (k = 0; k < width; ++k)
		bits[lowbit + k] = (unsigned char) ((value >> k) & 1);
}

/* own BCD formatter: no library timecode helper involved */
static void build_reference_frames(void) {
	int j;
	for (j = 0; j < NFRAMES; ++j) {
		/* 01:02:03:04 counting forward in frame time, hand-rolled carry */
		long total = ((1L * 3600 + 2L * 60 + 3L) * FPS) + 4L + j;
		int ff = (int) (total % FPS);
		long sec = total / FPS;
		int ss = (int) (sec % 60);
		long minute = sec / 60;
		int mm = (int) (minute % 60);
		int hh = (int) ((minute / 60) % 24);

		RefTC *t = &ref_tc[j];
		t->hh = hh; t->mm = mm; t->ss = ss; t->ff = ff;

		unsigned char *bits = ref_bits[j];
		memset(bits, 0, NBITS);

		int digits[8] = { ff % 10, ff / 10, ss % 10, ss / 10,
		                  mm % 10, mm / 10, hh % 10, hh / 10 };
		int d;
		for (d = 0; d < 8; ++d)
			put_nibble(bits, bcd_lowbit[d], bcd_width[d], digits[d]);

		/* distinct user nibbles per frame, assigned by the ltc.h layout:
		 * user1 bits 4-7, user2 12-15, ... user8 60-63 */
		int u;
		for (u = 0; u < 8; ++u) {
			int nib = (0x3 + j * 0x5 + u * 0x7) & 0xF;
			t->user[u] = (unsigned char) nib;
			put_nibble(bits, 4 + u * 8, 4, nib);
		}

		/* sync word 0011 1111 1111 1101, MSB first on the wire */
		const char *sw = "0011111111111101";
		int b;
		for (b = 0; b < 16; ++b)
			bits[64 + b] = (unsigned char) (sw[b] == '1');

		/* 25 fps parity is bit 59; make the total number of ones even.
		 * Computed last so the sync-word ones are included. */
		int ones = 0;
		for (b = 0; b < NBITS; ++b)
			if (b != 59) ones += bits[b];
		bits[59] = (unsigned char) (ones & 1);
	}
}

/* ------------------------------------------------------------------ */
/* Edge table and independent biphase-mark waveform                    */
/* ------------------------------------------------------------------ */

typedef struct {
	int rate;
	long total_bits;            /* NFRAMES * 80 */
	long len;                   /* audio samples */
	long *edge;                 /* edge[b]: sample of global bit b start */
	long *half;                 /* half[b]: sample of bit b mid edge */
	unsigned char *audio;       /* canonical u8 two-level waveform */
} Wave;

static long ceil_div(long num, long den) {
	return (num + den - 1) / den;
}

static long edge_at(long global_bit, int rate) {
	/* D = fps * 80 = 2000 biphase clocks per second */
	return ceil_div(global_bit * (long) rate, 2000L);
}

static long half_at(long global_bit, int rate) {
	return ceil_div((2L * global_bit + 1L) * (long) rate, 4000L);
}

static Wave *wave_build(int rate, int reverse, int invert) {
	Wave *w = calloc(1, sizeof(Wave));
	w->rate = rate;
	w->total_bits = (long) NFRAMES * NBITS;
	w->edge = calloc((size_t) w->total_bits + 1, sizeof(long));
	w->half = calloc((size_t) w->total_bits, sizeof(long));
	w->len = (long) NFRAMES * rate / FPS + 1;
	w->audio = malloc((size_t) w->len);

	long b;
	for (b = 0; b <= w->total_bits; ++b)
		w->edge[b] = edge_at(b, rate);
	for (b = 0; b < w->total_bits; ++b)
		w->half[b] = half_at(b, rate);

	/* monotonic edge table: cumulative rational placement must not drift */
	for (b = 1; b <= w->total_bits; ++b)
		check("ref.edge_monotonic", w->edge[b] > w->edge[b - 1],
		      DETAIL("rate=%d bit=%ld", rate, b));

	/* merge start edges and one-bit half edges into one ordered event list */
	long cap = w->total_bits * 2 + 1;
	long *events = malloc((size_t) cap * sizeof(long));
	long ne = 0;
	long j;
	for (j = 0; j < NFRAMES; ++j) {
		int k;
		for (k = 0; k < NBITS; ++k) {
			long gb = j * NBITS + k;
			events[ne++] = w->edge[gb];
			if (ref_bits[j][k])
				events[ne++] = w->half[gb];
		}
	}
	events[ne++] = w->edge[w->total_bits];

	int level = 0;
	long ei = 0;
	long i;
	for (i = 0; i < w->len; ++i) {
		while (ei < ne && events[ei] == i) {
			level = !level;
			++ei;
		}
		w->audio[i] = (unsigned char) (level ? LEVEL_HI : LEVEL_LO);
	}
	check("ref.edge_events_consumed", ei == ne, DETAIL("rate=%d", rate));
	free(events);

	if (reverse || invert) {
		unsigned char *out = malloc((size_t) w->len);
		for (i = 0; i < w->len; ++i) {
			unsigned char v = reverse ? w->audio[w->len - 1 - i] : w->audio[i];
			out[i] = invert ? (unsigned char) (256 - v) : v;
		}
		free(w->audio);
		w->audio = out;
	}
	return w;
}

static void wave_free(Wave *w) {
	free(w->edge);
	free(w->half);
	free(w->audio);
	free(w);
}

/* ------------------------------------------------------------------ */
/* Decoded frame capture                                              */
/* ------------------------------------------------------------------ */

typedef struct {
	LTCFrameExt *item;
	int n;
} Capture;

static Capture capture_new(void) {
	Capture c;
	c.n = 0;
	c.item = calloc(QUEUE_BIG, sizeof(LTCFrameExt));
	return c;
}

static void capture_free(Capture *c) {
	free(c->item);
	c->item = NULL;
	c->n = 0;
}

static void drain(LTCDecoder *d, Capture *c) {
	while (c->n < QUEUE_BIG && ltc_decoder_read(d, &c->item[c->n]) == 1)
		++c->n;
}

/* feed through one of the five public entry points, in fixed-size chunks */
static void feed(LTCDecoder *d, enum fmt fmt, const unsigned char *canon,
                 long n, long chunk, ltc_off_t origin) {
	long off = 0;
	if (chunk <= 0)
		chunk = n;
	while (off < n) {
		long c = n - off < chunk ? n - off : chunk;
		ltc_off_t pos = origin + off;
		long k;
		switch (fmt) {
			case FMT_U8:
				ltc_decoder_write(d, (ltcsnd_sample_t *) (canon + off), (size_t) c, pos);
				break;
			case FMT_U16: {
				unsigned short *p = malloc((size_t) c * sizeof(*p));
				for (k = 0; k < c; ++k)
					p[k] = canon[off + k] == LEVEL_HI ? 0xff00 : 0x0100;
				ltc_decoder_write_u16(d, p, (size_t) c, pos);
				free(p);
				break;
			}
			case FMT_S16: {
				short *p = malloc((size_t) c * sizeof(*p));
				for (k = 0; k < c; ++k)
					p[k] = canon[off + k] == LEVEL_HI ? 32512 : -32512;
				ltc_decoder_write_s16(d, p, (size_t) c, pos);
				free(p);
				break;
			}
			case FMT_FLOAT: {
				float *p = malloc((size_t) c * sizeof(*p));
				for (k = 0; k < c; ++k)
					p[k] = canon[off + k] == LEVEL_HI ? 1.f : -1.f;
				ltc_decoder_write_float(d, p, (size_t) c, pos);
				free(p);
				break;
			}
			case FMT_DOUBLE: {
				double *p = malloc((size_t) c * sizeof(*p));
				for (k = 0; k < c; ++k)
					p[k] = canon[off + k] == LEVEL_HI ? 1.0 : -1.0;
				ltc_decoder_write_double(d, p, (size_t) c, pos);
				free(p);
				break;
			}
			default:
				break;
		}
		off += c;
	}
}

static Capture run_decode(const Wave *w, enum fmt fmt, long chunk,
                          ltc_off_t origin, int queue_len) {
	LTCDecoder *d = ltc_decoder_create(w->rate / FPS, queue_len);
	check("decoder.create", d != NULL, NULL);
	feed(d, fmt, w->audio, w->len, chunk, origin);
	Capture c = capture_new();
	drain(d, &c);
	ltc_decoder_free(d);
	return c;
}

/* ------------------------------------------------------------------ */
/* Field comparison against the independent reference                 */
/* ------------------------------------------------------------------ */

static int tc_matches(const LTCFrame *f, const RefTC *t) {
	return f->frame_units == (t->ff % 10) && f->frame_tens == (t->ff / 10)
	    && f->secs_units  == (t->ss % 10) && f->secs_tens  == (t->ss / 10)
	    && f->mins_units  == (t->mm % 10) && f->mins_tens  == (t->mm / 10)
	    && f->hours_units == (t->hh % 10) && f->hours_tens == (t->hh / 10);
}

static int user_matches(const LTCFrame *f, const RefTC *t) {
	return f->user1 == t->user[0] && f->user2 == t->user[1]
	    && f->user3 == t->user[2] && f->user4 == t->user[3]
	    && f->user5 == t->user[4] && f->user6 == t->user[5]
	    && f->user7 == t->user[6] && f->user8 == t->user[7];
}

static long frame_bitwidth(const Wave *w) {
	return ceil_div((long) w->rate, 2000L);   /* ceil(R / (fps*80)) */
}

/* ------------------------------------------------------------------ */
/* Suite 1+2: all five entries, chunk sizes, directions, polarities    */
/* ------------------------------------------------------------------ */

static const long chunks[] = { -1, 1, 317, 1023, 1024, 1025 };

static void verify_capture(const Wave *w, const Capture *c, int reverse,
                           int invert, enum fmt fmt, long chunk) {
	long bw = frame_bitwidth(w);
	long span_expect = (long) (w->rate / FPS);   /* audio samples per frame */
	int strict_48k = (w->rate == 48000);
	int expected_n = reverse ? (NFRAMES - 1) : NFRAMES;
	char tag[128];
	snprintf(tag, sizeof(tag), "rate=%d rev=%d inv=%d fmt=%s chunk=%ld",
	         w->rate, reverse, invert, fmt_name[fmt], chunk);

	check("matrix.frame_count", c->n == expected_n,
	      DETAIL("%s got=%d want=%d", tag, c->n, expected_n));

	int q;
	for (q = 0; q < c->n; ++q) {
		const LTCFrameExt *f = &c->item[q];
		/* decode order: forward 0..N-1, reverse N-1..1 (last capture lost) */
		int ref_idx = reverse ? (NFRAMES - 1 - q) : q;
		const RefTC *t = &ref_tc[ref_idx];
		int is_capture = (q == 0);

		check("matrix.identity", tc_matches(&f->ltc, t),
		      DETAIL("%s q=%d", tag, q));
		check("matrix.user_bits", user_matches(&f->ltc, t),
		      DETAIL("%s q=%d", tag, q));
		check("matrix.direction", reverse ? (f->reverse != 0) : (f->reverse == 0),
		      DETAIL("%s q=%d reverse=%d", tag, q, f->reverse));

		/* 64 bit origin must survive every entry path; the very first
		 * estimate may sit up to one frame width before the origin */
		check("matrix.origin_above_2pow33",
		      f->off_end >= ORIGIN
		      && f->off_start >= ORIGIN - (ltc_off_t) span_expect,
		      DETAIL("%s q=%d start=%lld", tag, q, (long long) f->off_start));

		/* frame boundaries from the independent edge table. After a
		 * whole-stream sample reversal, forward sample s maps to
		 * len-1-s, so frame k occupies [len-1-E_{k+1}, len-1-E_k] */
		long e0 = w->edge[ref_idx * NBITS];
		long e1 = w->edge[(ref_idx + 1) * NBITS];
		ltc_off_t want_start, want_end;
		long ds, de;
		double tsum = 0;
		int b;
		for (b = 0; b < NBITS; ++b)
			tsum += f->biphase_tics[b];
		long span = (long) (f->off_end - f->off_start + 1);
		long ref_width = e1 - e0;

		if (!reverse) {
			want_start = ORIGIN + e0;
			want_end = ORIGIN + e1 - 1;
		} else {
			/* the reversed sync word of the neighboring frame is detected
			 * first; the 16-bit position compensation is documented on
			 * LTCFrameExt. Internal reversed frames then occupy
			 * [len-1-E_{k+1}+1, len-1-E_k], matching the edge table */
			want_start = ORIGIN + (w->len - 1 - e1) + 1;
			want_end = ORIGIN + (w->len - 1 - e0);
		}
		ds = (long) llabs(f->off_start - want_start);
		de = (long) llabs(f->off_end - want_end);

		if (is_capture) {
			/* first/last capture phase lacks one full sync word on one side:
			 * only bound the estimate to the frame span, never drop the frame */
			check("matrix.capture_estimate", ds <= span_expect && de <= span_expect,
			      DETAIL("%s q=%d ds=%ld de=%ld span=%ld", tag, q, ds, de, span_expect));
		} else if (strict_48k) {
			/* 48 kHz steady-state boundaries match the known edge table
			 * exactly, in both directions */
			check("matrix.steady48k.exact_start", ds == 0,
			      DETAIL("%s q=%d got=%lld want=%lld", tag, q,
			             (long long) f->off_start, (long long) want_start));
			check("matrix.steady48k.exact_end", de == 0,
			      DETAIL("%s q=%d got=%lld want=%lld", tag, q,
			             (long long) f->off_end, (long long) want_end));
		} else {
			/* 44.1 kHz: estimation error must stay within one bit width,
			 * which is the accuracy contract for the rational clock */
			check("matrix.steady44k.within_bitwidth", ds <= bw && de <= bw,
			      DETAIL("%s q=%d ds=%ld de=%ld bw=%ld", tag, q, ds, de, bw));
		}

		/* the 80 biphase tics always cover one frame width from the edge
		 * table; only internal frames are also required to equal the
		 * reported output span (capture frames are clipped at the ends) */
		if (strict_48k) {
			check("matrix.tics48k.sum_edge_width",
			      fabs(tsum - (double) ref_width) < 1e-3,
			      DETAIL("%s q=%d tics=%.4f width=%ld", tag, q, tsum, ref_width));
			if (!is_capture)
				check("matrix.tics48k.sum_span",
				      fabs(tsum - (double) span) < 1e-3,
				      DETAIL("%s q=%d tics=%.4f span=%ld", tag, q, tsum, span));
		} else {
			check("matrix.tics44k.sum_edge_width",
			      fabs(tsum - (double) ref_width) <= (double) bw,
			      DETAIL("%s q=%d tics=%.4f width=%ld", tag, q, tsum, ref_width));
			if (!is_capture)
				check("matrix.tics44k.sum_span",
				      fabs(tsum - (double) span) <= (double) bw,
				      DETAIL("%s q=%d tics=%.4f span=%ld", tag, q, tsum, span));
		}
	}
}

static void suite_entry_matrix(void) {
	const int rates[2] = { 48000, 44100 };
	int ri, rev, inv, fi, ci;
	for (ri = 0; ri < 2; ++ri) {
		for (rev = 0; rev <= 1; ++rev) {
			for (inv = 0; inv <= 1; ++inv) {
				Wave *w = wave_build(rates[ri], rev, inv);
				for (fi = 0; fi < NFMT; ++fi) {
					for (ci = 0; ci < (int) (sizeof(chunks) / sizeof(chunks[0])); ++ci) {
						Capture c = run_decode(w, (enum fmt) fi, chunks[ci],
						                       ORIGIN, QUEUE_BIG);
						verify_capture(w, &c, rev, inv, (enum fmt) fi, chunks[ci]);
						capture_free(&c);
					}
				}
				wave_free(w);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Suite 3: ring queue and queue_flush behavior                        */
/* ------------------------------------------------------------------ */

static void suite_queue(void) {
	Wave *w = wave_build(48000, 0, 0);
	LTCFrameExt f;

	/* (a) small ring buffer, writer must lap it at least twice (7 > 2*3) */
	{
		LTCDecoder *d = ltc_decoder_create(1920, QUEUE_SMALL);
		long off = 0;
		int total = 0;
		while (off < w->len) {
			long c = w->len - off < 500 ? w->len - off : 500;
			ltc_decoder_write(d, w->audio + off, (size_t) c, ORIGIN + off);
			off += c;
			while (ltc_decoder_read(d, &f) == 1) {
				check("queue.ring.identity",
				      tc_matches(&f.ltc, &ref_tc[total]),
				      DETAIL("total=%d", total));
				if (total > 0) {
					check("queue.ring.position",
					      f.off_start == ORIGIN + (long) total * 1920,
					      DETAIL("total=%d start=%lld", total,
					             (long long) f.off_start));
				}
				++total;
			}
		}
		check("queue.ring.all_frames_drained", total == NFRAMES,
		      DETAIL("total=%d", total));
		check("queue.ring.wrapped_two_laps", total >= 2 * QUEUE_SMALL + 1,
		      DETAIL("total=%d", total));
		ltc_decoder_free(d);
	}

	/* (b) flush while half a frame is in the parser: queued (completed)
	 * outputs disappear, the half-frame parser state must stay connected */
	{
		LTCDecoder *d = ltc_decoder_create(1920, QUEUE_BIG);
		long half = 1920 / 2;
		ltc_decoder_write(d, w->audio, (size_t) half, ORIGIN);
		check("queue.flush_mid.no_output_yet",
		      ltc_decoder_queue_length(d) == 0, NULL);
		ltc_decoder_queue_flush(d);
		ltc_decoder_write(d, w->audio + half, (size_t) (w->len - half),
		                  ORIGIN + half);
		Capture c = capture_new();
		drain(d, &c);
		check("queue.flush_mid.frame_count", c.n == NFRAMES,
		      DETAIL("got=%d", c.n));
		if (c.n > 0) {
			check("queue.flush_mid.first_identity",
			      tc_matches(&c.item[0].ltc, &ref_tc[0]), NULL);
			check("queue.flush_mid.first_position",
			      c.item[0].off_start == ORIGIN,
			      DETAIL("start=%lld", (long long) c.item[0].off_start));
		}
		int q;
		for (q = 1; q < c.n; ++q) {
			check("queue.flush_mid.continuity",
			      tc_matches(&c.item[q].ltc, &ref_tc[q]),
			      DETAIL("q=%d", q));
			check("queue.flush_mid.edge_aligned",
			      c.item[q].off_start == ORIGIN + (long) q * 1920,
			      DETAIL("q=%d start=%lld", q,
			                     (long long) c.item[q].off_start));
		}
		capture_free(&c);
		ltc_decoder_free(d);
	}

	/* (c) flush with completed frames already queued: only the queue is
	 * cleared; parsing of subsequent audio continues seamlessly */
	{
		LTCDecoder *d = ltc_decoder_create(1920, QUEUE_BIG);
		long two_frames = 2L * 1920 + 1;   /* two complete frames + 1 sample */
		ltc_decoder_write(d, w->audio, (size_t) two_frames, ORIGIN);
		check("queue.flush_discard.two_queued",
		      ltc_decoder_queue_length(d) == 2,
		      DETAIL("len=%d", ltc_decoder_queue_length(d)));
		ltc_decoder_queue_flush(d);
		check("queue.flush_discard.length_zero",
		      ltc_decoder_queue_length(d) == 0, NULL);
		check("queue.flush_discard.read_empty",
		      ltc_decoder_read(d, &f) == 0, NULL);

		ltc_decoder_write(d, w->audio + two_frames,
		                  (size_t) (w->len - two_frames), ORIGIN + two_frames);
		Capture c = capture_new();
		drain(d, &c);
		/* frames 2..N-1 must still arrive, no loss or duplication */
		check("queue.flush_discard.remaining_count",
		      c.n == NFRAMES - 2, DETAIL("got=%d", c.n));
		if (c.n > 0) {
			check("queue.flush_discard.resume_identity",
			      tc_matches(&c.item[0].ltc, &ref_tc[2]), NULL);
			check("queue.flush_discard.resume_position",
			      c.item[0].off_start == ORIGIN + 2L * 1920,
			      DETAIL("start=%lld", (long long) c.item[0].off_start));
		}
		int q;
		for (q = 0; q < c.n; ++q)
			check("queue.flush_discard.continuity",
			      tc_matches(&c.item[q].ltc, &ref_tc[q + 2]),
			      DETAIL("q=%d", q));
		capture_free(&c);
		ltc_decoder_free(d);
	}

	/* (d) repeated flushes must not touch parser state either */
	{
		LTCDecoder *d = ltc_decoder_create(1920, QUEUE_BIG);
		ltc_decoder_write(d, w->audio, 960, ORIGIN);
		ltc_decoder_queue_flush(d);
		ltc_decoder_queue_flush(d);
		ltc_decoder_write(d, w->audio + 960, (size_t) (w->len - 960),
		                  ORIGIN + 960);
		Capture c = capture_new();
		drain(d, &c);
		check("queue.flush_repeat.frame_count", c.n == NFRAMES,
		      DETAIL("got=%d", c.n));
		if (c.n > 0)
			check("queue.flush_repeat.first_position",
			      c.item[0].off_start == ORIGIN, NULL);
		capture_free(&c);
		ltc_decoder_free(d);
	}

	wave_free(w);
}

/* ------------------------------------------------------------------ */
/* Suite 4 (secondary): library encoder round-trip cross-check only    */
/* ------------------------------------------------------------------ */

static void suite_encoder_crosscheck(void) {
	Wave *w = wave_build(48000, 0, 0);
	Capture independent = run_decode(w, FMT_U8, -1, 0, QUEUE_BIG);
	check("cross.independent_available", independent.n >= 1, NULL);

	LTCEncoder *e = ltc_encoder_create(48000, FPS, LTC_TV_625_50, 0);
	check("cross.encoder_create", e != NULL, NULL);
	ltc_encoder_set_filter(e, 0);

	SMPTETimecode st = { 0 };
	const RefTC *t = &ref_tc[0];
	st.hours = t->hh; st.mins = t->mm; st.secs = t->ss; st.frame = t->ff;
	ltc_encoder_set_user_bits(e,
	    (unsigned long) t->user[0]
	  | ((unsigned long) t->user[1] << 4)
	  | ((unsigned long) t->user[2] << 8)
	  | ((unsigned long) t->user[3] << 12)
	  | ((unsigned long) t->user[4] << 16)
	  | ((unsigned long) t->user[5] << 20)
	  | ((unsigned long) t->user[6] << 24)
	  | ((unsigned long) t->user[7] << 28));
	/* set timecode last: it recomputes the 25 fps parity bit */
	ltc_encoder_set_timecode(e, &st);

	static ltcsnd_sample_t enc_buf[4096];
	ltc_encoder_encode_frame(e);
	int n = ltc_encoder_copy_buffer(e, enc_buf);
	ltc_encoder_end_encode(e);
	n += ltc_encoder_copy_buffer(e, enc_buf + n);
	ltc_encoder_free(e);

	LTCDecoder *d = ltc_decoder_create(1920, QUEUE_BIG);
	ltc_decoder_write(d, enc_buf, (size_t) n, 0);
	Capture encoded = capture_new();
	drain(d, &encoded);
	ltc_decoder_free(d);

	check("cross.encoded_frame_decodes", encoded.n == 1,
	      DETAIL("got=%d", encoded.n));
	if (encoded.n == 1 && independent.n >= 1) {
		check("cross.raw_bytes_match_independent",
		      memcmp(&encoded.item[0].ltc, &independent.item[0].ltc,
		             sizeof(LTCFrame)) == 0,
		      NULL);
	}
	capture_free(&encoded);
	capture_free(&independent);
	wave_free(w);
}

int main(void) {
	build_reference_frames();
	suite_entry_matrix();
	suite_queue();
	suite_encoder_crosscheck();

	printf("sync-clock: %d checks, %d failure(s)\n", g_checks, g_failures);
	if (g_failures == 0) {
		puts("LTC_SYNC_CLOCK_ALL_PASS");
		return 0;
	}
	return 1;
}
