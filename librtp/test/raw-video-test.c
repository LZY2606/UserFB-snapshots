// RFC 4175 uncompressed video (progressive YCbCr 4:2:2, 8/10-bit) tests.
//
// The wire-format cases build RTP packets byte by byte instead of reusing the
// library encoder, so the on-wire layout is checked independently. Encoder
// output is likewise parsed from raw bytes, never validated only by looping it
// back through the decoder.

#include "../../task_support/check.h"
#include "rtp-raw.h"
#include "rtp-payload.h"
#include "rtp-packet.h"
#include "rtp-util.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define TEST_PT 112
#define TEST_SSRC 0xAABBCCDDu
#define MAX_PKT 512
#define MAX_FRAME 256

struct rx_state
{
	int frames;
	int packet_calls;
	int fail_on_call; // <0 disables the failure injection
	uint8_t frame[MAX_FRAME];
	int bytes;
	uint32_t timestamp;
	int flags;
};

static void* rx_alloc(void* param, int bytes)
{
	(void)param;
	return malloc((size_t)bytes);
}

static void rx_free(void* param, void* packet)
{
	(void)param;
	free(packet);
}

// The decoder only lends the frame buffer for the duration of this callback;
// copy anything that must survive before returning.
static int rx_packet(void* param, const void* data, int bytes, uint32_t timestamp, int flags)
{
	struct rx_state* rx = (struct rx_state*)param;
	if (rx->fail_on_call >= 0 && rx->packet_calls == rx->fail_on_call)
	{
		++rx->packet_calls;
		return -7;
	}
	++rx->packet_calls;
	if (bytes <= (int)sizeof(rx->frame))
		memcpy(rx->frame, data, (size_t)bytes);
	rx->bytes = bytes;
	rx->timestamp = timestamp;
	rx->flags = flags;
	++rx->frames;
	return 0;
}

static struct rtp_payload_t rx_handler(struct rx_state* rx)
{
	struct rtp_payload_t h;
	h.alloc = rx_alloc;
	h.free = rx_free;
	h.packet = rx_packet;
	(void)rx;
	return h;
}

struct tx_state
{
	uint8_t pkts[64][MAX_PKT];
	int lens[64];
	int count;
	int fail_at; // <0 disables the failure injection
};

static void* tx_alloc(void* param, int bytes)
{
	(void)param;
	(void)bytes;
	return malloc((size_t)bytes);
}

static void tx_free(void* param, void* packet)
{
	(void)param;
	free(packet);
}

static int tx_packet(void* param, const void* data, int bytes, uint32_t timestamp, int flags)
{
	struct tx_state* tx = (struct tx_state*)param;
	(void)timestamp;
	(void)flags;
	if (bytes > MAX_PKT || tx->count >= 64)
		return -ENOSPC;
	memcpy(tx->pkts[tx->count], data, (size_t)bytes);
	tx->lens[tx->count] = bytes;
	if (tx->fail_at >= 0 && tx->count == tx->fail_at)
		return -9;
	++tx->count;
	return 0;
}

static struct rtp_payload_t tx_handler(struct tx_state* tx)
{
	struct rtp_payload_t h;
	h.alloc = tx_alloc;
	h.free = tx_free;
	h.packet = tx_packet;
	(void)tx;
	return h;
}

static void fill_frame(uint8_t* f, int n, unsigned seed)
{
	int i;
	for (i = 0; i < n; ++i)
		f[i] = (uint8_t)(i * 13 + seed);
}

// Hand-serialize one RTP packet (12-byte header + raw payload).
static int make_packet(uint8_t* buf, uint8_t pt, unsigned marker, uint32_t esn, uint32_t ts, uint32_t ssrc, const uint8_t* payload, int plen)
{
	buf[0] = 0x80; // V=2, no padding/extension/CSRC
	buf[1] = (uint8_t)((marker ? 0x80u : 0u) | (pt & 0x7Fu));
	nbo_w16(buf + 2, (uint16_t)(esn & 0xFFFFu));
	nbo_w32(buf + 4, ts);
	nbo_w32(buf + 8, ssrc);
	memcpy(buf + 12, payload, (size_t)plen);
	return 12 + plen;
}

// Append one 6-byte RFC 4175 line descriptor to a payload buffer.
static int put_desc(uint8_t* payload, int pos, uint16_t length, unsigned int line, int cont, unsigned int offset)
{
	nbo_w16(payload + pos, length);
	nbo_w16(payload + pos + 2, (uint16_t)(line & 0x7FFFu)); // F = 0
	nbo_w16(payload + pos + 4, (uint16_t)((cont ? 0x8000u : 0u) | (offset & 0x7FFFu)));
	return pos + 6;
}

