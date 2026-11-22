/**
 * @file red_playout.c  RFC 2198 RED parsing and RED jitter-buffer playout
 *
 * The tests use hand-built RED payloads, independent audio copies and a
 * microsecond clock.  No network, audio device or real timer is involved.
 */
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <re_types.h>
#include <re_fmt.h>
#include <re_mem.h>
#include <re_list.h>
#include <re_mbuf.h>
#include <re_rtp.h>
#include <re_jbuf.h>

#include "task_support/check.h"


/* ------------------------------------------------------------------ */
/* Payload construction helpers                                       */
/* ------------------------------------------------------------------ */

struct red_blk {
	uint8_t  pt;        /* payload type */
	uint16_t offset;    /* timestamp offset */
	const char *data;   /* audio bytes */
	size_t   len;
};


static struct mbuf *make_red(const struct red_blk *redv, size_t nred,
			     const struct red_blk *main)
{
	struct mbuf *mb = mbuf_alloc(256);
	size_t i;

	if (!mb)
		return NULL;

	/* Four-byte redundancy headers */
	for (i = 0; i < nred; ++i) {
		uint8_t h[4];

		h[0] = 0x80 | (redv[i].pt & 0x7f);
		h[1] = (uint8_t)(redv[i].offset >> 8);
		h[2] = (uint8_t)(redv[i].offset & 0xff);
		h[3] = (uint8_t)redv[i].len;

		if (mbuf_write_mem(mb, h, sizeof(h)))
			goto err;
	}

	/* Single-byte primary header */
	if (mbuf_write_u8(mb, main->pt & 0x7f))
		goto err;

	/* Payload data in header order */
	for (i = 0; i < nred; ++i) {
		if (mbuf_write_mem(mb, (const uint8_t *)redv[i].data,
				   redv[i].len))
			goto err;
	}
	if (mbuf_write_mem(mb, (const uint8_t *)main->data, main->len))
		goto err;

	mb->pos = 0;
	return mb;

 err:
	mem_deref(mb);
	return NULL;
}


static struct mbuf *make_audio(const char *data, size_t len)
{
	struct mbuf *mb = mbuf_alloc(len ? len : 1);

	if (!mb)
		return NULL;

	if (mbuf_write_mem(mb, (const uint8_t *)data, len)) {
		mem_deref(mb);
		return NULL;
	}
	mb->pos = 0;

	return mb;
}


static bool payload_is(const struct mbuf *mb, const char *data, size_t len)
{
	return mb && mbuf_get_left(mb) == len &&
		!memcmp(mbuf_buf(mb), data, len);
}


/* ------------------------------------------------------------------ */
/* RED wire parsing                                                   */
/* ------------------------------------------------------------------ */

static int test_wire_decode_basic(void)
{
	struct red_blk redv[2] = {
		{ 96, 640, "old1", 4 },
		{ 96, 320, "old2", 4 },
	};
	struct red_blk main = { 96, 0, "main", 4 };
	struct mbuf *mb = make_red(redv, 2, &main);
	struct rtp_red_block blockv[8];
	struct rtp_red red;
	int err;

	TASK_CHECK(mb);

	err = rtp_red_decode(&red, mb, blockv, 8);
	TASK_CHECK(!err);
	TASK_CHECK(red.n == 3);
	TASK_CHECK(red.blockv == blockv);
	TASK_CHECK(blockv[0].pt == 96);
	TASK_CHECK(blockv[0].ts_offset == 640);
	TASK_CHECK(blockv[0].len == 4);
	TASK_CHECK(blockv[1].ts_offset == 320);
	TASK_CHECK(blockv[2].ts_offset == 0);
	TASK_CHECK(blockv[2].len == 4);
	TASK_CHECK(red.hdr_len == 2 * 4 + 1);
	TASK_CHECK(red.data_len == 12);

	/* Ranges follow the header order and cover the data section */
	TASK_CHECK(blockv[0].off == red.hdr_len);
	TASK_CHECK(blockv[1].off == blockv[0].off + blockv[0].len);
	TASK_CHECK(blockv[2].off == blockv[1].off + blockv[1].len);
	TASK_CHECK(payload_is(mb, "old1", 4) == false); /* position untouched */
	TASK_CHECK(mb->pos == 0);

	mem_deref(mb);
	return 0;
}


