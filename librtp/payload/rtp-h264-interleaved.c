// RFC6184 RTP Payload Format for H.264 Video, interleaved mode.
// DON based de-interleaving entry. Packet structure is read through the shared
// rtp-h264-reader helpers so this file never re-implements H.264 parsing.

#include "rtp-h264-interleaved.h"
#include "rtp-packet.h"
#include "rtp-h264-reader.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define H264_NAL(v) ((v) & 0x1F)
#define H264_DON_HALF 0x8000 // exactly half of the 16-bit DON circle
#define H264_INTERLEAVED_MAX_DEPTH 32767

struct h264i_nal_t
{
	uint16_t don;
	uint32_t timestamp;
	uint8_t* data;
	int bytes;
	int coord; // signed DON coordinate inside the current reorder window
};

struct h264i_frag_t
{
	int active;
	int drop;            // 1 when the DON was already delivered/late, NAL discarded at completion
	uint16_t seq;         // sequence number of the last consumed fragment
	uint32_t timestamp;   // fragment RTP timestamp
	uint8_t nalu_header;  // reconstructed NAL unit header
	uint16_t don;         // DON carried by the FU-B start
	uint8_t* data;
	int size, capacity;
};

struct rtp_h264_interleaved_t
{
	struct rtp_payload_t handler;
	void* cbparam;
	int depth;

	struct h264i_nal_t* queue; // complete NAL units waiting for delivery
	int count;
	int capacity;

	uint16_t origin; // DON of queue[0], the anchor of the active reorder window

	uint16_t delivered_don; // DON of the last delivered NAL unit
	int have_delivered;

	struct h264i_frag_t frag;
};

static void h264i_frag_discard(struct h264i_frag_t* frag)
{
	frag->active = 0;
	frag->size = 0;
}

static void h264i_frag_destroy(struct h264i_frag_t* frag)
{
	if (frag->data)
		free(frag->data);
	memset(frag, 0, sizeof(*frag));
}

// Signed coordinate of don inside the current window. The active window is
// guaranteed to span less than half the DON circle, so values more than half a
// circle ahead of the origin belong to the previous turn instead.
static int h264i_coordinate(struct rtp_h264_interleaved_t* ctx, uint16_t don)
{
	uint16_t forward = (uint16_t)(don - ctx->origin);
	return forward < H264_DON_HALF ? (int)forward : (int)forward - 65536;
}

// Classify a complete NAL unit DON against the current state.
// @return 0-accept, 1-drop (already delivered or behind the delivery point),
//         <0-exactly half a circle apart
static int h264i_classify_don(struct rtp_h264_interleaved_t* ctx, uint16_t don)
{
	uint16_t anchor;
	uint16_t forward;

	// once delivery started the delivered DON is the front of the window,
	// otherwise the oldest pending NAL anchors the pre-delivery window
	if (ctx->have_delivered)
	{
		forward = (uint16_t)(don - ctx->delivered_don);
		if (0 == forward)
			return 1; // duplicate of an already delivered DON
		if (forward > H264_DON_HALF)
			return 1; // behind the delivery point, never rewind downstream
	}
	else if (ctx->count > 0)
	{
		anchor = ctx->queue[0].don; // origin while nothing has been delivered
		forward = (uint16_t)(don - anchor);
	}
	else
	{
		return 0; // first NAL of a fresh window
	}

	if (H264_DON_HALF == forward)
		return -EINVAL; // exactly half a circle apart is ambiguous
	return 0;
}

static int h264i_queue_insert(struct rtp_h264_interleaved_t* ctx, uint16_t don, uint32_t timestamp, const uint8_t* nalu, int bytes)
{
	int i, coord;
	struct h264i_nal_t* slot;
	uint8_t* copy;

	if (ctx->count >= ctx->capacity)
	{
		int capacity = ctx->capacity + 8; // grow: an earlier callback failure may have left the queue full
		void* p = realloc(ctx->queue, (size_t)capacity * sizeof(ctx->queue[0]));
		if (!p)
			return -ENOMEM;
		ctx->queue = (struct h264i_nal_t*)p;
		ctx->capacity = capacity;
	}

	copy = (uint8_t*)malloc((size_t)bytes);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, nalu, bytes);

	if (0 == ctx->count)
	{
		// start a fresh reorder window anchored at this DON
		ctx->origin = don;
		coord = 0;
	}
	else
	{
		coord = h264i_coordinate(ctx, don);
	}

	// stable insertion: ascending coordinate, equal DON keeps input order
	i = ctx->count;
	while (i > 0 && ctx->queue[i - 1].coord > coord)
		i--;
	memmove(&ctx->queue[i + 1], &ctx->queue[i], (size_t)(ctx->count - i) * sizeof(ctx->queue[0]));
	slot = &ctx->queue[i];
	slot->don = don;
	slot->timestamp = timestamp;
	slot->data = copy;
	slot->bytes = bytes;
	slot->coord = coord;
	ctx->count++;
	return 0;
}

