// RFC 4175 raw video (progressive YCbCr 4:2:2, 8-bit/10-bit) tests
// wire_layout:     hand-checked wire bytes, both encode and decode direction
// frame_recovery:  out-of-order/retransmit/late packets, callback contracts
// loss_clock:      16/32-bit sequence and timestamp wrap, packet loss flag

#include "rtp-raw.h"
#include "rtp-payload.h"
#include "rtp-packet.h"
#include "task_support/check.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CAP_MAX 64
#define CAP_PKT 2048

// encoder output capture
struct cap_t
{
	struct rtp_payload_t h;
	uint8_t data[CAP_MAX][CAP_PKT];
	int len[CAP_MAX];
	uint32_t ts[CAP_MAX];
	int flags[CAP_MAX];
	int count;
	int fail_after; // fail the (fail_after)-th packet(0-based) with err, -1 never
	int err;
};

static void* t_alloc(void* p, int n) { (void)p; return malloc((size_t)n); }
static void t_free(void* p, void* m) { (void)p; free(m); }

static int cap_packet(void* p, const void* data, int n, uint32_t ts, int flags)
{
	struct cap_t* c = (struct cap_t*)p;
	if (c->fail_after >= 0 && c->count == c->fail_after)
		return c->err;
	if (c->count >= CAP_MAX || n > CAP_PKT)
		return -1;
	memcpy(c->data[c->count], data, (size_t)n);
	c->len[c->count] = n;
	c->ts[c->count] = ts;
	c->flags[c->count] = flags;
	c->count++;
	return 0;
}

static void cap_init(struct cap_t* c)
{
	memset(c, 0, sizeof(*c));
	c->h.alloc = t_alloc;
	c->h.free = t_free;
	c->h.packet = cap_packet;
	c->fail_after = -1;
}

// decoder output capture
#define RX_MAX 8
#define RX_BYTES 4096

struct rx_t
{
	struct rtp_payload_t h;
	uint8_t frame[RX_MAX][RX_BYTES];
	int len[RX_MAX];
	uint32_t ts[RX_MAX];
	int flags[RX_MAX];
	int count;
	int ret; // callback return value
};

static int rx_packet(void* p, const void* data, int n, uint32_t ts, int flags)
{
	struct rx_t* r = (struct rx_t*)p;
	if (r->count < RX_MAX && n <= RX_BYTES)
	{
		memcpy(r->frame[r->count], data, (size_t)n);
		r->len[r->count] = n;
		r->ts[r->count] = ts;
		r->flags[r->count] = flags;
	}
	r->count++;
	return r->ret;
}

static void rx_init(struct rx_t* r)
{
	memset(r, 0, sizeof(*r));
	r->h.alloc = t_alloc;
	r->h.free = t_free;
	r->h.packet = rx_packet;
}

// build a raw-video RTP packet by hand: RTP header + extended sequence + payload
static int make_rtp(uint8_t* out, uint8_t pt, int marker, uint32_t seq32, uint32_t ts, uint32_t ssrc, const uint8_t* payload, int n)
{
	out[0] = 0x80; // V=2
	out[1] = (uint8_t)((marker ? 0x80 : 0) | pt);
	out[2] = (uint8_t)(seq32 >> 8); // low 16 bits
	out[3] = (uint8_t)seq32;
	out[4] = (uint8_t)(ts >> 24);
	out[5] = (uint8_t)(ts >> 16);
	out[6] = (uint8_t)(ts >> 8);
	out[7] = (uint8_t)ts;
	out[8] = (uint8_t)(ssrc >> 24);
	out[9] = (uint8_t)(ssrc >> 16);
	out[10] = (uint8_t)(ssrc >> 8);
	out[11] = (uint8_t)ssrc;
	out[12] = (uint8_t)(seq32 >> 24); // extended sequence number, high 16 bits
	out[13] = (uint8_t)(seq32 >> 16);
	memcpy(out + 14, payload, (size_t)n);
	return 14 + n;
}

static void gen_frame(uint8_t* f, int n, unsigned int seed)
{
	int i;
	for (i = 0; i < n; i++)
		f[i] = (uint8_t)(seed + (unsigned int)i * 7);
}

// ---------------------------------------------------------------------------
// wire_layout
// ---------------------------------------------------------------------------

