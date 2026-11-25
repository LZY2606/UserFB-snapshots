// RFC6184 H.264 interleaved-mode de-packetizer tests.
//
// Every test builds RTP packets as literal bytes, drives the public
// rtp_h264_interleaved_* API and checks the bytes, order, timestamps and
// callback count seen downstream.

#include "rtp-h264-interleaved.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define MAX_REC 32
#define MAX_REC_BYTES 64

struct record_t
{
	uint8_t data[MAX_REC_BYTES];
	int bytes;
	uint32_t timestamp;
	int flags;
};

static struct record_t g_rec[MAX_REC];
static int g_rec_count;
static int g_cb_error; // when non-zero, the callback returns this value

static void rec_reset(void)
{
	g_rec_count = 0;
	g_cb_error = 0;
}

static int rec_packet(void* param, const void* data, int bytes, uint32_t timestamp, int flags)
{
	struct record_t* r;
	(void)param;

	if (g_rec_count >= MAX_REC || bytes > MAX_REC_BYTES)
		return -100;

	r = &g_rec[g_rec_count++];
	memcpy(r->data, data, (size_t)bytes);
	r->bytes = bytes;
	r->timestamp = timestamp;
	r->flags = flags;
	return g_cb_error;
}

static int rec_match(int index, const uint8_t* data, int bytes, uint32_t timestamp)
{
	const struct record_t* r;
	if (index < 0 || index >= g_rec_count)
		return 0;
	r = &g_rec[index];
	return r->bytes == bytes && 0 == memcmp(r->data, data, (size_t)bytes) && r->timestamp == timestamp;
}

/// Build an RTP packet with a 12-byte header.
static int make_rtp(uint8_t* buf, uint16_t seq, uint32_t ts, const uint8_t* payload, int bytes)
{
	buf[0] = 0x80; // V=2
	buf[1] = 96;   // PT
	buf[2] = (uint8_t)(seq >> 8);
	buf[3] = (uint8_t)seq;
	buf[4] = (uint8_t)(ts >> 24);
	buf[5] = (uint8_t)(ts >> 16);
	buf[6] = (uint8_t)(ts >> 8);
	buf[7] = (uint8_t)ts;
	buf[8] = 0x00; buf[9] = 0x00; buf[10] = 0x00; buf[11] = 0x01; // SSRC
	memcpy(buf + 12, payload, (size_t)bytes);
	return 12 + bytes;
}

/// STAP-B with a single NAL unit.
static int make_stapb(uint8_t* out, uint16_t don, const uint8_t* nal, int bytes)
{
	out[0] = 0x79; // STAP-B
	out[1] = (uint8_t)(don >> 8);
	out[2] = (uint8_t)don;
	out[3] = (uint8_t)(bytes >> 8);
	out[4] = (uint8_t)bytes;
	memcpy(out + 5, nal, (size_t)bytes);
	return 5 + bytes;
}

static struct rtp_payload_t g_handler = { NULL, NULL, rec_packet };

