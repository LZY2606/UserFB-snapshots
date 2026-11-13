// Hand-written RTP byte tests for the RFC6184 interleaved H.264 entry.
// Every test builds complete RTP packets in local buffers (the de-interleaver
// must not retain them past input()), drives the public
// rtp-h264-interleaved.h API and asserts bytes, order, timestamps and counts.

#include "rtp-h264-interleaved.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(cond) do { if (!(cond)) return -(int)(__LINE__); asserts++; } while (0)

// ---------------------------------------------------------------------------
// minimal RTP packet builder
// ---------------------------------------------------------------------------

struct pkt_builder_t
{
	uint8_t buf[512];
	int len;
};

static void pkt_init(struct pkt_builder_t* b, uint16_t seq, uint32_t timestamp)
{
	memset(b->buf, 0, sizeof(b->buf));
	b->buf[0] = 0x80; // V=2
	b->buf[1] = 96;   // PT
	b->buf[2] = (uint8_t)(seq >> 8);
	b->buf[3] = (uint8_t)seq;
	b->buf[4] = (uint8_t)(timestamp >> 24);
	b->buf[5] = (uint8_t)(timestamp >> 16);
	b->buf[6] = (uint8_t)(timestamp >> 8);
	b->buf[7] = (uint8_t)timestamp;
	b->buf[8] = b->buf[9] = b->buf[10] = b->buf[11] = 0;
	b->len = 12;
}

static void pkt_u8(struct pkt_builder_t* b, uint8_t v) { b->buf[b->len++] = v; }
static void pkt_u16(struct pkt_builder_t* b, uint16_t v) { pkt_u8(b, (uint8_t)(v >> 8)); pkt_u8(b, (uint8_t)v); }
static void pkt_bytes(struct pkt_builder_t* b, const void* data, int n) { memcpy(b->buf + b->len, data, n); b->len += n; }

// STAP-B: NAL header(25) + DON, then (size + NAL unit) members.
static void stap_b(struct pkt_builder_t* b, uint16_t don, const uint8_t* const* nals, const int* sizes, int count)
{
	int i;
	pkt_u8(b, 25);
	pkt_u16(b, don);
	for (i = 0; i < count; i++)
	{
		pkt_u16(b, (uint16_t)sizes[i]);
		pkt_bytes(b, nals[i], sizes[i]);
	}
}

// MTAP member: size(2) + DOND(1) + TS offset(2/3) + NAL unit
static void mtap_member(struct pkt_builder_t* b, uint8_t dond, uint32_t offset, int offset_bytes, const uint8_t* nal, int nal_bytes)
{
	pkt_u16(b, (uint16_t)(1 + offset_bytes + nal_bytes));
	pkt_u8(b, dond);
	if (3 == offset_bytes)
		pkt_u8(b, (uint8_t)(offset >> 16));
	pkt_u16(b, (uint16_t)offset);
	pkt_bytes(b, nal, nal_bytes);
}

// FU fragment: indicator + FU header (+ DON for FU-B) + payload
static void fu_packet(struct pkt_builder_t* b, int fu_b, uint8_t nal_ref_idc, uint8_t nal_type, int start, int end, uint16_t don, const uint8_t* payload, int n)
{
	pkt_u8(b, (uint8_t)((nal_ref_idc << 5) | (fu_b ? 29 : 28)));
	pkt_u8(b, (uint8_t)((start ? 0x80 : 0) | (end ? 0x40 : 0) | nal_type));
	if (fu_b)
		pkt_u16(b, don);
	pkt_bytes(b, payload, n);
}

// ---------------------------------------------------------------------------
// callback recorder
// ---------------------------------------------------------------------------

#define RECORD_MAX 16

struct record_t
{
	uint8_t data[128];
	int bytes;
	uint32_t timestamp;
};

struct recorder_t
{
	struct record_t rec[RECORD_MAX];
	int count;
	int fail_after; // when >= 0, callback #(fail_after) returns this error code path
	int fail_code;
};

