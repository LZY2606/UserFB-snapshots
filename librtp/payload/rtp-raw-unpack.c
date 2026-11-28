/// RFC 4175 RTP Payload Format for Uncompressed Video
/// Progressive YCbCr 4:2:2 unpacker, 8-bit and 10-bit packed samples.
///
/// The decoder is bound to one RTP payload type / SSRC pair. Packets of one
/// frame may arrive out of order and the marker may arrive first; a frame is
/// delivered only after the marker was seen and every source line is fully
/// covered. New and old packets are told apart by the 32-bit extended
/// sequence number (modulo 2^32), never by the 16-bit RTP sequence or the
/// timestamp.

#include "rtp-raw-internal.h"
#include "rtp-packet.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct rtp_raw_decoder_t
{
	struct rtp_payload_t handler;
	void* cbparam;

	uint8_t pt;
	uint32_t ssrc;

	int pgroup; // bytes per pixel group (4 or 5)
	int pg_per_line; // pixel groups per source line
	int frame_bytes; // total packed frame size in bytes
	int total_groups; // height * pg_per_line, coverage bitmap size in bits
	unsigned int height;
	unsigned int line_start;

	uint8_t* frame; // reassembled packed frame, reused across frames
	uint8_t* coverage; // one bit per pixel group of the frame
	uint32_t* seqs; // extended sequence numbers already seen in this frame
	int seq_count;
	int seq_capacity;

	int active; // a frame is currently being assembled
	int delivered; // the active frame has already been delivered
	int marker_seen;
	int covered_groups; // number of coverage bits currently set
	int pending_lost; // LOST flag to attach to the next delivered frame
	uint32_t timestamp; // timestamp of the active frame
	uint32_t max_seq; // newest extended sequence number ever observed
	int have_seq;
};

void* rtp_raw_decode_create(const struct rtp_raw_format* fmt, uint8_t payload, uint32_t ssrc, struct rtp_payload_t* handler, void* cbparam)
{
	struct rtp_raw_decoder_t* dec;
	int pgroup, pg_per_line, frame_bytes, bitmap_bytes;

	if (rtp_raw_validate(fmt, &pgroup, &pg_per_line, &frame_bytes) < 0)
		return NULL;
	if (!handler || !handler->alloc || !handler->free || !handler->packet)
		return NULL;

	dec = (struct rtp_raw_decoder_t*)calloc(1, sizeof(*dec));
	if (!dec)
		return NULL;

	bitmap_bytes = (int)(((size_t)fmt->height * pg_per_line + 7) / 8);
	dec->frame = (uint8_t*)malloc((size_t)frame_bytes);
	dec->coverage = (uint8_t*)malloc((size_t)bitmap_bytes);
	if (!dec->frame || !dec->coverage)
	{
		free(dec->frame);
		free(dec->coverage);
		free(dec);
		return NULL;
	}

	memcpy(&dec->handler, handler, sizeof(dec->handler));
	dec->cbparam = cbparam;
	dec->pt = payload;
	dec->ssrc = ssrc;
	dec->pgroup = pgroup;
	dec->pg_per_line = pg_per_line;
	dec->frame_bytes = frame_bytes;
	dec->total_groups = (int)fmt->height * pg_per_line;
	dec->height = fmt->height;
	dec->line_start = fmt->line_start;
	return dec;
}

void rtp_raw_decode_destroy(void* decoder)
{
	struct rtp_raw_decoder_t* dec;
	dec = (struct rtp_raw_decoder_t*)decoder;
	if (dec)
	{
		free(dec->frame);
		free(dec->coverage);
		free(dec->seqs);
#if defined(_DEBUG) || defined(DEBUG)
		memset(dec, 0xCC, sizeof(*dec));
#endif
		free(dec);
	}
}

static int rtp_raw_seq_known(struct rtp_raw_decoder_t* dec, uint32_t seq)
{
	int i;
	for (i = 0; i < dec->seq_count; ++i)
	{
		if (dec->seqs[i] == seq)
			return 1;
	}
	return 0;
}

static int rtp_raw_seq_add(struct rtp_raw_decoder_t* dec, uint32_t seq)
{
	if (dec->seq_count >= dec->seq_capacity)
	{
		int capacity = dec->seq_capacity ? dec->seq_capacity * 2 : 16;
		uint32_t* ptr = (uint32_t*)realloc(dec->seqs, (size_t)capacity * sizeof(*ptr));
		if (!ptr)
			return -ENOMEM;
		dec->seqs = ptr;
		dec->seq_capacity = capacity;
	}
	dec->seqs[dec->seq_count++] = seq;
	return 0;
}

static void rtp_raw_frame_reset(struct rtp_raw_decoder_t* dec, uint32_t timestamp)
{
	dec->active = 1;
	dec->delivered = 0;
	dec->marker_seen = 0;
	dec->covered_groups = 0;
	dec->seq_count = 0;
	dec->timestamp = timestamp;
	memset(dec->coverage, 0, (size_t)((dec->total_groups + 7) / 8));
}

static void rtp_raw_cover(struct rtp_raw_decoder_t* dec, int line_index, int group, int count)
{
	int i, index;
	for (i = 0; i < count; ++i)
	{
		index = line_index * dec->pg_per_line + group + i;
		if (0 == (dec->coverage[index >> 3] & (uint8_t)(1u << (index & 7))))
		{
			dec->coverage[index >> 3] |= (uint8_t)(1u << (index & 7));
			++dec->covered_groups;
		}
	}
}

