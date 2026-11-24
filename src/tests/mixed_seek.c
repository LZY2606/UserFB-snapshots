/*
	mixed_seek.c: accurate mixed sample-rate seeking regression.

	Exercises MPG123_ACCURATE_MIXED_SEEK on task_support/mixed.mp2:
	  - timeline:  scan/length/tell agree with a full sequential decode
	  - seeking:   sample-exact SEEK_SET/CUR/END at any output coordinate,
	               with varied read chunk sizes and bounded rereads
	  - lifecycle: scan preserves pending PCM, handles are independent and
	               close/open resets the timeline

	Registered through task_support/check.h into the driver in
	task_support/driver.c.
*/

#include "task_support/check.h"

#include <mpg123.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define FIXTURE "task_support/mixed.mp2"
#define MMFLAG MPG123_ACCURATE_MIXED_SEEK

/* The fixed fixture has exactly 128 Layer II frames and decodes to
   195510 output frames at forced 48 kHz stereo s16. */
#define FIXTURE_FRAMES 195510

static unsigned char *the_ref;
static size_t the_ref_bytes;
static int the_setup_done;
static int the_setup_ok;

/* Deterministic pseudo random so every run exercises the same points. */
static uint64_t rng_state = 0x123456789abcdefULL;
static uint64_t rng_next(void)
{
	rng_state ^= rng_state << 7;
	rng_state ^= rng_state >> 9;
	return rng_state;
}

static mpg123_handle *open_mm(int accurate)
{
	int e = MPG123_OK;
	mpg123_handle *h = mpg123_new("generic", &e);
	if(!h) return NULL;
	if(mpg123_param(h, MPG123_ADD_FLAGS
		,	MPG123_QUIET | MPG123_FORCE_STEREO | (accurate ? MMFLAG : 0), 0)
	|| mpg123_param(h, MPG123_REMOVE_FLAGS
		,	MPG123_GAPLESS | MPG123_NO_FRANKENSTEIN, 0)
	|| mpg123_param(h, MPG123_FORCE_RATE, 48000, 0)
	|| mpg123_format_none(h)
	|| mpg123_format(h, 48000, MPG123_STEREO, MPG123_ENC_SIGNED_16)
	|| mpg123_open(h, FIXTURE))
	{
		mpg123_delete(h);
		return NULL;
	}
	return h;
}

/* Read until exactly want bytes are collected, tolerating NEW_FORMAT.
   Returns the collected byte count (may be shorter at end of stream). */
static size_t read_collect(mpg123_handle *h, unsigned char *dst, size_t want)
{
	size_t got = 0;
	while(got < want)
	{
		size_t n = 0;
		int rc = mpg123_read(h, dst + got, want - got, &n);
		got += n;
		if(rc == MPG123_NEW_FORMAT) continue;
		if(rc == MPG123_DONE) break;
		if(rc != MPG123_OK && got == 0) return got;
		if(rc != MPG123_OK) break;
	}
	return got;
}

static int decode_reference(void)
{
	mpg123_handle *h;
	size_t cap = 1 << 20, used = 0;
	unsigned char *p;
	if(the_setup_done) return the_setup_ok ? 0 : -1;
	the_setup_done = 1;
	if(mpg123_init() != MPG123_OK) return -1;
	h = open_mm(0);
	if(!h) return -1;
	p = (unsigned char *) malloc(cap);
	if(!p) { mpg123_delete(h); return -1; }
	for(;;)
	{
		unsigned char chunk[4096];
		size_t n = 0;
		int rc = mpg123_read(h, chunk, sizeof(chunk), &n);
		if(used + n > cap)
		{
			while(used + n > cap) cap *= 2;
			p = (unsigned char *) realloc(p, cap);
			if(!p) { mpg123_delete(h); return -1; }
		}
		memcpy(p + used, chunk, n);
		used += n;
		if(rc == MPG123_NEW_FORMAT) continue;
		if(rc == MPG123_DONE) break;
		if(rc != MPG123_OK) { free(p); mpg123_delete(h); return -1; }
	}
	mpg123_delete(h);
	if(!used || (used & 3)) { free(p); return -1; }
	the_ref = p;
	the_ref_bytes = used;
	the_setup_ok = 1;
	return 0;
}

