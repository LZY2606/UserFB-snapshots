/**
 * @file peer_bandwidth.c  RTMP sender flow-control regression tests
 *
 * Copyright (C) 2010 Creytiv.com
 *
 * The sandbox used for these tests forbids binding TCP ports, so the local
 * RTMP connection is carried over a non-blocking AF_UNIX socketpair.  A TCP
 * send helper moves the production tcp_send() bytes onto the socketpair
 * (and captures them), while incoming bytes are handed straight to the
 * RTMP receive handler.  Everything above the transport is the real
 * library: handshake, dechunker, control dispatcher and sender queue.
 */
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* production RTMP connection implementation and internals */
#include "src/rtmp/conn.c"

#include "task_support/check.h"
#include <string.h>


#define TEST_CHUNK_SIZE 128
#define HARD 0
#define SOFT 1
#define DYNAMIC 2


struct endpoint {
	int fd;
	struct tcp_conn *tc;
	struct tcp_helper *helper;
	struct mbuf *cap;      /* every byte handed to tcp_send() */
};

struct env {
	int sv[2];
	struct endpoint ep[2];
	struct rtmp_conn *rc[2];
};

struct got_msg {
	struct le le;
	uint8_t type;
	uint32_t timestamp;
	uint32_t stream_id;
	uint8_t *pld;
	size_t len;
};


static size_t fd_drain(int fd, struct mbuf *sink);
static struct mbuf *sink_alloc(void);
static void discard_fd(int fd);


/* TCP send helper: perform the real socketpair write and capture the wire */
static bool transport_send(int *err, struct mbuf *mb, void *arg)
{
	struct endpoint *ep = arg;
	const uint8_t *p = mbuf_buf(mb);
	size_t left = mbuf_get_left(mb);

	while (left) {
		ssize_t n = write(ep->fd, p, left);

		if (n > 0) {
			p += n;
			left -= (size_t)n;
		}
		else if (errno == EINTR)
			continue;
		else {
			*err = errno;
			return true;
		}
	}

	if (mbuf_write_mem(ep->cap, mbuf_buf(mb), mbuf_get_left(mb))) {
		*err = ENOMEM;
		return true;
	}

	*err = 0;
	return true;   /* fully handled: the real TCP socket is bypassed */
}


static struct mbuf *sink_alloc(void)
{
	return mbuf_alloc(1 << 16);
}


static void env_pump(struct env *e, int idx)
{
	struct mbuf *mb = mbuf_alloc(4096);
	uint8_t *p;
	ssize_t n;

	if (!mb)
		return;

	p = mb->buf;
	for (;;) {
		n = read(e->sv[idx], p, mb->size - mb->end);
		if (n <= 0)
			break;
		mb->end += (size_t)n;
		p += n;
		if (mb->end >= mb->size)
			break;
	}

	if (mb->end)
		tcp_recv_handler(mb, e->rc[idx]);

	mem_deref(mb);
}


static int endpoint_alloc(struct endpoint *ep, int fd, const struct sa *peer)
{
	int err;

	memset(ep, 0, sizeof(*ep));
	ep->fd = fd;
	ep->cap = mbuf_alloc(1 << 16);
	if (!ep->cap)
		return ENOMEM;

	err = tcp_conn_alloc(&ep->tc, peer, NULL, NULL, NULL, NULL);
	if (err)
		return err;

	err = tcp_register_helper(&ep->helper, ep->tc, 0, NULL,
				  transport_send, NULL, ep);
	return err;
}