// 10-bit pgroup(5 bytes per 2 pixels), two lines chained in one packet
static int wl_ten_bit_pgroup(void)
{
	struct rtp_raw_format fmt = { 4, 2, 10, 0 };
	struct cap_t cap;
	void* enc;
	int i;
	uint8_t frame[20];
	static const uint8_t expect[] = {
		0x80, 0xE0, 0x00, 0x02, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, // RTP header, M=1 PT=96 seq=2
		0x00, 0x01, // extended sequence number, high 16 bits
		0x00, 0x0A, 0x00, 0x00, 0x80, 0x00, // Length=10, F=0 Line=0, C=1 Offset=0
		0x00, 0x0A, 0x00, 0x01, 0x00, 0x00, // Length=10, F=0 Line=1, C=0 Offset=0
	};

	cap_init(&cap);
	for (i = 0; i < 20; i++)
		frame[i] = (uint8_t)(0x40 + i);
	enc = rtp_raw_encode_create(&fmt, 1500, 96, 0x00010002, 0x11223344, &cap.h, &cap);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame, sizeof(frame), 0x01020304));
	TASK_CHECK(cap.count == 1);
	TASK_CHECK(cap.len[0] == 12 + 2 + 12 + 20);
	TASK_CHECK(cap.ts[0] == 0x01020304);
	TASK_CHECK(0 == memcmp(cap.data[0], expect, sizeof(expect)));
	TASK_CHECK(0 == memcmp(cap.data[0] + sizeof(expect), frame, sizeof(frame))); // packed lines as-is
	rtp_raw_encode_destroy(enc);
	return 0;
}

// a line split across packets: C chain, nonzero line_start, pixel unit offset
static int wl_c_chain_offset(void)
{
	struct rtp_raw_format fmt = { 8, 3, 8, 100 };
	struct cap_t cap;
	void* enc;
	int i;
	uint8_t frame[48];

	cap_init(&cap);
	for (i = 0; i < 48; i++)
		frame[i] = (uint8_t)i;
	enc = rtp_raw_encode_create(&fmt, 28, 97, 10, 0x01020304, &cap.h, &cap);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame, sizeof(frame), 0xAABBCCDD));
	TASK_CHECK(cap.count == 6); // 4 pgroups per line, 2 pgroups per packet
	for (i = 0; i < 6; i++)
	{
		const uint8_t* p = cap.data[i];
		TASK_CHECK(cap.len[i] == 28);
		TASK_CHECK(cap.ts[i] == 0xAABBCCDD);
		TASK_CHECK(p[0] == 0x80);
		TASK_CHECK(p[1] == (uint8_t)((5 == i ? 0x80 : 0) | 97)); // marker on the last packet only
		TASK_CHECK(p[2] == 0 && p[3] == 10 + i); // RTP sequence, low 16 bits
		TASK_CHECK(0 == memcmp(p + 4, "\xAA\xBB\xCC\xDD", 4));
		TASK_CHECK(0 == memcmp(p + 8, "\x01\x02\x03\x04", 4));
		TASK_CHECK(p[12] == 0 && p[13] == 0); // extended sequence, high 16 bits
		TASK_CHECK(p[14] == 0 && p[15] == 8); // Length=8(2 pgroups)
		TASK_CHECK(p[16] == 0 && p[17] == 100 + i / 2); // F=0, Line No = line_start+y
		TASK_CHECK(p[18] == 0 && p[19] == (i % 2) * 4); // C=0, Offset in pixels(not bytes)
		TASK_CHECK(0 == memcmp(p + 20, frame + (i / 2) * 16 + (i % 2) * 8, 8));
	}
	rtp_raw_encode_destroy(enc);
	return 0;
}

// small mtu: several whole lines per packet, never one-line-per-packet
static int wl_multi_line_packing(void)
{
	struct rtp_raw_format fmt = { 4, 4, 8, 0 };
	struct cap_t cap;
	void* enc;
	int i;
	uint8_t frame[32];

	cap_init(&cap);
	for (i = 0; i < 32; i++)
		frame[i] = (uint8_t)(0xA0 + i);
	enc = rtp_raw_encode_create(&fmt, 42, 96, 100, 0x55667788, &cap.h, &cap);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame, sizeof(frame), 0x00001000));
	TASK_CHECK(cap.count == 2); // 2 lines per packet
	for (i = 0; i < 2; i++)
	{
		const uint8_t* p = cap.data[i];
		TASK_CHECK(cap.len[i] <= 42 && cap.len[i] == 42);
		TASK_CHECK(p[1] == (uint8_t)((1 == i ? 0x80 : 0) | 96));
		TASK_CHECK(p[2] == 0 && p[3] == 100 + i);
		TASK_CHECK(p[12] == 0 && p[13] == 0);
		TASK_CHECK(p[14] == 0 && p[15] == 8); // Length=8
		TASK_CHECK(p[16] == 0 && p[17] == 2 * i); // F=0, Line No of the first segment
		TASK_CHECK(p[18] == 0x80 && p[19] == 0); // C=1, Offset=0
		TASK_CHECK(0 == memcmp(p + 20, "\x00\x08\x00", 3)); // Length=8
		TASK_CHECK(p[23] == 2 * i + 1); // Line No of the second segment
		TASK_CHECK(p[24] == 0 && p[25] == 0); // C=0, Offset=0
		TASK_CHECK(0 == memcmp(p + 26, frame + i * 16, 16));
	}
	rtp_raw_encode_destroy(enc);
	return 0;
}

