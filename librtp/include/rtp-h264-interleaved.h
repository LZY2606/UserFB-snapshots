#ifndef _rtp_h264_interleaved_h_
#define _rtp_h264_interleaved_h_

// RFC6184 RTP Payload Format for H.264 Video
//
// 6.4. Interleaved Mode (packetization-mode=2)
//
//   STAP-Bs, MTAPs, FU-As, and FU-Bs MAY be used.
//   STAP-As and single NAL unit packets MUST NOT be used.
//   The transmission order of packets and NAL units is constrained as
//   specified in Section 5.5: NAL units may be transmitted out of decoding
//   order, and every complete NAL unit carries a 16-bit Decoding Order
//   Number (DON) that defines the decoding order.
//
// This module is an opt-in de-interleaving entry point. The default H.264
// decoder (rtp_payload_decode_create / rtp_h264_decode) is left untouched and
// continues to hand NAL units downstream in transmission order. Callers that
// receive packetization-mode=2 streams must explicitly create this decoder.
//
// The decoder shares the RTP/STAP/MTAP/FU structure reader with the default
// decoder, so both entry points accept the same wire format.

#include <stdint.h>
#include "rtp-payload.h"

#if defined(__cplusplus)
extern "C" {
#endif

/// Largest reorder depth accepted by rtp_h264_interleaved_create().
#define RTP_H264_INTERLEAVED_MAX_DEPTH 32767

struct rtp_h264_interleaved_t;

/// Create an H.264 interleaved-mode de-packetizer.
///
/// NAL units are held until the pending count exceeds `depth`; then the one
/// with the earliest decoding order number (DON) is delivered. `depth` is the
/// maximum number of complete NAL units kept pending: 0 delivers as soon as a
/// packet has been parsed, `RTP_H264_INTERLEAVED_MAX_DEPTH` allows the widest
/// reordering window. Values outside [0, RTP_H264_INTERLEAVED_MAX_DEPTH] fail.
///
/// @param[in] depth maximum number of complete NAL units kept pending [0,32767]
/// @param[in] handler user-defined callback functions; handler and
///                    handler->packet must both be non-NULL
/// @param[in] param user-defined parameter passed to the callbacks
/// @return NULL-error, other-de-packetizer
struct rtp_h264_interleaved_t* rtp_h264_interleaved_create(int depth, struct rtp_payload_t* handler, void* param);

/// Destroy the de-packetizer. Only memory is released; no callback is invoked,
/// and pending complete NAL units are dropped.
void rtp_h264_interleaved_destroy(struct rtp_h264_interleaved_t* deinterleaver);

/// Input one complete RTP packet (including the 12-byte RTP header).
///
/// Input preconditions:
///  - Packets are supplied in network sequence order. Network reordering is
///    not handled here; a sequence number of 65535 followed by 0 is treated
///    as contiguous.
///  - Only interleaved-mode payloads are accepted: STAP-B, MTAP16/MTAP24,
///    and fragmented NAL units that start with FU-B and continue with FU-A.
///    Single NAL unit packets and STAP-A are discarded (return 0).
///
/// After all NAL units of the packet have been collected, while the number of
/// pending complete NAL units is greater than `depth`, the pending NAL unit
/// with the earliest DON is delivered, until the count is back to `depth`.
/// NAL units with the same DON are kept in input order. A complete NAL unit
/// whose DON is earlier than an already delivered DON is discarded, so the
/// downstream decoder never moves backwards in decoding order.
///
/// `handler->packet` receives a pointer that is valid only for the duration of
/// that call; the caller must copy the bytes if it needs them afterwards. It
/// does not alias the input RTP packet.
///
/// @param[in] deinterleaver de-packetizer created by rtp_h264_interleaved_create
/// @param[in] packet complete RTP packet (RTP header included)
/// @param[in] bytes RTP packet length in bytes
/// @return 1-packet handled, 0-packet discarded, <0-error. When a callback
///         returns non-zero, that value is returned unchanged and no further
///         callbacks are made in this call.
int rtp_h264_interleaved_input(struct rtp_h264_interleaved_t* deinterleaver, const void* packet, int bytes);

/// Deliver all remaining complete NAL units in decoding (DON) order.
///
/// NAL units that are still being reassembled (an FU without its final
/// fragment) are not complete and are never delivered by flush.
///
/// @return 0-ok, <0-error (including a callback's non-zero return value)
int rtp_h264_interleaved_flush(struct rtp_h264_interleaved_t* deinterleaver);

#if defined(__cplusplus)
}
#endif
#endif /* !_rtp_h264_interleaved_h_ */