static int h264i_deliver_one(struct rtp_h264_interleaved_t* ctx)
{
	int r, i;
	struct h264i_nal_t* head;
	uint16_t don;

	head = &ctx->queue[0];
	r = ctx->handler.packet(ctx->cbparam, head->data, head->bytes, head->timestamp, 0);
	if (0 != r)
		return r; // callback refused the NAL: keep it queued so a later flush retries

	don = head->don;
	free(head->data);
	ctx->count--;
	memmove(&ctx->queue[0], &ctx->queue[1], (size_t)ctx->count * sizeof(ctx->queue[0]));

	// keep the coordinate anchor at the oldest pending NAL so every active NAL
	// stays within half a circle of it while the window slides forward
	if (ctx->count > 0)
	{
		ctx->origin = ctx->queue[0].don;
		for (i = 0; i < ctx->count; i++)
			ctx->queue[i].coord = h264i_coordinate(ctx, ctx->queue[i].don);
	}

	ctx->delivered_don = don;
	ctx->have_delivered = 1;
	return 0;
}

// Push the retained population back down to depth, earliest DON first.
static int h264i_drain(struct rtp_h264_interleaved_t* ctx)
{
	int r;

	while (ctx->count > ctx->depth)
	{
		r = h264i_deliver_one(ctx);
		if (0 != r)
			return r; // callback error: stop every later callback in this call
	}
	return 0;
}

// Accept one already validated complete NAL unit; drops stay silent.
static int h264i_accept(struct rtp_h264_interleaved_t* ctx, uint16_t don, uint32_t timestamp, const uint8_t* nalu, int bytes)
{
	int r;

	r = h264i_classify_don(ctx, don);
	if (r < 0)
		return r;
	if (r > 0)
		return 0; // late/duplicate NAL, dropped

	r = h264i_queue_insert(ctx, don, timestamp, nalu, bytes);
	if (0 != r)
		return r;
	return h264i_drain(ctx);
}

rtp_h264_interleaved_t* rtp_h264_interleaved_create(int depth, struct rtp_payload_t* handler, void* cbparam)
{
	rtp_h264_interleaved_t* ctx;

	if (depth < 0 || depth > H264_INTERLEAVED_MAX_DEPTH || !handler || !handler->packet)
		return NULL;

	ctx = (rtp_h264_interleaved_t*)calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	// normally depth retained NAL units plus the one being inserted suffice;
	// the queue grows itself if a previous callback error left more pending
	ctx->queue = (struct h264i_nal_t*)calloc((size_t)depth + 1, sizeof(ctx->queue[0]));
	if (!ctx->queue)
	{
		free(ctx);
		return NULL;
	}
	ctx->capacity = depth + 1;

	memcpy(&ctx->handler, handler, sizeof(ctx->handler));
	ctx->cbparam = cbparam;
	ctx->depth = depth;
	return ctx;
}

void rtp_h264_interleaved_destroy(rtp_h264_interleaved_t* ctx)
{
	int i;

	if (!ctx)
		return;

	for (i = 0; i < ctx->count; i++)
		free(ctx->queue[i].data);
	free(ctx->queue);
	h264i_frag_destroy(&ctx->frag);
	free(ctx);
}

int rtp_h264_interleaved_flush(rtp_h264_interleaved_t* ctx)
{
	int r;

	if (!ctx)
		return -EINVAL;

	// an unfinished FU is not a complete NAL unit and stays undelivered
	while (ctx->count > 0)
	{
		r = h264i_deliver_one(ctx);
		if (0 != r)
			return r;
	}
	return 0;
}