// hand-written wire packets into the decoder: 10-bit pgroup, C chain,
// nonzero line_start, pixel offset; plus 8-bit pixel offset -> byte offset
static int wl_decode_handcrafted(void)
{
	struct rtp_raw_format fmt = { 4, 2, 10, 50 };
	struct rx_t rx;
	void* dec;
	uint8_t pkt[64], pl[64], expect[20];
	int i, n;

	rx_init(&rx);
	dec = rtp_raw_decode_create(&fmt, 96, 0x99887766, &rx.h, &rx);
	TASK_CHECK(dec != NULL);

	// packet seq=8(marker) first: line 50 offset 2 pixels(1 pgroup), then line 51 whole
	pl[0] = 0x00; pl[1] = 0x05; pl[2] = 0x00; pl[3] = 0x32; pl[4] = 0x80; pl[5] = 0x02; // Length=5, Line=50, C=1, Offset=2
	pl[6] = 0x00; pl[7] = 0x0A; pl[8] = 0x00; pl[9] = 0x33; pl[10] = 0x00; pl[11] = 0x00; // Length=10, Line=51, C=0
	for (i = 0; i < 5; i++)
		pl[12 + i] = (uint8_t)(6 + i); // line 50, second pgroup
	for (i = 0; i < 10; i++)
		pl[17 + i] = (uint8_t)(11 + i); // line 51
	n = make_rtp(pkt, 96, 1, 8, 777, 0x99887766, pl, 27);
	TASK_CHECK(1 == rtp_raw_decode_input(dec, pkt, n));
	TASK_CHECK(rx.count == 0);

	// packet seq=7: line 50 offset 0(1 pgroup)
	pl[0] = 0x00; pl[1] = 0x05; pl[2] = 0x00; pl[3] = 0x32; pl[4] = 0x00; pl[5] = 0x00;
	for (i = 0; i < 5; i++)
		pl[6 + i] = (uint8_t)(1 + i); // line 50, first pgroup
	n = make_rtp(pkt, 96, 0, 7, 777, 0x99887766, pl, 11);
	TASK_CHECK(1 == rtp_raw_decode_input(dec, pkt, n));
	TASK_CHECK(rx.count == 1);
	TASK_CHECK(rx.len[0] == 20 && rx.ts[0] == 777 && rx.flags[0] == 0);
	for (i = 0; i < 20; i++)
		expect[i] = (uint8_t)(1 + i);
	TASK_CHECK(0 == memcmp(rx.frame[0], expect, sizeof(expect)));
	rtp_raw_decode_destroy(dec);

	// 8-bit: Offset=4 pixels must land at byte 8 of the line
	{
		struct rtp_raw_format fmt2 = { 6, 1, 8, 0 };
		void* dec2;
		rx_init(&rx);
		dec2 = rtp_raw_decode_create(&fmt2, 96, 0x55, &rx.h, &rx);
		TASK_CHECK(dec2 != NULL);
		pl[0] = 0x00; pl[1] = 0x08; pl[2] = 0x00; pl[3] = 0x00; pl[4] = 0x00; pl[5] = 0x00; // Length=8, Offset=0
		for (i = 0; i < 8; i++)
			pl[6 + i] = (uint8_t)(0x30 + i);
		n = make_rtp(pkt, 96, 0, 0x00010002, 0xFFFF0000, 0x55, pl, 14);
		TASK_CHECK(1 == rtp_raw_decode_input(dec2, pkt, n));
		pl[0] = 0x00; pl[1] = 0x04; pl[2] = 0x00; pl[3] = 0x00; pl[4] = 0x00; pl[5] = 0x04; // Length=4, Offset=4 pixels
		for (i = 0; i < 4; i++)
			pl[6 + i] = (uint8_t)(0x38 + i);
		n = make_rtp(pkt, 96, 1, 0x00010003, 0xFFFF0000, 0x55, pl, 10);
		TASK_CHECK(1 == rtp_raw_decode_input(dec2, pkt, n));
		TASK_CHECK(rx.count == 1 && rx.len[0] == 12);
		for (i = 0; i < 12; i++)
			expect[i] = (uint8_t)(0x30 + i);
		TASK_CHECK(0 == memcmp(rx.frame[0], expect, 12));
		rtp_raw_decode_destroy(dec2);
	}
	return 0;
}

