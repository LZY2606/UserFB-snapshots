// RFC 4175 RTP Payload Format for Uncompressed Video
// progressive YCbCr 4:2:2, 8-bit/10-bit, decoder(depacketizer)
//
// Packets of one frame may arrive out of order(marker first included).
// Frames are told apart by the full 32-bit extended sequence number
// (high 16 bits in the payload, low 16 bits in the RTP header), never by
// timestamp order. Working memory is a single frame plus its coverage
// bitmap and the sequence number set of the current frame.

#include "rtp-raw-internal.h"
#include "rtp-packet.h"
#include "rtp-util.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

struct rtp_raw_decode_t
{
	struct rtp_payload_t handler;
	void* cbparam;
	uint8_t payload;
	uint32_t ssrc;

	unsigned int width, height, line_start;
	int pgroup; // bytes per 2 pixels, 4(8-bit) or 5(10-bit)
	int line_bytes; // width/2*pgroup
	int frame_bytes; // line_bytes*height
	uint32_t line_pgroups; // width/2
	uint32_t total_pgroups; // line_pgroups*height

	uint8_t* frame; // one packed frame
	uint8_t* cover; // pgroup coverage bitmap of the current frame
	uint8_t* seen; // sequence number set of the current frame, see rtp_raw_seq_index
	uint32_t covered; // covered pgroups

	int have_frame; // a frame is being assembled
	uint32_t frame_ts;
	uint32_t ref_seq; // first sequence number seen of the current frame
	uint32_t min_seq, max_seq; // sequence number span seen of the current frame
	int marker; // the marker packet of the current frame has been seen
	int lost; // RTP_PAYLOAD_FLAG_PACKET_LOST pending for the next complete frame

	int have_hist; // a frame has ended(delivered or abandoned)
	uint32_t hist_seq; // maximum sequence number seen of the last ended frame
};

static int rtp_raw_bitmap_get(const uint8_t* map, uint32_t i)
{
	return (map[i >> 3] >> (i & 7)) & 1;
}

static void rtp_raw_bitmap_set(uint8_t* map, uint32_t i)
{
	map[i >> 3] |= (uint8_t)(1 << (i & 7));
}

// slot of a sequence number in the current frame's set: the signed distance
// to the frame reference is unique modulo total_pgroups(the frame span is
// smaller), even when the 32-bit sequence number wraps across zero
static uint32_t rtp_raw_seq_index(const struct rtp_raw_decode_t* unpacker, uint32_t seq)
{
	int32_t d = (int32_t)(seq - unpacker->ref_seq);
	int32_t m = (int32_t)unpacker->total_pgroups;
	int32_t r = d % m;
	return (uint32_t)(r < 0 ? r + m : r);
}

void* rtp_raw_decode_create(const struct rtp_raw_format *format, uint8_t payload, uint32_t ssrc, struct rtp_payload_t *handler, void* cbparam)
{
	struct rtp_raw_decode_t *unpacker;
	int pgroup, frame_bytes;
	uint32_t total;

	if (0 != rtp_raw_format_check(format, &pgroup, &frame_bytes) || payload > 127 || !handler || !handler->packet)
		return NULL;

	unpacker = (struct rtp_raw_decode_t *)calloc(1, sizeof(*unpacker));
	if (!unpacker) return NULL;

	memcpy(&unpacker->handler, handler, sizeof(unpacker->handler));
	unpacker->cbparam = cbparam;
	unpacker->payload = payload;
	unpacker->ssrc = ssrc;
	unpacker->width = format->width;
	unpacker->height = format->height;
	unpacker->line_start = format->line_start;
	unpacker->pgroup = pgroup;
	unpacker->line_pgroups = format->width / 2;
	unpacker->line_bytes = (int)unpacker->line_pgroups * pgroup;
	unpacker->frame_bytes = frame_bytes;
	total = unpacker->line_pgroups * format->height;
	unpacker->total_pgroups = total;

	unpacker->frame = (uint8_t*)malloc((size_t)frame_bytes);
	unpacker->cover = (uint8_t*)calloc(1, ((size_t)total + 7) / 8);
	unpacker->seen = (uint8_t*)calloc(1, ((size_t)total + 7) / 8);
	if (!unpacker->frame || !unpacker->cover || !unpacker->seen)
	{
		rtp_raw_decode_destroy(unpacker);
		return NULL;
	}
	return unpacker;
}

