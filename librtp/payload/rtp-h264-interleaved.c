// RFC6184 RTP Payload Format for H.264 Video
//
// 6.4. Interleaved Mode (packetization-mode=2)
//
// Opt-in de-interleaving entry point. The wire parsing is delegated to the
// shared reader (rtp-h264-reader.c) that the default decoder also uses, so this
// file only implements the decoding-order (DON) reordering window.

#include "rtp-h264-interleaved.h"
#include "rtp-h264-internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct rtp_h264_pending_t
{
	uint16_t don;
	uint32_t timestamp;
	int flags;
	int bytes;
	uint8_t* data;
};

struct rtp_h264_interleaved_t
{
	struct rtp_payload_t handler;
	void* cbparam;

	int depth;
	struct rtp_h264_reader_t reader;

	struct rtp_h264_pending_t* pending;
	int count;
	int capacity;

	int has_delivered;
	uint16_t delivered_don;
};

/// Difference of two 16-bit wrap-around DONs.
/// @return -32768(exactly half a circle), otherwise [-32767, 32767]
static int rtp_h264_don_diff(uint16_t a, uint16_t b)
{
	int d = (int)((uint16_t)(a - b));
	if (d >= 32768)
		d -= 65536;
	return d;
}

static int rtp_h264_pending_reserve(struct rtp_h264_interleaved_t* deinterleaver, int need)
{
	int capacity;
	struct rtp_h264_pending_t* p;

	if (need <= deinterleaver->capacity)
		return 0;

	capacity = deinterleaver->capacity > 0 ? deinterleaver->capacity : 16;
	while (capacity < need)
		capacity *= 2;

	p = (struct rtp_h264_pending_t*)realloc(deinterleaver->pending, (size_t)capacity * sizeof(*p));
	if (!p)
		return -ENOMEM;

	deinterleaver->pending = p;
	deinterleaver->capacity = capacity;
	return 0;
}

/// Insert a complete NAL unit into the decoding-order queue.
/// @return 1-inserted, 0-dropped(late), <0-error
static int rtp_h264_pending_insert(struct rtp_h264_interleaved_t* deinterleaver, const struct rtp_h264_nalu_t* nalu)
{
	struct rtp_h264_pending_t item;
	uint8_t* data;
	int i, d, r;

	// A complete NAL unit earlier than an already delivered DON would make
	// the downstream move backwards; drop it. Exactly half a circle apart is
	// ambiguous and is reported as an input error. A NAL unit with the same
	// DON is kept so that members sharing a DON stay in input order.
	if (deinterleaver->has_delivered)
	{
		d = rtp_h264_don_diff(nalu->don, deinterleaver->delivered_don);
		if (d == -32768)
			return -ERANGE;
		if (d < 0)
			return 0; // late NAL, already passed
	}

	for (i = 0; i < deinterleaver->count; i++)
	{
		d = rtp_h264_don_diff(nalu->don, deinterleaver->pending[i].don);
		if (d == -32768)
			return -ERANGE; // half a circle, cannot order
	}

	r = rtp_h264_pending_reserve(deinterleaver, deinterleaver->count + 1);
	if (r < 0)
		return r;

	data = (uint8_t*)malloc((size_t)nalu->bytes);
	if (!data)
		return -ENOMEM;
	memcpy(data, nalu->data, (size_t)nalu->bytes);

	item.don = nalu->don;
	item.timestamp = nalu->timestamp;
	item.flags = nalu->flags;
	item.bytes = nalu->bytes;
	item.data = data;

	// stable insertion: walk back over entries the new DON precedes, then
	// place it after any entry with the same DON
	i = deinterleaver->count;
	while (i > 0)
	{
		d = rtp_h264_don_diff(item.don, deinterleaver->pending[i - 1].don);
		if (d < 0)
		{
			i--;
			continue;
		}
		break;
	}

	if (i < deinterleaver->count)
		memmove(&deinterleaver->pending[i + 1], &deinterleaver->pending[i],
			(size_t)(deinterleaver->count - i) * sizeof(deinterleaver->pending[0]));
	deinterleaver->pending[i] = item;
	deinterleaver->count++;
	return 1;
}