// unsupported configurations are rejected, mtu/frame length are enforced
static int wl_create_validate(void)
{
	struct rtp_raw_format fmt = { 640, 480, 8, 0 };
	struct cap_t cap;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame[4];

	cap_init(&cap);
	rx_init(&rx);

	// mtu must hold RTP header + extended sequence + one line header + one pgroup
	TASK_CHECK(NULL == rtp_raw_encode_create(&fmt, 23, 96, 0, 1, &cap.h, &cap));
	enc = rtp_raw_encode_create(&fmt, 24, 96, 0, 1, &cap.h, &cap);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 > rtp_raw_encode_input(enc, frame, sizeof(frame), 0)); // wrong frame length
	rtp_raw_encode_destroy(enc);

	{
		struct rtp_raw_format f10 = { 640, 480, 10, 0 };
		TASK_CHECK(NULL == rtp_raw_encode_create(&f10, 24, 96, 0, 1, &cap.h, &cap)); // 10-bit needs 25
		enc = rtp_raw_encode_create(&f10, 25, 96, 0, 1, &cap.h, &cap);
		TASK_CHECK(enc != NULL);
		rtp_raw_encode_destroy(enc);
	}

	// unsupported formats
	{
		static const struct rtp_raw_format bad[] = {
			{ 640, 480, 9, 0 }, // depth
			{ 639, 480, 8, 0 }, // odd width
			{ 0, 480, 8, 0 }, // width
			{ 32770, 480, 8, 0 }, // width > 32768
			{ 640, 0, 8, 0 }, // height
			{ 2, 2, 8, 32767 }, // line_start+height-1 > 32767
			{ 32768, 30000, 10, 0 }, // frame bytes overflow int
		};
		size_t i;
		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		{
			TASK_CHECK(NULL == rtp_raw_encode_create(&bad[i], 1500, 96, 0, 1, &cap.h, &cap));
			TASK_CHECK(NULL == rtp_raw_decode_create(&bad[i], 96, 1, &rx.h, &rx));
		}
	}

	// boundary formats are accepted
	{
		struct rtp_raw_format edge[] = {
			{ 2, 1, 8, 32767 }, // last possible line
			{ 32768, 26000, 10, 0 }, // 2129920000 bytes, still a positive int
			{ 2, 1, 10, 0 },
		};
		size_t i;
		for (i = 0; i < sizeof(edge) / sizeof(edge[0]); i++)
		{
			enc = rtp_raw_encode_create(&edge[i], 1500, 96, 0, 1, &cap.h, &cap);
			TASK_CHECK(enc != NULL);
			rtp_raw_encode_destroy(enc);
		}
		dec = rtp_raw_decode_create(&edge[0], 96, 1, &rx.h, &rx);
		TASK_CHECK(dec != NULL);
		rtp_raw_decode_destroy(dec);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// frame_recovery
// ---------------------------------------------------------------------------

// packets of one frame arrive out of order, marker packet first
static int fr_reorder_marker_first(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 0 };
	struct cap_t cap;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame[64];
	int i;

	cap_init(&cap);
	rx_init(&rx);
	gen_frame(frame, sizeof(frame), 3);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1000, 0x10203040, &cap.h, &cap);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame, sizeof(frame), 90000));
	TASK_CHECK(cap.count == 8);

	for (i = 7; i >= 0; i--)
	{
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap.data[i], cap.len[i]));
		TASK_CHECK(rx.count == (i == 0 ? 1 : 0)); // delivered only when complete
	}
	TASK_CHECK(rx.len[0] == (int)sizeof(frame));
	TASK_CHECK(rx.ts[0] == 90000);
	TASK_CHECK(rx.flags[0] == 0); // gaps filled inside the frame are not a loss
	TASK_CHECK(0 == memcmp(rx.frame[0], frame, sizeof(frame)));

	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// retransmission of the same extended sequence number: the first copy wins