static int recorder_cb(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
	struct recorder_t* r = (struct recorder_t*)param;
	(void)flags;
	if (r->fail_after >= 0 && r->count == r->fail_after)
		return r->fail_code;
	if (r->count >= RECORD_MAX || bytes > (int)sizeof(r->rec[0].data))
		return -77;
	memcpy(r->rec[r->count].data, packet, bytes); // callback must not depend on caller storage
	r->rec[r->count].bytes = bytes;
	r->rec[r->count].timestamp = timestamp;
	r->count++;
	return 0;
}

static void recorder_reset(struct recorder_t* r)
{
	memset(r, 0, sizeof(*r));
	r->fail_after = -1;
}

// strict in-order DON checker for long sliding-window runs
struct order_check_t
{
	uint16_t expect;
	int count;
	int failed;
};

static int order_cb(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
	const uint8_t* b = (const uint8_t*)packet;
	struct order_check_t* c = (struct order_check_t*)param;
	(void)timestamp;
	(void)flags;
	if (3 != bytes || 0x61 != b[0])
		return -91;
	if (((uint16_t)b[1] << 8 | b[2]) != c->expect)
		c->failed = 1;
	c->expect = (uint16_t)(c->expect + 1);
	c->count++;
	return 0;
}

// ---------------------------------------------------------------------------
// test 1: DON wrap, stable order and depth driven delivery
// ---------------------------------------------------------------------------