static int env_start(struct env *e)
{
	struct sa peer;
	unsigned i;
	int err;

	memset(e, 0, sizeof(*e));
	sa_set_in(&peer, 0x7f000001, 9);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, e->sv) < 0)
		return errno;

	(void)fcntl(e->sv[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(e->sv[1], F_SETFL, O_NONBLOCK);

	for (i = 0; i < 2; ++i) {
		err = endpoint_alloc(&e->ep[i], e->sv[i], &peer);
		if (err)
			return err;
	}

	e->rc[0] = rtmp_conn_alloc(true, NULL, NULL, NULL, NULL);
	e->rc[1] = rtmp_conn_alloc(false, NULL, NULL, NULL, NULL);
	if (!e->rc[0] || !e->rc[1])
		return ENOMEM;

	e->rc[0]->tc = e->ep[0].tc;
	e->rc[1]->tc = e->ep[1].tc;

	/* the client handshake emits an AMF connect needing these strings */
	err = str_dup(&e->rc[0]->app, "testapp");
	err |= str_dup(&e->rc[0]->uri, "rtmp://127.0.0.1/testapp/live");
	err |= str_dup(&e->rc[0]->host, "127.0.0.1");
	err |= str_dup(&e->rc[0]->stream, "live");
	if (err)
		return err;

	/* drive the real client/server handshake over the socketpair */
	tcp_estab_handler(e->rc[0]);

	for (i = 0; i < 32; ++i) {
		env_pump(e, 1);
		env_pump(e, 0);
		if (e->rc[0]->state == RTMP_STATE_HANDSHAKE_DONE &&
		    e->rc[1]->state == RTMP_STATE_HANDSHAKE_DONE)
			break;
	}

	if (e->rc[0]->state != RTMP_STATE_HANDSHAKE_DONE ||
	    e->rc[1]->state != RTMP_STATE_HANDSHAKE_DONE)
		return EPROTO;

	/* deterministic small chunks for boundary tests */
	e->rc[0]->send_chunk_size = TEST_CHUNK_SIZE;
	e->rc[1]->send_chunk_size = TEST_CHUNK_SIZE;

	/* start tests with empty transport pipes (handshake already done) */
	{
		struct mbuf *tmp = sink_alloc();
		if (tmp) {
			(void)fd_drain(e->sv[0], tmp);
			(void)fd_drain(e->sv[1], tmp);
			mem_deref(tmp);
		}
	}

	return 0;
}


static void env_stop(struct env *e)
{
	mem_deref(e->ep[1].helper);
	mem_deref(e->ep[0].helper);

	/* the rtmp_conn destructors release the attached tcp_conn (and fds) */
	mem_deref(e->rc[1]);
	mem_deref(e->rc[0]);

	mem_deref(e->ep[1].cap);
	mem_deref(e->ep[0].cap);
	e->sv[0] = e->sv[1] = -1;
}


/* build a complete RTMP message on the wire (type 0 header + payload) */
static struct mbuf *msg_build(uint32_t csid, uint32_t ts, uint8_t type,
			      uint32_t stream_id, const void *pld, size_t len)
{
	struct rtmp_header hdr;
	struct mbuf *mb;

	memset(&hdr, 0, sizeof(hdr));
	hdr.format = 0;
	hdr.chunk_id = csid;
	hdr.timestamp = ts;
	hdr.length = (uint32_t)len;
	hdr.type_id = type;
	hdr.stream_id = stream_id;

	mb = mbuf_alloc(len + RTMP_HDR_MAX);
	if (!mb)
		return NULL;

	if (rtmp_header_encode(mb, &hdr) || mbuf_write_mem(mb, pld, len)) {
		mem_deref(mb);
		return NULL;
	}

	mb->pos = 0;
	return mb;
}


static struct mbuf *ctrl_build(uint8_t type, const uint8_t *pld, size_t len)
{
	return msg_build(RTMP_CHUNK_ID_CONTROL, 0, type,
			 RTMP_CONTROL_STREAM_ID, pld, len);
}


/* deliver a wire message to conn idx and run the production receive path */
static void env_inject(struct env *e, int idx, struct mbuf *mb)
{
	size_t off = 0;

	while (off < mbuf_get_left(mb)) {
		ssize_t n = write(e->sv[1 - idx], mbuf_buf(mb) + off,
				  mbuf_get_left(mb) - off);
		if (n <= 0)
			break;
		off += (size_t)n;
	}

	env_pump(e, idx);
}


static void inject_ack(struct env *e, int idx, uint32_t seq)
{
	uint32_t nseq = htonl(seq);
	struct mbuf *mb = ctrl_build(RTMP_TYPE_ACKNOWLEDGEMENT,
				     (uint8_t *)&nseq, sizeof(nseq));

	env_inject(e, idx, mb);
	mem_deref(mb);
}


static void inject_spb(struct env *e, int idx, uint32_t window, uint8_t limit)
{
	uint8_t pld[5];
	struct mbuf *mb;

	memcpy(pld, &(uint32_t){htonl(window)}, 4);
	pld[4] = limit;

	mb = ctrl_build(RTMP_TYPE_SET_PEER_BANDWIDTH, pld, 5);
	env_inject(e, idx, mb);
	mem_deref(mb);
}


static void inject_was(struct env *e, int idx, uint32_t size)
{
	uint32_t nsize = htonl(size);
	struct mbuf *mb = ctrl_build(RTMP_TYPE_WINDOW_ACK_SIZE,
				     (uint8_t *)&nsize, sizeof(nsize));

	env_inject(e, idx, mb);
	mem_deref(mb);
}


/* read every byte currently available on a socketpair end */
static size_t fd_drain(int fd, struct mbuf *sink)
{
	uint8_t buf[4096];
	size_t total = 0;

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n > 0) {
			if (mbuf_write_mem(sink, buf, (size_t)n))
				break;
			total += (size_t)n;
		}
		else
			break;
	}

	return total;
}


/* discard bytes already waiting (e.g. the WAS reply to Set Peer Bandwidth) */
static void discard_fd(int fd)
{
	struct mbuf *tmp = sink_alloc();

	if (tmp) {
		(void)fd_drain(fd, tmp);
		mem_deref(tmp);
	}
}


static const uint8_t *mem_find(const uint8_t *hay, size_t hlen,
			       const uint8_t *needle, size_t nlen)
{
	size_t i;

	if (!nlen || nlen > hlen)
		return NULL;

	for (i = 0; i + nlen <= hlen; ++i) {
		if (!memcmp(hay + i, needle, nlen))
			return hay + i;
	}

	return NULL;
}