/* Compare delivered bytes against the reference at output frame `at`. */
static int bytes_match_ref(int64_t at, const unsigned char *buf, size_t bytes)
{
	if(at < 0) return 0;
	if((size_t)at * 4 + bytes > the_ref_bytes)
	{
		/* Only the available overlap must match; callers truncate normally. */
		size_t avail = the_ref_bytes - (size_t)at * 4;
		if((size_t)at * 4 > the_ref_bytes) return 0;
		return memcmp(buf, the_ref + (size_t)at * 4, avail) == 0;
	}
	return memcmp(buf, the_ref + (size_t)at * 4, bytes) == 0;
}

/* ------------------------------ timeline ------------------------------ */

static int case_tl_length(void)
{
	mpg123_handle *h;
	int64_t len, tell;
	TASK_CHECK(decode_reference() == 0);
	TASK_CHECK(the_ref_bytes / 4 == FIXTURE_FRAMES);
	h = open_mm(1);
	TASK_CHECK(h != NULL);
	TASK_CHECK(mpg123_scan(h) == MPG123_OK);
	len = mpg123_length64(h);
	TASK_CHECK(len == (int64_t)(the_ref_bytes / 4));
	TASK_CHECK(len == FIXTURE_FRAMES);
	tell = mpg123_tell64(h);
	TASK_CHECK(tell == 0);
	mpg123_delete(h);
	return 0;
}

static int case_tl_sequential_identity(void)
{
	mpg123_handle *h;
	size_t used = 0, cap = 1 << 20;
	unsigned char *p;
	TASK_CHECK(decode_reference() == 0);
	/* Flag on, no scan: pure sequential path must be untouched. */
	h = open_mm(1);
	TASK_CHECK(h != NULL);
	p = (unsigned char *) malloc(cap);
	TASK_CHECK(p != NULL);
	for(;;)
	{
		size_t n = 0;
		int rc = mpg123_read(h, p + used, cap - used > 4096 ? 4096 : cap - used, &n);
		used += n;
		if(used + 4096 > cap) { cap *= 2; p = realloc(p, cap); TASK_CHECK(p != NULL); }
		if(rc == MPG123_NEW_FORMAT) continue;
		if(rc == MPG123_DONE) break;
		TASK_CHECK(rc == MPG123_OK);
	}
	TASK_CHECK(used == the_ref_bytes);
	TASK_CHECK(memcmp(p, the_ref, the_ref_bytes) == 0);
	free(p);
	mpg123_delete(h);

	/* Scanned handle: phase restoration must be a no-op, so a full decode
	   from zero is still byte-identical to the flag-off reference. */
	h = open_mm(1);
	TASK_CHECK(h != NULL);
	TASK_CHECK(mpg123_scan(h) == MPG123_OK);
	TASK_CHECK(mpg123_seek64(h, 0, SEEK_SET) == 0);
	{
		size_t used2 = 0;
		unsigned char *q = (unsigned char *) malloc(the_ref_bytes);
		TASK_CHECK(q != NULL);
		while(used2 < the_ref_bytes)
		{
			size_t g = read_collect(h, q + used2, the_ref_bytes - used2);
			TASK_CHECK(g > 0);
			used2 += g;
		}
		TASK_CHECK(used2 == the_ref_bytes);
		TASK_CHECK(memcmp(q, the_ref, the_ref_bytes) == 0);
		free(q);
	}
	mpg123_delete(h);
	return 0;
}