void rtp_raw_decode_destroy(void* decoder)
{
	struct rtp_raw_decode_t *unpacker;
	unpacker = (struct rtp_raw_decode_t *)decoder;
	if (unpacker->frame)
		free(unpacker->frame);
	if (unpacker->cover)
		free(unpacker->cover);
	if (unpacker->seen)
		free(unpacker->seen);
#if defined(_DEBUG) || defined(DEBUG)
	memset(unpacker, 0xCC, sizeof(*unpacker));
#endif
	free(unpacker);
}

// check the line headers and the data length, don't touch any state
// @return 0-ok, other-error
static int rtp_raw_payload_check(struct rtp_raw_decode_t *unpacker, const uint8_t* ptr, int bytes, int *psegments)
{
	uint32_t total;
	int segments;

	total = 0;
	segments = 0;
	for (;;)
	{
		uint16_t length, line, offset;
		int c, f;

		if (bytes < RTP_RAW_LINE_HEADER)
			return -1;
		length = nbo_r16(ptr);
		line = nbo_r16(ptr + 2);
		offset = nbo_r16(ptr + 4);
		c = (offset >> 15) & 1;
		offset &= 0x7FFF;
		f = (line >> 15) & 1;
		line &= 0x7FFF;

		if (f)
			return -1; // interlaced video is not supported
		if (line < unpacker->line_start || line >= unpacker->line_start + unpacker->height)
			return -1;
		if (0 != (offset % 2) || 0 == length || 0 != (length % unpacker->pgroup))
			return -1; // offset is in pixels, data is whole pgroups
		if ((uint32_t)offset / 2 + length / unpacker->pgroup > unpacker->line_pgroups)
			return -1;

		total += length;
		ptr += RTP_RAW_LINE_HEADER;
		bytes -= RTP_RAW_LINE_HEADER;
		++segments;
		if (!c)
			break;
	}

	if ((uint32_t)bytes != total)
		return -1; // the data must match the line headers exactly
	if (psegments) *psegments = segments;
	return 0;
}

static void rtp_raw_payload_load(struct rtp_raw_decode_t *unpacker, const uint8_t* ptr, int segments)
{
	uint32_t i, g, base;
	const uint8_t* data;
	uint16_t length, line, offset;

	data = ptr + segments * RTP_RAW_LINE_HEADER;
	for (i = 0; i < (uint32_t)segments; i++)
	{
		length = nbo_r16(ptr);
		line = nbo_r16(ptr + 2) & 0x7FFF;
		offset = nbo_r16(ptr + 4) & 0x7FFF;
		ptr += RTP_RAW_LINE_HEADER;

		line -= unpacker->line_start; // source line, [0, height)
		memcpy(unpacker->frame + line * unpacker->line_bytes + (uint32_t)offset / 2 * unpacker->pgroup, data, length);

		base = line * unpacker->line_pgroups + offset / 2;
		for (g = 0; g < (uint32_t)length / unpacker->pgroup; g++)
		{
			if (!rtp_raw_bitmap_get(unpacker->cover, base + g))
			{
				rtp_raw_bitmap_set(unpacker->cover, base + g);
				++unpacker->covered;
			}
		}
		data += length;
	}
}