static int test_wire_reject(void)
{
	struct red_blk main = { 96, 0, "main", 4 };
	struct rtp_red_block blockv[8];
	struct rtp_red red;
	int err;

	/* Empty payload */
	{
		struct mbuf *mb = mbuf_alloc(4);
		TASK_CHECK(mb);
		mb->pos = mb->end = 0;
		err = rtp_red_decode(&red, mb, blockv, 8);
		TASK_CHECK(err == EBADMSG);
		mem_deref(mb);
	}

	/* Truncated redundancy header (declares 3 bytes, only 2 present) */
	{
		const uint8_t raw[] = { 0x80 | 96, 0x00 };
		struct mbuf *mb = mbuf_alloc(8);
		TASK_CHECK(mb);
		mbuf_write_mem(mb, raw, sizeof(raw));
		mb->pos = 0;
		err = rtp_red_decode(&red, mb, blockv, 8);
		TASK_CHECK(err == EBADMSG);
		mem_deref(mb);
	}

	/* Empty audio block (redundant block length 0) */
	{
		struct red_blk r = { 96, 320, "", 0 };
		struct mbuf *mb = make_red(&r, 1, &main);
		TASK_CHECK(mb);
		err = rtp_red_decode(&red, mb, blockv, 8);
		TASK_CHECK(err == EBADMSG);
		mem_deref(mb);
	}

	/* Block lengths exceed the audio payload */
	{
		const uint8_t raw[] = { 0x80 | 96, 0x01, 0x40, 0x09, 96, 'a' };
		struct mbuf *mb = mbuf_alloc(16);
		TASK_CHECK(mb);
		mbuf_write_mem(mb, raw, sizeof(raw));
		mb->pos = 0;
		err = rtp_red_decode(&red, mb, blockv, 8);
		TASK_CHECK(err == EBADMSG);
		mem_deref(mb);
	}

	/* Empty primary payload */
	{
		struct red_blk r = { 96, 320, "aa", 2 };
		const uint8_t raw[] = { 0x80 | 96, 0x01, 0x40, 0x02, 96 };
		struct mbuf *mb = mbuf_alloc(16);
		(void)r;
		TASK_CHECK(mb);
		mbuf_write_mem(mb, raw, sizeof(raw));
		mb->pos = 0;
		err = rtp_red_decode(&red, mb, blockv, 8);
		TASK_CHECK(err == EBADMSG);
		mem_deref(mb);
	}

	/* More redundancy blocks than the configured history depth */
	{
		struct red_blk redv[3] = {
			{ 96, 960, "aaaa", 2 },
			{ 96, 640, "bbbb", 2 },
			{ 96, 320, "cccc", 2 },
		};
		struct mbuf *mb = make_red(redv, 3, &main);
		TASK_CHECK(mb);
		err = rtp_red_decode(&red, mb, blockv, 2);
		TASK_CHECK(err == EBADMSG);
		mem_deref(mb);
	}

	/* A partial parse must not leave a usable result */
	{
		struct red_blk redv[3] = {
			{ 96, 960, "aaaa", 2 },
			{ 96, 640, "bbbb", 2 },
			{ 96, 320, "cccc", 2 },
		};
		struct mbuf *mb = make_red(redv, 3, &main);
		TASK_CHECK(mb);
		memset(&red, 0xa5, sizeof(red));
		err = rtp_red_decode(&red, mb, blockv, 1);
		TASK_CHECK(err == EBADMSG);
		TASK_CHECK(red.n == 0);
		TASK_CHECK(red.hdr_len == 0);
		TASK_CHECK(red.data_len == 0);
		mem_deref(mb);
	}

	return 0;
}


/* ------------------------------------------------------------------ */
/* Jitter buffer helpers                                              */
/* ------------------------------------------------------------------ */

struct red_test {
	struct jbuf *jb;
	struct jbuf_red_cfg cfg;
};

#define TEST_RATE      8000
#define TEST_PTIME     320
#define TEST_DELAY_US  20000
#define TEST_SSRC      0x11223344u


static int red_setup(struct red_test *t, uint32_t window,
		     uint32_t history_depth)
{
	memset(t, 0, sizeof(*t));

	t->cfg.ssrc          = TEST_SSRC;
	t->cfg.pt            = 96;
	t->cfg.red_pt        = 97;
	t->cfg.rate          = TEST_RATE;
	t->cfg.ptime_ticks   = TEST_PTIME;
	t->cfg.delay_us      = TEST_DELAY_US;
	t->cfg.history_depth = history_depth;
	t->cfg.window        = window;

	if (jbuf_alloc_red(&t->jb, &t->cfg))
		return 1;

	return 0;
}


static void red_teardown(struct red_test *t)
{
	t->jb = mem_deref(t->jb);
}


static struct rtp_header mk_hdr(uint8_t pt, uint16_t seq, uint32_t ts)
{
	struct rtp_header h;

	memset(&h, 0, sizeof(h));
	h.ver  = RTP_VERSION;
	h.pt   = pt;
	h.seq  = seq;
	h.ts   = ts;
	h.ssrc = TEST_SSRC;

	return h;
}