static void fill_pattern(uint8_t *p, size_t len, uint8_t seed)
{
	size_t i;

	for (i = 0; i < len; ++i)
		p[i] = (uint8_t)(seed + (uint8_t)i);
}


static int collector(const struct rtmp_header *hdr, struct mbuf *mb, void *arg)
{
	struct list *ml = arg;
	struct got_msg *m;

	m = mem_zalloc(sizeof(*m), NULL);
	if (!m)
		return ENOMEM;

	m->type = hdr->type_id;
	m->timestamp = hdr->timestamp;
	m->stream_id = hdr->stream_id;
	m->len = mbuf_get_left(mb);
	m->pld = mem_alloc(m->len, NULL);
	if (!m->pld) {
		mem_deref(m);
		return ENOMEM;
	}

	memcpy(m->pld, mbuf_buf(mb), m->len);
	list_append(ml, &m->le, m);

	return 0;
}


static void msg_list_flush(struct list *ml)
{
	struct le *le;

	while ((le = list_head(ml)) != NULL) {
		struct got_msg *m = le->data;

		list_unlink(&m->le);
		mem_deref(m->pld);
		mem_deref(m);
	}
}


/* feed captured wire bytes through the production dechunker */
static int roundtrip_dechunk(struct mbuf *wire, size_t chunk_sz,
			     struct list *out)
{
	struct rtmp_dechunker *rd = NULL;
	int err;

	list_init(out);

	err = rtmp_dechunker_alloc(&rd, chunk_sz, collector, out);
	if (err)
		return err;

	while (mbuf_get_left(wire) > 0) {
		err = rtmp_dechunker_receive(rd, wire);
		if (err == ENODATA) {
			err = 0;
			break;
		}
		if (err)
			break;
	}

	mem_deref(rd);
	return err;
}


static struct rtmp_stream *make_stream(struct rtmp_conn *conn)
{
	struct rtmp_stream *strm = NULL;

	if (rtmp_stream_alloc(&strm, conn, 1, NULL, NULL, NULL, NULL,
			      NULL, NULL))
		return NULL;

	return strm;
}


/*
 * limits group
 */


static int case_hard_soft_dynamic(void)
{
	struct rtmp_flow *fl = NULL;

	TASK_CHECK(rtmp_flow_alloc(&fl) == 0);
	TASK_CHECK(!rtmp_flow_is_limited(fl));

	/* hard replaces the window and installs the hard type */
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 1000, HARD) == 0);
	TASK_CHECK(rtmp_flow_is_limited(fl));
	TASK_CHECK(rtmp_flow_window(fl) == 1000);
	TASK_CHECK(rtmp_flow_limit(fl) == HARD);

	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 500, HARD) == 0);
	TASK_CHECK(rtmp_flow_window(fl) == 500);

	/* soft takes the minimum with the current window */
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 800, SOFT) == 0);
	TASK_CHECK(rtmp_flow_window(fl) == 500);
	TASK_CHECK(rtmp_flow_limit(fl) == SOFT);

	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 300, SOFT) == 0);
	TASK_CHECK(rtmp_flow_window(fl) == 300);

	/* dynamic while soft is ignored completely */
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 9999, DYNAMIC) == 0);
	TASK_CHECK(rtmp_flow_window(fl) == 300);
	TASK_CHECK(rtmp_flow_limit(fl) == SOFT);

	/* hard replaces again */
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 700, HARD) == 0);
	TASK_CHECK(rtmp_flow_window(fl) == 700);
	TASK_CHECK(rtmp_flow_limit(fl) == HARD);

	/* dynamic while hard is handled exactly like hard */
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 250, DYNAMIC) == 0);
	TASK_CHECK(rtmp_flow_window(fl) == 250);
	TASK_CHECK(rtmp_flow_limit(fl) == HARD);

	/* soft with no previous limit takes the offered value */
	rtmp_flow_reset(fl);
	TASK_CHECK(!rtmp_flow_is_limited(fl));
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 400, SOFT) == 0);
	TASK_CHECK(rtmp_flow_window(fl) == 400);
	TASK_CHECK(rtmp_flow_limit(fl) == SOFT);

	mem_deref(fl);
	return 0;
}