static void* open_decoder(const struct rtp_raw_format* fmt, struct rtp_payload_t* h, struct rx_state* rx)
{
	return rtp_raw_decode_create(fmt, TEST_PT, TEST_SSRC, h, rx);
}

// ---------------------------------------------------------------- wire_layout

static int case_10bit_pgroup(void)
{
	struct rtp_raw_format fmt = { 4, 2, 10, 0 }; // 2 pgroups/line, 5 bytes each
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t frame[20], packet[MAX_PKT], payload[32];
	int p, n;
	uint32_t esn;

	TASK_CHECK(sizeof(frame) == 20);
	fill_frame(frame, sizeof(frame), 3);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	// line 0: ESN 0x00010000, high 16 bits lead the payload
	p = 0;
	nbo_w16(payload + p, 0x0001); p += 2;
	p = put_desc(payload, p, 10, 0, 0, 0);
	memcpy(payload + p, frame, 10); p += 10;
	esn = 0x00010000u;
	n = make_packet(packet, TEST_PT, 0, esn, 9000, TEST_SSRC, payload, p);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 1);

	// line 1 carries the marker
	p = 0;
	nbo_w16(payload + p, 0x0001); p += 2;
	p = put_desc(payload, p, 10, 1, 0, 0);
	memcpy(payload + p, frame + 10, 10); p += 10;
	esn = 0x00010001u;
	n = make_packet(packet, TEST_PT, 1, esn, 9000, TEST_SSRC, payload, p);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 1);

	TASK_CHECK(rx.frames == 1);
	TASK_CHECK(rx.bytes == 20);
	TASK_CHECK(rx.timestamp == 9000);
	TASK_CHECK(rx.flags == 0);
	TASK_CHECK(memcmp(rx.frame, frame, sizeof(frame)) == 0); // 10-bit bytes preserved
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_cross_line_c_chain(void)
{
	struct rtp_raw_format fmt = { 4, 3, 8, 7 }; // source lines 7, 8, 9
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t frame[24], packet[MAX_PKT], payload[40];
	int p, n;

	fill_frame(frame, sizeof(frame), 1);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	// one packet, two line descriptors: the first must set C=1
	p = 0;
	nbo_w16(payload + p, 0x5000); p += 2;
	p = put_desc(payload, p, 8, 7, 1, 0);
	p = put_desc(payload, p, 8, 8, 0, 0);
	memcpy(payload + p, frame, 8); p += 8;      // line 7 body
	memcpy(payload + p, frame + 8, 8); p += 8;  // line 8 body
	n = make_packet(packet, TEST_PT, 0, 0x50000005u, 3000, TEST_SSRC, payload, p);
	// independent byte-level checks of the descriptor chain (packet offset = 12-byte RTP header)
	TASK_CHECK(packet[12 + 2] == 0x00 && packet[12 + 3] == 0x08); // desc0 length
	TASK_CHECK(packet[12 + 4] == 0x00 && packet[12 + 5] == 0x07); // desc0 line 7, F=0
	TASK_CHECK(packet[12 + 6] == 0x80 && packet[12 + 7] == 0x00); // desc0 C=1, offset 0
	TASK_CHECK(packet[12 + 12] == 0x00 && packet[12 + 13] == 0x00); // desc1 C=0, offset 0
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 1);
	TASK_CHECK(rx.frames == 0); // last line still missing

	p = 0;
	nbo_w16(payload + p, 0x5000); p += 2;
	p = put_desc(payload, p, 8, 9, 0, 0);
	memcpy(payload + p, frame + 16, 8); p += 8;
	n = make_packet(packet, TEST_PT, 1, 0x50000006u, 3000, TEST_SSRC, payload, p);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 1);

	TASK_CHECK(rx.frames == 1);
	TASK_CHECK(rx.bytes == (int)sizeof(frame));
	TASK_CHECK(memcmp(rx.frame, frame, sizeof(frame)) == 0);
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_pixel_offset_split(void)
{
	struct rtp_raw_format fmt = { 6, 1, 8, 5 }; // 3 pgroups on source line 5
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t frame[12], packet[MAX_PKT], payload[24];
	int p, n;

	fill_frame(frame, sizeof(frame), 5);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	// first segment: pgroups 0-1 at pixel offset 0
	p = 0;
	nbo_w16(payload + p, 0x0000); p += 2;
	p = put_desc(payload, p, 8, 5, 0, 0);
	memcpy(payload + p, frame, 8); p += 8;
	n = make_packet(packet, TEST_PT, 0, 0x20, 700, TEST_SSRC, payload, p);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 1);

	// second segment: pgroup 2, pixel offset 4 (not byte offset 8)
	p = 0;
	nbo_w16(payload + p, 0x0000); p += 2;
	p = put_desc(payload, p, 4, 5, 0, 4);
	memcpy(payload + p, frame + 8, 4); p += 4;
	n = make_packet(packet, TEST_PT, 1, 0x21, 700, TEST_SSRC, payload, p);
	// the offset field is 4 pixels (payload offset 6/7, packet offset 18/19)
	TASK_CHECK(packet[18] == 0x00 && packet[19] == 0x04);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 1);

	TASK_CHECK(rx.frames == 1 && rx.bytes == 12);
	TASK_CHECK(memcmp(rx.frame, frame, sizeof(frame)) == 0);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// Parse every captured encoder packet independently from raw bytes.
