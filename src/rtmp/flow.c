/**
 * @file rtmp/flow.c  Real Time Messaging Protocol (RTMP) -- Sender flow control
 *
 * Copyright (C) 2010 Creytiv.com
 *
 * The sender keeps three independent positions:
 *
 *  - sent:   cumulative bytes successfully handed to TCP, including the
 *            RTMP handshake and every header and payload byte
 *  - acked:  cumulative position acknowledged by the peer (ACK messages)
 *  - window: peer acknowledgement window from Set Peer Bandwidth
 *
 * Media chunks may be submitted while (sent - acked) < window.  A complete
 * chunk is always allowed when credit is available, even if it makes the
 * outstanding amount slightly exceed the window.  Once the window is
 * reached, media submission stops until an ACK or a larger window arrives.
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


/** Set Peer Bandwidth limit types, as carried on the wire */
enum rtmp_bw_limit {
	RTMP_BW_LIMIT_HARD    = 0,
	RTMP_BW_LIMIT_SOFT    = 1,
	RTMP_BW_LIMIT_DYNAMIC = 2,
};


static void flow_destructor(void *data)
{
	(void)data;
}


int rtmp_flow_alloc(struct rtmp_flow **flp)
{
	struct rtmp_flow *fl;

	if (!flp)
		return EINVAL;

	fl = mem_zalloc(sizeof(*fl), flow_destructor);
	if (!fl)
		return ENOMEM;

	*flp = fl;

	return 0;
}


void rtmp_flow_reset(struct rtmp_flow *fl)
{
	if (!fl)
		return;

	fl->sent    = 0;
	fl->acked   = 0;
	fl->window  = 0;
	fl->limit   = RTMP_BW_LIMIT_HARD;
	fl->limited = false;
}


/**
 * Apply a Set Peer Bandwidth message
 *
 * Hard:    the window is replaced and the effective type becomes hard.
 * Soft:    the window becomes the minimum of the current and the new one;
 *          with no previous limit the new value is taken; the effective
 *          type becomes soft.
 * Dynamic: only honoured while the effective type is hard, then handled as
 *          hard; otherwise the message is ignored without state change.
 *
 * A zero window, a window of 2^31 or more, or an unknown limit type is a
 * protocol error and leaves all existing state untouched.
 *
 * @param fl     Sender flow state
 * @param window Peer acknowledgement window
 * @param limit  Limit type (hard/soft/dynamic)
 *
 * @return 0 if accepted or ignored, otherwise errorcode
 */
int rtmp_flow_set_bandwidth(struct rtmp_flow *fl, uint32_t window,
			    uint8_t limit)
{
	if (!fl)
		return EINVAL;

	if (window == 0 || window >= RTMP_FLOW_WINDOW_MAX)
		return EPROTO;

	if (limit > RTMP_BW_LIMIT_DYNAMIC)
		return EPROTO;

	switch (limit) {

	case RTMP_BW_LIMIT_HARD:
		fl->window  = window;
		fl->limit   = limit;
		fl->limited = true;
		break;

	case RTMP_BW_LIMIT_SOFT:
		if (!fl->limited)
			fl->window = window;
		else
			fl->window = min(fl->window, window);

		fl->limit   = limit;
		fl->limited = true;
		break;

	case RTMP_BW_LIMIT_DYNAMIC:
		/* dynamic is only treated as hard while hard is in effect */
		if (fl->limited && fl->limit == RTMP_BW_LIMIT_HARD)
			fl->window = window;
		break;

	default:
		return EPROTO;
	}

	return 0;
}


/**
 * Expand a 32-bit ACK sequence number to the 64-bit send position and
 * validate it
 *
 * The value is expanded to the position nearest to the current cumulative
 * send position.  Only acknowledgements that advance strictly monotonic and
 * do not pass the send position release credit; duplicate, stale and future
 * acknowledgements are ignored.
 *
 * @param fl  Sender flow state
 * @param seq 32-bit ACK sequence number from the wire
 *
 * @return true if the acknowledgement was accepted, false if ignored
 */
bool rtmp_flow_handle_ack(struct rtmp_flow *fl, uint32_t seq)
{
	uint64_t base, cand;

	if (!fl)
		return false;

	base = fl->sent & ~(uint64_t)0xffffffff;
	cand = base | seq;

	/* pick the expansion nearest to the cumulative send position */
	if (cand > fl->sent + RTMP_FLOW_WRAP_HALF)
		cand -= (uint64_t)1 << 32;
	else if (cand + RTMP_FLOW_WRAP_HALF <= fl->sent)
		cand += (uint64_t)1 << 32;

	if (cand <= fl->acked || cand > fl->sent)
		return false;

	fl->acked = cand;

	return true;
}


void rtmp_flow_commit(struct rtmp_flow *fl, size_t n)
{
	if (!fl)
		return;

	fl->sent += n;
}


bool rtmp_flow_media_allowed(const struct rtmp_flow *fl)
{
	if (!fl || !fl->limited)
		return true;

	return (fl->sent - fl->acked) < fl->window;
}


bool rtmp_flow_is_limited(const struct rtmp_flow *fl)
{
	return fl && fl->limited;
}


uint64_t rtmp_flow_sent(const struct rtmp_flow *fl)
{
	return fl ? fl->sent : 0;
}


uint64_t rtmp_flow_acked(const struct rtmp_flow *fl)
{
	return fl ? fl->acked : 0;
}


uint32_t rtmp_flow_window(const struct rtmp_flow *fl)
{
	return fl ? fl->window : 0;
}


uint8_t rtmp_flow_limit(const struct rtmp_flow *fl)
{
	return fl ? fl->limit : 0;
}


/*
 * Controlled counter origin for wrap-around testing: places the cumulative
 * positions near a 32-bit boundary without sending 4 GB of data.
 */
void rtmp_flow_set_counters(struct rtmp_flow *fl, uint64_t sent,
			    uint64_t acked)
{
	if (!fl || acked > sent)
		return;

	fl->sent  = sent;
	fl->acked = acked;
}