// Walk an aggregation packet once without publishing anything, so a malformed
// member rejects the whole packet atomically.
static int h264i_validate_stap(struct rtp_h264_interleaved_t* ctx, const uint8_t* payload, int bytes, uint32_t timestamp)
{
	int r;
	struct rtp_h264_stap_reader_t reader;
	struct rtp_h264_agg_unit_t unit;

	if (0 != rtp_h264_stap_reader_init(&reader, payload, bytes, 1))
		return -EINVAL;

	while (1)
	{
		r = rtp_h264_stap_reader_next(&reader, timestamp, &unit);
		if (r < 0)
			return -EINVAL;
		if (0 == r)
			break;
		if (H264_NAL(unit.nalu[0]) == 0 || H264_NAL(unit.nalu[0]) >= 24)
			return -EINVAL;
		r = h264i_classify_don(ctx, unit.don);
		if (r < 0)
			return r;
	}

	return 0 == reader.remain ? 0 : -EINVAL; // no unparsed trailing bytes
}

static int h264i_handle_stap(struct rtp_h264_interleaved_t* ctx, const uint8_t* payload, int bytes, uint32_t timestamp)
{
	int r;
	struct rtp_h264_stap_reader_t reader;
	struct rtp_h264_agg_unit_t unit;

	r = h264i_validate_stap(ctx, payload, bytes, timestamp);
	if (0 != r)
		return r;

	if (0 != rtp_h264_stap_reader_init(&reader, payload, bytes, 1))
		return -EINVAL;

	while (1)
	{
		r = rtp_h264_stap_reader_next(&reader, timestamp, &unit);
		if (r <= 0)
			break;
		r = h264i_accept(ctx, unit.don, unit.timestamp, unit.nalu, unit.bytes);
		if (0 != r)
			return r;
	}
	return 1; // packet consumed
}

static int h264i_validate_mtap(struct rtp_h264_interleaved_t* ctx, const uint8_t* payload, int bytes, int offset_bytes, uint32_t timestamp)
{
	int r;
	struct rtp_h264_mtap_reader_t reader;
	struct rtp_h264_agg_unit_t unit;

	if (0 != rtp_h264_mtap_reader_init(&reader, payload, bytes, offset_bytes, timestamp))
		return -EINVAL;

	while (1)
	{
		r = rtp_h264_mtap_reader_next(&reader, &unit);
		if (r < 0)
			return -EINVAL;
		if (0 == r)
			break;
		if (H264_NAL(unit.nalu[0]) == 0 || H264_NAL(unit.nalu[0]) >= 24)
			return -EINVAL;
		r = h264i_classify_don(ctx, unit.don);
		if (r < 0)
			return r;
	}

	return 0 == reader.remain ? 0 : -EINVAL;
}

static int h264i_handle_mtap(struct rtp_h264_interleaved_t* ctx, const uint8_t* payload, int bytes, int offset_bytes, uint32_t timestamp)
{
	int r;
	struct rtp_h264_mtap_reader_t reader;
	struct rtp_h264_agg_unit_t unit;

	r = h264i_validate_mtap(ctx, payload, bytes, offset_bytes, timestamp);
	if (0 != r)
		return r;

	if (0 != rtp_h264_mtap_reader_init(&reader, payload, bytes, offset_bytes, timestamp))
		return -EINVAL;

	while (1)
	{
		r = rtp_h264_mtap_reader_next(&reader, &unit);
		if (r <= 0)
			break;
		r = h264i_accept(ctx, unit.don, unit.timestamp, unit.nalu, unit.bytes);
		if (0 != r)
			return r;
	}
	return 1; // packet consumed
}

static int h264i_frag_append(struct h264i_frag_t* frag, const struct rtp_h264_fu_t* fu)
{
	if (frag->size + fu->bytes + 1 > frag->capacity)
	{
		int capacity = frag->size + fu->bytes + 1;
		void* p;
		capacity += capacity / 4 > 128000 ? capacity / 4 : 128000;
		p = realloc(frag->data, (size_t)capacity);
		if (!p)
			return -ENOMEM;
		frag->data = (uint8_t*)p;
		frag->capacity = capacity;
	}

	if (fu->bytes > 0)
	{
		memmove(frag->data + frag->size, fu->data, (size_t)fu->bytes);
		frag->size += fu->bytes;
	}
	return 0;
}