static int case_invalid_bandwidth(void)
{
	struct rtmp_flow *fl = NULL;
	struct env e;
	struct rtmp_header hdr;
	struct mbuf *mb;
	uint8_t pld[5];

	TASK_CHECK(rtmp_flow_alloc(&fl) == 0);
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 1000, HARD) == 0);

	/* zero window: protocol error, state must not move */
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 0, HARD) == EPROTO);
	TASK_CHECK(rtmp_flow_window(fl) == 1000);
	TASK_CHECK(rtmp_flow_limit(fl) == HARD);

	/* window of 2^31 and above is illegal */
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 1u << 31, HARD) == EPROTO);
	TASK_CHECK(rtmp_flow_window(fl) == 1000);

	/* unknown limit type: protocol error, state must not move */
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 100, 3) == EPROTO);
	TASK_CHECK(rtmp_flow_set_bandwidth(fl, 100, 255) == EPROTO);
	TASK_CHECK(rtmp_flow_window(fl) == 1000);
	TASK_CHECK(rtmp_flow_limit(fl) == HARD);

	mem_deref(fl);

	/* same guarantee through the production message dispatcher */
	TASK_CHECK(env_start(&e) == 0);

	TASK_CHECK(e.rc[0]->window_ack_size == 2500000);
	memcpy(pld, &(uint32_t){htonl(0)}, 4);
	pld[4] = HARD;
	memset(&hdr, 0, sizeof(hdr));
	hdr.format = 0;
	hdr.chunk_id = RTMP_CHUNK_ID_CONTROL;
	hdr.length = 5;
	hdr.type_id = RTMP_TYPE_SET_PEER_BANDWIDTH;
	hdr.stream_id = RTMP_CONTROL_STREAM_ID;

	/* the dispatcher works on de-chunked payload only (no chunk header) */
	mb = mbuf_alloc(8);
	TASK_CHECK(mb != NULL);
	TASK_CHECK(mbuf_write_mem(mb, pld, 5) == 0);
	mb->pos = 0;

	{
		int derr = rtmp_dechunk_handler(&hdr, mb, e.rc[0]);
		TASK_CHECK(derr == EPROTO);
	}
	TASK_CHECK(!rtmp_flow_is_limited(e.rc[0]->flow));
	TASK_CHECK(e.rc[0]->window_ack_size == 2500000);

	mem_deref(mb);
	env_stop(&e);
	return 0;
}


static int case_independent_thresholds(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t video[500];
	size_t before;

	fill_pattern(video, sizeof(video), 7);
	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	/* settle handshake bytes, then impose a small hard window */
	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	inject_spb(&e, 0, 200, HARD);
	discard_fd(e.sv[1]);
	TASK_CHECK(rtmp_flow_window(e.rc[0]->flow) == 200);

	/* Set Peer Bandwidth must not touch the receive ACK threshold */
	TASK_CHECK(e.rc[0]->window_ack_size == 2500000);

	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	TASK_CHECK(rtmp_send_head_offset(e.rc[0]) == 256);
	before = rtmp_send_head_offset(e.rc[0]);

	/* Window Acknowledgement Size only arms receive ACKs: it must not
	 * release sender credit or unblock the media queue */
	inject_was(&e, 0, 1234);
	TASK_CHECK(e.rc[0]->window_ack_size == 1234);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	TASK_CHECK(rtmp_send_head_offset(e.rc[0]) == before);
	TASK_CHECK(!rtmp_flow_media_allowed(e.rc[0]->flow));

	/* an ACK is what resumes the sender */
	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	mem_deref(strm);
	env_stop(&e);
	return 0;
}


static int case_per_connection_window(void)
{
	struct env a, b;
	struct rtmp_stream *sa, *sb;
	uint8_t video[200];

	fill_pattern(video, sizeof(video), 3);
	TASK_CHECK(env_start(&a) == 0);
	TASK_CHECK(env_start(&b) == 0);

	sa = make_stream(a.rc[0]);
	sb = make_stream(b.rc[0]);
	TASK_CHECK(sa && sb);

	inject_ack(&a, 0, (uint32_t)rtmp_flow_sent(a.rc[0]->flow));
	inject_spb(&a, 0, 100, HARD);
	discard_fd(a.sv[1]);

	/* only connection a is limited; connection b stays open */
	TASK_CHECK(rtmp_flow_is_limited(a.rc[0]->flow));
	TASK_CHECK(!rtmp_flow_is_limited(b.rc[0]->flow));
	TASK_CHECK(rtmp_flow_window(b.rc[0]->flow) == 0);

	(void)rtmp_send_video(sa, 1, video, sizeof(video));
	TASK_CHECK(rtmp_send_queue_count(a.rc[0]) == 1);

	TASK_CHECK(rtmp_send_video(sb, 1, video, sizeof(video)) == 0);
	TASK_CHECK(rtmp_send_queue_count(b.rc[0]) == 0);

	mem_deref(sa);
	mem_deref(sb);
	env_stop(&b);
	env_stop(&a);
	return 0;
}


/*
 * credit group
 */


