/// RFC 4175 RTP Payload Format for Uncompressed Video
/// Progressive YCbCr 4:2:2 packer, 8-bit and 10-bit packed samples.
///
/// Payload layout:
///   0                   1                   2                   3
///   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
///  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
///  |   Extended Sequence Number    |  Length  |F|     Line No     |
///  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
///  |C|  Offset     |            pixel data ...
///  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
/// All line descriptors of a packet come first, followed by the segment
/// payloads they describe. Pixel groups are never split across packets.

#include "rtp-raw-internal.h"
#include "rtp-packet.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Largest legal RTP packet (16-bit IP total length), header included.
#define RTP_RAW_MAX_MTU 65535

struct rtp_raw_segment_t
{
	const uint8_t* data; // pointer into the caller's frame
	unsigned int line; // source line number
	unsigned int offset; // pixel offset from the start of the line
	int groups; // pixel groups carried in this segment
};

struct rtp_raw_encoder_t
{
	struct rtp_packet_t pkt;
	struct rtp_payload_t handler;
	void* cbparam;

	uint32_t seq; // full 32-bit extended sequence number
	int mtu; // maximum RTP packet size including the 12-byte header
	int pgroup; // bytes per pixel group (4 or 5)
	int pg_per_line; // pixel groups per source line
	int frame_bytes; // total packed frame size in bytes
	int max_segments; // descriptors that can fit in one packet
	unsigned int height;
	unsigned int line_start;
	struct rtp_raw_segment_t* segments;
	uint8_t* payload; // serialization buffer, mtu - RTP header bytes
};

void* rtp_raw_encode_create(const struct rtp_raw_format* fmt, int mtu, uint8_t payload, uint32_t seq, uint32_t ssrc, struct rtp_payload_t* handler, void* cbparam)
{
	struct rtp_raw_encoder_t* enc;
	int pgroup, pg_per_line, frame_bytes, payload_capacity, max_segments;

	if (rtp_raw_validate(fmt, &pgroup, &pg_per_line, &frame_bytes) < 0)
		return NULL;
	// every packet must hold the extended seq number, one line descriptor
	// and at least one whole pixel group
	if (mtu < RTP_FIXED_HEADER + RTP_RAW_ESS_SIZE + RTP_RAW_DESC_SIZE + pgroup || mtu > RTP_RAW_MAX_MTU)
		return NULL;
	if (!handler || !handler->alloc || !handler->free || !handler->packet)
		return NULL;

	enc = (struct rtp_raw_encoder_t*)calloc(1, sizeof(*enc));
	if (!enc)
		return NULL;

	payload_capacity = mtu - RTP_FIXED_HEADER;
	// every segment costs one descriptor plus at least one pixel group
	max_segments = (payload_capacity - RTP_RAW_ESS_SIZE) / (RTP_RAW_DESC_SIZE + pgroup);
	enc->segments = (struct rtp_raw_segment_t*)calloc((size_t)max_segments, sizeof(*enc->segments));
	enc->payload = (uint8_t*)malloc((size_t)payload_capacity);
	if (!enc->segments || !enc->payload)
	{
		free(enc->segments);
		free(enc->payload);
		free(enc);
		return NULL;
	}

	memcpy(&enc->handler, handler, sizeof(enc->handler));
	enc->cbparam = cbparam;
	enc->seq = seq;
	enc->mtu = mtu;
	enc->pgroup = pgroup;
	enc->pg_per_line = pg_per_line;
	enc->frame_bytes = frame_bytes;
	enc->max_segments = max_segments;
	enc->height = fmt->height;
	enc->line_start = fmt->line_start;

	enc->pkt.rtp.v = RTP_VERSION;
	enc->pkt.rtp.pt = payload;
	enc->pkt.rtp.ssrc = ssrc;
	return enc;
}

void rtp_raw_encode_destroy(void* encoder)
{
	struct rtp_raw_encoder_t* enc;
	enc = (struct rtp_raw_encoder_t*)encoder;
	if (enc)
	{
		free(enc->segments);
		free(enc->payload);
#if defined(_DEBUG) || defined(DEBUG)
		memset(enc, 0xCC, sizeof(*enc));
#endif
		free(enc);
	}
}