/* Put a normal audio packet */
static int put_audio(struct red_test *t, uint16_t seq, uint32_t ts,
		     const char *data, size_t len, uint64_t now)
{
	struct rtp_header h = mk_hdr(t->cfg.pt, seq, ts);
	struct mbuf *mb = make_audio(data, len);
	int err;

	if (!mb)
		return 1;

	h.m = 1;
	err = jbuf_put_red(t->jb, &h, mb, now);
	mem_deref(mb);

	return err;
}


/* Put a RED packet with redundant blocks plus a primary block */
static int put_red(struct red_test *t, uint16_t seq, uint32_t ts,
		   const struct red_blk *redv, size_t nred,
		   const char *main, size_t main_len, uint64_t now)
{
	struct rtp_header h = mk_hdr(t->cfg.red_pt, seq, ts);
	struct red_blk m = { t->cfg.pt, 0, main, main_len };
	struct mbuf *mb = make_red(redv, nred, &m);
	int err;

	if (!mb)
		return 1;

	h.m = 1;
	err = jbuf_put_red(t->jb, &h, mb, now);
	mem_deref(mb);

	return err;
}


/* Get one block and free it */
static int get_one(struct red_test *t, uint64_t now, struct rtp_header *hdr,
		   char *buf, size_t bufsz, size_t *lenp, bool *redp)
{
	struct mbuf *mb = NULL;
	bool red = false;
	int err;

	err = jbuf_get_red(t->jb, now, hdr, &mb, &red);
	if (!err) {
		if (buf && bufsz) {
			size_t n = mbuf_get_left(mb);

			if (n > bufsz)
				n = bufsz;
			memcpy(buf, mbuf_buf(mb), n);
			if (lenp)
				*lenp = mbuf_get_left(mb);
		}
		mem_deref(mb);
	}

	if (redp)
		*redp = red;

	return err;
}


/* ------------------------------------------------------------------ */
/* Slot precedence                                                    */
/* ------------------------------------------------------------------ */

static int test_red_recovers_gap(void)
{
	struct red_test t;
	struct rtp_header h;
	struct red_blk redv[1];
	char buf[32];
	size_t len = 0;
	bool red = false;
	int err;

	TASK_CHECK(!red_setup(&t, 64, 4));

	/* Anchor on slot 0, primary payload "A0" */
	TASK_CHECK(!put_audio(&t, 1000, 32000, "A0", 2, 1000000));

	/* Slot 2 primary arrives, carrying slot 1 as redundancy */
	redv[0].pt = 96;
	redv[0].offset = 320;
	redv[0].data = "A1";
	redv[0].len = 2;
	TASK_CHECK(!put_red(&t, 1002, 32640, redv, 1, "A2", 2, 1000001));

	/* Slot 0 due */
	err = get_one(&t, 1000000 + TEST_DELAY_US, &h, buf, sizeof(buf),
		      &len, &red);
	TASK_CHECK(!err);
	TASK_CHECK(h.seq == 1000);
	TASK_CHECK(!red);
	TASK_CHECK(len == 2 && !memcmp(buf, "A0", 2));

	/* Slot 1 was restored from redundancy */
	err = get_one(&t, 1000000 + TEST_DELAY_US + 40000, &h, buf,
		      sizeof(buf), &len, &red);
	TASK_CHECK(!err);
	TASK_CHECK(red == true);
	TASK_CHECK(h.seq == 1001);
	TASK_CHECK(h.ts == 32000 + 320);
	TASK_CHECK(h.pt == 96);
	TASK_CHECK(h.m == false);
	TASK_CHECK(len == 2 && !memcmp(buf, "A1", 2));

	/* Slot 2 was the primary itself */
	err = get_one(&t, 1000000 + TEST_DELAY_US + 80000, &h, buf,
		      sizeof(buf), &len, &red);
	TASK_CHECK(!err);
	TASK_CHECK(red == false);
	TASK_CHECK(h.seq == 1002);
	TASK_CHECK(h.m == true);
	TASK_CHECK(len == 2 && !memcmp(buf, "A2", 2));

	red_teardown(&t);
	return 0;
}