static int check_encoder_wire(const struct tx_state* tx, int mtu, int pgroup, uint32_t start_seq, const uint8_t* frame, int frame_bytes, uint32_t ts)
{
	int i, total_data, seq_packets;
	TASK_CHECK(tx->count > 1); // the chosen MTU forces fragmentation
	seq_packets = 0;
	total_data = 0;
	for (i = 0; i < tx->count; ++i)
	{
		const uint8_t* pkt = tx->pkts[i];
		const uint8_t* pay;
		int payloadlen, pos, descs, sum_len, marker;
		uint32_t esn;

		TASK_CHECK(tx->lens[i] <= mtu);
		TASK_CHECK(tx->lens[i] >= 12 + 2 + 6 + pgroup);
		TASK_CHECK(pkt[0] == 0x80);
		TASK_CHECK((pkt[1] & 0x7Fu) == TEST_PT);
		TASK_CHECK(rtp_read_uint32(pkt + 8) == TEST_SSRC);
		TASK_CHECK(rtp_read_uint32(pkt + 4) == ts);

		payloadlen = tx->lens[i] - 12;
		pay = pkt + 12;
		esn = ((uint32_t)rtp_read_uint16(pay) << 16) | rtp_read_uint16(pkt + 2);
		TASK_CHECK(esn == start_seq + (uint32_t)i); // full 32-bit sequence, one per packet
		TASK_CHECK(rtp_read_uint16(pkt + 2) == (uint16_t)((start_seq + i) & 0xFFFFu));

		marker = (pkt[1] >> 7) & 1;
		TASK_CHECK(marker == (i == tx->count - 1));
		pos = 2;
		descs = 0;
		sum_len = 0;
		for (;;)
		{
			uint16_t length = rtp_read_uint16(pay + pos);
			unsigned int cont = (rtp_read_uint16(pay + pos + 4) >> 15) & 1u;
			TASK_CHECK(length > 0 && (length % pgroup) == 0);
			sum_len += length;
			++descs;
			pos += 6;
			if (!cont)
				break;
		}
		TASK_CHECK(2 + 6 * descs + sum_len == payloadlen);
		total_data += sum_len;
		++seq_packets;
	}
	TASK_CHECK(seq_packets == tx->count);
	TASK_CHECK(total_data == frame_bytes);

	// concatenate every segment body in capture order: it must reproduce the frame
	{
		uint8_t rebuilt[MAX_FRAME];
		int off = 0;
		for (i = 0; i < tx->count; ++i)
		{
			const uint8_t* pay = tx->pkts[i] + 12;
			int payloadlen = tx->lens[i] - 12;
			int pos = 2, descs = 0, d, body;
			do { pos += 6; ++descs; } while ((pay[pos - 2] & 0x80u) != 0);
			body = pos;
			for (d = 0; d < descs; ++d)
			{
				int length = rtp_read_uint16(pay + 2 + d * 6);
				memcpy(rebuilt + off, pay + body, (size_t)length);
				off += length;
				body += length;
			}
			TASK_CHECK(body == payloadlen);
		}
		TASK_CHECK(off == frame_bytes);
		TASK_CHECK(memcmp(rebuilt, frame, (size_t)frame_bytes) == 0);
	}
	return 0;
}

static int case_encoder_wire_8bit(void)
{
	struct rtp_raw_format fmt = { 8, 4, 8, 3 }; // 4 pgroups/line, source lines 3..6
	struct tx_state tx;
	struct rtp_payload_t h = tx_handler(&tx);
	void* enc;
	uint8_t frame[64];
	const int mtu = 60; // fits two whole lines per packet
	int r;

	fill_frame(frame, sizeof(frame), 9);
	memset(&tx, 0, sizeof(tx));
	tx.fail_at = -1;
	enc = rtp_raw_encode_create(&fmt, mtu, TEST_PT, 0x4000, TEST_SSRC, &h, &tx);
	TASK_CHECK(enc != NULL);
	r = rtp_raw_encode_input(enc, frame, sizeof(frame), 12345);
	TASK_CHECK(r == 0);
	TASK_CHECK(tx.count == 2); // two packets, two lines each
	// first packet really carries a C chain across two lines (payload offsets 6 and 12)
	TASK_CHECK((tx.pkts[0][12 + 6] & 0x80u) != 0);
	TASK_CHECK((tx.pkts[0][12 + 12] & 0x80u) == 0);
	TASK_CHECK(check_encoder_wire(&tx, mtu, 4, 0x4000, frame, sizeof(frame), 12345) == 0);
	rtp_raw_encode_destroy(enc);
	return 0;
}