static int case_window_stall_resume_next(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t big[500], nextp[60];
	struct mbuf *sink;
	size_t wire_partial, wire_total;

	fill_pattern(big, sizeof(big), 11);
	fill_pattern(nextp, sizeof(nextp), 99);
	sink = sink_alloc();
	TASK_CHECK(sink != NULL);

	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	inject_spb(&e, 0, 200, HARD);
	discard_fd(e.sv[1]);

	/* large video stops at a chunk boundary with the remainder queued */
	TASK_CHECK(rtmp_send_video(strm, 100, big, sizeof(big)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	TASK_CHECK(rtmp_send_head_len(e.rc[0]) == sizeof(big));
	TASK_CHECK(rtmp_send_head_offset(e.rc[0]) == 256);

	wire_partial = fd_drain(e.sv[1], sink);
	TASK_CHECK(wire_partial > 0);
	TASK_CHECK(rtmp_flow_sent(e.rc[0]->flow) >= wire_partial);

	/* a duplicate ACK releases nothing */
	{
		uint32_t dup = (uint32_t)rtmp_flow_acked(e.rc[0]->flow);
		inject_ack(&e, 0, dup);
	}
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	TASK_CHECK(rtmp_send_head_offset(e.rc[0]) == 256);

	/* a future ACK beyond the send position is ignored too */
	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow) + 64);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	TASK_CHECK(rtmp_send_head_offset(e.rc[0]) == 256);

	/* the valid ACK drains the remaining chunks of the same message */
	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	/* the window is full again after the overshoot, so the peer must
	 * acknowledge once more before the next message can flow */
	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));

	/* the following message recovers and goes out in full */
	TASK_CHECK(rtmp_send_video(strm, 200, nextp, sizeof(nextp)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	wire_total = fd_drain(e.sv[1], sink);
	TASK_CHECK(wire_total > wire_partial);

	/* the production dechunker must rebuild both messages unchanged */
	{
		struct list msgs;
		struct got_msg *m0, *m1;

		sink->pos = 0;
		TASK_CHECK(roundtrip_dechunk(sink, TEST_CHUNK_SIZE,
					     &msgs) == 0);
		TASK_CHECK(list_count(&msgs) == 2);

		m0 = list_ledata(msgs.head);
		m1 = list_ledata(m0->le.next);
		TASK_CHECK(m0->type == RTMP_TYPE_VIDEO);
		TASK_CHECK(m0->timestamp == 100);
		TASK_CHECK(m0->len == sizeof(big));
		TASK_CHECK(!memcmp(m0->pld, big, sizeof(big)));
		TASK_CHECK(m1->type == RTMP_TYPE_VIDEO);
		TASK_CHECK(m1->timestamp == 200);
		TASK_CHECK(m1->len == sizeof(nextp));
		TASK_CHECK(!memcmp(m1->pld, nextp, sizeof(nextp)));

		msg_list_flush(&msgs);
	}

	mem_deref(sink);
	mem_deref(strm);
	env_stop(&e);
	return 0;
}


static int case_control_overtakes_media(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t video[500];
	uint8_t amf[12] = {0x02, 0, 4, 'p', 'i', 'n', 'g', 0, 0, 0, 0, 0};
	struct mbuf *sink;
	size_t cap_before, cap_after_amf, cap_after_control;
	uint8_t pld[6];

	fill_pattern(video, sizeof(video), 5);
	sink = sink_alloc();
	TASK_CHECK(sink != NULL);

	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	inject_spb(&e, 0, 200, HARD);
	discard_fd(e.sv[1]);

	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	(void)fd_drain(e.sv[1], sink);

	/* an AMF command is not control traffic: it queues behind media */
	cap_before = e.ep[0].cap->end;
	TASK_CHECK(rtmp_send_amf_command(e.rc[0], 0, RTMP_CHUNK_ID_CONN,
					 RTMP_TYPE_AMF0, 0, amf,
					 sizeof(amf)) == 0);
	cap_after_amf = e.ep[0].cap->end;
	TASK_CHECK(cap_after_amf == cap_before);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 2);
	TASK_CHECK(!rtmp_send_head_is_control(e.rc[0]));

	/* protocol control (types 1-6) overtakes the waiting media */
	memcpy(pld, &(uint16_t){htons(RTMP_EVENT_PING_RESPONSE)}, 2);
	memcpy(pld + 2, &(uint32_t){htonl(0)}, 4);
	cap_before = e.ep[0].cap->end;
	TASK_CHECK(rtmp_conn_send_msg(e.rc[0], 0, RTMP_CHUNK_ID_CONTROL, 0, 0,
				      RTMP_TYPE_USER_CONTROL_MSG,
				      RTMP_CONTROL_STREAM_ID,
				      pld, sizeof(pld)) == 0);
	cap_after_control = e.ep[0].cap->end;
	TASK_CHECK(cap_after_control > cap_before);
	TASK_CHECK(!rtmp_send_head_is_control(e.rc[0]));
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 2);

	/* the control message really reached the wire and dechunks */
	{
		struct mbuf *in = sink_alloc();
		struct rtmp_dechunker *rd = NULL;
		struct list msgs;
		struct got_msg *m;

		list_init(&msgs);
		(void)fd_drain(e.sv[1], in);
		in->pos = 0;
		TASK_CHECK(rtmp_dechunker_alloc(&rd, TEST_CHUNK_SIZE,
						 collector, &msgs) == 0);
		TASK_CHECK(rtmp_dechunker_receive(rd, in) == 0);
		TASK_CHECK(list_count(&msgs) == 1);
		m = list_ledata(msgs.head);
		TASK_CHECK(m->type == RTMP_TYPE_USER_CONTROL_MSG);
		TASK_CHECK(m->len == 6);
		msg_list_flush(&msgs);
		mem_deref(rd);
		mem_deref(in);
	}

	/* media remains blocked until ACKs arrive; keep acknowledging until
	 * the video remainder and the queued AMF command are both drained */
	TASK_CHECK(rtmp_send_head_offset(e.rc[0]) == 256);
	{
		unsigned i;

		for (i = 0; i < 10 && rtmp_send_queue_count(e.rc[0]); ++i)
			inject_ack(&e, 0,
				   (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	}
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	mem_deref(sink);
	mem_deref(strm);
	env_stop(&e);
	return 0;
}


static int case_ack_wrap_and_validation(void)
{
	struct rtmp_flow *fl = NULL;
	const uint64_t sent = 0x100000010ull;
	const uint64_t acked = 0xfffffff0ull;

	TASK_CHECK(rtmp_flow_alloc(&fl) == 0);
	rtmp_flow_set_counters(fl, sent, acked);
	TASK_CHECK(rtmp_flow_sent(fl) == sent);
	TASK_CHECK(rtmp_flow_acked(fl) == acked);

	/* ACK crossing the 32-bit boundary expands to the current position */
	TASK_CHECK(rtmp_flow_handle_ack(fl, 0x00000008));
	TASK_CHECK(rtmp_flow_acked(fl) == 0x100000008ull);

	/* stale acknowledgement below the current position is ignored */
	TASK_CHECK(!rtmp_flow_handle_ack(fl, 0xffffffee));
	TASK_CHECK(rtmp_flow_acked(fl) == 0x100000008ull);

	/* duplicate acknowledgement is ignored */
	TASK_CHECK(!rtmp_flow_handle_ack(fl, 0x00000008));
	TASK_CHECK(rtmp_flow_acked(fl) == 0x100000008ull);

	/* future acknowledgement beyond sent is ignored */
	TASK_CHECK(!rtmp_flow_handle_ack(fl, 0x00000020));
	TASK_CHECK(rtmp_flow_acked(fl) == 0x100000008ull);

	/* monotonic advance inside the current epoch is accepted */
	TASK_CHECK(rtmp_flow_handle_ack(fl, 0x0000000f));
	TASK_CHECK(rtmp_flow_acked(fl) == 0x10000000full);

	/* and may advance all the way to the send position */
	TASK_CHECK(rtmp_flow_handle_ack(fl, 0x00000010));
	TASK_CHECK(rtmp_flow_acked(fl) == sent);

	mem_deref(fl);
	return 0;
}


static int case_tiny_window_one_chunk(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t video[300];

	fill_pattern(video, sizeof(video), 1);
	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	/* window smaller than a single chunk: must not deadlock */
	inject_spb(&e, 0, 10, HARD);
	discard_fd(e.sv[1]);
	/* the WAS control reply consumed some credit; acknowledge it so the
	 * video starts with outstanding < window */
	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));

	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	/* exactly one full chunk (128 payload bytes) made it through */
	TASK_CHECK(rtmp_send_head_offset(e.rc[0]) == TEST_CHUNK_SIZE);
	TASK_CHECK(!rtmp_flow_media_allowed(e.rc[0]->flow));

	/* continue ACKing until the overshooting chunks all drained */
	{
		unsigned i;

		for (i = 0; i < 10 && rtmp_send_queue_count(e.rc[0]); ++i)
			inject_ack(&e, 0,
				   (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	}
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	mem_deref(strm);
	env_stop(&e);
	return 0;
}


static int case_reentrant_control_from_recv(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t video[500];
	uint8_t pld[6];
	struct mbuf *mb;
	size_t cap_before;

	fill_pattern(video, sizeof(video), 21);
	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	inject_spb(&e, 0, 200, HARD);
	discard_fd(e.sv[1]);
	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);

	/* Ping Request is answered from inside the receive callback; the
	 * Ping Response control message must overtake the stalled media
	 * without re-entering the drain recursively */
	memcpy(pld, &(uint16_t){htons(RTMP_EVENT_PING_REQUEST)}, 2);
	memcpy(pld + 2, &(uint32_t){htonl(0)}, 4);
	mb = ctrl_build(RTMP_TYPE_USER_CONTROL_MSG, pld, 6);
	cap_before = e.ep[0].cap->end;
	env_inject(&e, 0, mb);
	mem_deref(mb);

	TASK_CHECK(e.ep[0].cap->end > cap_before);
	TASK_CHECK(e.rc[0]->drain_active == false);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	TASK_CHECK(rtmp_send_head_offset(e.rc[0]) == 256);

	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	mem_deref(strm);
	env_stop(&e);
	return 0;
}