static int test_late_primary_replaces_recovered(void)
{
	struct red_test t;
	struct rtp_header h;
	struct red_blk redv[1];
	char buf[32];
	size_t len;
	bool red;
	int err;

	TASK_CHECK(!red_setup(&t, 64, 4));

	TASK_CHECK(!put_audio(&t, 1000, 32000, "A0", 2, 1000000));

	redv[0].pt = 96;
	redv[0].offset = 320;
	redv[0].data = "R1";
	redv[0].len = 2;
	TASK_CHECK(!put_red(&t, 1002, 32640, redv, 1, "A2", 2, 1000001));

	/* The real primary for slot 1 arrives before playout */
	TASK_CHECK(!put_audio(&t, 1001, 32320, "P1", 2, 1000002));

	/* slot 0 */
	TASK_CHECK(!get_one(&t, 1000000 + TEST_DELAY_US, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(!red);

	/* slot 1 must be the primary copy, not the recovered one */
	err = get_one(&t, 1000000 + TEST_DELAY_US + 40000, &h, buf,
		      sizeof(buf), &len, &red);
	TASK_CHECK(!err);
	TASK_CHECK(red == false);
	TASK_CHECK(len == 2 && !memcmp(buf, "P1", 2));

	red_teardown(&t);
	return 0;
}


static int test_first_copy_wins(void)
{
	struct red_test t;
	struct rtp_header h;
	struct red_blk redv[1];
	char buf[32];
	size_t len;
	bool red;

	TASK_CHECK(!red_setup(&t, 64, 4));

	TASK_CHECK(!put_audio(&t, 1000, 32000, "A0", 2, 1000000));

	/* Two different redundant copies of slot 1; first accepted wins */
	redv[0].pt = 96;
	redv[0].offset = 320;
	redv[0].data = "F1";
	redv[0].len = 2;
	TASK_CHECK(!put_red(&t, 1002, 32640, redv, 1, "A2", 2, 1000001));

	redv[0].data = "S1";
	TASK_CHECK(!put_red(&t, 1003, 32960, redv, 1, "A3", 2, 1000002));

	TASK_CHECK(!get_one(&t, 1000000 + TEST_DELAY_US, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(!get_one(&t, 1000000 + TEST_DELAY_US + 40000, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(red == true);
	TASK_CHECK(len == 2 && !memcmp(buf, "F1", 2));

	red_teardown(&t);
	return 0;
}


static int test_no_replace_after_playout(void)
{
	struct red_test t;
	struct rtp_header h;
	struct red_blk redv[1];
	char buf[32];
	size_t len;
	bool red;
	int err;

	TASK_CHECK(!red_setup(&t, 64, 4));

	TASK_CHECK(!put_audio(&t, 1000, 32000, "A0", 2, 1000000));
	TASK_CHECK(!put_audio(&t, 1001, 32320, "B1", 2, 1000001));

	TASK_CHECK(!get_one(&t, 1000000 + TEST_DELAY_US, &h, buf,
			    sizeof(buf), &len, &red));

	/* A late primary for the played-out slot must be rejected */
	err = put_audio(&t, 1000, 32000, "XX", 2, 1000002);
	TASK_CHECK(err == ETIMEDOUT);

	/* Slot 1 is still intact */
	TASK_CHECK(!get_one(&t, 1000000 + TEST_DELAY_US + 40000, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(len == 2 && !memcmp(buf, "B1", 2));

	red_teardown(&t);
	return 0;
}


static int test_bad_primary_leaves_no_copy(void)
{
	struct red_test t;
	struct rtp_header h;
	struct red_blk redv[1];
	char buf[32];
	size_t len;
	bool red;
	int err;

	TASK_CHECK(!red_setup(&t, 32, 4));

	TASK_CHECK(!put_audio(&t, 0, 0, "A0", 2, 1000));

	/* RED packet with a valid redundant block for slot 1 but a final
	   block carrying the wrong payload type: the whole packet is
	   rejected and the redundant copy must not be stored */
	redv[0].pt = 96;
	redv[0].offset = 320;
	redv[0].data = "R1";
	redv[0].len = 2;

	{
		struct red_blk r = { 96, 320, "R1", 2 };
		struct red_blk m = { 98, 0, "A2", 2 };
		struct mbuf *mb = make_red(&r, 1, &m);
		struct rtp_header hh = mk_hdr(97, 2, 640);

		TASK_CHECK(mb);
		(void)redv;
		err = jbuf_put_red(t.jb, &hh, mb, 1001);
		TASK_CHECK(err == EBADMSG);
		mem_deref(mb);
	}

	/* Slot 0 plays, but slot 1 has no recovered copy and slot 2 was
	   never accepted: the frontier stays at slot 0 */
	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US, &h, buf, sizeof(buf),
			    &len, &red));
	TASK_CHECK(h.seq == 0);
	TASK_CHECK(get_one(&t, 1000 + TEST_DELAY_US + 40000, &h, NULL, 0,
			   NULL, NULL) == ENOENT);

	red_teardown(&t);
	return 0;
}


static int test_redundancy_depth_limit(void)
{
	struct red_test t;
	struct rtp_header h;
	struct red_blk redv[3];
	char buf[32];
	size_t len;
	bool red;
	int err;

	TASK_CHECK(!red_setup(&t, 32, 2));

	TASK_CHECK(!put_audio(&t, 0, 0, "A0", 2, 1000));

	/* Three redundant blocks exceed history_depth=2 */
	redv[0].pt = 96;
	redv[0].offset = 960;
	redv[0].data = "Z3";
	redv[0].len = 2;
	redv[1].pt = 96;
	redv[1].offset = 640;
	redv[1].data = "Z2";
	redv[1].len = 2;
	redv[2].pt = 96;
	redv[2].offset = 320;
	redv[2].data = "Z1";
	redv[2].len = 2;

	err = put_red(&t, 3, 960, redv, 3, "A3", 2, 1001);
	TASK_CHECK(err == EBADMSG);

	/* Offset beyond the configured depth is rejected too */
	{
		struct red_blk deep = { 96, 960, "Z4", 2 };

		err = put_red(&t, 3, 960, &deep, 1, "A3", 2, 1002);
		TASK_CHECK(err == EBADMSG);
	}

	/* Frontier unchanged: slot 1 is unknown */
	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US, &h, buf, sizeof(buf),
			    &len, &red));
	TASK_CHECK(get_one(&t, 1000 + TEST_DELAY_US + 40000, &h, NULL, 0,
			   NULL, NULL) == ENOENT);

	red_teardown(&t);
	return 0;
}


/* ------------------------------------------------------------------ */
/* Playout                                                            */
/* ------------------------------------------------------------------ */

static int test_timing_and_missing_slot(void)
{
	struct red_test t;
	struct rtp_header h;
	char buf[32];
	size_t len;
	bool red;
	int err;

	TASK_CHECK(!red_setup(&t, 64, 4));

	TASK_CHECK(!put_audio(&t, 50, 0xffffff00u, "A0", 2, 5000000));
	/* Slot 2 is present, extending the primary frontier past slot 1 */
	TASK_CHECK(!put_audio(&t, 52, 0xffffff00u + 2 * TEST_PTIME, "A2", 2,
			      5000000));

	/* Not due one microsecond early */
	err = get_one(&t, 5000000 + TEST_DELAY_US - 1, &h, NULL, 0, NULL,
		      NULL);
	TASK_CHECK(err == EAGAIN);

	/* Slot 0 is due exactly at the delay */
	err = get_one(&t, 5000000 + TEST_DELAY_US, &h, buf, sizeof(buf),
		      &len, &red);
	TASK_CHECK(!err);
	TASK_CHECK(h.seq == 50);
	TASK_CHECK(h.ts == 0xffffff00u);

	/* Slot 1 is empty and due later: ENODATA advances one slot */
	err = get_one(&t, 5000000 + TEST_DELAY_US + 40000, &h, buf,
		      sizeof(buf), &len, &red);
	TASK_CHECK(err == ENODATA);

	/* Nothing changed in the output parameters on ENODATA */
	TASK_CHECK(h.seq == 50);
	TASK_CHECK(len == 2);

	/* Slot 2 is now the current slot and has content */
	err = get_one(&t, 5000000 + TEST_DELAY_US + 80000, &h, buf,
		      sizeof(buf), &len, &red);
	TASK_CHECK(!err);
	TASK_CHECK(h.seq == 52);
	TASK_CHECK(len == 2 && !memcmp(buf, "A2", 2));

	/* Slot 3 is still unknown: beyond the frontier */
	err = get_one(&t, 5000000 + TEST_DELAY_US + 120000, &h, NULL, 0,
		      NULL, NULL);
	TASK_CHECK(err == ENOENT);

	red_teardown(&t);
	return 0;
}


static int test_wrap_sequence_and_timestamp(void)
{
	struct red_test t;
	struct rtp_header h;
	char buf[32];
	size_t len;
	bool red;

	TASK_CHECK(!red_setup(&t, 64, 4));

	/* Sequence wraps at 65535 and timestamp wraps at 2^32 */
	TASK_CHECK(!put_audio(&t, 0xfffe, 0xfffffe00u, "W0", 2, 1000));
	TASK_CHECK(!put_audio(&t, 0xffff, 0xffffff40u, "W1", 2, 1001));
	TASK_CHECK(!put_audio(&t, 0x0000, 0x00000080u, "W2", 2, 1002));
	TASK_CHECK(!put_audio(&t, 0x0001, 0x000001c0u, "W3", 2, 1003));

	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US, &h, buf, sizeof(buf),
			    &len, &red));
	TASK_CHECK(h.seq == 0xfffe && h.ts == 0xfffffe00u);
	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US + 40000, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(h.seq == 0xffff && h.ts == 0xffffff40u);
	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US + 80000, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(h.seq == 0 && h.ts == 0x00000080u);
	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US + 120000, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(h.seq == 1 && h.ts == 0x000001c0u);

	red_teardown(&t);
	return 0;
}


static int test_out_of_order(void)
{
	struct red_test t;
	struct rtp_header h;
	char buf[32];
	size_t len;
	bool red;

	TASK_CHECK(!red_setup(&t, 64, 4));

	/* Anchor then send slot 2 before slot 1 */
	TASK_CHECK(!put_audio(&t, 10, 1000, "O0", 2, 1000));
	TASK_CHECK(!put_audio(&t, 12, 1640, "O2", 2, 1001));
	TASK_CHECK(!put_audio(&t, 11, 1320, "O1", 2, 1002));

	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US, &h, buf, sizeof(buf),
			    &len, &red));
	TASK_CHECK(h.seq == 10);
	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US + 40000, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(h.seq == 11 && len == 2 && !memcmp(buf, "O1", 2));
	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US + 80000, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(h.seq == 12 && len == 2 && !memcmp(buf, "O2", 2));

	red_teardown(&t);
	return 0;
}