static int case_encoder_wire_10bit_split(void)
{
	struct rtp_raw_format fmt = { 10, 2, 10, 0 }; // 5 pgroups/line, 25 bytes/line
	struct tx_state tx;
	struct rtp_payload_t h = tx_handler(&tx);
	void* enc;
	uint8_t frame[50];
	const int mtu = 30; // payload 18: only 2 pgroups per segment, lines must split
	int r, saw_split;

	fill_frame(frame, sizeof(frame), 2);
	memset(&tx, 0, sizeof(tx));
	tx.fail_at = -1;
	enc = rtp_raw_encode_create(&fmt, mtu, TEST_PT, 0, TEST_SSRC, &h, &tx);
	TASK_CHECK(enc != NULL);
	r = rtp_raw_encode_input(enc, frame, sizeof(frame), 77);
	TASK_CHECK(r == 0);
	TASK_CHECK(tx.count >= 4);
	// at least one continuation segment must show a non-zero pixel offset
	saw_split = 0;
	{
		int i;
		for (i = 0; i < tx.count; ++i)
		{
			const uint8_t* pay = tx.pkts[i] + 12;
			unsigned int offset = rtp_read_uint16(pay + 2 + 4) & 0x7FFFu;
			if (offset != 0)
				saw_split = 1;
		}
	}
	TASK_CHECK(saw_split);
	TASK_CHECK(check_encoder_wire(&tx, mtu, 5, 0, frame, sizeof(frame), 77) == 0);
	rtp_raw_encode_destroy(enc);
	return 0;
}

static int case_config_limits(void)
{
	struct rtp_raw_format good = { 4, 2, 8, 0 }; // 2 pgroups/line, 16-byte frame
	struct rtp_raw_format odd_width = { 7, 2, 8, 0 };
	struct rtp_raw_format zero_width = { 0, 2, 8, 0 };
	struct rtp_raw_format zero_height = { 8, 0, 8, 0 };
	struct rtp_raw_format bad_depth = { 8, 2, 12, 0 };
	struct rtp_raw_format line_overflow = { 8, 100, 8, 32700 };
	struct rtp_raw_format huge = { 32768, 32768, 10, 0 };
	struct rtp_raw_format too_wide = { 32770, 1, 8, 0 };
	struct rtp_payload_t h = tx_handler(NULL);
	struct tx_state tx;
	void* enc;
	uint8_t frame[16];

	memset(&tx, 0, sizeof(tx));
	tx.fail_at = -1;
	TASK_CHECK(rtp_raw_encode_create(&odd_width, 1400, TEST_PT, 0, TEST_SSRC, &h, &tx) == NULL);
	TASK_CHECK(rtp_raw_encode_create(&zero_width, 1400, TEST_PT, 0, TEST_SSRC, &h, &tx) == NULL);
	TASK_CHECK(rtp_raw_encode_create(&zero_height, 1400, TEST_PT, 0, TEST_SSRC, &h, &tx) == NULL);
	TASK_CHECK(rtp_raw_encode_create(&bad_depth, 1400, TEST_PT, 0, TEST_SSRC, &h, &tx) == NULL);
	TASK_CHECK(rtp_raw_encode_create(&line_overflow, 1400, TEST_PT, 0, TEST_SSRC, &h, &tx) == NULL);
	TASK_CHECK(rtp_raw_encode_create(&huge, 1400, TEST_PT, 0, TEST_SSRC, &h, &tx) == NULL);
	TASK_CHECK(rtp_raw_encode_create(&too_wide, 1400, TEST_PT, 0, TEST_SSRC, &h, &tx) == NULL);

	// MTU must hold extended seq + one descriptor + one pgroup
	TASK_CHECK(rtp_raw_encode_create(&good, 12 + 2 + 6 + 3, TEST_PT, 0, TEST_SSRC, &h, &tx) == NULL);
	enc = rtp_raw_encode_create(&good, 12 + 2 + 6 + 4, TEST_PT, 0, TEST_SSRC, &h, &tx);
	TASK_CHECK(enc != NULL);
	fill_frame(frame, sizeof(frame), 4);
	TASK_CHECK(rtp_raw_encode_input(enc, frame, 15, 1) == -EINVAL); // length must match exactly
	TASK_CHECK(rtp_raw_encode_input(enc, frame, (int)sizeof(frame), 1) == 0);
	rtp_raw_encode_destroy(enc);
	return 0;
}

// ------------------------------------------------------------ frame_recovery

