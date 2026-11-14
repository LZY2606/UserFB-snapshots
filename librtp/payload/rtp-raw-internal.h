#ifndef _rtp_raw_internal_h_
#define _rtp_raw_internal_h_

#include "rtp-raw.h"
#include <limits.h>

#define RTP_RAW_EXT_SEQ 2 // extended sequence number, high 16 bits
#define RTP_RAW_LINE_HEADER 6 // Length + F/Line No + C/Offset

// @return 0-ok(format supported), other-error
static inline int rtp_raw_format_check(const struct rtp_raw_format *format, int *ppgroup, int *pframe_bytes)
{
	uint64_t bytes;
	int pgroup;

	if (!format)
		return -1;
	if (format->width < 2 || format->width > 32768 || 0 != (format->width % 2))
		return -1;
	if (format->height < 1)
		return -1;
	if (8 != format->depth && 10 != format->depth)
		return -1;
	if (format->line_start + format->height - 1 > 32767)
		return -1;

	pgroup = 8 == format->depth ? 4 : 5;
	bytes = (uint64_t)(format->width / 2) * pgroup * format->height;
	if (bytes > (uint64_t)INT_MAX)
		return -1;

	if (ppgroup) *ppgroup = pgroup;
	if (pframe_bytes) *pframe_bytes = (int)bytes;
	return 0;
}

#endif /* !_rtp_raw_internal_h_ */