static int test_reject_does_not_advance(void)
{
	struct red_test t;
	struct rtp_header h;
	struct red_blk redv[1];
	char buf[32];
	size_t len;
	bool red;
	int err;

	TASK_CHECK(!red_setup(&t, 8, 2));

	TASK_CHECK(!put_audio(&t, 100, 8000, "A0", 2, 1000));

	/* Input beyond the configured window is rejected and changes
	   nothing: slot 7 is still unknown afterwards */
	err = put_audio(&t, 108, 8000 + 8 * TEST_PTIME, "AF", 2, 1001);
	TASK_CHECK(err == EINVAL);

	/* Bad timestamp step on the fixed timeline is rejected */
	err = put_audio(&t, 101, 8000 + TEST_PTIME + 1, "AX", 2, 1002);
	TASK_CHECK(err == EINVAL);

	/* Wrong SSRC and wrong payload type are rejected */
	{
		struct rtp_header bad = mk_hdr(96, 101, 8320);
		struct mbuf *mb = make_audio("AX", 2);

		TASK_CHECK(mb);
		bad.ssrc = 1234;
		TASK_CHECK(jbuf_put_red(t.jb, &bad, mb, 1003) == EINVAL);
		bad.ssrc = TEST_SSRC;
		bad.pt = 111;
		TASK_CHECK(jbuf_put_red(t.jb, &bad, mb, 1003) == EINVAL);
		mem_deref(mb);
	}

	/* A bad final block must not leave the earlier redundant copy */
	{
		/* Build a RED packet whose primary block is empty */
		const uint8_t raw[] = {
			0x80 | 96, 0x01, 0x40, 0x02, 96
		};
		struct mbuf *mb = mbuf_alloc(16);
		struct rtp_header hh = mk_hdr(97, 101, 8320);

		TASK_CHECK(mb);
		mbuf_write_mem(mb, raw, sizeof(raw));
		mb->pos = 0;
		err = jbuf_put_red(t.jb, &hh, mb, 1004);
		TASK_CHECK(err == EBADMSG);
		mem_deref(mb);
	}

	/* Nothing was committed for slot 1.  The frontier is still slot 0,
	   so slot 1 is not known yet: ENOENT */
	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US, &h, buf, sizeof(buf),
			    &len, &red));
	err = get_one(&t, 1000 + TEST_DELAY_US + 40000, &h, NULL, 0, NULL,
		      NULL);
	TASK_CHECK(err == ENOENT);

	/* The buffer still works: a valid primary extends the frontier */
	TASK_CHECK(!put_audio(&t, 101, 8320, "R1", 2, 1005));
	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US + 40000, &h, buf,
			    sizeof(buf), &len, &red));
	TASK_CHECK(len == 2 && !memcmp(buf, "R1", 2));

	/* A late primary for the played-out slot is rejected */
	err = put_audio(&t, 100, 8000, "XX", 2, 1006);
	TASK_CHECK(err == ETIMEDOUT);

	red_teardown(&t);
	return 0;
}