static void line_packet(uint8_t* packet, const struct rtp_raw_format* fmt, uint32_t esn, uint32_t ts, unsigned int line_index, const uint8_t* line_data, int line_bytes, int marker)
{
	uint8_t payload[64];
	int p = 0;
	nbo_w16(payload + p, (uint16_t)(esn >> 16)); p += 2;
	p = put_desc(payload, p, (uint16_t)line_bytes, fmt->line_start + line_index, 0, 0);
	memcpy(payload + p, line_data, (size_t)line_bytes); p += line_bytes;
	make_packet(packet, TEST_PT, marker, esn, ts, TEST_SSRC, payload, p);
}

static int case_marker_first(void)
{
	struct rtp_raw_format fmt = { 4, 3, 8, 0 };
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t frame[24], packets[3][MAX_PKT];
	int line_bytes = 8, i;

	fill_frame(frame, sizeof(frame), 6);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);
	for (i = 0; i < 3; ++i)
		line_packet(packets[i], &fmt, 100u + i, 5000, i, frame + i * line_bytes, line_bytes, i == 2);

	TASK_CHECK(rtp_raw_decode_input(dec, packets[2], 12 + 2 + 6 + 8) == 1); // marker first
	TASK_CHECK(rx.frames == 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packets[0], 12 + 2 + 6 + 8) == 1);
	TASK_CHECK(rx.frames == 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packets[1], 12 + 2 + 6 + 8) == 1);
	TASK_CHECK(rx.frames == 1);
	TASK_CHECK(rx.flags == 0);
	TASK_CHECK(memcmp(rx.frame, frame, sizeof(frame)) == 0);
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_retransmission_first_copy(void)
{
	struct rtp_raw_format fmt = { 4, 2, 8, 0 };
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t frame[16], packet[MAX_PKT], payload[24];
	int p, n;

	fill_frame(frame, sizeof(frame), 8);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	line_packet(packet, &fmt, 100, 800, 0, frame, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 8) == 1);

	// original line-1 packet
	line_packet(packet, &fmt, 101, 800, 1, frame + 8, 8, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 8) == 1);
	TASK_CHECK(rx.frames == 1);
	TASK_CHECK(memcmp(rx.frame, frame, sizeof(frame)) == 0);

	// retransmission of ESN 101 carrying garbage: must be discarded
	p = 0;
	nbo_w16(payload + p, 0x0000); p += 2;
	p = put_desc(payload, p, 8, 1, 0, 0);
	memset(payload + p, 0xEE, 8); p += 8;
	n = make_packet(packet, TEST_PT, 1, 101, 800, TEST_SSRC, payload, p);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 0);
	TASK_CHECK(rx.frames == 1);
	TASK_CHECK(memcmp(rx.frame, frame, sizeof(frame)) == 0);
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_seq_wrap_16bit(void)
{
	struct rtp_raw_format fmt = { 4, 1, 8, 0 };
	struct tx_state tx;
	struct rx_state rx;
	struct rtp_payload_t th = tx_handler(&tx), rh = rx_handler(&rx);
	void* enc, *dec;
	uint8_t frame_a[8], frame_b[8];

	fill_frame(frame_a, sizeof(frame_a), 1);
	fill_frame(frame_b, sizeof(frame_b), 2);
	memset(&tx, 0, sizeof(tx));
	tx.fail_at = -1;
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;

	enc = rtp_raw_encode_create(&fmt, 1400, TEST_PT, 0xFFFFu, TEST_SSRC, &th, &tx);
	dec = open_decoder(&fmt, &rh, &rx);
	TASK_CHECK(enc != NULL && dec != NULL);

	TASK_CHECK(rtp_raw_encode_input(enc, frame_a, 8, 100) == 0);
	TASK_CHECK(rtp_raw_encode_input(enc, frame_b, 8, 200) == 0);
	TASK_CHECK(tx.count == 2);
	// low 16 bits wrap 0xFFFF -> 0x0000 while the extended seq keeps climbing
	TASK_CHECK(rtp_read_uint16(tx.pkts[0] + 2) == 0xFFFFu);
	TASK_CHECK(rtp_read_uint16(tx.pkts[1] + 2) == 0x0000u);
	TASK_CHECK(rtp_read_uint16(tx.pkts[1] + 12) == 0x0001u); // high 16 bits

	TASK_CHECK(rtp_raw_decode_input(dec, tx.pkts[0], tx.lens[0]) == 1);
	TASK_CHECK(rtp_raw_decode_input(dec, tx.pkts[1], tx.lens[1]) == 1);
	TASK_CHECK(rx.frames == 2);
	TASK_CHECK(rx.timestamp == 200);
	TASK_CHECK(memcmp(rx.frame, frame_b, sizeof(frame_b)) == 0);
	rtp_raw_encode_destroy(enc);
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_seq_wrap_32bit(void)
{
	struct rtp_raw_format fmt = { 2, 1, 8, 0 }; // one 4-byte pgroup, one packet/frame
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t packet[MAX_PKT];
	uint8_t fa[4], fb[4], fc[4];

	fill_frame(fa, sizeof(fa), 1);
	fill_frame(fb, sizeof(fb), 2);
	fill_frame(fc, sizeof(fc), 3);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	line_packet(packet, &fmt, 0xFFFFFFFFu, 0x11111111u, 0, fa, 4, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 4) == 1);
	line_packet(packet, &fmt, 0x00000000u, 0x22222222u, 0, fb, 4, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 4) == 1);
	line_packet(packet, &fmt, 0x00000001u, 0x33333333u, 0, fc, 4, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 4) == 1);

	TASK_CHECK(rx.frames == 3); // 0x00000000 is newer than 0xFFFFFFFF modulo 2^32
	TASK_CHECK(memcmp(rx.frame, fc, sizeof(fc)) == 0);
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_timestamp_wrap(void)
{
	struct rtp_raw_format fmt = { 2, 1, 8, 0 };
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t packet[MAX_PKT];
	uint8_t fa[4], fb[4], fc[4];

	fill_frame(fa, sizeof(fa), 4);
	fill_frame(fb, sizeof(fb), 5);
	fill_frame(fc, sizeof(fc), 6);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	line_packet(packet, &fmt, 10, 0xFFFFFFF0u, 0, fa, 4, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 4) == 1);
	line_packet(packet, &fmt, 11, 0x00000008u, 0, fb, 4, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 4) == 1);
	line_packet(packet, &fmt, 12, 0x00000010u, 0, fc, 4, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 4) == 1);

	TASK_CHECK(rx.frames == 3); // frame order comes from the ESN, not timestamp magnitude
	TASK_CHECK(rx.timestamp == 0x00000010u);
	TASK_CHECK(memcmp(rx.frame, fc, sizeof(fc)) == 0);
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_late_after_delivery(void)
{
	struct rtp_raw_format fmt = { 4, 2, 8, 0 };
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t frame[16], packet[MAX_PKT];

	fill_frame(frame, sizeof(frame), 7);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	line_packet(packet, &fmt, 20, 900, 0, frame, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 8) == 1);
	line_packet(packet, &fmt, 21, 900, 1, frame + 8, 8, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 8) == 1);
	TASK_CHECK(rx.frames == 1);

	// duplicate ESN and an older ESN with the same timestamp must not reopen the frame
	line_packet(packet, &fmt, 20, 900, 0, frame, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 8) == 0);
	line_packet(packet, &fmt, 19, 900, 0, frame, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 8) == 0);
	TASK_CHECK(rx.frames == 1);
	rtp_raw_decode_destroy(dec);
	return 0;
}

