// RFC6184 RTP Payload Format for H.264 Video
//
// 6.2. Single NAL Unit Mode (All receivers MUST support this mode)
//   packetization-mode media type parameter is equal to 0 or the packetization - mode is not present.
//   Only single NAL unit packets MAY be used in this mode.
//   STAPs, MTAPs, and FUs MUST NOT be used.
//   The transmission order of single NAL unit packets MUST comply with the NAL unit decoding order.
// 6.3. Non-Interleaved Mode (This mode SHOULD be supported)
//   packetization-mode media type parameter is equal to 1.
//   Only single NAL unit packets, STAP - As, and FU - As MAY be used in this mode.
//   STAP-Bs, MTAPs, and FU-Bs MUST NOT be used.
//   The transmission order of NAL units MUST comply with the NAL unit decoding order
// 6.4. Interleaved Mode
//   packetization-mode media type parameter is equal to 2.
//   STAP-Bs, MTAPs, FU-As, and FU-Bs MAY be used.
//   STAP-As and single NAL unit packets MUST NOT be used.
//   The transmission order of packets and NAL units is constrained as specified in Section 5.5.
//
// The default decoder hands NAL units to the callback in transmission order.
// De-interleaving by decoding order number is an opt-in entry point, see
// <rtp-h264-interleaved.h>.

#include "rtp-h264-internal.h"
#include "rtp-payload-internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct rtp_decode_h264_t
{
	struct rtp_payload_t handler;
	void* cbparam;
	struct rtp_h264_reader_t reader;
};

static int rtp_h264_unpack_onnalu(void* param, const struct rtp_h264_nalu_t* nalu)
{
	struct rtp_decode_h264_t *unpacker;
	unpacker = (struct rtp_decode_h264_t *)param;
	return unpacker->handler.packet(unpacker->cbparam, nalu->data, nalu->bytes, nalu->timestamp, nalu->flags);
}

static void* rtp_h264_unpack_create(struct rtp_payload_t *handler, void* param)
{
	struct rtp_decode_h264_t *unpacker;
	unpacker = (struct rtp_decode_h264_t *)calloc(1, sizeof(*unpacker));
	if(!unpacker)
		return NULL;

	memcpy(&unpacker->handler, handler, sizeof(unpacker->handler));
	unpacker->cbparam = param;
	rtp_h264_reader_init(&unpacker->reader);
	return unpacker;
}

static void rtp_h264_unpack_destroy(void* p)
{
	struct rtp_decode_h264_t *unpacker;
	unpacker = (struct rtp_decode_h264_t *)p;

	rtp_h264_reader_free(&unpacker->reader);
#if defined(_DEBUG) || defined(DEBUG)
	memset(unpacker, 0xCC, sizeof(*unpacker));
#endif
	free(unpacker);
}

static int rtp_h264_unpack_input(void* p, const void* packet, int bytes)
{
	struct rtp_decode_h264_t *unpacker;
	unpacker = (struct rtp_decode_h264_t *)p;
	if(!unpacker)
		return -EINVAL;

	// non-interleaved entry: never accept interleaved-only payloads
	return rtp_h264_reader_input(&unpacker->reader, packet, bytes, rtp_h264_unpack_onnalu, unpacker, 0);
}

struct rtp_payload_decode_t *rtp_h264_decode()
{
	static struct rtp_payload_decode_t unpacker = {
		rtp_h264_unpack_create,
		rtp_h264_unpack_destroy,
		rtp_h264_unpack_input,
	};

	return &unpacker;
}