static int test_capacity_reject_then_recover(void)
{
	struct red_test t;
	struct rtp_header h;
	char buf[32];
	size_t len;
	bool red;
	int err;

	TASK_CHECK(!red_setup(&t, 4, 2));

	TASK_CHECK(!put_audio(&t, 0, 0, "C0", 2, 1000));

	/* Slot 4 is just outside the window */
	err = put_audio(&t, 4, 4 * TEST_PTIME, "C4", 2, 1001);
	TASK_CHECK(err == EINVAL);

	/* Slot 3 is the last valid one */
	TASK_CHECK(!put_audio(&t, 3, 3 * TEST_PTIME, "C3", 2, 1002));

	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US, &h, buf, sizeof(buf),
			    &len, &red));
	TASK_CHECK(h.seq == 0);

	/* Slots 1 and 2 are empty but inside the frontier */
	err = get_one(&t, 1000 + TEST_DELAY_US + 40000, &h, NULL, 0, NULL,
		      NULL);
	TASK_CHECK(err == ENODATA);
	err = get_one(&t, 1000 + TEST_DELAY_US + 80000, &h, NULL, 0, NULL,
		      NULL);
	TASK_CHECK(err == ENODATA);

	/* Slot 3 was accepted after the overflow rejection */
	err = get_one(&t, 1000 + TEST_DELAY_US + 120000, &h, buf,
		      sizeof(buf), &len, &red);
	TASK_CHECK(!err);
	TASK_CHECK(h.seq == 3);
	TASK_CHECK(len == 2 && !memcmp(buf, "C3", 2));

	/* Slot 4 was never accepted, so it is beyond the frontier */
	err = get_one(&t, 1000 + TEST_DELAY_US + 160000, &h, NULL, 0, NULL,
		      NULL);
	TASK_CHECK(err == ENOENT);

	red_teardown(&t);
	return 0;
}