// PCMU loopback callbacks used by case_pcmu_control.
struct pc_state
{
	void* dec;
	unsigned int packets, decoded;
	size_t bytes;
	uint8_t expected[2000];
};

static int baseline_encode_packet(void* param, const void* data, int n, uint32_t ts, int flags)
{
	struct pc_state* s = (struct pc_state*)param;
	struct rtp_packet_t pkt;
	(void)flags;
	TASK_CHECK(rtp_packet_deserialize(&pkt, data, n) == 0);
	TASK_CHECK(pkt.rtp.ssrc == 0x10203040u && pkt.rtp.pt == 0);
	++s->packets;
	TASK_CHECK(rtp_payload_decode_input(s->dec, data, n) == 1);
	(void)ts;
	return 0;
}

static int baseline_decode_packet(void* param, const void* data, int n, uint32_t ts, int flags)
{
	struct pc_state* s = (struct pc_state*)param;
	TASK_CHECK(ts == 0xfffffff0u && flags == 0);
	TASK_CHECK(s->bytes + (size_t)n <= sizeof(s->expected));
	TASK_CHECK(memcmp(data, s->expected + s->bytes, (size_t)n) == 0);
	s->bytes += (size_t)n;
	++s->decoded;
	return 0;
}

static int case_pcmu_control(void)
{
	// the pre-existing payload factory path must keep working unchanged
	struct rtp_payload_t rx_h, tx_h;
	struct pc_state s;
	void* encoder, *decoder;
	uint8_t data[2000];
	unsigned int i;

	extern void rtp_packet_setsize(int bytes);
	rtp_packet_setsize(564);
	memset(&s, 0, sizeof(s));
	for (i = 0; i < sizeof(data); ++i)
		s.expected[i] = data[i] = (uint8_t)(i * 13u + 7u);
	rx_h.alloc = tx_h.alloc = rx_alloc;
	rx_h.free = tx_h.free = rx_free;
	rx_h.packet = baseline_decode_packet;
	tx_h.packet = baseline_encode_packet;
	decoder = rtp_payload_decode_create(0, "PCMU", &rx_h, &s);
	encoder = rtp_payload_encode_create(0, "PCMU", 65535, 0x10203040u, &tx_h, &s);
	TASK_CHECK(encoder != NULL && decoder != NULL);
	s.dec = decoder;
	TASK_CHECK(rtp_payload_encode_input(encoder, data, sizeof(data), 0xfffffff0u) == 0);
	TASK_CHECK(s.packets == 4 && s.decoded == 4 && s.bytes == sizeof(data));
	rtp_payload_encode_destroy(encoder);
	rtp_payload_decode_destroy(decoder);
	return 0;
}

