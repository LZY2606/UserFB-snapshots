#ifndef _rtp_raw_internal_h_
#define _rtp_raw_internal_h_

// RFC 4175 RTP Payload Format for Uncompressed Video
// Shared helpers for the progressive YCbCr 4:2:2 packer/unpacker.

#include "rtp-raw.h"
#include "rtp-payload.h"
#include "rtp-util.h"
#include <limits.h>

#define RTP_RAW_ESS_SIZE		2	// extended sequence number, high 16 bits
#define RTP_RAW_DESC_SIZE		6	// one line descriptor
#define RTP_RAW_MAX_LINE		32767	// 15-bit line number / offset field

// bytes per pixel group: 8-bit Cb Y0 Cr Y1 = 4, 10-bit packed = 5
static inline int rtp_raw_pgroup(unsigned int depth)
{
	return 10 == depth ? 5 : 4;
}

// Validate the public format and derive the packed frame geometry.
// @return 0-ok, -1-unsupported configuration
static inline int rtp_raw_validate(const struct rtp_raw_format* fmt, int* pgroup, int* pg_per_line, int* frame_bytes)
{
	unsigned int pg;
	unsigned long long bytes;

	if (!fmt)
		return -1;

	// width: positive even number, at most 32768 pixels
	if (0 == fmt->width || (fmt->width & 0x01U) || fmt->width > 32768U)
		return -1;
	if (0 == fmt->height)
		return -1;
	if (8 != fmt->depth && 10 != fmt->depth)
		return -1;
	// line number occupies 15 bits beside the F bit
	if ((unsigned long long)fmt->line_start + (unsigned long long)fmt->height - 1 > RTP_RAW_MAX_LINE)
		return -1;

	pg = (unsigned int)rtp_raw_pgroup(fmt->depth);
	bytes = (unsigned long long)(fmt->width / 2) * pg * fmt->height;
	if (0 == bytes || bytes > (unsigned long long)INT_MAX)
		return -1;

	if (pgroup) *pgroup = (int)pg;
	if (pg_per_line) *pg_per_line = (int)(fmt->width / 2);
	if (frame_bytes) *frame_bytes = (int)bytes;
	return 0;
}

// Write one 6-byte line descriptor.
// @param[in] length segment payload length in bytes (multiple of pgroup)
// @param[in] line 15-bit source line number
// @param[in] cont C bit: 1 when another descriptor follows
// @param[in] offset pixel offset from the start of the line
static inline void rtp_raw_write_desc(uint8_t* ptr, uint16_t length, unsigned int line, int cont, unsigned int offset)
{
	nbo_w16(ptr, length);
	nbo_w16(ptr + 2, (uint16_t)(line & RTP_RAW_MAX_LINE)); // F = 0, progressive
	nbo_w16(ptr + 4, (uint16_t)((cont ? 0x8000U : 0) | (offset & RTP_RAW_MAX_LINE)));
}

static inline uint16_t rtp_raw_desc_length(const uint8_t* ptr) { return nbo_r16(ptr); }
static inline unsigned int rtp_raw_desc_field(const uint8_t* ptr) { return nbo_r16(ptr + 2); }
static inline unsigned int rtp_raw_desc_cont(const uint8_t* ptr) { return (nbo_r16(ptr + 4) >> 15) & 0x01; }
static inline unsigned int rtp_raw_desc_offset(const uint8_t* ptr) { return nbo_r16(ptr + 4) & RTP_RAW_MAX_LINE; }
static inline unsigned int rtp_raw_desc_line(const uint8_t* ptr) { return nbo_r16(ptr + 2) & RTP_RAW_MAX_LINE; }
static inline unsigned int rtp_raw_desc_f(const uint8_t* ptr) { return (nbo_r16(ptr + 2) >> 15) & 0x01; }

// Modulo 2^32 sequence comparison, valid while the activity span stays below 2^31.
// @return >0 when a is newer than b, 0 when equal, <0 when older
static inline int rtp_raw_seq_compare(uint32_t a, uint32_t b)
{
	uint32_t d = a - b;
	if (0 == d)
		return 0;
	return (d & 0x80000000U) ? -1 : 1;
}

#endif /* !_rtp_raw_internal_h_ */