// ---------------------------------------------------------------------------
// test_h264_don_order
// ---------------------------------------------------------------------------
int test_h264_don_order(void)
{
	int n = 0;
	uint8_t payload[64];
	uint8_t pkt[128];
	int plen, len, r;
	struct rtp_h264_interleaved_t* d;
	uint8_t nal0[2], nal1[2], nal2[2];

#define CHK(c) do { if(!(c)) return -1; n++; } while(0)

	// --- the task example: depth=2, DON 102,100,101 ------------------------
	rec_reset();
	d = rtp_h264_interleaved_create(2, &g_handler, NULL);
	CHK(d != NULL);

	nal0[0] = 0x65; nal0[1] = 0xA2;
	nal1[0] = 0x65; nal1[1] = 0xA0;
	nal2[0] = 0x65; nal2[1] = 0xA1;

	plen = make_stapb(payload, 102, nal0, 2);
	len = make_rtp(pkt, 1, 9000, payload, plen);
	r = rtp_h264_interleaved_input(d, pkt, len);
	CHK(r == 1);
	CHK(g_rec_count == 0); // 102 held

	plen = make_stapb(payload, 100, nal1, 2);
	len = make_rtp(pkt, 2, 9000, payload, plen);
	r = rtp_h264_interleaved_input(d, pkt, len);
	CHK(r == 1);
	CHK(g_rec_count == 0); // 100 held (only 2 pending == depth)

	plen = make_stapb(payload, 101, nal2, 2);
	len = make_rtp(pkt, 3, 9000, payload, plen);
	r = rtp_h264_interleaved_input(d, pkt, len);
	CHK(r == 1);
	CHK(g_rec_count == 1); // 3 pending > depth, deliver earliest(100)
	CHK(rec_match(0, nal1, 2, 9000));

	r = rtp_h264_interleaved_flush(d);
	CHK(r == 0);
	CHK(g_rec_count == 3);
	CHK(rec_match(1, nal2, 2, 9000));
	CHK(rec_match(2, nal0, 2, 9000));
	rtp_h264_interleaved_destroy(d);

	// --- wrap-around DON 0, 65535, 1 --------------------------------------
	rec_reset();
	d = rtp_h264_interleaved_create(2, &g_handler, NULL);
	CHK(d != NULL);

	plen = make_stapb(payload, 0, (const uint8_t*)"\x65\x10", 2);
	len = make_rtp(pkt, 10, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(g_rec_count == 0);

	plen = make_stapb(payload, 65535, (const uint8_t*)"\x65\x0F", 2);
	len = make_rtp(pkt, 11, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(g_rec_count == 0);

	plen = make_stapb(payload, 1, (const uint8_t*)"\x65\x11", 2);
	len = make_rtp(pkt, 12, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(g_rec_count == 1); // 65535 is earliest, not 0
	CHK(rec_match(0, (const uint8_t*)"\x65\x0F", 2, 9000));

	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 3);
	CHK(rec_match(1, (const uint8_t*)"\x65\x10", 2, 9000));
	CHK(rec_match(2, (const uint8_t*)"\x65\x11", 2, 9000));
	rtp_h264_interleaved_destroy(d);

	// --- same DON keeps input order ---------------------------------------
	rec_reset();
	d = rtp_h264_interleaved_create(2, &g_handler, NULL);
	CHK(d != NULL);

	plen = make_stapb(payload, 7, (const uint8_t*)"\x65\x01", 2);
	len = make_rtp(pkt, 20, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	plen = make_stapb(payload, 7, (const uint8_t*)"\x65\x02", 2);
	len = make_rtp(pkt, 21, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	plen = make_stapb(payload, 8, (const uint8_t*)"\x65\x03", 2);
	len = make_rtp(pkt, 22, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	CHK(g_rec_count == 1); // first same-DON entry delivered
	CHK(rec_match(0, (const uint8_t*)"\x65\x01", 2, 9000));
	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 3);
	CHK(rec_match(1, (const uint8_t*)"\x65\x02", 2, 9000));
	CHK(rec_match(2, (const uint8_t*)"\x65\x03", 2, 9000));
	rtp_h264_interleaved_destroy(d);

	// --- exactly half a circle is an error --------------------------------
	rec_reset();
	d = rtp_h264_interleaved_create(2, &g_handler, NULL);
	CHK(d != NULL);
	plen = make_stapb(payload, 100, (const uint8_t*)"\x65\x50", 2);
	len = make_rtp(pkt, 30, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	plen = make_stapb(payload, (uint16_t)(100 + 32768), (const uint8_t*)"\x65\x51", 2);
	len = make_rtp(pkt, 31, 9000, payload, plen);
	r = rtp_h264_interleaved_input(d, pkt, len);
	CHK(r < 0); // half circle: cannot order
	CHK(g_rec_count == 0);
	rtp_h264_interleaved_destroy(d);

	// --- a complete NAL at/before a delivered DON is dropped --------------
	rec_reset();
	d = rtp_h264_interleaved_create(0, &g_handler, NULL);
	CHK(d != NULL);
	plen = make_stapb(payload, 10, (const uint8_t*)"\x65\x0A", 2);
	len = make_rtp(pkt, 40, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(g_rec_count == 1);
	CHK(rec_match(0, (const uint8_t*)"\x65\x0A", 2, 9000));

	plen = make_stapb(payload, 9, (const uint8_t*)"\x65\x09", 2);
	len = make_rtp(pkt, 41, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(g_rec_count == 1); // late NAL dropped, no backwards delivery
	rtp_h264_interleaved_destroy(d);

	// --- depth range and argument checks ----------------------------------
	CHK(rtp_h264_interleaved_create(-1, &g_handler, NULL) == NULL);
	CHK(rtp_h264_interleaved_create(32768, &g_handler, NULL) == NULL);
	CHK(rtp_h264_interleaved_create(0, NULL, NULL) == NULL);
	d = rtp_h264_interleaved_create(0, &g_handler, NULL);
	CHK(d != NULL);
	rtp_h264_interleaved_destroy(d);
	d = rtp_h264_interleaved_create(32767, &g_handler, NULL);
	CHK(d != NULL);
	rtp_h264_interleaved_destroy(d);

	// --- destroy releases without calling back ----------------------------
	rec_reset();
	d = rtp_h264_interleaved_create(5, &g_handler, NULL);
	CHK(d != NULL);
	plen = make_stapb(payload, 1000, (const uint8_t*)"\x65\xEE", 2);
	len = make_rtp(pkt, 50, 9000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(g_rec_count == 0); // still pending
	rtp_h264_interleaved_destroy(d);
	CHK(g_rec_count == 0); // destroy must not deliver

	// --- callback error is returned unchanged, later callbacks stop -------
	rec_reset();
	d = rtp_h264_interleaved_create(0, &g_handler, NULL);
	CHK(d != NULL);
	{
		// STAP-B with two members -> two deliveries in one input call
		uint8_t two[16];
		two[0] = 0x79; two[1] = 0x00; two[2] = 0x05; // DON 5
		two[3] = 0x00; two[4] = 0x02; two[5] = 0x65; two[6] = 0x61;
		two[7] = 0x00; two[8] = 0x02; two[9] = 0x65; two[10] = 0x62;
		plen = 11;
		len = make_rtp(pkt, 60, 9000, two, plen);
		g_cb_error = 7;
		r = rtp_h264_interleaved_input(d, pkt, len);
		CHK(r == 7);           // unchanged non-zero callback result
		CHK(g_rec_count == 1); // second callback not invoked
		g_cb_error = 0;
	}
	rtp_h264_interleaved_destroy(d);

	// --- an incomplete aggregation packet publishes no partial member -----
	// first member complete, second member size runs past the packet
	{
		uint8_t bad[16];
		bad[0] = 0x79; bad[1] = 0x00; bad[2] = 0x0A; // STAP-B DON 10
		bad[3] = 0x00; bad[4] = 0x02; bad[5] = 0x65; bad[6] = 0xA0; // ok member
		bad[7] = 0x00; bad[8] = 0x09; bad[9] = 0x65;                 // size 9, truncated
		plen = 10;

		rec_reset();
		d = rtp_h264_interleaved_create(4, &g_handler, NULL);
		CHK(d != NULL);
		len = make_rtp(pkt, 70, 9000, bad, plen);
		r = rtp_h264_interleaved_input(d, pkt, len);
		CHK(r < 0);            // malformed aggregation
		CHK(g_rec_count == 0); // no member published, not even the good one
		rtp_h264_interleaved_destroy(d);
	}

	// --- truncated MTAP member is rejected without publishing -------------
	{
		uint8_t bad[16];
		bad[0] = 0x7A; bad[1] = 0x00; bad[2] = 0x00; // MTAP16 DONB 0
		bad[3] = 0x00; bad[4] = 0x04;                // NAL size 4 (nal-only)
		bad[5] = 0x00;                               // DOND
		bad[6] = 0x00; bad[7] = 0x00;                // TS offset
		bad[8] = 0x65; bad[9] = 0x01;                // only 2 of 4 NAL bytes
		plen = 10;

		rec_reset();
		d = rtp_h264_interleaved_create(4, &g_handler, NULL);
		CHK(d != NULL);
		len = make_rtp(pkt, 71, 9000, bad, plen);
		r = rtp_h264_interleaved_input(d, pkt, len);
		CHK(r < 0);
		CHK(g_rec_count == 0);
		rtp_h264_interleaved_destroy(d);
	}

#undef CHK
	return n;
}

// ---------------------------------------------------------------------------
// test_h264_interleaved_fragments
// ---------------------------------------------------------------------------
int test_h264_interleaved_fragments(void)
{
	int n = 0;
	uint8_t payload[64];
	uint8_t pkt[128];
	int plen, len;
	struct rtp_h264_interleaved_t* d;

#define CHK(c) do { if(!(c)) return -1; n++; } while(0)

	// --- FU-B start + FU-A end reassembles one NAL -------------------------
	rec_reset();
	d = rtp_h264_interleaved_create(4, &g_handler, NULL);
	CHK(d != NULL);

	// FU-B start, DON=50, NAL type 5, payload 0xAA
	payload[0] = 0x7D; // FU-B (type 29)
	payload[1] = 0x85; // S=1, type=5
	payload[2] = 0x00; payload[3] = 0x32; // DON 50
	payload[4] = 0xAA;
	len = make_rtp(pkt, 10, 1000, payload, 5);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(g_rec_count == 0); // not complete yet

	// FU-A end, payload 0xBB
	payload[0] = 0x7C; // FU-A (type 28)
	payload[1] = 0x45; // E=1, type=5
	payload[2] = 0xBB;
	len = make_rtp(pkt, 11, 1000, payload, 3);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 1);
	CHK(rec_match(0, (const uint8_t*)"\x65\xAA\xBB", 3, 1000));
	rtp_h264_interleaved_destroy(d);

	// --- a new start fragment discards the unfinished one -----------------
	rec_reset();
	d = rtp_h264_interleaved_create(4, &g_handler, NULL);
	CHK(d != NULL);

	plen = make_stapb(payload, 70, (const uint8_t*)"\x65\x70", 2);
	len = make_rtp(pkt, 20, 2000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	payload[0] = 0x7D; payload[1] = 0x85; payload[2] = 0x00; payload[3] = 0x47; payload[4] = 0x01; // DON 71
	len = make_rtp(pkt, 21, 2000, payload, 5);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	payload[0] = 0x7D; payload[1] = 0x85; payload[2] = 0x00; payload[3] = 0x48; payload[4] = 0x02; // DON 72, new start
	len = make_rtp(pkt, 22, 2000, payload, 5);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	payload[0] = 0x7C; payload[1] = 0x45; payload[2] = 0x03;
	len = make_rtp(pkt, 23, 2000, payload, 3);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	plen = make_stapb(payload, 73, (const uint8_t*)"\x65\x73", 2);
	len = make_rtp(pkt, 24, 2000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 3);
	CHK(rec_match(0, (const uint8_t*)"\x65\x70", 2, 2000));
	CHK(rec_match(1, (const uint8_t*)"\x65\x02\x03", 3, 2000)); // old 0x01 dropped
	CHK(rec_match(2, (const uint8_t*)"\x65\x73", 2, 2000));
	rtp_h264_interleaved_destroy(d);

	// --- a missing sequence number discards the fragment, queue survives --
	rec_reset();
	d = rtp_h264_interleaved_create(4, &g_handler, NULL);
	CHK(d != NULL);

	payload[0] = 0x7D; payload[1] = 0x85; payload[2] = 0x00; payload[3] = 0x50; payload[4] = 0x0A; // DON 80
	len = make_rtp(pkt, 30, 3000, payload, 5);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	// sequence 31 is missing: the FU-A end cannot complete the fragment
	payload[0] = 0x7C; payload[1] = 0x45; payload[2] = 0x0B;
	len = make_rtp(pkt, 32, 3000, payload, 3);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 0); // discarded

	plen = make_stapb(payload, 81, (const uint8_t*)"\x65\x81", 2);
	len = make_rtp(pkt, 33, 3000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 1); // only the complete STAP-B NAL
	CHK(rec_match(0, (const uint8_t*)"\x65\x81", 2, 3000));
	rtp_h264_interleaved_destroy(d);

	// --- mismatched timestamp is not joined ------------------------------
	rec_reset();
	d = rtp_h264_interleaved_create(4, &g_handler, NULL);
	CHK(d != NULL);

	payload[0] = 0x7D; payload[1] = 0x85; payload[2] = 0x00; payload[3] = 0x5A; payload[4] = 0x0C; // DON 90
	len = make_rtp(pkt, 40, 5000, payload, 5);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	payload[0] = 0x7C; payload[1] = 0x45; payload[2] = 0x0D;
	len = make_rtp(pkt, 41, 6000, payload, 3); // different timestamp
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 0);

	plen = make_stapb(payload, 91, (const uint8_t*)"\x65\x91", 2);
	len = make_rtp(pkt, 42, 5000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 1);
	CHK(rec_match(0, (const uint8_t*)"\x65\x91", 2, 5000));
	rtp_h264_interleaved_destroy(d);

	// --- mismatched reconstructed NAL type is not joined ------------------
	rec_reset();
	d = rtp_h264_interleaved_create(4, &g_handler, NULL);
	CHK(d != NULL);

	payload[0] = 0x7D; payload[1] = 0x85; payload[2] = 0x00; payload[3] = 0x64; payload[4] = 0x0E; // DON 100, type5
	len = make_rtp(pkt, 50, 7000, payload, 5);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	payload[0] = 0x7C; payload[1] = 0x46; payload[2] = 0x0F; // type 6, not 5
	len = make_rtp(pkt, 51, 7000, payload, 3);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 0);

	plen = make_stapb(payload, 101, (const uint8_t*)"\x65\xA1", 2);
	len = make_rtp(pkt, 52, 7000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);

	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 1);
	CHK(rec_match(0, (const uint8_t*)"\x65\xA1", 2, 7000));
	rtp_h264_interleaved_destroy(d);

	// --- flush never emits an unfinished FU ------------------------------
	rec_reset();
	d = rtp_h264_interleaved_create(4, &g_handler, NULL);
	CHK(d != NULL);
	payload[0] = 0x7D; payload[1] = 0x85; payload[2] = 0x00; payload[3] = 0x6E; payload[4] = 0x1E; // DON 110
	len = make_rtp(pkt, 60, 8000, payload, 5);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 0); // incomplete FU is not a complete NAL
	rtp_h264_interleaved_destroy(d);

	// --- interleaved entry rejects non-DON payloads ----------------------
	rec_reset();
	d = rtp_h264_interleaved_create(4, &g_handler, NULL);
	CHK(d != NULL);

	// single NAL unit packet (no DON) must be discarded
	payload[0] = 0x65; payload[1] = 0x42;
	len = make_rtp(pkt, 70, 9000, payload, 2);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 0);

	// STAP-A (no DON) must be discarded
	payload[0] = 0x78; // STAP-A
	payload[1] = 0x00; payload[2] = 0x02; payload[3] = 0x65; payload[4] = 0x43;
	len = make_rtp(pkt, 71, 9000, payload, 5);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 0);
	CHK(g_rec_count == 0);
	rtp_h264_interleaved_destroy(d);

#undef CHK
	return n;
}

// ---------------------------------------------------------------------------
// test_h264_mtap_timestamps
// ---------------------------------------------------------------------------
int test_h264_mtap_timestamps(void)
{
	int n = 0;
	uint8_t payload[64];
	uint8_t pkt[128];
	int plen, len, p;
	struct rtp_h264_interleaved_t* d;
#define CHK(c) do { if(!(c)) return -1; n++; } while(0)

	// --- MTAP16: multiple members, DONB+DOND, 32-bit ts wrap --------------
	rec_reset();
	d = rtp_h264_interleaved_create(8, &g_handler, NULL);
	CHK(d != NULL);

	payload[0] = 0x7A; // MTAP16
	payload[1] = 0x00; payload[2] = 0x0A; // DONB = 10
	p = 3;
	// member 1: DON = 10+0, offset 0x20, NAL 0x65 0x01
	payload[p++] = 0x00; payload[p++] = 0x02; // size = only the NAL unit
	payload[p++] = 0x00;                      // DOND
	payload[p++] = 0x00; payload[p++] = 0x20; // TS offset 32
	payload[p++] = 0x65; payload[p++] = 0x01;
	// member 2: DON = 10+5, offset 0x30, NAL 0x65 0x02
	payload[p++] = 0x00; payload[p++] = 0x02;
	payload[p++] = 0x05;
	payload[p++] = 0x00; payload[p++] = 0x30;
	payload[p++] = 0x65; payload[p++] = 0x02;
	plen = p;
	len = make_rtp(pkt, 1, 0xFFFFFFF0, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 2);
	CHK(rec_match(0, (const uint8_t*)"\x65\x01", 2, 0x00000010)); // 0xFFFFFFF0 + 0x20
	CHK(rec_match(1, (const uint8_t*)"\x65\x02", 2, 0x00000020)); // 0xFFFFFFF0 + 0x30
	rtp_h264_interleaved_destroy(d);

	// --- MTAP24: 24-bit offset, DONB+DOND wraps 16 bits ------------------
	rec_reset();
	d = rtp_h264_interleaved_create(8, &g_handler, NULL);
	CHK(d != NULL);

	payload[0] = 0x7B; // MTAP24
	payload[1] = 0xFF; payload[2] = 0xFF; // DONB = 65535
	p = 3;
	// member 1: DON = 65535+2 = 1, offset 0x000100, NAL 0x65 0x11
	payload[p++] = 0x00; payload[p++] = 0x02; // size = only the NAL unit
	payload[p++] = 0x02;                      // DOND
	payload[p++] = 0x00; payload[p++] = 0x01; payload[p++] = 0x00; // offset 256
	payload[p++] = 0x65; payload[p++] = 0x11;
	// member 2: DON = 65535+4 = 3, offset 0x000200, NAL 0x65 0x12
	payload[p++] = 0x00; payload[p++] = 0x02;
	payload[p++] = 0x04;
	payload[p++] = 0x00; payload[p++] = 0x02; payload[p++] = 0x00; // offset 512
	payload[p++] = 0x65; payload[p++] = 0x12;
	plen = p;
	len = make_rtp(pkt, 2, 0xFFFFFF00, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 2);
	CHK(rec_match(0, (const uint8_t*)"\x65\x11", 2, 0x00000000)); // 0xFFFFFF00+0x100
	CHK(rec_match(1, (const uint8_t*)"\x65\x12", 2, 0x00000100)); // 0xFFFFFF00+0x200
	rtp_h264_interleaved_destroy(d);

	// --- order follows DON, not timestamp --------------------------------
	rec_reset();
	d = rtp_h264_interleaved_create(8, &g_handler, NULL);
	CHK(d != NULL);

	payload[0] = 0x7A; // MTAP16
	payload[1] = 0x00; payload[2] = 0x14; // DONB = 20
	p = 3;
	// member 1: DON = 20+1 = 21, offset 0x100 (later timestamp)
	payload[p++] = 0x00; payload[p++] = 0x02;
	payload[p++] = 0x01;
	payload[p++] = 0x01; payload[p++] = 0x00;
	payload[p++] = 0x65; payload[p++] = 0x21;
	// member 2: DON = 20+10 = 30, offset 0 (earlier timestamp)
	payload[p++] = 0x00; payload[p++] = 0x02;
	payload[p++] = 0x0A;
	payload[p++] = 0x00; payload[p++] = 0x00;
	payload[p++] = 0x65; payload[p++] = 0x30;
	plen = p;
	len = make_rtp(pkt, 3, 1000, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 2);
	// DON order: 21 (ts 1256) before 30 (ts 1000), proving DON wins
	CHK(rec_match(0, (const uint8_t*)"\x65\x21", 2, 1256));
	CHK(rec_match(1, (const uint8_t*)"\x65\x30", 2, 1000));
	rtp_h264_interleaved_destroy(d);

	// --- NAL size excludes DOND and offset, both are not NAL bytes --------
	rec_reset();
	d = rtp_h264_interleaved_create(8, &g_handler, NULL);
	CHK(d != NULL);
	payload[0] = 0x7A;
	payload[1] = 0x00; payload[2] = 0x00; // DONB 0
	p = 3;
	payload[p++] = 0x00; payload[p++] = 0x04; // size = only the NAL unit(4)
	payload[p++] = 0x00;                      // DOND
	payload[p++] = 0x00; payload[p++] = 0x00; // offset 0
	payload[p++] = 0x65; payload[p++] = 0xAA; payload[p++] = 0xBB; payload[p++] = 0xCC;
	plen = p;
	len = make_rtp(pkt, 4, 500, payload, plen);
	CHK(rtp_h264_interleaved_input(d, pkt, len) == 1);
	CHK(rtp_h264_interleaved_flush(d) == 0);
	CHK(g_rec_count == 1);
	CHK(g_rec[0].bytes == 4);
	CHK(rec_match(0, (const uint8_t*)"\x65\xAA\xBB\xCC", 4, 500));
	rtp_h264_interleaved_destroy(d);

#undef CHK
	return n;
}