static int case_caller_releases_payload(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t *video;
	size_t len = 500;
	uint8_t saved[500];

	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	video = mem_alloc(len, NULL);
	TASK_CHECK(video != NULL);
	fill_pattern(video, len, 33);
	memcpy(saved, video, len);

	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	inject_spb(&e, 0, 200, HARD);
	discard_fd(e.sv[1]);

	/* submit succeeds by queuing the library-owned remainder... */
	TASK_CHECK(rtmp_send_video(strm, 100, video, len) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);

	/* ...so the caller may release and even clobber the memory */
	mem_deref(video);

	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	{
		struct mbuf *sink = sink_alloc();
		struct list msgs;
		struct got_msg *m;

		TASK_CHECK(sink != NULL);
		(void)fd_drain(e.sv[1], sink);
		sink->pos = 0;
		TASK_CHECK(roundtrip_dechunk(sink, TEST_CHUNK_SIZE,
					     &msgs) == 0);
		TASK_CHECK(list_count(&msgs) == 1);
		m = list_ledata(msgs.head);
		TASK_CHECK(m->len == len);
		TASK_CHECK(!memcmp(m->pld, saved, len));
		msg_list_flush(&msgs);
		mem_deref(sink);
	}

	mem_deref(strm);
	env_stop(&e);
	return 0;
}


