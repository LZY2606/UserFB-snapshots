/**
 * @file rtmp/send.c  Real Time Messaging Protocol (RTMP) -- Sender queue
 *
 * Copyright (C) 2010 Creytiv.com
 *
 * Outgoing RTMP messages are split into individual chunks (see chunk.c).
 * Media chunks are gated by the peer acknowledgement window: a slow
 * receiver stalls the media queue while protocol control messages keep
 * using the control chunk stream and overtake any waiting media.
 *
 * The chunks queued here are owned by the library, so a media caller may
 * release its payload as soon as rtmp_send_audio()/rtmp_send_video()
 * returns successfully.
 */
#include <string.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_net.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_tcp.h>
#include <re_rtmp.h>
#include "rtmp.h"


struct rtmp_send_entry {
	struct le le;
	struct rtmp_header hdr;
	uint8_t *pld;       /* remaining payload, owned by this entry */
	size_t len;
	size_t offset;      /* payload bytes of the message already sent */
	bool control;
};


static void entry_destructor(void *data)
{
	struct rtmp_send_entry *e = data;

	list_unlink(&e->le);
	mem_deref(e->pld);
}


static int entry_alloc(struct rtmp_send_entry **ep, const struct rtmp_header *hdr,
		       const uint8_t *payload, size_t len, bool control)
{
	struct rtmp_send_entry *e;

	e = mem_zalloc(sizeof(*e), entry_destructor);
	if (!e)
		return ENOMEM;

	e->hdr = *hdr;
	e->control = control;
	e->len = len;

	e->pld = mem_alloc(len, NULL);
	if (!e->pld) {
		mem_deref(e);
		return ENOMEM;
	}

	memcpy(e->pld, payload, len);

	*ep = e;

	return 0;
}


void rtmp_send_flush(struct rtmp_conn *conn)
{
	list_flush(&conn->sendq);
	conn->drain_active = false;
}


/**
 * Submit one complete chunk to TCP
 *
 * Every byte accepted by TCP, chunk header and payload, advances the
 * cumulative send position.
 */
static int send_one_chunk(struct rtmp_conn *conn, const struct rtmp_header *hdr,
			  const uint8_t *payload, size_t payload_len)
{
	struct mbuf *mb;
	size_t wire_len;
	int err;

	mb = mbuf_alloc(payload_len + RTMP_HDR_MAX);
	if (!mb)
		return ENOMEM;

	err = rtmp_chunk_encode(mb, hdr, payload, payload_len);
	if (err)
		goto out;

	wire_len = mb->end;
	mb->pos = 0;

	err = tcp_send(conn->tc, mb);
	if (err)
		goto out;

	rtmp_flow_commit(conn->flow, wire_len);

 out:
	mem_deref(mb);

	return err;
}


/* Send the next chunk of the head entry; the entry stays queued on error */
static int send_head_entry(struct rtmp_conn *conn)
{
	struct rtmp_send_entry *e = list_ledata(conn->sendq.head);
	struct rtmp_header hdr;
	const uint8_t *pld;
	size_t chunk_sz;
	int err;

	if (!e)
		return 0;

	hdr = e->hdr;

	if (e->offset)
		hdr.format = 3;

	chunk_sz = min(e->len - e->offset, conn->send_chunk_size);
	pld = e->pld + e->offset;

	err = send_one_chunk(conn, &hdr, pld, chunk_sz);
	if (err)
		return err;

	e->offset += chunk_sz;

	if (e->offset >= e->len) {
		list_unlink(&e->le);
		mem_deref(e);
	}

	return 0;
}


/*
 * Drain the sender queue.
 *
 * Protocol control messages (types 1-6) always pass, even with no window
 * credit; media chunks stop at the window boundary.  The flag makes the
 * drain re-entrant: a control message sent from inside a receive callback
 * is appended behind the currently processed media entry and is picked up
 * by the outer drain instead of being drained recursively.  TCP send
 * failures stop the drain and are reported through the existing close
 * semantics.
 */
int rtmp_send_drain(struct rtmp_conn *conn)
{
	int err = 0;

	if (!conn || conn->drain_active)
		return 0;

	conn->drain_active = true;

	while (conn->sendq.head) {

		struct rtmp_send_entry *e = list_ledata(conn->sendq.head);

		if (!e->control && !rtmp_flow_media_allowed(conn->flow))
			break;

		err = send_head_entry(conn);
		if (err)
			break;
	}

	conn->drain_active = false;

	return err;
}


static int enqueue_message(struct rtmp_conn *conn,
			   unsigned format, uint32_t chunk_id,
			   uint32_t timestamp, uint32_t timestamp_delta,
			   uint8_t msg_type_id, uint32_t msg_stream_id,
			   const uint8_t *payload, size_t payload_len,
			   bool control)
{
	struct rtmp_send_entry *e;
	struct rtmp_header hdr;
	int err;

	memset(&hdr, 0, sizeof(hdr));
	hdr.format          = format;
	hdr.chunk_id        = chunk_id;
	hdr.timestamp       = timestamp;
	hdr.timestamp_delta = timestamp_delta;
	hdr.length          = (uint32_t)payload_len;
	hdr.type_id         = msg_type_id;
	hdr.stream_id       = msg_stream_id;

	err = entry_alloc(&e, &hdr, payload, payload_len, control);
	if (err)
		return err;

	if (control) {
		/* control overtakes waiting media, but stays behind control
		 * messages already queued so control ordering is preserved */
		struct le *le;

		for (le = conn->sendq.tail; le; le = le->prev) {
			struct rtmp_send_entry *ce = le->data;

			if (ce->control) {
				list_insert_after(&conn->sendq, &ce->le,
						  &e->le, e);
				goto queued;
			}
		}

		list_prepend(&conn->sendq, &e->le, e);
	}
	else {
		list_append(&conn->sendq, &e->le, e);
	}

 queued:
	mem_ref(e);
	err = rtmp_send_drain(conn);
	/* the entry may have been fully sent and removed by the drain */
	mem_deref(e);

	return err;
}


int rtmp_send_submit(struct rtmp_conn *conn,
		     unsigned format, uint32_t chunk_id,
		     uint32_t timestamp, uint32_t timestamp_delta,
		     uint8_t msg_type_id, uint32_t msg_stream_id,
		     const uint8_t *payload, size_t payload_len)
{
	bool control;

	if (!conn || !payload || !payload_len)
		return EINVAL;

	control = msg_type_id >= RTMP_TYPE_SET_CHUNK_SIZE &&
		  msg_type_id <= RTMP_TYPE_SET_PEER_BANDWIDTH;

	return enqueue_message(conn, format, chunk_id, timestamp,
			       timestamp_delta, msg_type_id, msg_stream_id,
			       payload, payload_len, control);
}


uint32_t rtmp_send_queue_count(const struct rtmp_conn *conn)
{
	return conn ? list_count(&conn->sendq) : 0;
}


bool rtmp_send_head_is_control(const struct rtmp_conn *conn)
{
	struct rtmp_send_entry *e;

	if (!conn || !conn->sendq.head)
		return false;

	e = list_ledata(conn->sendq.head);

	return e->control;
}


size_t rtmp_send_head_offset(const struct rtmp_conn *conn)
{
	struct rtmp_send_entry *e;

	if (!conn || !conn->sendq.head)
		return 0;

	e = list_ledata(conn->sendq.head);

	return e->offset;
}


size_t rtmp_send_head_len(const struct rtmp_conn *conn)
{
	struct rtmp_send_entry *e;

	if (!conn || !conn->sendq.head)
		return 0;

	e = list_ledata(conn->sendq.head);

	return e->len;
}