static int fr_retransmit_first_wins(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 0 };
	struct cap_t cap;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame[64];
	int i;

	cap_init(&cap);
	rx_init(&rx);
	gen_frame(frame, sizeof(frame), 11);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1000, 0x10203040, &cap.h, &cap);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame, sizeof(frame), 12345));
	TASK_CHECK(cap.count == 8);

	for (i = 0; i < 8; i++)
	{
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap.data[i], cap.len[i]));
		if (3 == i)
		{
			cap.data[i][20] ^= 0xFF; // corrupt a data byte of the retransmission
			TASK_CHECK(0 == rtp_raw_decode_input(dec, cap.data[i], cap.len[i])); // discarded
			TASK_CHECK(0 == rtp_raw_decode_input(dec, cap.data[i], cap.len[i])); // again
		}
	}
	TASK_CHECK(rx.count == 1);
	TASK_CHECK(rx.flags[0] == 0);
	TASK_CHECK(0 == memcmp(rx.frame[0], frame, sizeof(frame))); // the first copy won

	TASK_CHECK(0 == rtp_raw_decode_input(dec, cap.data[0], cap.len[0])); // late dup of a delivered frame
	TASK_CHECK(rx.count == 1); // not delivered again

	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// a late packet of a delivered frame must not reopen the frame
static int fr_late_packet_no_reopen(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 0 };
	struct cap_t cap1, cap2;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame1[64], frame2[64];
	int i;

	cap_init(&cap1);
	cap_init(&cap2);
	rx_init(&rx);
	gen_frame(frame1, sizeof(frame1), 5);
	gen_frame(frame2, sizeof(frame2), 90);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1000, 0x10203040, &cap1.h, &cap1);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame1, sizeof(frame1), 100));
	rtp_raw_encode_destroy(enc);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1008, 0x10203040, &cap2.h, &cap2);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame2, sizeof(frame2), 200));
	TASK_CHECK(cap1.count == 8 && cap2.count == 8);

	for (i = 0; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap1.data[i], cap1.len[i]));
	TASK_CHECK(rx.count == 1 && rx.ts[0] == 100 && rx.flags[0] == 0);
	TASK_CHECK(0 == memcmp(rx.frame[0], frame1, sizeof(frame1)));

	TASK_CHECK(0 == rtp_raw_decode_input(dec, cap1.data[3], cap1.len[3])); // late, already delivered
	TASK_CHECK(rx.count == 1);

	for (i = 0; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap2.data[i], cap2.len[i]));
	TASK_CHECK(rx.count == 2 && rx.ts[1] == 200 && rx.flags[1] == 0);
	TASK_CHECK(0 == memcmp(rx.frame[1], frame2, sizeof(frame2)));

	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// an incomplete frame is dropped on a newer timestamp; late packets of the
// old frame must not corrupt the new one
static int fr_old_packet_no_overwrite(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 0 };
	struct cap_t cap1, cap2, cap3;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame1[64], frame2[64], frame3[64];
	int i;

	cap_init(&cap1);
	cap_init(&cap2);
	cap_init(&cap3);
	rx_init(&rx);
	gen_frame(frame1, sizeof(frame1), 5);
	gen_frame(frame2, sizeof(frame2), 90);
	gen_frame(frame3, sizeof(frame3), 150);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1000, 0x10203040, &cap1.h, &cap1);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame1, sizeof(frame1), 100));
	rtp_raw_encode_destroy(enc);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1008, 0x10203040, &cap2.h, &cap2);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame2, sizeof(frame2), 200));
	rtp_raw_encode_destroy(enc);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1016, 0x10203040, &cap3.h, &cap3);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame3, sizeof(frame3), 300));

	for (i = 0; i < 6; i++) // frame1 misses its last 2 packets
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap1.data[i], cap1.len[i]));
	for (i = 0; i < 4; i++) // frame2 starts, frame1 is abandoned
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap2.data[i], cap2.len[i]));
	TASK_CHECK(0 == rtp_raw_decode_input(dec, cap1.data[6], cap1.len[6])); // late packet of frame1
	for (i = 4; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap2.data[i], cap2.len[i]));
	TASK_CHECK(rx.count == 1);
	TASK_CHECK(rx.ts[0] == 200 && (rx.flags[0] & RTP_PAYLOAD_FLAG_PACKET_LOST));
	TASK_CHECK(0 == memcmp(rx.frame[0], frame2, sizeof(frame2))); // not polluted by frame1

	TASK_CHECK(0 == rtp_raw_decode_input(dec, cap1.data[7], cap1.len[7])); // still late
	for (i = 0; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap3.data[i], cap3.len[i]));
	TASK_CHECK(rx.count == 2);
	TASK_CHECK(rx.ts[1] == 300 && rx.flags[1] == 0); // the loss flag was delivered once
	TASK_CHECK(0 == memcmp(rx.frame[1], frame3, sizeof(frame3)));

	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// callback errors are passed through as-is on both ends