int rtp_raw_decode_input(void* decoder, const void* packet, int bytes)
{
	struct rtp_raw_decode_t *unpacker;
	struct rtp_packet_t pkt;
	const uint8_t *ptr;
	uint32_t seq;
	int r, segments, fresh;

	unpacker = (struct rtp_raw_decode_t *)decoder;
	if (0 != rtp_packet_deserialize(&pkt, packet, bytes))
		return -1;
	if (pkt.rtp.pt != unpacker->payload || pkt.rtp.ssrc != unpacker->ssrc)
		return 0; // not our stream
	if (pkt.payloadlen < RTP_RAW_EXT_SEQ + RTP_RAW_LINE_HEADER)
		return -1;

	ptr = (const uint8_t*)pkt.payload;
	seq = ((uint32_t)nbo_r16(ptr) << 16) | pkt.rtp.seq; // full 32-bit sequence number

	// classify by the extended sequence number, never by timestamp order
	fresh = 0;
	if (unpacker->have_frame && pkt.rtp.timestamp == unpacker->frame_ts)
	{
		// a packet of the frame being assembled, check for retransmission
		if ((uint32_t)(seq - unpacker->min_seq) >= unpacker->total_pgroups && (uint32_t)(unpacker->max_seq - seq) >= unpacker->total_pgroups)
			return 0; // out of the frame sequence span
		if (rtp_raw_bitmap_get(unpacker->seen, rtp_raw_seq_index(unpacker, seq)))
			return 0; // retransmission, only the first one counts
	}
	else if (unpacker->have_frame)
	{
		if ((int32_t)(seq - unpacker->ref_seq) <= 0)
			return 0; // late packet of a previous frame, don't touch the new one
		fresh = 1; // a newer frame, give up the incomplete one
	}
	else if (unpacker->have_hist)
	{
		if ((int32_t)(seq - unpacker->hist_seq) <= 0)
			return 0; // late packet of the delivered frame, don't reopen it
		fresh = 1;
	}
	else
	{
		fresh = 1; // the very first packet
	}

	// validate before touching any state
	r = rtp_raw_payload_check(unpacker, ptr + RTP_RAW_EXT_SEQ, pkt.payloadlen - RTP_RAW_EXT_SEQ, &segments);
	if (0 != r)
		return r;

	if (fresh)
	{
		if (unpacker->have_frame)
		{
			unpacker->lost = 1; // the previous frame was not complete
			unpacker->hist_seq = unpacker->max_seq;
			unpacker->have_hist = 1;
		}
		memset(unpacker->cover, 0, (unpacker->total_pgroups + 7) / 8);
		memset(unpacker->seen, 0, (unpacker->total_pgroups + 7) / 8);
		unpacker->covered = 0;
		unpacker->marker = 0;
		unpacker->frame_ts = pkt.rtp.timestamp;
		unpacker->ref_seq = seq;
		unpacker->min_seq = seq;
		unpacker->max_seq = seq;
		unpacker->have_frame = 1;
	}
	else
	{
		if ((int32_t)(seq - unpacker->min_seq) < 0)
			unpacker->min_seq = seq;
		if ((int32_t)(seq - unpacker->max_seq) > 0)
			unpacker->max_seq = seq;
	}

	rtp_raw_bitmap_set(unpacker->seen, rtp_raw_seq_index(unpacker, seq));
	rtp_raw_payload_load(unpacker, ptr + RTP_RAW_EXT_SEQ, segments);
	if (pkt.rtp.m)
		unpacker->marker = 1;

	if (unpacker->marker && unpacker->covered == unpacker->total_pgroups)
	{
		// a complete frame holds a contiguous sequence span; a gap against the
		// previous frame means packets(whole frames) were lost in between
		int flags = (unpacker->lost || (unpacker->have_hist && unpacker->min_seq != unpacker->hist_seq + 1)) ? RTP_PAYLOAD_FLAG_PACKET_LOST : 0;
		r = unpacker->handler.packet(unpacker->cbparam, unpacker->frame, unpacker->frame_bytes, unpacker->frame_ts, flags);
		unpacker->lost = 0; // the flag is delivered with this frame only
		unpacker->have_frame = 0;
		unpacker->have_hist = 1;
		unpacker->hist_seq = unpacker->max_seq;
		return 0 == r ? 1 : r;
	}

	return 1;
}
