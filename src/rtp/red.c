/**
 * @file red.c  RTP payload format for redundant audio data (RFC 2198)
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_mbuf.h>
#include <re_rtp.h>


/**
 * Decode an RFC 2198 RED payload
 *
 * The payload starts with a chain of four-byte redundancy headers followed
 * by a single-byte header for the primary block.  The payload data follows
 * the headers in the same order as the headers.  The primary block is the
 * last one and its timestamp offset is zero.
 *
 * The buffer position is not modified.  Nothing is handed out unless the
 * complete header chain, all block lengths and the remaining primary payload
 * have been validated.
 *
 * @param red     Decoded RED information (blockv, n, hdr_len, data_len)
 * @param mb      Buffer positioned at the start of the RTP payload
 * @param blockv  Caller supplied array of at least depth+1 blocks
 * @param depth   Maximum number of redundant blocks accepted
 *
 * @return 0 if success, otherwise errorcode
 */
int rtp_red_decode(struct rtp_red *red, struct mbuf *mb,
		   struct rtp_red_block *blockv, size_t depth)
{
	const uint8_t *p;
	size_t left, pos = 0, nred = 0, i, total_red = 0, data_len;
	size_t primary_len;

	if (!red || !mb || !blockv)
		return EINVAL;

	red->blockv    = blockv;
	red->n         = 0;
	red->hdr_len   = 0;
	red->data_len  = 0;

	p    = mbuf_buf(mb);
	left = mbuf_get_left(mb);

	if (!p || left < 1)
		return EBADMSG;

	/* Parse the redundancy header chain ending in the primary header */
	for (;;) {
		uint8_t fpt;

		if (pos >= left)
			return EBADMSG;

		fpt = p[pos++];

		if (fpt & 0x80) {
			uint16_t off;
			size_t len;

			/* depth is the configured history depth */
			if (nred >= depth)
				return EBADMSG;

			if (left - pos < 3)
				return EBADMSG;

			off = ((uint16_t)p[pos] << 8) | p[pos + 1];
			len = p[pos + 2];
			pos += 3;

			/* empty audio blocks are not allowed */
			if (len == 0)
				return EBADMSG;

			blockv[nred].pt        = fpt & 0x7f;
			blockv[nred].ts_offset = off;
			blockv[nred].off       = 0;
			blockv[nred].len       = len;
			++nred;
		}
		else {
			/* Final primary header (one byte, no length) */
			blockv[nred].pt        = fpt & 0x7f;
			blockv[nred].ts_offset = 0;
			blockv[nred].off       = 0;
			blockv[nred].len       = 0;
			++nred;
			break;
		}
	}

	data_len = left - pos;

	/* Verify that the redundant block lengths fit in the payload */
	for (i = 0; i + 1 < nred; ++i) {
		if (blockv[i].len > data_len - total_red)
			return EBADMSG;
		total_red += blockv[i].len;
	}

	/* The primary block takes the remaining payload and must be non-empty */
	if (total_red >= data_len)
		return EBADMSG;

	primary_len = data_len - total_red;

	/* Fill in the payload ranges */
	for (i = 0, total_red = 0; i + 1 < nred; ++i) {
		blockv[i].off = pos + total_red;
		total_red += blockv[i].len;
	}

	blockv[nred - 1].off = pos + total_red;
	blockv[nred - 1].len = primary_len;

	red->n        = nred;
	red->hdr_len  = pos;
	red->data_len = data_len;

	return 0;
}