// FU-B starts carry the DON; FU-A packets may only continue an active fragment.
static int h264i_handle_fu(struct rtp_h264_interleaved_t* ctx, const uint8_t* payload, int bytes, uint16_t seq, uint32_t timestamp, int fu_b)
{
	int r;
	struct rtp_h264_fu_t fu;
	struct h264i_frag_t* frag;

	if (0 != rtp_h264_read_fu(payload, bytes, fu_b, &fu))
		return -EINVAL;

	frag = &ctx->frag;

	if (fu_b)
	{
		// only FU-B start fragments are accepted, a continuation must use FU-A
		if (!fu.start)
			return -EINVAL;
		// fragmentation is only defined for regular NAL unit types (RFC6184 5.8)
		if (H264_NAL(fu.nalu_header) == 0 || H264_NAL(fu.nalu_header) >= 24)
			return -EINVAL;

		r = h264i_classify_don(ctx, fu.don);
		if (r < 0)
			return r;

		h264i_frag_discard(frag); // a new start replaces any unfinished fragment
		frag->active = 1;
		frag->drop = r > 0; // late/duplicate DON: reassemble anyway, discard at completion
		frag->don = fu.don;
		frag->seq = seq;
		frag->timestamp = timestamp;
		frag->nalu_header = fu.nalu_header;
		frag->size = 0;

		// reserve room for the reconstructed NAL header plus this fragment
		if (frag->capacity < 1 + fu.bytes)
		{
			void* p = realloc(frag->data, (size_t)(1 + fu.bytes + 128000));
			if (!p)
				return -ENOMEM;
			frag->data = (uint8_t*)p;
			frag->capacity = 1 + fu.bytes + 128000;
		}
		frag->data[0] = fu.nalu_header;
		frag->size = 1;

		r = h264i_frag_append(frag, &fu);
		if (0 != r)
			return r;
	}
	else
	{
		// FU-A starts carry no DON and are not part of the interleaved profile
		if (fu.start)
			return -EINVAL;
		if (!frag->active)
			return 0; // orphan continuation, nothing to attach to

		// only consecutive fragments sharing timestamp and NAL type belong together
		if ((uint16_t)(frag->seq + 1) != seq || frag->timestamp != timestamp ||
			H264_NAL(frag->nalu_header) != H264_NAL(fu.nalu_header))
		{
			h264i_frag_discard(frag); // lost/mismatched fragment, queue stays usable
			return 0;
		}

		frag->seq = seq;
		r = h264i_frag_append(frag, &fu);
		if (0 != r)
			return r;
	}

	if (!fu.end)
		return 1; // wait for more continuations

	// fragment complete: publish it as one complete NAL unit
	if (!frag->drop)
		r = h264i_accept(ctx, frag->don, frag->timestamp, frag->data, frag->size);
	else
		r = 0;
	h264i_frag_discard(frag);
	return 0 == r ? 1 : r;
}

int rtp_h264_interleaved_input(rtp_h264_interleaved_t* ctx, const void* packet, int bytes)
{
	int nalt;
	struct rtp_packet_t pkt;

	if (!ctx || !packet || 0 != rtp_packet_deserialize(&pkt, packet, bytes) || pkt.payloadlen < 1)
		return -EINVAL;

	nalt = H264_NAL(((const uint8_t*)pkt.payload)[0]);
	switch (nalt)
	{
	case 25: // STAP-B
		return h264i_handle_stap(ctx, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp);

	case 26: // MTAP16
		return h264i_handle_mtap(ctx, (const uint8_t*)pkt.payload, pkt.payloadlen, 2, pkt.rtp.timestamp);

	case 27: // MTAP24
		return h264i_handle_mtap(ctx, (const uint8_t*)pkt.payload, pkt.payloadlen, 3, pkt.rtp.timestamp);

	case 28: // FU-A continuation
	case 29: // FU-B start
		return h264i_handle_fu(ctx, (const uint8_t*)pkt.payload, pkt.payloadlen, (uint16_t)pkt.rtp.seq, pkt.rtp.timestamp, 29 == nalt);

	default: // single NAL (1-23), STAP-A (24) and reserved types carry no DON here
		return -EINVAL;
	}
}