static int case_close_releases_queue(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t video[500];

	fill_pattern(video, sizeof(video), 41);
	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	inject_spb(&e, 0, 200, HARD);
	discard_fd(e.sv[1]);
	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);

	/* explicit flush empties the queue */
	rtmp_send_flush(e.rc[0]);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	/* queued entries on a closing connection are released without leaking */
	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	mem_deref(strm);
	env_stop(&e);
	return 0;
}


/*
 * wire group
 */


static int case_unlimited_normal_path(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t video[300], audio[100];
	struct mbuf *sink;
	struct list msgs;
	struct got_msg *m0, *m1;

	fill_pattern(video, sizeof(video), 61);
	fill_pattern(audio, sizeof(audio), 71);
	sink = sink_alloc();
	TASK_CHECK(sink != NULL);

	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	/* no Set Peer Bandwidth ever arrives: behaviour is unchanged */
	TASK_CHECK(!rtmp_flow_is_limited(e.rc[0]->flow));
	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);
	TASK_CHECK(rtmp_send_audio(strm, 110, audio, sizeof(audio)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	(void)fd_drain(e.sv[1], sink);
	sink->pos = 0;
	TASK_CHECK(roundtrip_dechunk(sink, TEST_CHUNK_SIZE, &msgs) == 0);
	TASK_CHECK(list_count(&msgs) == 2);

	m0 = list_ledata(msgs.head);
	m1 = list_ledata(m0->le.next);
	TASK_CHECK(m0->type == RTMP_TYPE_VIDEO);
	TASK_CHECK(m0->timestamp == 100);
	TASK_CHECK(m0->len == sizeof(video));
	TASK_CHECK(!memcmp(m0->pld, video, sizeof(video)));
	TASK_CHECK(m1->type == RTMP_TYPE_AUDIO);
	TASK_CHECK(m1->timestamp == 110);
	TASK_CHECK(m1->len == sizeof(audio));
	TASK_CHECK(!memcmp(m1->pld, audio, sizeof(audio)));

	msg_list_flush(&msgs);
	mem_deref(sink);
	mem_deref(strm);
	env_stop(&e);
	return 0;
}


static int case_handwritten_headers(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t video[300];
	const uint8_t *p;
	struct mbuf *sink;

	/* fmt0/csid6, ts=100, len=300, type=video(9), stream id 1 LE */
	static const uint8_t expect_hdr0[] = {
		0x06, 0x00, 0x00, 0x64, 0x00, 0x01, 0x2c, 0x09,
		0x01, 0x00, 0x00, 0x00
	};
	static const uint8_t expect_hdr3 = 0xc6; /* fmt3/csid6 */

	fill_pattern(video, sizeof(video), 81);
	sink = sink_alloc();
	TASK_CHECK(sink != NULL);

	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);
	(void)fd_drain(e.sv[1], sink);

	/* hand-written header bytes must appear verbatim on the wire */
	p = mem_find(sink->buf, sink->end, expect_hdr0, sizeof(expect_hdr0));
	TASK_CHECK(p != NULL);
	TASK_CHECK(!memcmp(p + sizeof(expect_hdr0), video, TEST_CHUNK_SIZE));

	/* second chunk: single fmt3 byte then 128 payload bytes */
	TASK_CHECK(p[12 + TEST_CHUNK_SIZE] == expect_hdr3);
	TASK_CHECK(!memcmp(p + 12 + TEST_CHUNK_SIZE + 1,
			   video + TEST_CHUNK_SIZE, TEST_CHUNK_SIZE));

	/* third chunk: single fmt3 byte then the remaining 44 bytes */
	TASK_CHECK(p[12 + TEST_CHUNK_SIZE + 1 + TEST_CHUNK_SIZE] ==
		   expect_hdr3);
	TASK_CHECK(!memcmp(p + 12 + 2 * (TEST_CHUNK_SIZE + 1),
			   video + 2 * TEST_CHUNK_SIZE,
			   sizeof(video) - 2 * TEST_CHUNK_SIZE));

	/* total wire size: 12+128, 1+128, 1+44 = 314 bytes */
	TASK_CHECK((size_t)(p - sink->buf) + 12 + 128 + 1 + 128 + 1 + 44 <=
		   sink->end);

	mem_deref(sink);
	mem_deref(strm);
	env_stop(&e);
	return 0;
}