static int case_tl_tell_sequence(void)
{
	static const size_t blocks[] = { 148, 616, 3108, 4096, 1024, 20 };
	mpg123_handle *h;
	int64_t delivered = 0;
	size_t bi = 0;
	TASK_CHECK(decode_reference() == 0);
	h = open_mm(1);
	TASK_CHECK(h != NULL);
	TASK_CHECK(mpg123_scan(h) == MPG123_OK);
	for(;;)
	{
		unsigned char buf[4096];
		size_t want = blocks[bi % (sizeof(blocks)/sizeof(*blocks))];
		size_t got;
		int64_t tell;
		if(want > sizeof(buf)) want = sizeof(buf);
		got = read_collect(h, buf, want);
		TASK_CHECK((got & 3) == 0);
		delivered += (int64_t)got / 4;
		tell = mpg123_tell64(h);
		TASK_CHECK(tell == delivered);
		if(got < want) break; /* DONE */
		++bi;
		if(delivered > FIXTURE_FRAMES) { TASK_CHECK(0); break; }
	}
	TASK_CHECK(delivered == FIXTURE_FRAMES);
	TASK_CHECK(mpg123_tell64(h) == FIXTURE_FRAMES);
	mpg123_delete(h);
	return 0;
}

/* ------------------------------- seeking ------------------------------- */

static int check_point(mpg123_handle *h, int64_t target, size_t bytes)
{
	int64_t at, tell;
	unsigned char *buf;
	size_t got;
	if(bytes & 3) bytes &= ~(size_t)3;
	if(bytes == 0) bytes = 4;
	buf = (unsigned char *) malloc(bytes);
	TASK_CHECK(buf != NULL);
	at = mpg123_seek64(h, target, SEEK_SET);
	TASK_CHECK(at == target);
	TASK_CHECK(mpg123_tell64(h) == target);
	got = read_collect(h, buf, bytes);
	TASK_CHECK((got & 3) == 0);
	TASK_CHECK(bytes_match_ref(target, buf, got));
	tell = mpg123_tell64(h);
	TASK_CHECK(tell == target + (int64_t)got / 4);
	free(buf);
	return 0;
}

static int case_seek_points(void)
{
	static const int64_t fixed_points[] = {
		0, 1, 2, 3, 7, 63, 100, 1252, 1253, 1254, 1255,
		52660, 52661, 52662, 52663, 52664, 52665, 52674, 52675, 52676,
		87550, 87551, 87552, 87553, 140240, 140255, 140256, 140257,
		146632, 156408, 190000, 195508, 195509
	};
	mpg123_handle *h;
	unsigned i;
	TASK_CHECK(decode_reference() == 0);
	h = open_mm(1);
	TASK_CHECK(h != NULL);
	TASK_CHECK(mpg123_scan(h) == MPG123_OK);
	for(i = 0; i < sizeof(fixed_points)/sizeof(*fixed_points); ++i)
		TASK_CHECK(check_point(h, fixed_points[i], 4096) == 0);
	/* Spread pseudo-random positions over the whole coordinate range. */
	rng_state = 99;
	for(i = 0; i < 600; ++i)
	{
		int64_t t = (int64_t)(rng_next() % FIXTURE_FRAMES);
		size_t nb = 16 + (size_t)(rng_next() % 6000);
		TASK_CHECK(check_point(h, t, nb) == 0);
	}
	mpg123_delete(h);
	return 0;
}

/* Several jumps per block size, streaming a longer contiguous run after each
   jump and checking sound + counters across many chunk boundaries. */
static int case_seek_block_sizes(void)
{
	static const size_t blocks[] = { 148, 512, 1024, 3108, 4096 };
	static const int64_t targets[] = {
		0, 1254, 40000, 52663, 80000, 110000, 140256, 170000, 195000
	};
	mpg123_handle *h;
	unsigned bi, ti, jump;
	TASK_CHECK(decode_reference() == 0);
	h = open_mm(1);
	TASK_CHECK(h != NULL);
	TASK_CHECK(mpg123_scan(h) == MPG123_OK);
	for(bi = 0; bi < sizeof(blocks)/sizeof(*blocks); ++bi)
	{
		for(jump = 0; jump < 3; ++jump)
		{
			ti = (unsigned)((bi*3 + jump) % (sizeof(targets)/sizeof(*targets)));
			int64_t target = targets[ti];
			size_t run_frames = 6000;
			size_t run_bytes = run_frames * 4;
			unsigned char *run = (unsigned char *) malloc(run_bytes);
			size_t got = 0;
			TASK_CHECK(run != NULL);
			TASK_CHECK(mpg123_seek64(h, target, SEEK_SET) == target);
			while(got < run_bytes)
			{
				size_t want = blocks[bi];
				size_t n;
				int rc;
				if(want > run_bytes - got) want = run_bytes - got;
				rc = mpg123_read(h, run + got, want, &n);
				got += n;
				if(rc == MPG123_NEW_FORMAT) continue;
				if(rc == MPG123_DONE) break;
				TASK_CHECK(rc == MPG123_OK);
			}
			TASK_CHECK((got & 3) == 0);
			TASK_CHECK(bytes_match_ref(target, run, got));
			TASK_CHECK(mpg123_tell64(h) == target + (int64_t)got/4);
			free(run);
		}
	}
	mpg123_delete(h);
	return 0;
}

