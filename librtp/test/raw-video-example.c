// RFC 4175 uncompressed video (progressive YCbCr 4:2:2) usage example.
//
// Build:
//   cc -std=c99 -Ilibrtp/include librtp/test/raw-video-example.c \
//      librtp/payload/rtp-raw-pack.c librtp/payload/rtp-raw-unpack.c \
//      librtp/source/rtp-packet.c -o raw-video-example
//
// This file is documentation that also compiles and runs a small loopback.

#include "rtp-raw.h"
#include "rtp-payload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Packed line layout
//
// The library never converts colors: every source scan line must already be
// stored in the exact on-wire pgroup order, and lines are contiguous.
//
//   depth 8, pgroup = 4 bytes for two pixels:  Cb Y0 Cr Y1
//   depth 10, pgroup = 5 bytes for two pixels: four 10-bit components
//             Cb, Y0, Cr, Y1 packed from the most significant bit, i.e.
//             byte0 = Cb[9:2], byte1 = (Cb[1:0] << 6) | (Y0[9:4]), ...
//
// A width-W line holds W/2 pgroups, so a whole frame is exactly
// (width / 2) * pgroup * height bytes, with no stride or padding.
// ---------------------------------------------------------------------------

struct sender
{
	int packets;
};

// The alloc callback owns the RTP packet storage for the duration of one
// packet() call. The encoder reuses no pointer after packet() returns, so the
// buffer may be freed immediately in free(). Send/copy the datagram *inside*
// packet(); the memory must not be retained past the return.
static void* sender_alloc(void* param, int bytes)
{
	(void)param;
	return malloc((size_t)bytes);
}

static void sender_free(void* param, void* packet)
{
	(void)param;
	free(packet);
}

static int sender_packet(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
	struct sender* s = (struct sender*)param;
	(void)flags;
	(void)packet;
	// A real sender would sendto() the RTP datagram here. The pointer is only
	// valid for the length of this call; do not queue it for later.
	printf("tx packet %d: %d bytes, ts=%u\n", s->packets, bytes, timestamp);
	++s->packets;
	return 0; // non-zero aborts the frame and is returned by rtp_raw_encode_input
}

struct receiver
{
	void* frame; // owned copy; the callback buffer is only borrowed
	int bytes;
	uint32_t timestamp;
	int flags;
	int frames;
};

// The frame pointer handed to the decoder callback is the decoder's internal
// reassembly buffer. It is reused for the next frame and is only valid during
// this call, so anything needed afterwards must be copied out here.
static int receiver_packet(void* param, const void* frame, int bytes, uint32_t timestamp, int flags)
{
	struct receiver* r = (struct receiver*)param;
	void* copy = realloc(r->frame, (size_t)bytes);
	if (!copy)
		return -1; // propagated verbatim from rtp_raw_decode_input
	memcpy(copy, frame, (size_t)bytes);
	r->frame = copy;
	r->bytes = bytes;
	r->timestamp = timestamp;
	r->flags = flags;
	++r->frames;
	return 0;
}

int main(void)
{
	const struct rtp_raw_format fmt = {
		1920, // width: positive even number, <= 32768
		2,    // height: positive
		10,   // depth: 8 or 10
		0,    // line_start: RTP line number of the first active line
	};
	const int pgroup = (fmt.depth == 10) ? 5 : 4;
	const int frame_bytes = (int)(fmt.width / 2) * pgroup * (int)fmt.height;
	const int mtu = 1400; // includes the 12-byte RTP header
	const uint8_t pt = 112;
	const uint32_t ssrc = 0xAABBCCDDu;

	struct rtp_payload_t tx = { sender_alloc, sender_free, sender_packet };
	struct rtp_payload_t rx = { sender_alloc, sender_free, receiver_packet };
	struct sender sender = { 0 };
	struct receiver receiver = { 0 };
	void* encoder;
	void* decoder;
	uint8_t* frame;
	int i;

	encoder = rtp_raw_encode_create(&fmt, mtu, pt, 0 /*initial ext. seq*/, ssrc, &tx, &sender);
	decoder = rtp_raw_decode_create(&fmt, pt, ssrc, &rx, &receiver);
	if (!encoder || !decoder)
	{
		fprintf(stderr, "unsupported format\n");
		return 1;
	}

	// One whole packed frame per encode call; the length must match exactly.
	frame = (uint8_t*)malloc((size_t)frame_bytes);
	for (i = 0; i < frame_bytes; ++i)
		frame[i] = (uint8_t)(i * 13 + 7); // stand-in for captured packed lines

	if (rtp_raw_encode_input(encoder, frame, frame_bytes, 90000u /*90kHz ts*/) != 0)
	{
		fprintf(stderr, "encode failed\n");
		return 1;
	}

	// In a real process the packets would arrive from the network, one
	// complete RTP datagram per rtp_raw_decode_input call. Here we only show
	// the lifetime rules; the sender callback above is where they are sent.
	printf("sent %d packets\n", sender.packets);

	rtp_raw_encode_destroy(encoder);
	rtp_raw_decode_destroy(decoder); // no partial frame is delivered on destroy
	free(frame);
	free(receiver.frame);
	return 0;
}