static int fr_callback_error(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 0 };
	struct cap_t cap;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame[64];
	int i;

	gen_frame(frame, sizeof(frame), 7);

	// encoder: the packet callback fails at once, nothing is sent
	cap_init(&cap);
	cap.fail_after = 0;
	cap.err = 9;
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1000, 0x10203040, &cap.h, &cap);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(9 == rtp_raw_encode_input(enc, frame, sizeof(frame), 1));
	TASK_CHECK(cap.count == 0);
	rtp_raw_encode_destroy(enc);

	// encoder: the callback fails on the second packet, no more packets are sent
	cap_init(&cap);
	cap.fail_after = 1;
	cap.err = 9;
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1000, 0x10203040, &cap.h, &cap);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(9 == rtp_raw_encode_input(enc, frame, sizeof(frame), 1));
	TASK_CHECK(cap.count == 1);
	rtp_raw_encode_destroy(enc);

	// decoder: the frame callback return value is passed through
	cap_init(&cap);
	rx_init(&rx);
	rx.ret = 7;
	enc = rtp_raw_encode_create(&fmt, 28, 96, 1000, 0x10203040, &cap.h, &cap);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame, sizeof(frame), 1));
	for (i = 0; i < 7; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap.data[i], cap.len[i]));
	TASK_CHECK(7 == rtp_raw_decode_input(dec, cap.data[7], cap.len[7]));
	TASK_CHECK(rx.count == 1);
	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// ---------------------------------------------------------------------------
// loss_clock
// ---------------------------------------------------------------------------

// the 16-bit RTP sequence number wraps inside a frame
static int lc_seq16_wrap(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 0 };
	struct cap_t cap;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame[64];
	int i;

	cap_init(&cap);
	rx_init(&rx);
	gen_frame(frame, sizeof(frame), 21);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 0x0000FFFE, 0x10203040, &cap.h, &cap);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame, sizeof(frame), 5000));
	TASK_CHECK(cap.count == 8);
	for (i = 0; i < 8; i++)
	{
		const uint8_t* p = cap.data[i];
		uint32_t seq = 0x0000FFFE + (uint32_t)i;
		TASK_CHECK(p[2] == (uint8_t)(seq >> 8) && p[3] == (uint8_t)seq); // low 16 bits wrap
		TASK_CHECK(p[12] == (uint8_t)(seq >> 24) && p[13] == (uint8_t)(seq >> 16)); // high 16 bits
	}
	for (i = 0; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap.data[i], cap.len[i]));
	TASK_CHECK(rx.count == 1 && rx.flags[0] == 0);
	TASK_CHECK(0 == memcmp(rx.frame[0], frame, sizeof(frame)));
	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// the full 32-bit extended sequence number wraps across zero inside a frame
static int lc_seq32_wrap(void)
{
	struct rtp_raw_format fmt = { 12, 3, 8, 0 }; // 18 pgroups, not a divisor of 2^32
	struct cap_t cap;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame[72];
	int i;

	cap_init(&cap);
	rx_init(&rx);
	gen_frame(frame, sizeof(frame), 33);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 0xFFFFFFFC, 0x10203040, &cap.h, &cap);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame, sizeof(frame), 6000));
	TASK_CHECK(cap.count == 9);
	for (i = 0; i < 9; i++)
	{
		const uint8_t* p = cap.data[i];
		uint32_t seq = 0xFFFFFFFC + (uint32_t)i; // wraps to 0,1,2...
		TASK_CHECK(p[2] == (uint8_t)(seq >> 8) && p[3] == (uint8_t)seq);
		TASK_CHECK(p[12] == (uint8_t)(seq >> 24) && p[13] == (uint8_t)(seq >> 16));
	}
	// fully reversed order: marker packet first, across the wrap boundary
	for (i = 8; i >= 0; i--)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap.data[i], cap.len[i]));
	TASK_CHECK(rx.count == 1 && rx.flags[0] == 0);
	TASK_CHECK(0 == memcmp(rx.frame[0], frame, sizeof(frame)));
	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// the 90kHz timestamp wraps across zero between frames