// Parse and apply one packet payload to the active frame.
// @return 0-applied, <0-malformed packet
static int rtp_raw_apply(struct rtp_raw_decoder_t* dec, const struct rtp_packet_t* pkt, uint32_t esn)
{
	const uint8_t* payload;
	int payloadlen, p, data_total, descs, r, cont;
	int line_bytes;

	payload = (const uint8_t*)pkt->payload;
	payloadlen = pkt->payloadlen;
	line_bytes = dec->pg_per_line * dec->pgroup;

	// extended seq(2) + at least one descriptor(6) + one pgroup
	if (payloadlen < RTP_RAW_ESS_SIZE + RTP_RAW_DESC_SIZE + dec->pgroup)
		return -EINVAL;
	if (rtp_raw_seq_known(dec, esn))
		return 1; // retransmission: first copy wins

	p = RTP_RAW_ESS_SIZE;
	data_total = 0;
	descs = 0;
	do
	{
		uint16_t length;
		unsigned int line, offset, ngroups;

		if (p + RTP_RAW_DESC_SIZE > payloadlen)
			return -EINVAL;
		length = rtp_raw_desc_length(payload + p);
		cont = rtp_raw_desc_cont(payload + p);
		offset = rtp_raw_desc_offset(payload + p);
		line = rtp_raw_desc_line(payload + p);

		if (rtp_raw_desc_f(payload + p)) // interlaced field, unsupported
			return -EINVAL;
		if (0 == length || 0 != (length % dec->pgroup))
			return -EINVAL;
		if (line < dec->line_start || line >= dec->line_start + dec->height)
			return -EINVAL;
		if (offset & 0x01U) // pgroups are two pixels wide
			return -EINVAL;
		ngroups = length / dec->pgroup;
		if ((size_t)offset / 2 + ngroups > (size_t)dec->pg_per_line)
			return -EINVAL;

		data_total += length;
		p += RTP_RAW_DESC_SIZE;
		++descs;
	} while (cont && p < payloadlen);

	// the last descriptor must clear C and consume exactly the rest of the packet
	if (cont || p + data_total != payloadlen)
		return -EINVAL;

	// second walk: descriptors and their data regions are in the same order
	{
		int d, data_cursor = p;
		for (d = 0; d < descs; ++d)
		{
			int dp = RTP_RAW_ESS_SIZE + d * RTP_RAW_DESC_SIZE;
			uint16_t length = rtp_raw_desc_length(payload + dp);
			unsigned int line = rtp_raw_desc_line(payload + dp);
			unsigned int offset = rtp_raw_desc_offset(payload + dp);
			int line_index = (int)(line - dec->line_start);
			int ngroups = length / dec->pgroup;
			uint8_t* dst = dec->frame + (size_t)line_index * line_bytes
				+ (size_t)(offset / 2) * dec->pgroup;

			if (data_cursor + length > payloadlen)
				return -EINVAL;
			memcpy(dst, payload + data_cursor, length);
			rtp_raw_cover(dec, line_index, (int)(offset / 2), ngroups);
			data_cursor += length;
		}
	}

	r = rtp_raw_seq_add(dec, esn);
	if (r < 0)
		return r;
	if (pkt->rtp.m)
		dec->marker_seen = 1;
	return 0;
}

int rtp_raw_decode_input(void* decoder, const void* packet, int bytes)
{
	struct rtp_raw_decoder_t* dec;
	struct rtp_packet_t pkt;
	uint32_t esn;
	int r, newer;

	dec = (struct rtp_raw_decoder_t*)decoder;
	if (!dec || !packet)
		return -EINVAL;
	if (0 != rtp_packet_deserialize(&pkt, packet, bytes))
		return -EINVAL;
	if (pkt.rtp.pt != dec->pt || pkt.rtp.ssrc != dec->ssrc)
		return 0; // bound to a single PT/SSRC
	if (pkt.rtp.cc != 0 || pkt.rtp.x != 0) // no CSRC / header extensions
		return 0;

	esn = ((uint32_t)pkt.payloadlen >= RTP_RAW_ESS_SIZE)
		? (((uint32_t)rtp_read_uint16((const uint8_t*)pkt.payload) << 16) | pkt.rtp.seq)
		: 0;
	if (pkt.payloadlen < RTP_RAW_ESS_SIZE)
		return -EINVAL;

	if (!dec->have_seq)
	{
		dec->have_seq = 1;
		dec->max_seq = esn;
		rtp_raw_frame_reset(dec, pkt.rtp.timestamp);
	}
	else if (dec->active && pkt.rtp.timestamp == dec->timestamp)
	{
		if (dec->delivered)
			return 0; // late packet of a frame already handed out
		if (rtp_raw_seq_known(dec, esn))
			return 0; // retransmission: first copy wins
	}
	else
	{
		newer = rtp_raw_seq_compare(esn, dec->max_seq) > 0;
		if (!newer)
			return 0; // stale packet of an abandoned/older frame

		// a newer frame starts before the previous one was completed
		if (dec->active && !dec->delivered)
			dec->pending_lost = 1;
		rtp_raw_frame_reset(dec, pkt.rtp.timestamp);
		dec->max_seq = esn;
	}

	if (rtp_raw_seq_compare(esn, dec->max_seq) > 0)
		dec->max_seq = esn;

	r = rtp_raw_apply(&dec[0], &pkt, esn);
	if (1 == r)
		return 0; // duplicate inside apply
	if (r < 0)
		return r;

	if (!dec->delivered && dec->marker_seen && dec->covered_groups == dec->total_groups)
	{
		int flags = dec->pending_lost ? RTP_PAYLOAD_FLAG_PACKET_LOST : 0;
		int cb;
		dec->pending_lost = 0; // the flag applies to one frame only
		dec->delivered = 1;
		cb = dec->handler.packet(dec->cbparam, dec->frame, dec->frame_bytes, dec->timestamp, flags);
		return cb ? cb : 1; // callback error is passed through unchanged
	}

	return 1; // packet handled, frame still incomplete
}
