#ifndef _rtp_raw_h_
#define _rtp_raw_h_

// RFC 4175 RTP Payload Format for Uncompressed Video
// progressive scan, YCbCr 4:2:2, 8-bit or 10-bit samples.
//
// The frame memory is already the packed wire byte order:
// - 8-bit:  4 bytes per 2 pixels, Cb Y0 Cr Y1
// - 10-bit: 5 bytes per 2 pixels, Cb/Y0/Cr/Y1 10-bit samples packed MSB-first
// No color conversion is done inside the library.

#include <stdint.h>
#include "rtp-payload.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rtp_raw_format
{
	unsigned int width;		/// pixels per line, even, [2, 32768]
	unsigned int height;	/// lines per frame, >= 1
	unsigned int depth;		/// bits per sample, 8 or 10
	unsigned int line_start;/// line number of the first line, line_start+height-1 <= 32767
};

/// Create RTP raw video(RFC 4175) encoder
/// @param[in] format video format, see rtp_raw_format(create fails if unsupported)
/// @param[in] mtu maximum RTP packet size in bytes, include the 12-byte RTP header
/// @param[in] payload RTP header PT filed
/// @param[in] seq RTP initial sequence number(full 32-bit, high 16 bits go into the payload)
/// @param[in] ssrc RTP header SSRC filed
/// @param[in] handler user-defined callback functions
/// @param[in] cbparam user-defined parameter
/// @return NULL-error, other-ok
void* rtp_raw_encode_create(const struct rtp_raw_format *format, int mtu, uint8_t payload, uint32_t seq, uint32_t ssrc, struct rtp_payload_t *handler, void* cbparam);
void rtp_raw_encode_destroy(void* encoder);

/// Encode one whole frame
/// @param[in] encoder RTP packet encoder(create by rtp_raw_encode_create)
/// @param[in] data packed frame, width/2*pgroup*height bytes(pgroup: 4 bytes for 8-bit, 5 bytes for 10-bit)
/// @param[in] bytes frame length in bytes, must match the format exactly
/// @param[in] timestamp RTP header timestamp(90kHz)
/// @return 0-ok, ENOMEM-alloc failed, <0-failed, other-the packet callback return value
int rtp_raw_encode_input(void* encoder, const void* data, int bytes, uint32_t timestamp);

/// Create RTP raw video(RFC 4175) decoder, bound to a single PT/SSRC stream
/// @param[in] format video format, see rtp_raw_format(create fails if unsupported)
/// @param[in] payload RTP header PT filed
/// @param[in] ssrc RTP header SSRC filed
/// @param[in] handler user-defined callback functions
/// @param[in] cbparam user-defined parameter
/// @return NULL-error, other-ok
void* rtp_raw_decode_create(const struct rtp_raw_format *format, uint8_t payload, uint32_t ssrc, struct rtp_payload_t *handler, void* cbparam);
void rtp_raw_decode_destroy(void* decoder);

/// Decode RTP packet
/// @param[in] decoder RTP packet decoder(create by rtp_raw_decode_create)
/// @param[in] packet RTP packet, include rtp header(12 bytes)
/// @param[in] bytes RTP packet length in bytes
/// @return 1-packet handled, 0-packet discard, <0-failed, other-the packet callback return value
int rtp_raw_decode_input(void* decoder, const void* packet, int bytes);

#ifdef __cplusplus
}
#endif
#endif /* !_rtp_raw_h_ */
