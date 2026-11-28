#ifndef _rtp_raw_h_
#define _rtp_raw_h_

// RFC 4175 RTP Payload Format for Uncompressed Video
// Progressive YCbCr 4:2:2, 8-bit and 10-bit samples only.
//
// Every source scan line is already stored in the packed on-wire pgroup
// byte order, so the library never performs any color space conversion:
//   depth 8:  two pixels per 4-byte pgroup, Cb Y0 Cr Y1
//   depth 10: two pixels per 5-byte pgroup, four 10-bit components
//             (Cb Y0 Cr Y1) packed consecutively from the most
//             significant bit, RFC 4175 section 4.3 (Figure 5).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rtp_raw_format
{
	unsigned int width;		// pixels per line, must be a positive even number
	unsigned int height;	// number of active lines, must be positive
	unsigned int depth;		// sample depth: 8 or 10
	unsigned int line_start; // RTP line number of the first active line (0-based)
};

struct rtp_payload_t;

/// Create an RFC 4175 uncompressed-video RTP encoder.
/// @param[in] fmt packed frame geometry, copied on success
/// @param[in] mtu maximum RTP packet size including the 12-byte RTP header
/// @param[in] payload RTP payload type (PT)
/// @param[in] seq initial 32-bit extended sequence number
/// @param[in] ssrc RTP SSRC field
/// @param[in] handler alloc/free/packet callbacks
/// @param[in] cbparam user parameter handed to the callbacks
/// @return NULL on unsupported configuration or allocation failure
void* rtp_raw_encode_create(const struct rtp_raw_format* fmt, int mtu, uint8_t payload, uint32_t seq, uint32_t ssrc, struct rtp_payload_t* handler, void* cbparam);

/// Packetize one whole packed frame.
/// @param[in] frame packed frame, exactly (width/2 * pgroup * height) bytes
/// @param[in] bytes frame length in bytes
/// @param[in] timestamp 90kHz RTP timestamp, kept on every packet
/// @return 0-ok, ENOMEM-alloc failed, EINVAL-bad frame, other-callback error
int rtp_raw_encode_input(void* encoder, const void* frame, int bytes, uint32_t timestamp);

void rtp_raw_encode_destroy(void* encoder);

/// Create an RFC 4175 uncompressed-video RTP decoder bound to one PT/SSRC.
/// @return NULL on unsupported configuration or allocation failure
void* rtp_raw_decode_create(const struct rtp_raw_format* fmt, uint8_t payload, uint32_t ssrc, struct rtp_payload_t* handler, void* cbparam);

/// Feed one complete RTP packet (including the 12-byte RTP header).
/// @return 1-packet handled, 0-packet discarded (wrong PT/SSRC or stale), <0-error
int rtp_raw_decode_input(void* decoder, const void* packet, int bytes);

void rtp_raw_decode_destroy(void* decoder);

#ifdef __cplusplus
}
#endif
#endif /* !_rtp_raw_h_ */
