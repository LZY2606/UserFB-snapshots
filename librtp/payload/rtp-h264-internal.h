#ifndef _rtp_h264_internal_h_
#define _rtp_h264_internal_h_

#include <stdint.h>
#include "rtp-packet.h"

#if defined(__cplusplus)
extern "C" {
#endif

/// NAL unit type of a H.264 NAL header byte
#define RTP_H264_NAL_TYPE(v)	((v) & 0x1F)
/// FU header start/end bits (RFC6184 5.8)
#define RTP_H264_FU_START(v)	((v) & 0x80)
#define RTP_H264_FU_END(v)		((v) & 0x40)

/// A complete NAL unit produced by the shared RTP/H.264 payload reader.
struct rtp_h264_nalu_t
{
	const uint8_t* data;	// NAL unit bytes(start with the NAL unit header)
	int bytes;				// NAL unit size in bytes(aggregation fields excluded)
	uint32_t timestamp;		// NALU timestamp
	uint16_t don;			// decoding order number(valid only if has_don)
	int has_don;			// 1-the NAL unit carried/derived a DON, 0-no DON
	int flags;				// RTP_PAYLOAD_FLAG_PACKET_xxx
};

/// Called once for every complete NAL unit. The `data` pointer is only valid
/// during the callback.
/// @return 0-ok, other-stop parsing and return the value to the caller
typedef int (*rtp_h264_nalu_handler)(void* param, const struct rtp_h264_nalu_t* nalu);

/// Shared H.264 RTP payload reader state.
/// The default(non interleaved) decoder and the interleaved decoder both drive
/// this state, so the RTP packet / STAP / MTAP / FU parsing exists only once.
struct rtp_h264_reader_t
{
	uint16_t seq;			// last RTP sequence number
	int flags;				// pending RTP_PAYLOAD_FLAG_PACKET_xxx

	uint8_t* fu;			// fragmented NAL unit reassembly buffer
	int fu_size;
	int fu_capacity;
	int fu_active;			// 1-a FU is being reassembled
	uint16_t fu_seq;		// sequence number of the last FU fragment
	uint32_t fu_timestamp;	// timestamp of the FU
	uint16_t fu_don;		// DON carried by the FU-B start fragment
	int fu_has_don;			// 1-the FU start fragment carried a DON(FU-B)
	int fu_nal_type;		// reconstructed NAL unit type

	int mtap_size_nal_only;	// 1-MTAP NALU size counts only the NAL unit,
							// 0-MTAP NALU size covers DOND + TS offset
							//   (the historical default decoder reading)
	int strict;				// 1-any incomplete member/trailing byte is an
							//   error, 0-ignore a trailing run too short to
							//   hold a size field (historical behaviour)
};

void rtp_h264_reader_init(struct rtp_h264_reader_t* reader);
void rtp_h264_reader_free(struct rtp_h264_reader_t* reader);

/// Parse one complete RTP packet and emit its complete NAL units.
/// @param[in] reader reader state
/// @param[in] packet RTP packet(include the RTP header)
/// @param[in] bytes RTP packet length in bytes
/// @param[in] handler complete NAL unit callback
/// @param[in] param callback param
/// @param[in] interleaved 0-default packetization modes, 1-interleaved mode only
/// @return 1-packet handled, 0-packet discarded, other-error(or handler value)
int rtp_h264_reader_input(struct rtp_h264_reader_t* reader, const void* packet, int bytes,
	rtp_h264_nalu_handler handler, void* param, int interleaved);

#if defined(__cplusplus)
}
#endif
#endif /* !_rtp_h264_internal_h_ */
