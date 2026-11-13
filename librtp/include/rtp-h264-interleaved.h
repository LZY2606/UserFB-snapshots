#ifndef _rtp_h264_interleaved_h_
#define _rtp_h264_interleaved_h_

// RFC6184 RTP Payload Format for H.264 Video, interleaved packetization
// (STAP-B / MTAP16 / MTAP24 / FU-B + FU-A). This decoder reorders complete
// NAL units by their Decoding Order Number (DON) before handing them to the
// rtp_payload_t callback. The default rtp_h264_decode() entry keeps the
// historical transmission-order behavior and is unchanged.

#include "rtp-payload.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtp_h264_interleaved_t rtp_h264_interleaved_t;

/// Create a DON aware H.264 de-interleaver.
/// @param[in] depth Maximum number of complete NAL units that may be retained
///                  waiting for earlier-DON units. Range [0, 32767]. Once more
///                  than depth complete NAL units are pending, the one with the
///                  earliest DON is delivered until depth units remain. A depth
///                  of 0 delivers every complete NAL unit immediately.
/// @param[in] handler Callbacks; only the packet() callback is used. The bytes
///                    passed to packet() are valid for that call only, the
///                    caller does not retain the input RTP packet.
/// @param[in] cbparam User parameter forwarded to every callback.
/// @return NULL when depth is out of range or handler/packet is missing.
rtp_h264_interleaved_t* rtp_h264_interleaved_create(int depth, struct rtp_payload_t* handler, void* cbparam);

/// Feed one complete RTP packet (including the 12-byte RTP header).
///
/// Input preconditions:
/// - packets are already ordered by RTP sequence number (network reordering is
///   not handled); sequence 65535 followed by 0 counts as continuous;
/// - only interleaved payloads are accepted: STAP-B (25), MTAP16 (26),
///   MTAP24 (27), FU-B (29) fragments carrying the start bit and FU-A (28)
///   continuations. Single NAL units and STAP-A carry no DON and are rejected;
/// - the active reorder window spans less than half of the 16-bit DON circle.
///
/// DON handling: DON is a 16-bit wrapping number and NAL units with the same
/// DON stay in input (stable) order. A complete NAL whose DON is not after the
/// already delivered DON is dropped. Two DON values exactly half a circle
/// apart are reported as an error.
///
/// Aggregation packets are parsed atomically: a malformed member makes the
/// whole packet fail and none of its members is published. An FU-B start
/// discards any unfinished previous fragment; FU-A continuations only attach
/// when sequence number, timestamp and reconstructed NAL unit type match.
///
/// @return 1-packet consumed (it may deliver zero or more NAL units),
///         0-packet discarded (e.g. orphan FU-A continuation),
///         <0-error including a nonzero value returned by packet() or a
///         malformed packet; when packet() fails no further callback is made
///         for this call and its error is returned unchanged.
int rtp_h264_interleaved_input(rtp_h264_interleaved_t* ctx, const void* packet, int bytes);

/// Deliver every retained complete NAL unit in DON order (same-DON units keep
/// input order). An unfinished FU fragment is not a complete NAL unit and is
/// not delivered. Callback errors stop the flush and are returned unchanged.
/// @return 0 on success, <0 on callback/parameter error.
int rtp_h264_interleaved_flush(rtp_h264_interleaved_t* ctx);

/// Release all owned memory. No callback is invoked, retained NAL units and
/// unfinished fragments are discarded.
void rtp_h264_interleaved_destroy(rtp_h264_interleaved_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* !_rtp_h264_interleaved_h_ */
