// RFC 4175 raw video (progressive YCbCr 4:2:2) usage example.
//
// Packed lines
// ------------
// The frame handed to rtp_raw_encode_input() is already the packed wire
// byte order, one line after another, top to bottom. Each line is
// width/2 pixel groups(pgroups):
// - 8-bit:  4 bytes per pgroup, Cb Y0 Cr Y1, one byte per sample;
// - 10-bit: 5 bytes per pgroup, Cb/Y0/Cr/Y1 10-bit samples packed
//   MSB-first in the same component order.
// The library never converts colors or repacks samples, it only cuts the
// frame into whole pgroups. So a 1920x1080 10-bit frame is
// 1920/2*5*1080 = 5184000 bytes, and line y starts at y*(1920/2*5).
//
// Callback lifetimes
// ------------------
// Both the encoder RTP packets and the decoder frames are borrowed
// buffers, valid only inside the callback(like the other payload
// implementations in this library):
// - the RTP packet given to the encode callback is freed right after the
//   callback returns; send it out or copy it inside the callback;
// - the frame given to the decode callback is the decoder's single
//   working buffer and is overwritten by the next frame; copy it or
//   consume it before returning.
// Return 0 from the callbacks on success; any other value aborts the
// current call and is passed back to the caller as-is.

#include "rtp-raw.h"
#include <stdlib.h>
#include <string.h>

struct example_sink_t
{
	struct rtp_payload_t io;
	void* decoder; // loopback target for this example
	unsigned int rtp_packets;
	unsigned int frames;
};

static void* example_alloc(void* param, int bytes)
{
	(void)param;
	return malloc((size_t)bytes);
}

static void example_free(void* param, void* packet)
{
	(void)param;
	free(packet);
}

// encode side: the RTP packet is borrowed, hand it to the network here
static int example_on_rtp(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
	struct example_sink_t* s = (struct example_sink_t*)param;
	(void)timestamp;
	(void)flags;
	s->rtp_packets++;
	// this example loops back into the decoder; normally you would
	// sendto() the packet instead. The decoder also borrows the packet,
	// so the buffer is still owned by the encoder side.
	return rtp_raw_decode_input(s->decoder, packet, bytes);
}

// decode side: the frame is borrowed, copy it if it must outlive the callback
static int example_on_frame(void* param, const void* frame, int bytes, uint32_t timestamp, int flags)
{
	struct example_sink_t* s = (struct example_sink_t*)param;
	(void)frame;
	(void)bytes;
	(void)timestamp;
	if (flags & RTP_PAYLOAD_FLAG_PACKET_LOST)
	{
		// the previous frame was incomplete, this one is whole again
	}
	s->frames++;
	return 0;
}

int rtp_raw_example(void)
{
	struct rtp_raw_format format = { 1920, 1080, 10, 0 };
	struct example_sink_t sink;
	uint8_t* frame;
	void* encoder;
	int bytes, r;

	memset(&sink, 0, sizeof(sink));
	sink.io.alloc = example_alloc;
	sink.io.free = example_free;
	sink.io.packet = example_on_frame;

	// the decoder is bound to one PT/SSRC stream
	sink.decoder = rtp_raw_decode_create(&format, 96, 0x11223344, &sink.io, &sink);
	if (!sink.decoder)
		return -1;

	// one packed frame: width/2*5 bytes per line for 10-bit 4:2:2
	bytes = (int)(format.width / 2) * 5 * (int)format.height;
	frame = (uint8_t*)malloc((size_t)bytes);
	if (!frame)
		return -1;
	memset(frame, 0x80, (size_t)bytes); // pretend this came from a capture card

	// the encode callback is replaced with the loopback sink
	sink.io.packet = example_on_rtp;
	encoder = rtp_raw_encode_create(&format, 1500, 96, 0, 0x11223344, &sink.io, &sink);
	if (!encoder)
		return -1;

	// one call per frame, timestamp in 90kHz units
	r = rtp_raw_encode_input(encoder, frame, bytes, 90000);

	rtp_raw_encode_destroy(encoder);
	rtp_raw_decode_destroy(sink.decoder);
	free(frame);
	return r;
}