int test_h264_don_order(void)
{
	int asserts = 0;
	struct rtp_payload_t handler;
	rtp_h264_interleaved_t* ctx;
	struct pkt_builder_t pkt;
	struct recorder_t rec;
	const uint8_t n102[] = { 0x65, 0xAA }; // NAL type 5
	const uint8_t n100[] = { 0x61, 0xBB }; // NAL type 1
	const uint8_t n101[] = { 0x41, 0xCC }; // NAL type 1, different ref idc
	const uint8_t* nals[1];
	int sizes[1];
	int r;

	recorder_reset(&rec);
	memset(&handler, 0, sizeof(handler));
	handler.packet = recorder_cb;

	// depth=2: DON 102, 100, 101 arrive in that transmission order.
	ctx = rtp_h264_interleaved_create(2, &handler, &rec);
	ASSERT(NULL != ctx);

	nals[0] = n102; sizes[0] = (int)sizeof(n102);
	pkt_init(&pkt, 1, 9000);
	stap_b(&pkt, 102, nals, sizes, 1);
	r = rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len);
	ASSERT(1 == r);
	ASSERT(0 == rec.count); // retained, depth not exceeded

	nals[0] = n100; sizes[0] = (int)sizeof(n100);
	pkt_init(&pkt, 2, 9000);
	stap_b(&pkt, 100, nals, sizes, 1);
	r = rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len);
	ASSERT(1 == r);
	ASSERT(0 == rec.count); // two pending NAL units still fit depth

	nals[0] = n101; sizes[0] = (int)sizeof(n101);
	pkt_init(&pkt, 3, 9000);
	stap_b(&pkt, 101, nals, sizes, 1);
	r = rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len);
	ASSERT(1 == r);
	ASSERT(1 == rec.count); // population 3 > depth 2: earliest DON 100 goes out
	ASSERT(2 == rec.rec[0].bytes);
	ASSERT(0 == memcmp(rec.rec[0].data, n100, 2));
	ASSERT(9000 == rec.rec[0].timestamp);

	r = rtp_h264_interleaved_flush(ctx);
	ASSERT(0 == r);
	ASSERT(3 == rec.count);
	ASSERT(0 == memcmp(rec.rec[1].data, n101, 2)); // DON 101 then 102
	ASSERT(0 == memcmp(rec.rec[2].data, n102, 2));
	ASSERT(rec.rec[1].timestamp == 9000 && rec.rec[2].timestamp == 9000);

	// a NAL older than the delivered DON must be dropped, never rewind
	nals[0] = n100; sizes[0] = (int)sizeof(n100);
	pkt_init(&pkt, 4, 9000);
	stap_b(&pkt, 99, nals, sizes, 1);
	r = rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len);
	ASSERT(1 == r);
	ASSERT(3 == rec.count);
	r = rtp_h264_interleaved_flush(ctx);
	ASSERT(0 == r);
	ASSERT(3 == rec.count); // nothing added, nothing delivered

	rtp_h264_interleaved_destroy(ctx);

	// wrap: DON 0, 65535, 1 must be delivered as 65535 -> 0 -> 1
	recorder_reset(&rec);
	ctx = rtp_h264_interleaved_create(3, &handler, &rec);
	ASSERT(NULL != ctx);

	nals[0] = n102; sizes[0] = 2; pkt_init(&pkt, 10, 9000); stap_b(&pkt, 0, nals, sizes, 1);
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	nals[0] = n100; sizes[0] = 2; pkt_init(&pkt, 11, 9000); stap_b(&pkt, 65535, nals, sizes, 1);
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	nals[0] = n101; sizes[0] = 2; pkt_init(&pkt, 12, 9000); stap_b(&pkt, 1, nals, sizes, 1);
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rec.count);
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(3 == rec.count);
	ASSERT(0 == memcmp(rec.rec[0].data, n100, 2)); // 65535
	ASSERT(0 == memcmp(rec.rec[1].data, n102, 2)); // 0
	ASSERT(0 == memcmp(rec.rec[2].data, n101, 2)); // 1

	// same DON: stable input order, then numeric ordering against another DON
	{
		const uint8_t sa[] = { 0x67, 0x11 };
		const uint8_t sb[] = { 0x67, 0x22 };
		const uint8_t sc[] = { 0x65, 0x33 };
		const uint8_t* pair[2] = { sa, sb };
		int psize[2] = { 2, 2 };
		recorder_reset(&rec);
		rtp_h264_interleaved_destroy(ctx);
		ctx = rtp_h264_interleaved_create(4, &handler, &rec);
		ASSERT(NULL != ctx);
		pkt_init(&pkt, 20, 9000); stap_b(&pkt, 50, pair, psize, 2); // DON 50, 51
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		nals[0] = sc; sizes[0] = 2; pkt_init(&pkt, 21, 9000); stap_b(&pkt, 50, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len)); // DON 50 again
		ASSERT(0 == rtp_h264_interleaved_flush(ctx));
		ASSERT(3 == rec.count);
		ASSERT(0 == memcmp(rec.rec[0].data, sa, 2)); // first DON-50 unit
		ASSERT(0 == memcmp(rec.rec[1].data, sc, 2)); // second DON-50 unit, stable
		ASSERT(0 == memcmp(rec.rec[2].data, sb, 2)); // DON 51
		rtp_h264_interleaved_destroy(ctx);
	}

	// exactly half a circle apart is an error
	{
		recorder_reset(&rec);
		ctx = rtp_h264_interleaved_create(4, &handler, &rec);
		ASSERT(NULL != ctx);
		nals[0] = n102; sizes[0] = 2; pkt_init(&pkt, 30, 9000); stap_b(&pkt, 100, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		pkt_init(&pkt, 31, 9000); stap_b(&pkt, 100 + 0x8000, nals, sizes, 1);
		ASSERT(rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len) < 0);
		rtp_h264_interleaved_destroy(ctx);
	}

	// depth=0 delivers immediately; interleaved-only input is enforced
	{
		recorder_reset(&rec);
		ctx = rtp_h264_interleaved_create(0, &handler, &rec);
		ASSERT(NULL != ctx);
		nals[0] = n102; sizes[0] = 2; pkt_init(&pkt, 40, 9000); stap_b(&pkt, 7, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		ASSERT(1 == rec.count);
		// single NAL unit and STAP-A carry no DON and must be rejected
		pkt_init(&pkt, 41, 9000); pkt_u8(&pkt, 0x65); pkt_u8(&pkt, 0x01);
		ASSERT(rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len) < 0);
		pkt_init(&pkt, 42, 9000); pkt_u8(&pkt, 24);
		pkt_u16(&pkt, 2); pkt_bytes(&pkt, n100, 2);
		ASSERT(rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len) < 0);
		rtp_h264_interleaved_destroy(ctx);
	}

	// malformed aggregation publishes nothing
	{
		recorder_reset(&rec);
		ctx = rtp_h264_interleaved_create(2, &handler, &rec);
		ASSERT(NULL != ctx);
		pkt_init(&pkt, 50, 9000);
		pkt_u8(&pkt, 25); pkt_u16(&pkt, 60);
		pkt_u16(&pkt, 2); pkt_bytes(&pkt, n100, 2);          // valid first member
		pkt_u16(&pkt, 9); pkt_bytes(&pkt, n101, 2);          // declared 9, only 2 available
		ASSERT(rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len) < 0);
		ASSERT(0 == rec.count);
		ASSERT(0 == rtp_h264_interleaved_flush(ctx));
		ASSERT(0 == rec.count);
		rtp_h264_interleaved_destroy(ctx);
	}

	// callback error propagates unchanged and stops later callbacks
	{
		recorder_reset(&rec);
		rec.fail_after = 0;
		rec.fail_code = -123;
		ctx = rtp_h264_interleaved_create(0, &handler, &rec);
		ASSERT(NULL != ctx);
		nals[0] = n102; sizes[0] = 2; pkt_init(&pkt, 60, 9000); stap_b(&pkt, 1, nals, sizes, 1);
		ASSERT(-123 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		ASSERT(0 == rec.count); // failing callback records nothing
		rtp_h264_interleaved_destroy(ctx);
	}

	ASSERT(NULL == rtp_h264_interleaved_create(32768, &handler, &rec));
	ASSERT(NULL == rtp_h264_interleaved_create(-1, &handler, &rec));

	// half-circle crossing while the delivery window slides forward over the wrap
	{
		struct order_check_t check;
		uint8_t nal[3];
		int i;
		check.expect = 65530;
		check.count = 0;
		check.failed = 0;
		memset(&handler, 0, sizeof(handler));
		handler.packet = order_cb;
		ctx = rtp_h264_interleaved_create(2, &handler, &check);
		ASSERT(NULL != ctx);
		// ascending DONs starting at 65530: the anchor slides across 0 while
		// every delivery must stay strictly in DON order
		for (i = 0; i < 40; i++)
		{
			uint16_t don = (uint16_t)(65530 + i);
			nal[0] = 0x61;
			nal[1] = (uint8_t)(don >> 8);
			nal[2] = (uint8_t)don;
			nals[0] = nal; sizes[0] = 3;
			pkt_init(&pkt, (uint16_t)(100 + i), 9000);
			stap_b(&pkt, don, nals, sizes, 1);
			ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		}
		ASSERT(0 == rtp_h264_interleaved_flush(ctx));
		ASSERT(40 == check.count);
		ASSERT(0 == check.failed); // 65530..65535 then 0..33, no half-circle failure
		rtp_h264_interleaved_destroy(ctx);
		memset(&handler, 0, sizeof(handler));
		handler.packet = recorder_cb;
	}

	// late NAL after wrap-around delivery point is dropped
	{
		const uint8_t la[] = { 0x65, 0xD1 };
		const uint8_t lb[] = { 0x61, 0xD2 };
		const uint8_t lc[] = { 0x41, 0xD3 };
		recorder_reset(&rec);
		ctx = rtp_h264_interleaved_create(0, &handler, &rec);
		ASSERT(NULL != ctx);
		nals[0] = la; sizes[0] = 2; pkt_init(&pkt, 1, 9000); stap_b(&pkt, 5, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		nals[0] = lb; sizes[0] = 2; pkt_init(&pkt, 2, 9000); stap_b(&pkt, 6, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		// DON 4 is one step behind DON 5 across a turn boundary, must be dropped
		nals[0] = lc; sizes[0] = 2; pkt_init(&pkt, 3, 9000); stap_b(&pkt, 4, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		// half circle ahead of the delivered DON is also an error
		pkt_init(&pkt, 4, 9000); stap_b(&pkt, 6 + 0x8000, nals, sizes, 1);
		ASSERT(rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len) < 0);
		ASSERT(2 == rec.count);
		rtp_h264_interleaved_destroy(ctx);
	}

	// destroy releases retained NAL units without invoking any callback
	{
		recorder_reset(&rec);
		ctx = rtp_h264_interleaved_create(3, &handler, &rec);
		ASSERT(NULL != ctx);
		nals[0] = n102; sizes[0] = 2; pkt_init(&pkt, 1, 9000); stap_b(&pkt, 1, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		nals[0] = n100; sizes[0] = 2; pkt_init(&pkt, 2, 9000); stap_b(&pkt, 2, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		rtp_h264_interleaved_destroy(ctx);
		ASSERT(0 == rec.count);
		rtp_h264_interleaved_destroy(NULL); // must be a no-op
	}

	// retained bytes must not alias the caller's RTP packet buffer
	{
		const uint8_t keep[] = { 0x65, 0xAB, 0xCD };
		recorder_reset(&rec);
		ctx = rtp_h264_interleaved_create(1, &handler, &rec);
		ASSERT(NULL != ctx);
		nals[0] = keep; sizes[0] = (int)sizeof(keep);
		pkt_init(&pkt, 1, 9000); stap_b(&pkt, 9, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		memset(pkt.buf, 0xEE, sizeof(pkt.buf)); // destroy every input byte
		ASSERT(0 == rtp_h264_interleaved_flush(ctx));
		ASSERT(1 == rec.count);
		ASSERT(0 == memcmp(rec.rec[0].data, keep, sizeof(keep)));
		rtp_h264_interleaved_destroy(ctx);
	}

	// flush stops at the first callback error and keeps the remaining NAL units
	{
		const uint8_t fa[] = { 0x67, 0xF1 };
		const uint8_t fb[] = { 0x61, 0xF2 };
		const uint8_t fc[] = { 0x65, 0xF3 };
		recorder_reset(&rec);
		ctx = rtp_h264_interleaved_create(3, &handler, &rec);
		ASSERT(NULL != ctx);
		nals[0] = fa; sizes[0] = 2; pkt_init(&pkt, 1, 9000); stap_b(&pkt, 1, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		nals[0] = fb; sizes[0] = 2; pkt_init(&pkt, 2, 9000); stap_b(&pkt, 2, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		nals[0] = fc; sizes[0] = 2; pkt_init(&pkt, 3, 9000); stap_b(&pkt, 3, nals, sizes, 1);
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		rec.fail_after = 1; // second flush callback fails
		rec.fail_code = -45;
		ASSERT(-45 == rtp_h264_interleaved_flush(ctx));
		ASSERT(1 == rec.count); // only the first NAL was delivered
		rec.fail_after = -1;
		ASSERT(0 == rtp_h264_interleaved_flush(ctx)); // retry delivers the rest
		ASSERT(3 == rec.count);
		rtp_h264_interleaved_destroy(ctx);
	}

	return asserts;
}

// ---------------------------------------------------------------------------
// test 2: FU-B/FU-A reassembly, lost fragments and later healthy NAL units
// ---------------------------------------------------------------------------

int test_h264_interleaved_fragments(void)
{
	int asserts = 0;
	struct rtp_payload_t handler;
	rtp_h264_interleaved_t* ctx;
	struct pkt_builder_t pkt;
	struct recorder_t rec;
	const uint8_t part_a[] = { 0x10, 0x11, 0x12 };
	const uint8_t part_b[] = { 0x20, 0x21 };
	const uint8_t part_c[] = { 0x30, 0x31, 0x32, 0x33 };
	const uint8_t standalone[] = { 0x65, 0x77 };
	const uint8_t* one[1];
	int one_size[1];
	uint8_t expect[1 + 3 + 2 + 4];

	recorder_reset(&rec);
	memset(&handler, 0, sizeof(handler));
	handler.packet = recorder_cb;
	ctx = rtp_h264_interleaved_create(2, &handler, &rec);
	ASSERT(NULL != ctx);

	// FU-B start (DON 300) + two FU-A continuations, seq 1..3, same ts/type
	pkt_init(&pkt, 1, 5000);
	fu_packet(&pkt, 1, 3, 5, 1, 0, 300, part_a, (int)sizeof(part_a));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rec.count); // fragment incomplete, never delivered early

	pkt_init(&pkt, 2, 5000);
	fu_packet(&pkt, 0, 3, 5, 0, 0, 0, part_b, (int)sizeof(part_b));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rec.count);

	pkt_init(&pkt, 3, 5000);
	fu_packet(&pkt, 0, 3, 5, 0, 1, 0, part_c, (int)sizeof(part_c));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rec.count); // retained by depth, flush must yield the rebuilt NAL

	expect[0] = (uint8_t)((3 << 5) | 5);
	memcpy(expect + 1, part_a, 3);
	memcpy(expect + 4, part_b, 2);
	memcpy(expect + 6, part_c, 4);
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(1 == rec.count);
	ASSERT((int)sizeof(expect) == rec.rec[0].bytes);
	ASSERT(0 == memcmp(rec.rec[0].data, expect, sizeof(expect)));
	ASSERT(5000 == rec.rec[0].timestamp);

	// seq 65535 -> 0 continuity
	recorder_reset(&rec);
	rtp_h264_interleaved_destroy(ctx);
	ctx = rtp_h264_interleaved_create(2, &handler, &rec);
	pkt_init(&pkt, 65535, 6000);
	fu_packet(&pkt, 1, 2, 1, 1, 0, 400, part_a, (int)sizeof(part_a));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	pkt_init(&pkt, 0, 6000);
	fu_packet(&pkt, 0, 2, 1, 0, 1, 0, part_b, (int)sizeof(part_b));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(1 == rec.count);
	ASSERT(1 + 3 + 2 == rec.rec[0].bytes);
	ASSERT(6000 == rec.rec[0].timestamp);

	// lost continuation: gap in sequence number discards the fragment,
	// the reorder queue must still accept later complete NAL units
	recorder_reset(&rec);
	rtp_h264_interleaved_destroy(ctx);
	ctx = rtp_h264_interleaved_create(2, &handler, &rec);
	pkt_init(&pkt, 10, 7000);
	fu_packet(&pkt, 1, 3, 5, 1, 0, 500, part_a, (int)sizeof(part_a));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	pkt_init(&pkt, 12, 7000); // seq 11 missing
	fu_packet(&pkt, 0, 3, 5, 0, 1, 0, part_b, (int)sizeof(part_b));
	ASSERT(0 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len)); // discarded
	one[0] = standalone; one_size[0] = (int)sizeof(standalone);
	pkt_init(&pkt, 13, 7000);
	stap_b(&pkt, 501, one, one_size, 1);
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(1 == rec.count); // only the healthy STAP-B NAL survives
	ASSERT(0 == memcmp(rec.rec[0].data, standalone, sizeof(standalone)));

	// timestamp / NAL type mismatch also breaks the fragment
	recorder_reset(&rec);
	rtp_h264_interleaved_destroy(ctx);
	ctx = rtp_h264_interleaved_create(2, &handler, &rec);
	pkt_init(&pkt, 20, 8000);
	fu_packet(&pkt, 1, 3, 5, 1, 0, 600, part_a, (int)sizeof(part_a));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	pkt_init(&pkt, 21, 8001); // timestamp changed
	fu_packet(&pkt, 0, 3, 5, 0, 1, 0, part_b, (int)sizeof(part_b));
	ASSERT(0 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(0 == rec.count);

	// a new FU-B start replaces the unfinished fragment
	recorder_reset(&rec);
	rtp_h264_interleaved_destroy(ctx);
	ctx = rtp_h264_interleaved_create(2, &handler, &rec);
	pkt_init(&pkt, 30, 9000);
	fu_packet(&pkt, 1, 3, 5, 1, 0, 700, part_a, (int)sizeof(part_a));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	pkt_init(&pkt, 31, 9000);
	fu_packet(&pkt, 1, 3, 6, 1, 0, 701, part_b, (int)sizeof(part_b)); // new start
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	pkt_init(&pkt, 32, 9000);
	fu_packet(&pkt, 0, 3, 6, 0, 1, 0, part_c, (int)sizeof(part_c));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(1 == rec.count);
	ASSERT(1 + 2 + 4 == rec.rec[0].bytes); // second fragment only
	ASSERT((uint8_t)((3 << 5) | 6) == rec.rec[0].data[0]);

	// FU-A start (no DON) and FU-B non-start are rejected
	pkt_init(&pkt, 40, 9000);
	fu_packet(&pkt, 0, 3, 5, 1, 0, 0, part_a, (int)sizeof(part_a));
	ASSERT(rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len) < 0);
	pkt_init(&pkt, 41, 9000);
	fu_packet(&pkt, 1, 3, 5, 0, 0, 800, part_a, (int)sizeof(part_a));
	ASSERT(rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len) < 0);

	// orphan FU-A with no active fragment is discarded, not an error
	recorder_reset(&rec);
	rtp_h264_interleaved_destroy(ctx);
	ctx = rtp_h264_interleaved_create(2, &handler, &rec);
	pkt_init(&pkt, 50, 9000);
	fu_packet(&pkt, 0, 3, 5, 0, 1, 0, part_a, (int)sizeof(part_a));
	ASSERT(0 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));

	// FU-B carrying a non-fragmentable NAL type is rejected without asserts
	pkt_init(&pkt, 53, 9000);
	fu_packet(&pkt, 1, 0, 24, 1, 0, 901, part_a, (int)sizeof(part_a));
	ASSERT(rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len) < 0);

	// an unfinished FU never counts as a complete NAL unit, even at flush
	pkt_init(&pkt, 51, 9000);
	fu_packet(&pkt, 1, 3, 5, 1, 0, 900, part_a, (int)sizeof(part_a));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	pkt_init(&pkt, 52, 9000);
	fu_packet(&pkt, 0, 3, 5, 0, 0, 0, part_b, (int)sizeof(part_b));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len)); // still no end bit
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(0 == rec.count);
	rtp_h264_interleaved_destroy(ctx);
	return asserts;
}