static int lc_timestamp_wrap(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 0 };
	struct cap_t cap1, cap2;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame1[64], frame2[64];
	int i;

	cap_init(&cap1);
	cap_init(&cap2);
	rx_init(&rx);
	gen_frame(frame1, sizeof(frame1), 41);
	gen_frame(frame2, sizeof(frame2), 77);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 2000, 0x10203040, &cap1.h, &cap1);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame1, sizeof(frame1), 0xFFFFFFF8));
	rtp_raw_encode_destroy(enc);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 2008, 0x10203040, &cap2.h, &cap2);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame2, sizeof(frame2), 0x00000008)); // +0x10, wrapped

	for (i = 0; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap1.data[i], cap1.len[i]));
	for (i = 0; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap2.data[i], cap2.len[i]));
	TASK_CHECK(rx.count == 2);
	TASK_CHECK(rx.ts[0] == 0xFFFFFFF8 && rx.flags[0] == 0);
	TASK_CHECK(rx.ts[1] == 0x00000008 && rx.flags[1] == 0);
	TASK_CHECK(0 == memcmp(rx.frame[0], frame1, sizeof(frame1)));
	TASK_CHECK(0 == memcmp(rx.frame[1], frame2, sizeof(frame2)));
	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// a lost packet drops the frame; the next complete frame carries
// RTP_PAYLOAD_FLAG_PACKET_LOST, the one after is clean again
static int lc_loss_flag_recovery(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 0 };
	struct cap_t cap1, cap2, cap3;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame1[64], frame2[64], frame3[64];
	int i;

	cap_init(&cap1);
	cap_init(&cap2);
	cap_init(&cap3);
	rx_init(&rx);
	gen_frame(frame1, sizeof(frame1), 5);
	gen_frame(frame2, sizeof(frame2), 90);
	gen_frame(frame3, sizeof(frame3), 150);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 3000, 0x10203040, &cap1.h, &cap1);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame1, sizeof(frame1), 1));
	rtp_raw_encode_destroy(enc);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 3008, 0x10203040, &cap2.h, &cap2);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame2, sizeof(frame2), 2));
	rtp_raw_encode_destroy(enc);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 3016, 0x10203040, &cap3.h, &cap3);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame3, sizeof(frame3), 3));

	for (i = 0; i < 8; i++)
	{
		if (2 == i) continue; // drop one packet of frame1
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap1.data[i], cap1.len[i]));
	}
	TASK_CHECK(rx.count == 0); // frame1 never completes
	for (i = 0; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap2.data[i], cap2.len[i]));
	TASK_CHECK(rx.count == 1);
	TASK_CHECK(rx.flags[0] == RTP_PAYLOAD_FLAG_PACKET_LOST);
	TASK_CHECK(0 == memcmp(rx.frame[0], frame2, sizeof(frame2)));
	for (i = 0; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap3.data[i], cap3.len[i]));
	TASK_CHECK(rx.count == 2);
	TASK_CHECK(rx.flags[1] == 0); // flag cleared after being delivered once
	TASK_CHECK(0 == memcmp(rx.frame[1], frame3, sizeof(frame3)));
	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// a wholly skipped frame between two delivered frames: the extended
// sequence gap flags the next complete frame
static int lc_gap_frame_loss(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 0 };
	struct cap_t cap1, cap2, cap3;
	struct rx_t rx;
	void* enc;
	void* dec;
	uint8_t frame1[64], frame2[64], frame3[64];
	int i;

	cap_init(&cap1);
	cap_init(&cap2);
	cap_init(&cap3);
	rx_init(&rx);
	gen_frame(frame1, sizeof(frame1), 5);
	gen_frame(frame2, sizeof(frame2), 90);
	gen_frame(frame3, sizeof(frame3), 150);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 4000, 0x10203040, &cap1.h, &cap1);
	dec = rtp_raw_decode_create(&fmt, 96, 0x10203040, &rx.h, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame1, sizeof(frame1), 1));
	rtp_raw_encode_destroy(enc);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 4008, 0x10203040, &cap2.h, &cap2); // never fed
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame2, sizeof(frame2), 2));
	rtp_raw_encode_destroy(enc);
	enc = rtp_raw_encode_create(&fmt, 28, 96, 4016, 0x10203040, &cap3.h, &cap3);
	TASK_CHECK(enc != NULL);
	TASK_CHECK(0 == rtp_raw_encode_input(enc, frame3, sizeof(frame3), 3));

	for (i = 0; i < 8; i++)
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap1.data[i], cap1.len[i]));
	TASK_CHECK(rx.count == 1 && rx.flags[0] == 0);
	for (i = 0; i < 8; i++) // frame2 is skipped entirely
		TASK_CHECK(1 == rtp_raw_decode_input(dec, cap3.data[i], cap3.len[i]));
	TASK_CHECK(rx.count == 2);
	TASK_CHECK(rx.flags[1] == RTP_PAYLOAD_FLAG_PACKET_LOST);
	TASK_CHECK(0 == memcmp(rx.frame[1], frame3, sizeof(frame3)));
	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// plain PCMU through the classic payload factory, mirroring task_support/baseline.c