static int test_duplicate_does_not_accumulate(void)
{
	struct red_test t;
	struct rtp_header h;
	char buf[32];
	size_t len;
	bool red;

	TASK_CHECK(!red_setup(&t, 16, 4));

	/* Same primary packet twice: the second must not consume a slot */
	TASK_CHECK(!put_audio(&t, 20, 100, "D0", 2, 1000));
	TASK_CHECK(!put_audio(&t, 20, 100, "D0", 2, 1001));

	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US, &h, buf, sizeof(buf),
			    &len, &red));
	TASK_CHECK(h.seq == 20);
	/* Slot 1 is still beyond the frontier */
	TASK_CHECK(get_one(&t, 1000 + TEST_DELAY_US + 40000, &h, NULL, 0,
			   NULL, NULL) == ENOENT);

	red_teardown(&t);
	return 0;
}


static int test_flush_reanchors(void)
{
	struct red_test t;
	struct rtp_header h;
	char buf[32];
	size_t len;
	bool red;

	TASK_CHECK(!red_setup(&t, 32, 4));

	TASK_CHECK(!put_audio(&t, 500, 5000, "E0", 2, 1000));
	TASK_CHECK(!put_audio(&t, 501, 5320, "E1", 2, 1001));

	TASK_CHECK(!get_one(&t, 1000 + TEST_DELAY_US, &h, buf, sizeof(buf),
			    &len, &red));
	TASK_CHECK(h.seq == 500);

	jbuf_flush(t.jb);

	/* Configuration is retained, the anchor is rebuilt */
	TASK_CHECK(get_one(&t, 1000 + TEST_DELAY_US + 40000, &h, NULL, 0,
			   NULL, NULL) == ENOENT);
	TASK_CHECK(!put_audio(&t, 900, 90000, "F0", 2, 2000));
	TASK_CHECK(!get_one(&t, 2000 + TEST_DELAY_US, &h, buf, sizeof(buf),
			    &len, &red));
	TASK_CHECK(h.seq == 900);
	TASK_CHECK(h.ts == 90000);

	red_teardown(&t);
	return 0;
}


/* ------------------------------------------------------------------ */
/* Production end-to-end path: rtp_hdr_encode/decode -> jbuf           */
/* ------------------------------------------------------------------ */

static int test_production_wire_path(void)
{
	struct red_test t;
	struct rtp_header h;
	struct rtp_header decoded;
	struct mbuf *wire;
	struct mbuf *payload;
	bool red;

	TASK_CHECK(!red_setup(&t, 32, 4));

	/* Build a real RTP packet on the wire: header + RED payload */
	wire = mbuf_alloc(128);
	TASK_CHECK(wire);

	{
		struct red_blk r = { 96, 320, "R1", 2 };
		struct red_blk m = { 96, 0, "A2", 2 };
		struct mbuf *pl = make_red(&r, 1, &m);

		TASK_CHECK(pl);
		h = mk_hdr(97, 2002, 32640);
		h.m = 1;
		TASK_CHECK(!rtp_hdr_encode(wire, &h));
		TASK_CHECK(!mbuf_write_mem(wire, mbuf_buf(pl),
					   mbuf_get_left(pl)));
		mem_deref(pl);
	}

	/* First packet on slot 0 as a plain audio packet */
	{
		struct rtp_header h0 = mk_hdr(96, 2000, 32000);
		struct mbuf *w0 = mbuf_alloc(64);

		TASK_CHECK(w0);
		h0.m = 1;
		TASK_CHECK(!rtp_hdr_encode(w0, &h0));
		TASK_CHECK(!mbuf_write_mem(w0, (const uint8_t *)"A0", 2));
		w0->pos = 0;
		TASK_CHECK(!rtp_hdr_decode(&decoded, w0));
		TASK_CHECK(!jbuf_put_red(t.jb, &decoded, w0, 1000));
		mem_deref(w0);
	}

	/* Decode the RED packet from the wire and put it into the jbuf */
	wire->pos = 0;
	TASK_CHECK(!rtp_hdr_decode(&decoded, wire));
	TASK_CHECK(decoded.seq == 2002);
	TASK_CHECK(!jbuf_put_red(t.jb, &decoded, wire, 1001));
	mem_deref(wire);

	/* Slot 0 */
	payload = NULL;
	TASK_CHECK(!jbuf_get_red(t.jb, 1000 + TEST_DELAY_US, &h, &payload,
				 &red));
	TASK_CHECK(!red);
	TASK_CHECK(mbuf_get_left(payload) == 2 &&
		   !memcmp(mbuf_buf(payload), "A0", 2));
	mem_deref(payload);

	/* Slot 1 recovered from RED */
	payload = NULL;
	TASK_CHECK(!jbuf_get_red(t.jb, 1000 + TEST_DELAY_US + 40000, &h,
				 &payload, &red));
	TASK_CHECK(red);
	TASK_CHECK(h.seq == 2001);
	TASK_CHECK(mbuf_get_left(payload) == 2 &&
		   !memcmp(mbuf_buf(payload), "R1", 2));
	mem_deref(payload);

	/* Slot 2 primary */
	payload = NULL;
	TASK_CHECK(!jbuf_get_red(t.jb, 1000 + TEST_DELAY_US + 80000, &h,
				 &payload, &red));
	TASK_CHECK(!red);
	TASK_CHECK(h.seq == 2002);
	TASK_CHECK(mbuf_get_left(payload) == 2 &&
		   !memcmp(mbuf_buf(payload), "A2", 2));
	mem_deref(payload);

	red_teardown(&t);
	return 0;
}