// --------------------------------------------------------------- loss_clock

static int case_loss_recover_flag(void)
{
	struct rtp_raw_format fmt = { 4, 2, 8, 0 };
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t a[16], b[16], c[16], packet[MAX_PKT];
	const int plen = 12 + 2 + 6 + 8;

	fill_frame(a, sizeof(a), 1);
	fill_frame(b, sizeof(b), 2);
	fill_frame(c, sizeof(c), 3);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	// frame A: only the first line ever arrives
	line_packet(packet, &fmt, 10, 100, 0, a, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 1);
	TASK_CHECK(rx.frames == 0);

	// frame B complete: A is abandoned, B arrives flagged LOST
	line_packet(packet, &fmt, 12, 200, 0, b, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 1);
	line_packet(packet, &fmt, 13, 200, 1, b + 8, 8, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 1);
	TASK_CHECK(rx.frames == 1);
	TASK_CHECK(rx.flags == RTP_PAYLOAD_FLAG_PACKET_LOST);
	TASK_CHECK((rx.flags & RTP_PAYLOAD_FLAG_PACKET_CORRUPT) == 0);
	TASK_CHECK(memcmp(rx.frame, b, sizeof(b)) == 0);

	// frame C complete: the LOST flag was only for one frame
	line_packet(packet, &fmt, 14, 300, 0, c, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 1);
	line_packet(packet, &fmt, 15, 300, 1, c + 8, 8, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 1);
	TASK_CHECK(rx.frames == 2);
	TASK_CHECK(rx.flags == 0);
	TASK_CHECK(memcmp(rx.frame, c, sizeof(c)) == 0);
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_old_frame_never_overwrites(void)
{
	struct rtp_raw_format fmt = { 4, 2, 8, 0 };
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t a[16], b[16], stale[8], packet[MAX_PKT];
	const int plen = 12 + 2 + 6 + 8;

	fill_frame(a, sizeof(a), 1);
	fill_frame(b, sizeof(b), 2);
	fill_frame(stale, sizeof(stale), 99);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	// frame A left incomplete
	line_packet(packet, &fmt, 30, 100, 0, a, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 1);
	// frame B completes
	line_packet(packet, &fmt, 31, 200, 0, b, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 1);
	line_packet(packet, &fmt, 32, 200, 1, b + 8, 8, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 1);
	TASK_CHECK(rx.frames == 1);

	// the late tail of A must be dropped and cannot corrupt B's storage
	line_packet(packet, &fmt, 30, 100, 0, stale, 8, 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 0);
	line_packet(packet, &fmt, 29, 100, 0, stale, 8, 0);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, plen) == 0);
	TASK_CHECK(rx.frames == 1);
	TASK_CHECK(memcmp(rx.frame, b, sizeof(b)) == 0);
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_in_frame_gap_fill(void)
{
	struct rtp_raw_format fmt = { 4, 3, 8, 2 };
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t frame[24], packets[3][MAX_PKT];
	const int plen = 12 + 2 + 6 + 8;
	int i;

	fill_frame(frame, sizeof(frame), 3);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);
	for (i = 0; i < 3; ++i)
		line_packet(packets[i], &fmt, 40u + i, 4000, i, frame + i * 8, 8, i == 2);

	// marker, then line 0, then line 1: the in-frame gap fills without loss
	TASK_CHECK(rtp_raw_decode_input(dec, packets[2], plen) == 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packets[0], plen) == 1);
	TASK_CHECK(rtp_raw_decode_input(dec, packets[1], plen) == 1);
	TASK_CHECK(rx.frames == 1);
	TASK_CHECK(rx.flags == 0);
	TASK_CHECK(memcmp(rx.frame, frame, sizeof(frame)) == 0);
	rtp_raw_decode_destroy(dec);
	return 0;
}