// ---------------------------------------------------------------------------
// test 3: MTAP16/MTAP24 DON derivation, member bytes and 32-bit time wrap
// ---------------------------------------------------------------------------

int test_h264_mtap_timestamps(void)
{
	int asserts = 0;
	struct rtp_payload_t handler;
	rtp_h264_interleaved_t* ctx;
	struct pkt_builder_t pkt;
	struct recorder_t rec;
	const uint8_t m1[] = { 0x67, 0xA1 };
	const uint8_t m2[] = { 0x61, 0xB2, 0xB3 };
	const uint8_t m3[] = { 0x65, 0xC4 };

	recorder_reset(&rec);
	memset(&handler, 0, sizeof(handler));
	handler.packet = recorder_cb;
	ctx = rtp_h264_interleaved_create(4, &handler, &rec);
	ASSERT(NULL != ctx);

	// MTAP16: DONB=1000; members DOND 2, 0, 1 -> DON 1002, 1000, 1001.
	// packet timestamp near the 32-bit boundary to exercise time wrap.
	pkt_init(&pkt, 1, 0xFFFFFFF0u);
	pkt_u8(&pkt, 26);
	pkt_u16(&pkt, 1000);
	mtap_member(&pkt, 2, 0x20, 2, m1, (int)sizeof(m1));
	mtap_member(&pkt, 0, 0x10, 2, m2, (int)sizeof(m2));
	mtap_member(&pkt, 1, 0x40, 2, m3, (int)sizeof(m3));
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rec.count);
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(3 == rec.count);

	// delivery follows DON 1000 (m2), 1001 (m3), 1002 (m1), never the offsets
	ASSERT(0 == memcmp(rec.rec[0].data, m2, sizeof(m2)));
	ASSERT((int)sizeof(m2) == rec.rec[0].bytes);
	ASSERT(0xFFFFFFF0u + 0x10 == rec.rec[0].timestamp); // wraps to 0
	ASSERT(0 == memcmp(rec.rec[1].data, m3, sizeof(m3)));
	ASSERT(0xFFFFFFF0u + 0x40 == rec.rec[1].timestamp);
	ASSERT(0 == memcmp(rec.rec[2].data, m1, sizeof(m1)));
	ASSERT(0xFFFFFFF0u + 0x20 == rec.rec[2].timestamp);

	// MTAP24: 24-bit offset and DOND wrap around the 16-bit DON circle
	recorder_reset(&rec);
	rtp_h264_interleaved_destroy(ctx);
	ctx = rtp_h264_interleaved_create(4, &handler, &rec);
	ASSERT(NULL != ctx);
	pkt_init(&pkt, 2, 0x80000010u);
	pkt_u8(&pkt, 27);
	pkt_u16(&pkt, 65534); // DONB
	mtap_member(&pkt, 5, 0x010203, 3, m1, (int)sizeof(m1)); // DON 3
	mtap_member(&pkt, 1, 0x000040, 3, m2, (int)sizeof(m2)); // DON 65535
	ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(2 == rec.count);
	ASSERT(0 == memcmp(rec.rec[0].data, m2, sizeof(m2))); // DON 65535 before DON 3
	ASSERT(0 == memcmp(rec.rec[1].data, m1, sizeof(m1)));
	ASSERT(0x80000010u + 0x000040u == rec.rec[0].timestamp);
	ASSERT(0x80000010u + 0x010203u == rec.rec[1].timestamp);

	// malformed MTAP member rejects the whole packet atomically
	recorder_reset(&rec);
	rtp_h264_interleaved_destroy(ctx);
	ctx = rtp_h264_interleaved_create(4, &handler, &rec);
	pkt_init(&pkt, 3, 100);
	pkt_u8(&pkt, 26);
	pkt_u16(&pkt, 900);
	mtap_member(&pkt, 0, 0x10, 2, m1, (int)sizeof(m1)); // valid
	pkt_u16(&pkt, 9);                                        // truncated member header
	ASSERT(rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len) < 0);
	ASSERT(0 == rec.count);
	ASSERT(0 == rtp_h264_interleaved_flush(ctx));
	ASSERT(0 == rec.count);

	// equal DOND gives equal DON: members keep their input (stable) order
	{
		const uint8_t s1[] = { 0x67, 0xE1 };
		const uint8_t s2[] = { 0x65, 0xE2 };
		const uint8_t s3[] = { 0x61, 0xE3 };
		recorder_reset(&rec);
		rtp_h264_interleaved_destroy(ctx);
		ctx = rtp_h264_interleaved_create(4, &handler, &rec);
		ASSERT(NULL != ctx);
		pkt_init(&pkt, 4, 200);
		pkt_u8(&pkt, 26);
		pkt_u16(&pkt, 300); // DONB
		mtap_member(&pkt, 0, 0x10, 2, s1, (int)sizeof(s1)); // DON 300
		mtap_member(&pkt, 0, 0x20, 2, s2, (int)sizeof(s2)); // DON 300 again
		mtap_member(&pkt, 1, 0x30, 2, s3, (int)sizeof(s3)); // DON 301
		ASSERT(1 == rtp_h264_interleaved_input(ctx, pkt.buf, pkt.len));
		ASSERT(0 == rtp_h264_interleaved_flush(ctx));
		ASSERT(3 == rec.count);
		ASSERT(0 == memcmp(rec.rec[0].data, s1, sizeof(s1)));
		ASSERT(0 == memcmp(rec.rec[1].data, s2, sizeof(s2)));
		ASSERT(0 == memcmp(rec.rec[2].data, s3, sizeof(s3)));
		ASSERT(200 + 0x10 == rec.rec[0].timestamp);
		ASSERT(200 + 0x20 == rec.rec[1].timestamp);
		ASSERT(200 + 0x30 == rec.rec[2].timestamp);
		// DOND/offset bytes never leak into the delivered NAL units
		ASSERT((int)sizeof(s1) == rec.rec[0].bytes);
		ASSERT((int)sizeof(s2) == rec.rec[1].bytes);
		ASSERT((int)sizeof(s3) == rec.rec[2].bytes);
	}

	rtp_h264_interleaved_destroy(ctx);
	return asserts;
}