static int case_seek_whence_and_end(void)
{
	mpg123_handle *h;
	int64_t r;
	unsigned char buf[1024];
	size_t got;
	int rc;
	TASK_CHECK(decode_reference() == 0);
	h = open_mm(1);
	TASK_CHECK(h != NULL);
	TASK_CHECK(mpg123_scan(h) == MPG123_OK);

	r = mpg123_seek64(h, -12345, SEEK_SET);
	TASK_CHECK(r == 0);
	TASK_CHECK(mpg123_tell64(h) == 0);
	r = mpg123_seek64(h, FIXTURE_FRAMES + 99999, SEEK_SET);
	TASK_CHECK(r == FIXTURE_FRAMES);
	TASK_CHECK(mpg123_tell64(h) == FIXTURE_FRAMES);

	r = mpg123_seek64(h, -100, SEEK_END);
	TASK_CHECK(r == FIXTURE_FRAMES - 100);
	r = mpg123_seek64(h, 0, SEEK_END);
	TASK_CHECK(r == FIXTURE_FRAMES);
	/* Usual lseek semantics: positive offset extends past the end -> clamp. */
	r = mpg123_seek64(h, 50, SEEK_END);
	TASK_CHECK(r == FIXTURE_FRAMES);

	TASK_CHECK(mpg123_seek64(h, 5000, SEEK_SET) == 5000);
	r = mpg123_seek64(h, 123, SEEK_CUR);
	TASK_CHECK(r == 5123);
	r = mpg123_seek64(h, -200, SEEK_CUR);
	TASK_CHECK(r == 4923);
	r = mpg123_seek64(h, -1000000, SEEK_CUR);
	TASK_CHECK(r == 0);

	/* At the end, reads deliver nothing and report DONE. */
	TASK_CHECK(mpg123_seek64(h, FIXTURE_FRAMES, SEEK_SET) == FIXTURE_FRAMES);
	got = 0;
	rc = mpg123_read(h, buf, sizeof(buf), &got);
	TASK_CHECK(got == 0);
	TASK_CHECK(rc == MPG123_DONE);

	/* Seek to last 13 frames and drain exactly to the end. */
	r = mpg123_seek64(h, FIXTURE_FRAMES - 13, SEEK_SET);
	TASK_CHECK(r == FIXTURE_FRAMES - 13);
	got = read_collect(h, buf, 52);
	TASK_CHECK(got == 52);
	TASK_CHECK(memcmp(buf, the_ref + (FIXTURE_FRAMES - 13) * 4, 52) == 0);
	rc = mpg123_read(h, buf, sizeof(buf), &got);
	TASK_CHECK(got == 0 && rc == MPG123_DONE);
	mpg123_delete(h);
	return 0;
}

/* Counting reader to prove a locate does not decode the whole prefix. */
struct cio
{
	FILE *f;
	long long bytes;
	long long size;
	long long min_read_off;
	int track;
};
static int cio_read(void *handle, void *buf, size_t n, size_t *got)
{
	struct cio *c = (struct cio *) handle;
	long pos = ftell(c->f);
	size_t r = fread(buf, 1, n, c->f);
	*got = r;
	if(c->track)
	{
		c->bytes += (long long) r;
		if(pos >= 0 && (c->min_read_off < 0 || pos < c->min_read_off))
			c->min_read_off = pos;
	}
	return 0;
}
static int64_t cio_lseek(void *handle, int64_t off, int whence)
{
	struct cio *c = (struct cio *) handle;
	if(fseek(c->f, (long) off, whence) != 0) return -1;
	return (int64_t) ftell(c->f);
}
static void cio_cleanup(void *handle)
{
	struct cio *c = (struct cio *) handle;
	if(c->f) fclose(c->f);
	free(c);
}