static int case_callback_errors(void)
{
	// decoder: a failing packet callback is passed through verbatim
	{
		struct rtp_raw_format fmt = { 4, 1, 8, 0 };
		struct rx_state rx;
		struct rtp_payload_t h = rx_handler(&rx);
		void* dec;
		uint8_t frame[8], packet[MAX_PKT];
		fill_frame(frame, sizeof(frame), 1);
		memset(&rx, 0, sizeof(rx));
		rx.fail_on_call = 0; // first delivery fails
		dec = open_decoder(&fmt, &h, &rx);
		TASK_CHECK(dec != NULL);
		line_packet(packet, &fmt, 50, 500, 0, frame, 8, 1);
		TASK_CHECK(rtp_raw_decode_input(dec, packet, 12 + 2 + 6 + 8) == -7);
		rtp_raw_decode_destroy(dec);
	}

	// encoder: after a callback failure no further packets go out and the
	// extended sequence only counts the packets handed out successfully
	{
		struct rtp_raw_format fmt = { 8, 4, 8, 0 }; // tight MTU forces several packets
		struct tx_state tx;
		struct rtp_payload_t h = tx_handler(&tx);
		void* enc;
		uint8_t frame[64];
		fill_frame(frame, sizeof(frame), 2);
		memset(&tx, 0, sizeof(tx));
		tx.fail_at = 2; // third packet callback fails
		enc = rtp_raw_encode_create(&fmt, 34, TEST_PT, 0x9000, TEST_SSRC, &h, &tx);
		TASK_CHECK(enc != NULL);
		TASK_CHECK(rtp_raw_encode_input(enc, frame, sizeof(frame), 600) == -9);
		TASK_CHECK(tx.count == 2);
		// low 16 bits live in the RTP header, high 16 bits lead the payload
		TASK_CHECK(rtp_read_uint16(tx.pkts[0] + 2) == 0x9000);
		TASK_CHECK(rtp_read_uint16(tx.pkts[1] + 2) == 0x9001);
		TASK_CHECK(rtp_read_uint16(tx.pkts[0] + 12) == 0x0000);
		rtp_raw_encode_destroy(enc);
	}
	return 0;
}

static int case_pt_ssrc_filter(void)
{
	struct rtp_raw_format fmt = { 2, 1, 8, 0 };
	struct rx_state rx;
	struct rtp_payload_t h = rx_handler(&rx);
	void* dec;
	uint8_t frame[4], packet[MAX_PKT], payload[16];
	int p, n;

	fill_frame(frame, sizeof(frame), 1);
	memset(&rx, 0, sizeof(rx));
	rx.fail_on_call = -1;
	dec = open_decoder(&fmt, &h, &rx);
	TASK_CHECK(dec != NULL);

	p = 0;
	nbo_w16(payload + p, 0); p += 2;
	p = put_desc(payload, p, 4, 0, 0, 0);
	memcpy(payload + p, frame, 4); p += 4;

	n = make_packet(packet, TEST_PT + 1, 1, 60, 1, TEST_SSRC, payload, p);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 0); // wrong PT
	n = make_packet(packet, TEST_PT, 1, 60, 1, TEST_SSRC ^ 0x01u, payload, p);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 0); // wrong SSRC
	n = make_packet(packet, TEST_PT, 1, 60, 1, TEST_SSRC, payload, p);
	TASK_CHECK(rtp_raw_decode_input(dec, packet, n) == 1);
	TASK_CHECK(rx.frames == 1);
	rtp_raw_decode_destroy(dec);
	return 0;
}

const struct task_case task_cases[] =
{
	{ "wire_layout", "pgroup_10bit", case_10bit_pgroup },
	{ "wire_layout", "cross_line_c_chain", case_cross_line_c_chain },
	{ "wire_layout", "pixel_offset_split", case_pixel_offset_split },
	{ "wire_layout", "encoder_wire_8bit", case_encoder_wire_8bit },
	{ "wire_layout", "encoder_wire_10bit_split", case_encoder_wire_10bit_split },
	{ "wire_layout", "config_limits", case_config_limits },

	{ "frame_recovery", "marker_first", case_marker_first },
	{ "frame_recovery", "retransmission_first_copy", case_retransmission_first_copy },
	{ "frame_recovery", "seq_wrap_16bit", case_seq_wrap_16bit },
	{ "frame_recovery", "seq_wrap_32bit", case_seq_wrap_32bit },
	{ "frame_recovery", "timestamp_wrap", case_timestamp_wrap },
	{ "frame_recovery", "late_after_delivery", case_late_after_delivery },
	{ "frame_recovery", "pcmu_control", case_pcmu_control },

	{ "loss_clock", "loss_recover_flag", case_loss_recover_flag },
	{ "loss_clock", "old_frame_never_overwrites", case_old_frame_never_overwrites },
	{ "loss_clock", "in_frame_gap_fill", case_in_frame_gap_fill },
	{ "loss_clock", "callback_errors", case_callback_errors },
	{ "loss_clock", "pt_ssrc_filter", case_pt_ssrc_filter },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