int rtp_raw_encode_input(void* encoder, const void* frame, int bytes, uint32_t timestamp)
{
	struct rtp_raw_encoder_t* enc;
	const uint8_t* src;
	const uint8_t* line_ptr;
	uint8_t* rtp;
	int nseg, r, n, used, room, line_bytes, payload_capacity, consumed, remain, groups, i;
	unsigned int y;
	size_t payload_len;

	enc = (struct rtp_raw_encoder_t*)encoder;
	if (!enc || !frame)
		return -EINVAL;
	if (bytes != enc->frame_bytes)
		return -EINVAL;

	src = (const uint8_t*)frame;
	line_bytes = enc->pg_per_line * enc->pgroup;
	payload_capacity = enc->mtu - RTP_FIXED_HEADER;
	r = 0;
	y = 0;

	while (y < enc->height && 0 == r)
	{
		nseg = 0;
		used = 0; // descriptor + data bytes committed, extended seq excluded

		// fill the packet line by line, each line in whole pixel groups
		while (y < enc->height)
		{
			// a new line needs a fresh descriptor and at least one pgroup
			room = payload_capacity - RTP_RAW_ESS_SIZE - used;
			if (nseg > 0 && room < RTP_RAW_DESC_SIZE + enc->pgroup)
				break;

			line_ptr = (const uint8_t*)frame + (size_t)y * line_bytes;
			consumed = (int)((src - line_ptr) / enc->pgroup); // 0 unless a split line continues
			remain = enc->pg_per_line - consumed;
			groups = (room - RTP_RAW_DESC_SIZE) / enc->pgroup;
			if (groups > remain)
				groups = remain;
			if (groups <= 0)
			{
				if (0 == nseg)
					return -EINVAL; // guaranteed not to happen by the create check
				break;
			}

			enc->segments[nseg].data = src;
			enc->segments[nseg].line = enc->line_start + y;
			enc->segments[nseg].groups = groups;
			++nseg;
			used += RTP_RAW_DESC_SIZE + groups * enc->pgroup;
			src += (size_t)groups * enc->pgroup;

			if (groups == remain)
				++y; // whole source line consumed
			else
				break; // line split: its remainder starts the next packet
		}

		// offset is measured in pixels, derived from the line start pointer
		payload_len = RTP_RAW_ESS_SIZE;
		for (i = 0; i < nseg; ++i)
		{
			line_ptr = (const uint8_t*)frame + (size_t)(enc->segments[i].line - enc->line_start) * line_bytes;
			enc->segments[i].offset = (unsigned int)((enc->segments[i].data - line_ptr) / enc->pgroup) * 2u;
			payload_len += RTP_RAW_DESC_SIZE + (size_t)enc->segments[i].groups * enc->pgroup;
		}

		// serialize: extended seq, all descriptors, then all segment data
		n = 0;
		nbo_w16(enc->payload, (uint16_t)(enc->seq >> 16));
		n += RTP_RAW_ESS_SIZE;
		for (i = 0; i < nseg; ++i)
		{
			int cont = (i + 1 < nseg) ? 1 : 0;
			rtp_raw_write_desc(enc->payload + n, (uint16_t)(enc->segments[i].groups * enc->pgroup),
				enc->segments[i].line, cont, enc->segments[i].offset);
			n += RTP_RAW_DESC_SIZE;
		}
		for (i = 0; i < nseg; ++i)
		{
			size_t len = (size_t)enc->segments[i].groups * enc->pgroup;
			memcpy(enc->payload + n, enc->segments[i].data, len);
			n += (int)len;
		}

		if (n != (int)payload_len || n > payload_capacity || nseg > enc->max_segments)
			return -EINVAL;

		rtp = (uint8_t*)enc->handler.alloc(enc->cbparam, RTP_FIXED_HEADER + n);
		if (!rtp)
			return -ENOMEM;

		enc->pkt.rtp.timestamp = timestamp;
		enc->pkt.rtp.seq = (uint16_t)(enc->seq & 0xFFFF);
		enc->pkt.rtp.m = (y >= enc->height) ? 1 : 0;
		enc->pkt.payload = enc->payload;
		enc->pkt.payloadlen = n;

		{
			int total = rtp_packet_serialize(&enc->pkt, rtp, RTP_FIXED_HEADER + n);
			if (total != RTP_FIXED_HEADER + n)
			{
				enc->handler.free(enc->cbparam, rtp);
				return -EINVAL;
			}
			r = enc->handler.packet(enc->cbparam, rtp, total, timestamp, 0);
		}
		enc->handler.free(enc->cbparam, rtp);

		if (0 == r)
			++enc->seq; // count only packets handed out successfully
	}

	return r;
}