static int case_seek_io_bounded(void)
{
	mpg123_handle *h;
	struct cio *c = (struct cio *) calloc(1, sizeof(*c));
	int64_t target;
	long long bytes, minoff, fsize;
	unsigned char buf[512];
	size_t got;
	TASK_CHECK(decode_reference() == 0);
	TASK_CHECK(c != NULL);
	c->f = fopen(FIXTURE, "rb");
	TASK_CHECK(c->f != NULL);
	TASK_CHECK(fseek(c->f, 0, SEEK_END) == 0);
	c->size = ftell(c->f);
	TASK_CHECK(fseek(c->f, 0, SEEK_SET) == 0);
	c->min_read_off = -1;

	h = mpg123_new("generic", NULL);
	TASK_CHECK(h != NULL);
	TASK_CHECK(mpg123_reader64(h, cio_read, cio_lseek, cio_cleanup) == MPG123_OK);
	TASK_CHECK(mpg123_param(h, MPG123_ADD_FLAGS
		,	MPG123_QUIET | MPG123_FORCE_STEREO | MMFLAG, 0) == MPG123_OK);
	TASK_CHECK(mpg123_param(h, MPG123_FORCE_RATE, 48000, 0) == MPG123_OK);
	TASK_CHECK(mpg123_format_none(h) == MPG123_OK);
	TASK_CHECK(mpg123_format(h, 48000, MPG123_STEREO, MPG123_ENC_SIGNED_16) == MPG123_OK);
	TASK_CHECK(mpg123_open_handle64(h, c) == MPG123_OK);
	TASK_CHECK(mpg123_scan(h) == MPG123_OK);

	target = mpg123_length64(h) * 4 / 5;
	c->bytes = 0;
	c->min_read_off = -1;
	c->track = 1;
	TASK_CHECK(mpg123_seek64(h, target, SEEK_SET) == target);
	got = read_collect(h, buf, sizeof(buf));
	c->track = 0;
	TASK_CHECK(got == sizeof(buf));
	TASK_CHECK(bytes_match_ref(target, buf, got));
	bytes = c->bytes;
	minoff = c->min_read_off;
	fsize = c->size;
	/* At most MM_PREROLL+1 = 17 compressed frames are touched; allow the
	   task budget of 24 frames with a 768 byte upper bound per frame. */
	TASK_CHECK(bytes <= 24 * 768);
	/* It must jump near the target instead of rereading from file start. */
	TASK_CHECK(minoff > fsize / 3);
	mpg123_delete(h); /* cio_cleanup frees c */
	return 0;
}

/* ------------------------------ lifecycle ----------------------------- */

static int case_life_scan_preserves(void)
{
	static const size_t pres[] = { 0, 148, 3108, 8000 };
	unsigned pi;
	TASK_CHECK(decode_reference() == 0);
	for(pi = 0; pi < sizeof(pres)/sizeof(*pres); ++pi)
	{
		mpg123_handle *h = open_mm(1);
		unsigned char prebuf[8192];
		unsigned char tail[12000];
		size_t pre = pres[pi], g;
		int64_t t1, t2;
		TASK_CHECK(h != NULL);
		TASK_CHECK(mpg123_scan(h) == MPG123_OK);
		g = read_collect(h, prebuf, pre);
		TASK_CHECK(g == pre);
		TASK_CHECK(memcmp(prebuf, the_ref, pre) == 0);
		t1 = mpg123_tell64(h);
		TASK_CHECK(t1 == (int64_t)pre / 4);
		/* Scan again with PCM still in the output buffer. */
		TASK_CHECK(mpg123_scan(h) == MPG123_OK);
		t2 = mpg123_tell64(h);
		TASK_CHECK(t2 == t1);
		g = read_collect(h, tail, sizeof(tail));
		TASK_CHECK((g & 3) == 0);
		TASK_CHECK(bytes_match_ref(t2, tail, g));
		mpg123_delete(h);
	}
	return 0;
}