static int case_throttle_wire_bytes(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t video[500];
	struct mbuf *sink;
	size_t open_bytes, limited_bytes, final_bytes;

	fill_pattern(video, sizeof(video), 91);
	sink = sink_alloc();
	TASK_CHECK(sink != NULL);

	TASK_CHECK(env_start(&e) == 0);
	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);

	/* before the limit the whole message reaches the peer immediately */
	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);
	open_bytes = fd_drain(e.sv[1], sink);
	TASK_CHECK(open_bytes >= sizeof(video));
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);

	/* after a tight window, bytes stop at the window boundary */
	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	inject_spb(&e, 0, 200, HARD);
	discard_fd(e.sv[1]);
	TASK_CHECK(rtmp_send_video(strm, 200, video, sizeof(video)) == 0);
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 1);
	limited_bytes = fd_drain(e.sv[1], sink);
	TASK_CHECK(limited_bytes > 0);
	TASK_CHECK(limited_bytes < open_bytes);

	/* the ACK releases the rest; the peer ends up with every byte,
	 * identical in content to the unlimited path (dechunker-checked) */
	inject_ack(&e, 0, (uint32_t)rtmp_flow_sent(e.rc[0]->flow));
	TASK_CHECK(rtmp_send_queue_count(e.rc[0]) == 0);
	final_bytes = fd_drain(e.sv[1], sink);
	TASK_CHECK(final_bytes > 0);
	/* the throttled message produced strictly fewer bytes than the
	 * identical unlimited message before it was unblocked */
	TASK_CHECK(limited_bytes < open_bytes);

	{
		struct list msgs;
		struct got_msg *m;

		sink->pos = 0;
		TASK_CHECK(roundtrip_dechunk(sink, TEST_CHUNK_SIZE,
					     &msgs) == 0);
		TASK_CHECK(list_count(&msgs) == 2);
		m = list_ledata(msgs.tail);
		TASK_CHECK(m->type == RTMP_TYPE_VIDEO);
		TASK_CHECK(m->timestamp == 200);
		TASK_CHECK(m->len == sizeof(video));
		TASK_CHECK(!memcmp(m->pld, video, sizeof(video)));
		msg_list_flush(&msgs);
	}

	mem_deref(sink);
	mem_deref(strm);
	env_stop(&e);
	return 0;
}


static int case_handshake_accounting(void)
{
	struct env e;
	struct rtmp_stream *strm;
	uint8_t video[200];
	struct mbuf *sink;
	size_t readable;
	uint64_t sent_after_handshake;

	fill_pattern(video, sizeof(video), 51);
	sink = sink_alloc();
	TASK_CHECK(sink != NULL);

	TASK_CHECK(env_start(&e) == 0);

	/* cumulative send position includes the full handshake */
	sent_after_handshake = rtmp_flow_sent(e.rc[0]->flow);
	TASK_CHECK(sent_after_handshake >=
		   (size_t)(1 + RTMP_HANDSHAKE_SIZE) + RTMP_HANDSHAKE_SIZE);

	strm = make_stream(e.rc[0]);
	TASK_CHECK(strm != NULL);
	TASK_CHECK(rtmp_send_video(strm, 100, video, sizeof(video)) == 0);

	readable = fd_drain(e.sv[1], sink);
	/* the new bytes on the wire match the position advance exactly */
	TASK_CHECK(readable ==
		   rtmp_flow_sent(e.rc[0]->flow) - sent_after_handshake);
	TASK_CHECK(readable > 0);
	TASK_CHECK(sent_after_handshake < rtmp_flow_sent(e.rc[0]->flow));

	mem_deref(sink);
	mem_deref(strm);
	env_stop(&e);
	return 0;
}


const struct task_case task_cases[] = {

	/* limits */
	{"limits", "hard_soft_dynamic",        case_hard_soft_dynamic},
	{"limits", "invalid_bandwidth",        case_invalid_bandwidth},
	{"limits", "independent_thresholds",   case_independent_thresholds},
	{"limits", "per_connection_window",    case_per_connection_window},

	/* credit */
	{"credit", "window_stall_resume_next", case_window_stall_resume_next},
	{"credit", "control_overtakes_media",  case_control_overtakes_media},
	{"credit", "ack_wrap_and_validation",  case_ack_wrap_and_validation},
	{"credit", "tiny_window_one_chunk",    case_tiny_window_one_chunk},
	{"credit", "reentrant_control_recv",   case_reentrant_control_from_recv},
	{"credit", "caller_releases_payload",  case_caller_releases_payload},
	{"credit", "close_releases_queue",     case_close_releases_queue},

	/* wire */
	{"wire", "unlimited_normal_path",      case_unlimited_normal_path},
	{"wire", "handwritten_headers",        case_handwritten_headers},
	{"wire", "throttle_wire_bytes",        case_throttle_wire_bytes},
	{"wire", "handshake_accounting",       case_handshake_accounting},
};

const size_t task_case_count = RE_ARRAY_SIZE(task_cases);