static int test_classic_mode_unchanged(void)
{
	struct jbuf *jb = NULL;
	struct rtp_header h;
	struct mbuf *mb;
	void *payload = NULL;
	bool red = false;

	TASK_CHECK(!jbuf_alloc(&jb, 0, 8));

	mb = make_audio("abc", 3);
	TASK_CHECK(mb);
	h = mk_hdr(96, 65535, 0xffffff00u);
	TASK_CHECK(!jbuf_put(jb, &h, mb));
	mem_deref(mb);

	h = mk_hdr(96, 1, 0xffffff00u + 320);
	mb = make_audio("def", 3);
	TASK_CHECK(mb);
	TASK_CHECK(!jbuf_put(jb, &h, mb));
	mem_deref(mb);

	TASK_CHECK(!jbuf_get(jb, &h, &payload));
	TASK_CHECK(h.seq == 65535);
	payload = mem_deref(payload);

	/* The RED API is not available in classic mode */
	TASK_CHECK(jbuf_get_red(jb, 0, &h, (struct mbuf **)&payload,
				&red) == EINVAL);

	jbuf_flush(jb);
	mem_deref(jb);
	return 0;
}


static int test_mode_misuse(void)
{
	struct red_test t;
	struct rtp_header h = mk_hdr(96, 1, 0);
	struct mbuf *mb = make_audio("zz", 2);
	void *payload = NULL;
	bool red = false;

	TASK_CHECK(mb);

	TASK_CHECK(!red_setup(&t, 8, 2));

	/* Classic put/get must be refused in RED mode without changing state */
	TASK_CHECK(jbuf_put(t.jb, &h, mb) == EINVAL);
	TASK_CHECK(jbuf_get(t.jb, &h, &payload) == EINVAL);
	TASK_CHECK(payload == NULL);

	red_teardown(&t);
	mem_deref(mb);
	return 0;
}


/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{ "red_wire", "decode_basic",             test_wire_decode_basic },
	{ "red_wire", "reject",                   test_wire_reject },

	{ "slot_precedence", "recover_gap",       test_red_recovers_gap },
	{ "slot_precedence", "late_primary",      test_late_primary_replaces_recovered },
	{ "slot_precedence", "first_copy_wins",   test_first_copy_wins },
	{ "slot_precedence", "after_playout",     test_no_replace_after_playout },
	{ "slot_precedence", "reject_no_advance", test_reject_does_not_advance },
	{ "slot_precedence", "bad_primary",       test_bad_primary_leaves_no_copy },
	{ "slot_precedence", "depth_limit",       test_redundancy_depth_limit },
	{ "slot_precedence", "capacity_recover",  test_capacity_reject_then_recover },
	{ "slot_precedence", "duplicate",         test_duplicate_does_not_accumulate },

	{ "playout", "timing_missing",            test_timing_and_missing_slot },
	{ "playout", "wrap",                      test_wrap_sequence_and_timestamp },
	{ "playout", "out_of_order",              test_out_of_order },
	{ "playout", "flush_reanchor",            test_flush_reanchors },
	{ "playout", "production_wire",           test_production_wire_path },
	{ "playout", "classic_mode",              test_classic_mode_unchanged },
	{ "playout", "mode_misuse",               test_mode_misuse },
};

const size_t task_case_count = ARRAY_SIZE(task_cases);
