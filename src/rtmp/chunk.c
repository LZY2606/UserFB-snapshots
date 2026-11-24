/**
 * @file rtmp/chunk.c  Real Time Messaging Protocol (RTMP) -- Chunking
 *
 * Copyright (C) 2010 Creytiv.com
 *
 * One RTMP chunk is encoded per call: the chunk header followed by at most
 * the connection chunk size worth of payload.  The sender queue (send.c)
 * calls this repeatedly until a whole message has been emitted, which keeps
 * the wire format identical to the previous stateless multi-chunk encoder
 * while allowing large messages to stop and resume at chunk boundaries.
 */
#include <re_types.h>
#include <re_fmt.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_list.h>
#include <re_sa.h>
#include <re_tcp.h>
#include <re_rtmp.h>
#include "rtmp.h"


int rtmp_chunk_encode(struct mbuf *mb, const struct rtmp_header *hdr,
		      const uint8_t *payload, size_t payload_len)
{
	int err;

	if (!mb || !hdr || !payload || !payload_len)
		return EINVAL;

	err = rtmp_header_encode(mb, (struct rtmp_header *)hdr);
	if (err)
		return err;

	return mbuf_write_mem(mb, payload, payload_len);
}
