// RFC 4175 RTP Payload Format for Uncompressed Video
// progressive YCbCr 4:2:2, 8-bit/10-bit, encoder(packetizer)
//
// 4.3. Payload Data (p6)
/*
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Extended Sequence Number    |            Length             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|F|          Line No            |C|           Offset            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
//
// The frame memory is the packed wire byte order already:
// 8-bit 4:2:2 pgroup is 4 bytes(Cb Y0 Cr Y1) per 2 pixels,
// 10-bit 4:2:2 pgroup is 5 bytes per 2 pixels.

#include "rtp-raw-internal.h"
#include "rtp-payload-internal.h" // rtp_packet_serialize_header
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

struct rtp_raw_segment_t
{
	unsigned int line; // source line, [0, height)
	unsigned int pixel; // pixel offset within the line
	unsigned int bytes; // segment data length in bytes
};

struct rtp_raw_encode_t
{
	struct rtp_packet_t pkt;
	struct rtp_payload_t handler;
	void* cbparam;

	unsigned int width, height, depth, line_start;
	int pgroup; // bytes per 2 pixels, 4(8-bit) or 5(10-bit)
	int line_bytes; // width/2*pgroup
	int frame_bytes; // line_bytes*height

	int mtu; // maximum packet size, include the 12-byte RTP header
	uint32_t seq; // full 32-bit sequence number, low 16 bits go into the RTP header

	struct rtp_raw_segment_t* segments;
	int capacity; // maximum segments per packet
};

void* rtp_raw_encode_create(const struct rtp_raw_format *format, int mtu, uint8_t payload, uint32_t seq, uint32_t ssrc, struct rtp_payload_t *handler, void* cbparam)
{
	struct rtp_raw_encode_t *packer;
	int pgroup, frame_bytes, budget;

	if (0 != rtp_raw_format_check(format, &pgroup, &frame_bytes) || !handler || !handler->alloc || !handler->free || !handler->packet)
		return NULL;

	// the mtu must hold the RTP header, the extended sequence number,
	// one 6-byte line header and at least one pgroup
	if (mtu < RTP_FIXED_HEADER + RTP_RAW_EXT_SEQ + RTP_RAW_LINE_HEADER + pgroup)
		return NULL;

	packer = (struct rtp_raw_encode_t *)calloc(1, sizeof(*packer));
	if (!packer) return NULL;

	memcpy(&packer->handler, handler, sizeof(packer->handler));
	packer->cbparam = cbparam;
	packer->width = format->width;
	packer->height = format->height;
	packer->depth = format->depth;
	packer->line_start = format->line_start;
	packer->pgroup = pgroup;
	packer->line_bytes = (int)(format->width / 2) * pgroup;
	packer->frame_bytes = frame_bytes;
	packer->mtu = mtu;
	packer->seq = seq;

	budget = mtu - RTP_FIXED_HEADER - RTP_RAW_EXT_SEQ;
	packer->capacity = budget / (RTP_RAW_LINE_HEADER + pgroup) + 1;
	packer->segments = (struct rtp_raw_segment_t *)malloc(sizeof(packer->segments[0]) * packer->capacity);
	if (!packer->segments)
	{
		rtp_raw_encode_destroy(packer);
		return NULL;
	}

	packer->pkt.rtp.v = RTP_VERSION;
	packer->pkt.rtp.pt = payload;
	packer->pkt.rtp.ssrc = ssrc;
	return packer;
}

void rtp_raw_encode_destroy(void* encoder)
{
	struct rtp_raw_encode_t *packer;
	packer = (struct rtp_raw_encode_t *)encoder;
	if (packer->segments)
		free(packer->segments);
#if defined(_DEBUG) || defined(DEBUG)
	memset(packer, 0xCC, sizeof(*packer));
#endif
	free(packer);
}

static int rtp_raw_encode_packet(struct rtp_raw_encode_t *packer, const uint8_t* frame, int count, uint32_t timestamp, int marker)
{
	int r, i, n;
	uint8_t *rtp, *ptr;

	n = RTP_FIXED_HEADER + RTP_RAW_EXT_SEQ + count * RTP_RAW_LINE_HEADER;
	for (i = 0; i < count; i++)
		n += packer->segments[i].bytes;
	assert(n <= packer->mtu);

	rtp = (uint8_t*)packer->handler.alloc(packer->cbparam, n);
	if (!rtp) return -ENOMEM;

	packer->pkt.rtp.m = marker;
	packer->pkt.rtp.seq = (uint16_t)packer->seq; // low 16 bits
	packer->pkt.rtp.timestamp = timestamp;
	r = rtp_packet_serialize_header(&packer->pkt, rtp, n);
	if (r != RTP_FIXED_HEADER)
	{
		assert(0);
		packer->handler.free(packer->cbparam, rtp);
		return -1;
	}

	ptr = rtp + RTP_FIXED_HEADER;
	nbo_w16(ptr, (uint16_t)(packer->seq >> 16)); // high 16 bits
	ptr += RTP_RAW_EXT_SEQ;

	// line headers first, C=1 on all but the last one
	for (i = 0; i < count; i++)
	{
		const struct rtp_raw_segment_t *seg = &packer->segments[i];
		nbo_w16(ptr, (uint16_t)seg->bytes); // Length
		nbo_w16(ptr + 2, (uint16_t)(packer->line_start + seg->line)); // F=0(progressive) + Line No
		nbo_w16(ptr + 4, (uint16_t)((i + 1 < count ? 0x8000 : 0) | seg->pixel)); // C + Offset(in pixels)
		ptr += RTP_RAW_LINE_HEADER;
	}

	// segment data
	for (i = 0; i < count; i++)
	{
		const struct rtp_raw_segment_t *seg = &packer->segments[i];
		memcpy(ptr, frame + seg->line * packer->line_bytes + seg->pixel / 2 * packer->pgroup, seg->bytes);
		ptr += seg->bytes;
	}
	assert(ptr == rtp + n);

	r = packer->handler.packet(packer->cbparam, rtp, n, timestamp, 0);
	packer->handler.free(packer->cbparam, rtp);
	if (0 == r)
		++packer->seq; // only count packets that have been handed out
	return r;
}

int rtp_raw_encode_input(void* encoder, const void* data, int bytes, uint32_t timestamp)
{
	struct rtp_raw_encode_t *packer;
	const uint8_t *frame;
	unsigned int line, pixel;
	int r;

	packer = (struct rtp_raw_encode_t *)encoder;
	frame = (const uint8_t*)data;
	if (!frame || bytes != packer->frame_bytes)
		return -EINVAL;

	r = 0;
	line = 0;
	pixel = 0;
	while (line < packer->height)
	{
		int budget, count;

		budget = packer->mtu - RTP_FIXED_HEADER - RTP_RAW_EXT_SEQ;
		count = 0;
		while (line < packer->height && budget >= RTP_RAW_LINE_HEADER + packer->pgroup)
		{
			struct rtp_raw_segment_t *seg;
			unsigned int pgroups;

			pgroups = (unsigned int)(budget - RTP_RAW_LINE_HEADER) / packer->pgroup;
			if (pgroups > (unsigned int)(65535 / packer->pgroup))
				pgroups = (unsigned int)(65535 / packer->pgroup); // Length field is 16 bits
			if (pgroups > (packer->width - pixel) / 2)
				pgroups = (packer->width - pixel) / 2; // finish the line
			assert(pgroups > 0 && count < packer->capacity);

			seg = &packer->segments[count++];
			seg->line = line;
			seg->pixel = pixel;
			seg->bytes = pgroups * packer->pgroup;
			budget -= RTP_RAW_LINE_HEADER + seg->bytes;

			pixel += pgroups * 2;
			if (pixel >= packer->width)
			{
				pixel = 0;
				++line; // the line is done, try the next one
			}
		}
		assert(count > 0);

		r = rtp_raw_encode_packet(packer, frame, count, timestamp, line >= packer->height ? 1 : 0);
		if (0 != r)
			return r; // stop at once, the caller destroys the encoder
	}

	return 0;
}