struct pcmu_t
{
	struct rtp_payload_t rxh, txh;
	void* decoder;
	unsigned int packets, decoded;
	size_t bytes;
	uint8_t expected[2000];
	int ok;
};

static void* pcmu_get(void* p, int n) { (void)p; return malloc((size_t)n); }
static void pcmu_put(void* p, void* m) { (void)p; free(m); }

static int pcmu_decoded(void* p, const void* data, int n, uint32_t ts, int flags)
{
	struct pcmu_t* s = (struct pcmu_t*)p;
	if (ts != 0xfffffff0U || flags != 0 || n <= 0)
		s->ok = 0;
	if (s->bytes + (size_t)n > sizeof(s->expected) || 0 != memcmp(data, s->expected + s->bytes, (size_t)n))
		s->ok = 0;
	s->bytes += (size_t)n;
	s->decoded++;
	return 0;
}

static int pcmu_encoded(void* p, const void* data, int n, uint32_t ts, int flags)
{
	struct pcmu_t* s = (struct pcmu_t*)p;
	struct rtp_packet_t pkt;
	if (n > 564 || flags != 0 || ts != 0xfffffff0U)
		s->ok = 0;
	if (0 != rtp_packet_deserialize(&pkt, data, n))
		s->ok = 0;
	else if (pkt.rtp.seq != (uint16_t)(65535U + s->packets) || pkt.rtp.ssrc != 0x10203040U || pkt.rtp.pt != 0 || pkt.rtp.m != 0)
		s->ok = 0;
	s->packets++;
	if (1 != rtp_payload_decode_input(s->decoder, data, n))
		s->ok = 0;
	return 0;
}

static int lc_pcmu_control(void)
{
	struct pcmu_t s;
	void* encoder;
	unsigned int i;
	uint16_t seq;
	uint32_t ts;

	memset(&s, 0, sizeof(s));
	s.ok = 1;
	s.rxh.alloc = pcmu_get; s.rxh.free = pcmu_put; s.rxh.packet = pcmu_decoded;
	s.txh.alloc = pcmu_get; s.txh.free = pcmu_put; s.txh.packet = pcmu_encoded;
	for (i = 0; i < sizeof(s.expected); i++)
		s.expected[i] = (uint8_t)(i * 13U + 7U);
	rtp_packet_setsize(564);
	s.decoder = rtp_payload_decode_create(0, "PCMU", &s.rxh, &s);
	TASK_CHECK(s.decoder != NULL);
	encoder = rtp_payload_encode_create(0, "PCMU", 65535, 0x10203040U, &s.txh, &s);
	TASK_CHECK(encoder != NULL);
	TASK_CHECK(0 == rtp_payload_encode_input(encoder, s.expected, sizeof(s.expected), 0xfffffff0U));
	TASK_CHECK(s.bytes == sizeof(s.expected) && s.packets == 4 && s.decoded == 4);
	rtp_payload_encode_getinfo(encoder, &seq, &ts);
	TASK_CHECK(seq == 3 && ts == 0xfffffff0U);
	TASK_CHECK(s.ok);
	rtp_payload_encode_destroy(encoder);
	rtp_payload_decode_destroy(s.decoder);
	return 0;
}

const struct task_case task_cases[] = {
	{ "wire_layout", "ten_bit_pgroup", wl_ten_bit_pgroup },
	{ "wire_layout", "c_chain_offset", wl_c_chain_offset },
	{ "wire_layout", "multi_line_packing", wl_multi_line_packing },
	{ "wire_layout", "decode_handcrafted", wl_decode_handcrafted },
	{ "wire_layout", "create_validate", wl_create_validate },
	{ "frame_recovery", "reorder_marker_first", fr_reorder_marker_first },
	{ "frame_recovery", "retransmit_first_wins", fr_retransmit_first_wins },
	{ "frame_recovery", "late_packet_no_reopen", fr_late_packet_no_reopen },
	{ "frame_recovery", "old_packet_no_overwrite", fr_old_packet_no_overwrite },
	{ "frame_recovery", "callback_error", fr_callback_error },
	{ "loss_clock", "seq16_wrap", lc_seq16_wrap },
	{ "loss_clock", "seq32_wrap", lc_seq32_wrap },
	{ "loss_clock", "timestamp_wrap", lc_timestamp_wrap },
	{ "loss_clock", "loss_flag_recovery", lc_loss_flag_recovery },
	{ "loss_clock", "gap_frame_loss", lc_gap_frame_loss },
	{ "loss_clock", "pcmu_control", lc_pcmu_control },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