static int case_life_two_handles(void)
{
	mpg123_handle *a = open_mm(1);
	mpg123_handle *b = open_mm(1);
	unsigned i;
	TASK_CHECK(decode_reference() == 0);
	TASK_CHECK(a && b);
	TASK_CHECK(mpg123_scan(a) == MPG123_OK);
	TASK_CHECK(mpg123_scan(b) == MPG123_OK);
	rng_state = 0xBEEFBEEF;
	for(i = 0; i < 250; ++i)
	{
		mpg123_handle *x = (i & 1) ? a : b;
		mpg123_handle *y = (i & 1) ? b : a;
		int64_t tx = (int64_t)(rng_next() % FIXTURE_FRAMES);
		int64_t ty = (int64_t)(rng_next() % FIXTURE_FRAMES);
		unsigned char bx[900], by[512];
		size_t gx, gy;
		TASK_CHECK(mpg123_seek64(x, tx, SEEK_SET) == tx);
		gx = read_collect(x, bx, sizeof(bx));
		TASK_CHECK(bytes_match_ref(tx, bx, gx));
		TASK_CHECK(mpg123_seek64(y, ty, SEEK_SET) == ty);
		gy = read_collect(y, by, sizeof(by));
		TASK_CHECK(bytes_match_ref(ty, by, gy));
		/* Going back to x must still work on x's own timeline. */
		TASK_CHECK(mpg123_tell64(x) == tx + (int64_t)gx/4);
	}
	mpg123_delete(a);
	mpg123_delete(b);
	return 0;
}

static int case_life_reopen(void)
{
	mpg123_handle *h = open_mm(1);
	unsigned char buf[4096];
	size_t got;
	TASK_CHECK(decode_reference() == 0);
	TASK_CHECK(h != NULL);
	TASK_CHECK(mpg123_scan(h) == MPG123_OK);
	TASK_CHECK(mpg123_length64(h) == FIXTURE_FRAMES);
	TASK_CHECK(mpg123_seek64(h, 123456, SEEK_SET) == 123456);
	/* Close and reopen the same handle: old timeline must not carry over. */
	TASK_CHECK(mpg123_close(h) == MPG123_OK);
	TASK_CHECK(mpg123_open(h, FIXTURE) == MPG123_OK);
	TASK_CHECK(mpg123_scan(h) == MPG123_OK);
	TASK_CHECK(mpg123_length64(h) == FIXTURE_FRAMES);
	TASK_CHECK(mpg123_seek64(h, 123456, SEEK_SET) == 123456);
	got = read_collect(h, buf, sizeof(buf));
	TASK_CHECK(got == sizeof(buf));
	TASK_CHECK(memcmp(buf, the_ref + 123456 * 4, sizeof(buf)) == 0);
	/* A freshly allocated handle starts clean as well. */
	{
		mpg123_handle *h2 = open_mm(1);
		TASK_CHECK(h2 != NULL);
		TASK_CHECK(mpg123_scan(h2) == MPG123_OK);
		TASK_CHECK(mpg123_length64(h2) == FIXTURE_FRAMES);
		mpg123_delete(h2);
	}
	mpg123_delete(h);
	return 0;
}

const struct task_case task_cases[] = {
	{"timeline", "length_matches_decode", case_tl_length},
	{"timeline", "sequential_flag_identity", case_tl_sequential_identity},
	{"timeline", "tell_advances_with_frames", case_tl_tell_sequence},
	{"seeking", "points_sample_exact", case_seek_points},
	{"seeking", "block_sizes_and_jumps", case_seek_block_sizes},
	{"seeking", "whence_clamps_and_done", case_seek_whence_and_end},
	{"seeking", "locate_rereads_at_most_24_frames", case_seek_io_bounded},
	{"lifecycle", "scan_preserves_pcm_and_position", case_life_scan_preserves},
	{"lifecycle", "two_handles_independent", case_life_two_handles},
	{"lifecycle", "close_open_resets_timeline", case_life_reopen}
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