/// Deliver the earliest pending NAL unit.
/// @return 0-ok, other-callback return value
static int rtp_h264_pending_deliver(struct rtp_h264_interleaved_t* deinterleaver)
{
	struct rtp_h264_pending_t item;
	int r;

	item = deinterleaver->pending[0];
	deinterleaver->count--;
	if (deinterleaver->count > 0)
		memmove(&deinterleaver->pending[0], &deinterleaver->pending[1],
			(size_t)deinterleaver->count * sizeof(deinterleaver->pending[0]));

	deinterleaver->has_delivered = 1;
	deinterleaver->delivered_don = item.don;

	// the bytes are only promised for the duration of this callback
	r = deinterleaver->handler.packet(deinterleaver->cbparam, item.data, item.bytes, item.timestamp, item.flags);
	free(item.data);
	return r;
}

static int rtp_h264_pending_drain(struct rtp_h264_interleaved_t* deinterleaver, int limit)
{
	int r;

	while (deinterleaver->count > limit)
	{
		r = rtp_h264_pending_deliver(deinterleaver);
		if (r)
			return r;
	}
	return 0;
}

static int rtp_h264_interleaved_onnalu(void* param, const struct rtp_h264_nalu_t* nalu)
{
	struct rtp_h264_interleaved_t* deinterleaver;
	int r;

	deinterleaver = (struct rtp_h264_interleaved_t*)param;

	// Interleaved mode requires a DON on every complete NAL unit; the shared
	// reader already rejects payload types without one.
	if (!nalu->has_don || nalu->bytes <= 0)
		return 0;

	r = rtp_h264_pending_insert(deinterleaver, nalu);
	if (r < 0)
		return r;
	return 0; // 0-inserted or dropped-late, keep reading this packet
}

struct rtp_h264_interleaved_t* rtp_h264_interleaved_create(int depth, struct rtp_payload_t* handler, void* param)
{
	struct rtp_h264_interleaved_t* deinterleaver;

	if (depth < 0 || depth > RTP_H264_INTERLEAVED_MAX_DEPTH)
		return NULL;
	if (!handler || !handler->packet)
		return NULL;

	deinterleaver = (struct rtp_h264_interleaved_t*)calloc(1, sizeof(*deinterleaver));
	if (!deinterleaver)
		return NULL;

	memcpy(&deinterleaver->handler, handler, sizeof(deinterleaver->handler));
	deinterleaver->cbparam = param;
	deinterleaver->depth = depth;
	rtp_h264_reader_init(&deinterleaver->reader);
	// interleaved entry uses RFC6184 5.7.2 sizes: NALU size counts only the
	// NAL unit, DOND/TS offset are not part of it
	deinterleaver->reader.mtap_size_nal_only = 1;
	// the interleaved entry never ignores an incomplete aggregation member
	deinterleaver->reader.strict = 1;
	return deinterleaver;
}

void rtp_h264_interleaved_destroy(struct rtp_h264_interleaved_t* deinterleaver)
{
	int i;

	if (!deinterleaver)
		return;

	for (i = 0; i < deinterleaver->count; i++)
		free(deinterleaver->pending[i].data);
	free(deinterleaver->pending);
	rtp_h264_reader_free(&deinterleaver->reader);

#if defined(_DEBUG) || defined(DEBUG)
	memset(deinterleaver, 0xCC, sizeof(*deinterleaver));
#endif
	free(deinterleaver);
}

int rtp_h264_interleaved_input(struct rtp_h264_interleaved_t* deinterleaver, const void* packet, int bytes)
{
	int r;

	if (!deinterleaver)
		return -EINVAL;

	r = rtp_h264_reader_input(&deinterleaver->reader, packet, bytes,
		rtp_h264_interleaved_onnalu, deinterleaver, 1);
	if (r < 0)
		return r; // malformed packet, ordering error or callback error
	if (0 == r)
		return 0; // packet discarded, nothing to deliver

	// all NAL units of the packet are collected: release the backlog until
	// only `depth` complete NAL units remain pending
	r = rtp_h264_pending_drain(deinterleaver, deinterleaver->depth);
	if (r)
		return r;

	return 1; // packet handled
}

int rtp_h264_interleaved_flush(struct rtp_h264_interleaved_t* deinterleaver)
{
	if (!deinterleaver)
		return -EINVAL;

	// incomplete FU fragments are not complete NAL units and are never flushed
	return rtp_h264_pending_drain(deinterleaver, 0);
}
