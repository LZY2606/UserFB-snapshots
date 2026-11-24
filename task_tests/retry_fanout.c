/* librist. SPDX-License-Identifier: BSD-2-Clause
 *
 * Retransmission fan-out regression tests.
 *
 * Drives the production retry enqueue/dequeue path (rist_retry_enqueue /
 * rist_retry_dequeue) with several independent receiver peers sharing one
 * cached media packet, and asserts that:
 *  - successful retransmit counts belong to one (receiver, packet) pair;
 *  - the single-peer and multi-peer paths behave identically;
 *  - duplicate NACK merging and RTT spacing (NORMAL/AGGRESSIVE/OFF) work;
 *  - peer lifecycle and recovery-cache eviction never corrupt other state;
 *  - the Simple/Main/Advanced wire formats keep their sequence numbers,
 *    retransmission marks and 188-byte MPEG-TS payload.
 *
 * Datagrams are recorded through the pluggable transport sendto callback,
 * and timing is driven through the internal, injectable retry clock.
 */

#include "../task_support/check.h"

#include "rist-private.h"
#include "udp-private.h"
#include "rist-retry-state.h"
#include "proto/rist_time.h"
#include "proto/gre.h"
#include "proto/rtp.h"
#include "proto/adv.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FLOW_ID 200u
#define TEST_SEQ 7u
#define TS_PACKET_SIZE 188u
#define MAX_DATAGRAM 1100

struct captured {
	uint8_t data[MAX_DATAGRAM];
	size_t len;
};

struct capture_state {
	struct captured per_fd[2];
	unsigned count[2];
	/* When >= 0, the next send on that fd fails (transport error). */
	int fail_once_fd;
};

static struct capture_state cap;

static ssize_t capture_sendto(void *arg, int fd, const void *data, size_t n,
                               int flags, const struct sockaddr *addr,
                               socklen_t alen)
{
	(void)arg; (void)flags; (void)addr; (void)alen;
	if (fd < 10 || fd > 11 || n == 0 || n > MAX_DATAGRAM)
		return -1;
	unsigned slot = (unsigned)(fd - 10);
	if (cap.fail_once_fd == fd) {
		cap.fail_once_fd = -1;
		return -1;
	}
	memcpy(cap.per_fd[slot].data, data, n);
	cap.per_fd[slot].len = n;
	cap.count[slot]++;
	return (ssize_t)n;
}

static void capture_reset(void)
{
	memset(&cap, 0, sizeof(cap));
	cap.fail_once_fd = -1;
}

/* One millisecond expressed in the NTP fixed-point tick unit. */
#define ONE_MS ((uint64_t)RIST_CLOCK)

static void test_clock_reset(void)
{
	/* Start from the real monotonic time so packet ages stay sensible;
	 * every subsequent retry-path read is then driven deterministically. */
	rist_retry_clock_set(timestampNTP_u64());
}

static void make_ts_packet(uint8_t *out, uint8_t fill)
{
	memset(out, fill, TS_PACKET_SIZE);
	out[0] = 0x47; /* MPEG-TS sync byte */
}

static struct rist_sender *make_sender(enum rist_profile profile,
                                       struct rist_ctx **api_out)
{
	struct rist_ctx *api = NULL;
	if (rist_sender_create(&api, profile, TEST_FLOW_ID, NULL) != 0)
		return NULL;
	struct rist_sender *s = api->sender_ctx;
	s->common.transport.sendto = capture_sendto;
	s->common.transport_active = true;
	s->max_nacksperloop = 1000;
	*api_out = api;
	return s;
}

/* Crafted peers not linked into common.PEERS are released manually. */
static struct rist_peer *make_peer(struct rist_sender *s, int sd)
{
	struct rist_peer *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->sender_ctx = s;
	p->peer_data = p;
	p->sd = sd;
	p->is_data = true;
	p->is_rtcp = true;
	p->adv_flow_id = TEST_FLOW_ID;
	p->adv_peer_id = (uint32_t)(sd - 9);
	p->retry_generation = ++s->retry_generation_next;
	p->config.max_retries = 1;
	p->config.recovery_mode = RIST_RECOVERY_MODE_TIME;
	p->config.congestion_control_mode = RIST_CONGESTION_CONTROL_MODE_OFF;
	p->config.recovery_length_max = 10000;
	p->config.recovery_maxbitrate = 1000000;
	p->config.recovery_rtt_min = 5 * RIST_CLOCK;   /* 5 ms */
	p->config.recovery_rtt_max = 500 * RIST_CLOCK; /* 500 ms */
	p->config.virt_dst_port = 1968;
	p->rist_gre_version = RIST_GRE_VERSION_CUR;
	return p;
}

/* Cache a packet exactly like the production sender loop indexes them. */
static struct rist_buffer *cache_packet(struct rist_sender *s,
                                        uint32_t seq, uint16_t seq_rtp,
                                        uint8_t fill)
{
	uint8_t ts[TS_PACKET_SIZE];
	make_ts_packet(ts, fill);
	struct rist_buffer *b = rist_new_buffer(&s->common, ts, sizeof(ts),
	                                        RIST_PAYLOAD_TYPE_DATA_RAW,
	                                        seq, timestampNTP_u64(), 1968, 1968);
	if (!b)
		return NULL;
	b->seq = seq;
	b->seq_rtp = seq_rtp;
	size_t idx;
	if (s->common.profile == RIST_PROFILE_ADVANCED) {
		idx = seq & (s->sender_queue_max - 1);
		s->seq_index[seq & (s->sender_queue_max - 1)] = (uint32_t)idx;
		if (s->seq_rtp_index)
			s->seq_rtp_index[seq_rtp] = (uint32_t)idx;
	} else {
		idx = seq_rtp;
		s->seq_index[seq_rtp] = (uint32_t)idx;
	}
	s->sender_queue[idx] = b;
	return b;
}

static int evict_packet(struct rist_sender *s, struct rist_buffer *b)
{
	size_t idx;
	if (s->common.profile == RIST_PROFILE_ADVANCED)
		idx = b->seq & (s->sender_queue_max - 1);
	else
		idx = b->seq_rtp;
	TASK_CHECK(s->sender_queue[idx] == b);
	s->sender_queue[idx] = NULL;
	free_rist_buffer(&s->common, b);
	return 0;
}

/* Count retry slots still waiting to be dequeued. Independent of the
 * ring's read/write index conventions. */
static size_t count_active_retries(struct rist_sender *s)
{
	size_t count = 0;
	for (size_t i = 0; i < s->sender_retry_queue_size; i++) {
		if (s->sender_retry_queue[i].active)
			count++;
	}
	return count;
}

/* ------------------------------------------------------------------ */
/* fanout group                                                       */
/* ------------------------------------------------------------------ */

/* The original defect: A spending its budget must not starve B. */
static int fanout_counts_are_per_receiver(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	struct rist_peer *b = make_peer(s, 11);
	TASK_CHECK(a && b);

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x34);
	TASK_CHECK(buf != NULL);

	/* Original transmission never creates retry state. */
	TASK_CHECK(rist_retry_state_get(buf, a, false) == NULL);
	TASK_CHECK(rist_retry_state_get(buf, b, false) == NULL);

	/* A's first retransmission succeeds and spends A's single try. */
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	/* B's first request for the same packet must still be served. */
	rist_retry_enqueue(s, TEST_SEQ, b);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);

	TASK_CHECK(cap.count[0] == 1);
	TASK_CHECK(cap.count[1] == 1);
	struct rist_buffer_retry *ra = rist_retry_state_get(buf, a, false);
	struct rist_buffer_retry *rb = rist_retry_state_get(buf, b, false);
	TASK_CHECK(ra && rb);
	TASK_CHECK(ra->success_count == 1);
	TASK_CHECK(rb->success_count == 1);

	/* Both peers are now at their own cap; the cap never reopens. */
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) < 0);
	rist_retry_enqueue(s, TEST_SEQ, b);
	TASK_CHECK(rist_retry_dequeue(s) < 0);
	TASK_CHECK(ra->success_count == 1);
	TASK_CHECK(rb->success_count == 1);
	TASK_CHECK(cap.count[0] == 1);
	TASK_CHECK(cap.count[1] == 1);

	free(a); free(b);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* A failed send is not a success and must release the pending merge flag. */
static int fanout_send_failure_not_counted(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x35);
	TASK_CHECK(buf != NULL);

	cap.fail_once_fd = 10;
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) < 0);

	struct rist_buffer_retry *ra = rist_retry_state_get(buf, a, false);
	TASK_CHECK(ra != NULL);
	TASK_CHECK(ra->success_count == 0);
	TASK_CHECK(ra->outstanding == false);
	TASK_CHECK(cap.count[0] == 0);

	/* A fresh NACK after the failure is accepted (no stuck pending) and
	 * this send completes, still within the single retry budget. */
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	TASK_CHECK(cap.count[0] == 1);
	TASK_CHECK(ra->success_count == 1);

	free(a);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* OFF keeps accepting duplicate requests but success stays capped, and
 * unhandled duplicates still merge. */
static int fanout_off_mode_merges_but_caps(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);
	a->config.congestion_control_mode = RIST_CONGESTION_CONTROL_MODE_OFF;
	a->config.max_retries = 2;

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x36);
	TASK_CHECK(buf != NULL);

	/* Two duplicate NACKs while nothing was dequeued merge into one. */
	rist_retry_enqueue(s, TEST_SEQ, a);
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(count_active_retries(s) == 1);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	TASK_CHECK(cap.count[0] == 1);

	/* OFF applies no spacing: an immediate re-request is accepted. */
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	TASK_CHECK(cap.count[0] == 2);

	/* Budget exhausted; further OFF-mode requests are refused at dequeue. */
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) < 0);
	TASK_CHECK(cap.count[0] == 2);
	struct rist_buffer_retry *ra = rist_retry_state_get(buf, a, false);
	TASK_CHECK(ra && ra->success_count == 2);

	free(a);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* NORMAL/AGGRESSIVE spacing, identical for one- and two-peer layouts. */
static int spacing_scenario(enum rist_congestion_control_mode mode,
                            size_t peer_count, uint64_t spacing_ms)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);
	a->config.congestion_control_mode = mode;
	a->config.max_retries = 10;
	/* The multi-peer layout adds a second independent receiver; the
	 * enqueue/dequeue code path under test is the same either way. */
	struct rist_peer *b = NULL;
	if (peer_count > 1) {
		b = make_peer(s, 11);
		TASK_CHECK(b != NULL);
		b->config.congestion_control_mode = mode;
		b->config.max_retries = 10;
	}

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x37);
	TASK_CHECK(buf != NULL);
	struct rist_buffer_retry *ra = rist_retry_state_get(buf, a, true);
	TASK_CHECK(ra != NULL);

	/* First request is always accepted. */
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(count_active_retries(s) == 1);
	/* Too-soon duplicate merges while the request is still pending. */
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(count_active_retries(s) == 1);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	TASK_CHECK(ra->outstanding == false);

	/* Rejected spacing duplicates do not move the acceptance stamp. */
	uint64_t accepted_stamp = ra->last_accepted;
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a); /* 1 ms < spacing: reject */
	TASK_CHECK(ra->last_accepted == accepted_stamp);
	rist_retry_clock_advance(ONE_MS * (spacing_ms - 1));
	rist_retry_enqueue(s, TEST_SEQ, a); /* now exactly spacing old: accept */
	TASK_CHECK(ra->last_accepted != accepted_stamp);
	TASK_CHECK(count_active_retries(s) == 1);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	TASK_CHECK(cap.count[0] == 2);

	free(a); free(b);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

static int fanout_normal_spacing_single(void)
{
	return spacing_scenario(RIST_CONGESTION_CONTROL_MODE_NORMAL, 1, 5);
}

static int fanout_normal_spacing_multi(void)
{
	return spacing_scenario(RIST_CONGESTION_CONTROL_MODE_NORMAL, 2, 5);
}

static int fanout_aggressive_spacing_multi(void)
{
	return spacing_scenario(RIST_CONGESTION_CONTROL_MODE_AGGRESSIVE, 2, 10);
}

/* A measured RTT above recovery_rtt_max is clamped down to the max for
 * spacing; one below recovery_rtt_min is clamped up to the min. */
static int fanout_rtt_clamped_to_configured_bounds(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);
	a->config.congestion_control_mode = RIST_CONGESTION_CONTROL_MODE_NORMAL;
	a->config.max_retries = 10;

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x38);
	TASK_CHECK(buf != NULL);

	/* Measured RTT of 1000 ms must be clamped to the 500 ms maximum. */
	a->last_rtt = 1000 * RIST_CLOCK;
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	struct rist_buffer_retry *ra = rist_retry_state_get(buf, a, false);
	TASK_CHECK(ra != NULL);
	uint64_t stamp = ra->last_accepted;

	rist_retry_clock_advance(499 * ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a); /* still inside the 500 ms cap */
	TASK_CHECK(ra->last_accepted == stamp);
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a); /* 500 ms elapsed: accepted */
	TASK_CHECK(ra->last_accepted != stamp);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);

	/* A tiny measured RTT must be clamped up to the 5 ms minimum. */
	a->last_rtt = 0;
	stamp = ra->last_accepted;
	rist_retry_clock_advance(4 * ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(ra->last_accepted == stamp);
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(ra->last_accepted != stamp);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);

	free(a);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle group                                                    */
/* ------------------------------------------------------------------ */

/* Destroying a peer scrubs its queued retries; another peer's pending
 * request keeps working. */
static int lifecycle_peer_destroy_scrubs(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	struct rist_peer *b = make_peer(s, 11);
	TASK_CHECK(a && b);
	peer_append(a);
	peer_append(b);
	sender_peer_append(s, a);
	sender_peer_append(s, b);

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x40);
	TASK_CHECK(buf != NULL);

	rist_retry_enqueue(s, TEST_SEQ, a);
	rist_retry_enqueue(s, TEST_SEQ, b);
	TASK_CHECK(count_active_retries(s) == 2);

	/* Production removal: frees A and NULLs its retry-queue references. */
	TASK_CHECK(rist_peer_remove(&s->common, a, NULL) == 0);

	/* A's stale entry drains without a dangling dereference. */
	TASK_CHECK(rist_retry_dequeue(s) < 0);
	/* B's request is untouched and still sends. */
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	TASK_CHECK(cap.count[0] == 0);
	TASK_CHECK(cap.count[1] == 1);

	rist_destroy(api); /* frees B and the remaining cached packet */
	rist_retry_clock_set(0);
	return 0;
}

/* A replacement endpoint gets a new generation and no inherited counts. */
static int lifecycle_recreated_peer_is_clean(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);
	peer_append(a);
	sender_peer_append(s, a);

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x41);
	TASK_CHECK(buf != NULL);

	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	struct rist_buffer_retry *ra = rist_retry_state_get(buf, a, false);
	TASK_CHECK(ra && ra->success_count == 1);
	uint64_t old_generation = a->retry_generation;

	TASK_CHECK(rist_peer_remove(&s->common, a, NULL) == 0);

	/* New endpoint on the same socket number: fresh generation. */
	struct rist_peer *a2 = make_peer(s, 10);
	TASK_CHECK(a2 != NULL);
	TASK_CHECK(a2->retry_generation != old_generation);
	peer_append(a2);
	sender_peer_append(s, a2);

	/* The cached packet's old record does not belong to the new peer. */
	TASK_CHECK(rist_retry_state_get(buf, a2, false) == NULL);
	rist_retry_enqueue(s, TEST_SEQ, a2);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	struct rist_buffer_retry *ra2 = rist_retry_state_get(buf, a2, false);
	TASK_CHECK(ra2 && ra2->success_count == 1);
	TASK_CHECK(cap.count[0] == 2);

	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* Adding/removing members must not reset surviving peers' accounting. */
static int lifecycle_membership_change_isolates_state(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	struct rist_peer *b = make_peer(s, 11);
	TASK_CHECK(a && b);

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x42);
	TASK_CHECK(buf != NULL);

	/* A uses its single retry; B has not used any. */
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	struct rist_buffer_retry *ra = rist_retry_state_get(buf, a, false);
	struct rist_buffer_retry *rb = rist_retry_state_get(buf, b, false);
	TASK_CHECK(ra && ra->success_count == 1);
	TASK_CHECK(rb == NULL);

	/* A third peer comes and goes again. */
	struct rist_peer *c = make_peer(s, 12);
	TASK_CHECK(c != NULL);
	peer_append(c);
	sender_peer_append(s, c);
	TASK_CHECK(rist_peer_remove(&s->common, c, NULL) == 0);

	/* A remains capped, B still has its full budget available. */
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) < 0);
	TASK_CHECK(ra->success_count == 1);
	rist_retry_enqueue(s, TEST_SEQ, b);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	rb = rist_retry_state_get(buf, b, false);
	TASK_CHECK(rb && rb->success_count == 1);
	TASK_CHECK(cap.count[0] == 1);
	TASK_CHECK(cap.count[1] == 1);

	free(a); free(b);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* Eviction + ring-slot reuse, including a 16-bit sequence collision. */
static int lifecycle_cache_eviction_and_reuse(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);

	struct rist_buffer *old = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x50);
	TASK_CHECK(old != NULL);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(count_active_retries(s) == 1);

	/* The packet leaves the recovery cache while the request is queued. */
	size_t slot = (size_t)TEST_SEQ;
	TASK_CHECK(evict_packet(s, old) == 0);
	TASK_CHECK(s->sender_queue[slot] == NULL);

	/* A new packet reuses the slot and even the same low-16-bit sequence. */
	struct rist_buffer *newbuf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x51);
	TASK_CHECK(newbuf != NULL);
	TASK_CHECK(s->sender_queue[slot] == newbuf);
	TASK_CHECK(newbuf->retry_states == NULL); /* no inherited counts */

	/* The stale queued request must not be sent as the new packet. */
	TASK_CHECK(rist_retry_dequeue(s) < 0);
	TASK_CHECK(cap.count[0] == 0);

	/* A fresh NACK resolves to the new packet and sends its body. */
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	TASK_CHECK(cap.count[0] == 1);
	struct rist_buffer_retry *ra = rist_retry_state_get(newbuf, a, false);
	TASK_CHECK(ra && ra->success_count == 1);
	const uint8_t *body = cap.per_fd[0].data + 12; /* SIMPLE: RTP then TS */
	TASK_CHECK(body[0] == 0x47);
	TASK_CHECK(body[1] == 0x51);

	free(a);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* Retry records are reclaimed together with the media cache entry. */
static int lifecycle_state_reclaimed_with_cache(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);

	struct rist_buffer *old = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x52);
	TASK_CHECK(old != NULL);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	TASK_CHECK(rist_retry_state_get(old, a, false) != NULL);
	TASK_CHECK(evict_packet(s, old) == 0);

	/* Replacement packet starts from zero for the same peer. */
	struct rist_buffer *newbuf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x53);
	TASK_CHECK(newbuf != NULL);
	TASK_CHECK(rist_retry_state_get(newbuf, a, false) == NULL);
	rist_retry_clock_advance(ONE_MS);
	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)200);
	struct rist_buffer_retry *ra = rist_retry_state_get(newbuf, a, false);
	TASK_CHECK(ra && ra->success_count == 1);

	free(a);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* wire group                                                         */
/* ------------------------------------------------------------------ */

/* SIMPLE: bare RTP (no GRE), retransmission flips the SSRC odd bit. */
static int wire_simple_profile(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_SIMPLE, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);

	uint8_t ts[TS_PACKET_SIZE];
	make_ts_packet(ts, 0x60);
	struct rist_buffer *buf = rist_new_buffer(&s->common, ts, sizeof(ts),
	                                        RIST_PAYLOAD_TYPE_DATA_RAW,
	                                        TEST_SEQ, timestampNTP_u64(), 0, 1968);
	TASK_CHECK(buf != NULL);
	buf->seq = TEST_SEQ;
	buf->seq_rtp = (uint16_t)TEST_SEQ;
	s->sender_queue[TEST_SEQ] = buf;
	s->seq_index[TEST_SEQ] = (uint32_t)TEST_SEQ;

	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) == (ssize_t)(12 + TS_PACKET_SIZE));

	const struct captured *d = &cap.per_fd[0];
	TASK_CHECK(d->len == 12 + TS_PACKET_SIZE);
	const uint8_t *rtp = d->data;       /* SIMPLE sends the RTP header directly */
	const uint8_t *body = d->data + 12;
	TASK_CHECK(rtp[0] == 0x80);
	TASK_CHECK(rtp[1] == RTP_PTYPE_MPEGTS);
	TASK_CHECK(ntohs(*(const uint16_t *)(rtp + 2)) == TEST_SEQ);
	/* Retransmission mark: odd SSRC. */
	TASK_CHECK(ntohl(*(const uint32_t *)(rtp + 8)) == (TEST_FLOW_ID | 1u));
	TASK_CHECK(body[0] == 0x47);
	TASK_CHECK(memcmp(body, ts, TS_PACKET_SIZE) == 0);

	free(a);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* MAIN: GRE/VSF reduced overhead followed by RTP, 212 bytes total. */
static int wire_main_profile(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_MAIN, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x61);
	TASK_CHECK(buf != NULL);
	uint8_t want[TS_PACKET_SIZE];
	make_ts_packet(want, 0x61);

	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) > 0);

	const struct captured *d = &cap.per_fd[0];
	/* GRE(4)+seq(4)+VSF(4)+reduced(4) = 16, then RTP(12) and TS(188). */
	const size_t main_prefix = 16;
	TASK_CHECK(d->len == main_prefix + 12 + TS_PACKET_SIZE);
	const uint8_t *p = d->data;
	TASK_CHECK(p[0] == 0x10); /* seq bit set */
	TASK_CHECK(p[2] == 0xCC && p[3] == 0xE0); /* VSF RIST ethertype */
	const uint8_t *rtp = p + main_prefix;
	const uint8_t *body = p + main_prefix + 12;
	TASK_CHECK(rtp[0] == 0x80);
	TASK_CHECK(rtp[1] == RTP_PTYPE_MPEGTS);
	TASK_CHECK(ntohs(*(const uint16_t *)(rtp + 2)) == TEST_SEQ);
	TASK_CHECK(ntohl(*(const uint32_t *)(rtp + 8)) == (TEST_FLOW_ID | 1u));
	TASK_CHECK(memcmp(body, want, TS_PACKET_SIZE) == 0);

	free(a);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* Advanced context serving a peer that negotiated down to Main keeps the
 * Main/GRE wire format and resolves in the 16-bit RTP domain. */
static int wire_advanced_context_main_peer(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_ADVANCED, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);
	a->remote_supports_advanced = false; /* negotiated down to Main */

	struct rist_buffer *buf = cache_packet(s, TEST_SEQ, (uint16_t)TEST_SEQ, 0x62);
	TASK_CHECK(buf != NULL);
	uint8_t want[TS_PACKET_SIZE];
	make_ts_packet(want, 0x62);

	rist_retry_enqueue(s, TEST_SEQ, a);
	TASK_CHECK(rist_retry_dequeue(s) > 0);

	const struct captured *d = &cap.per_fd[0];
	/* Main-downgraded peer: full GRE reduced prefix (16) + RTP + TS. */
	const size_t main_prefix = 16;
	TASK_CHECK(d->len == main_prefix + 12 + TS_PACKET_SIZE);
	const uint8_t *rtp = d->data + main_prefix;
	const uint8_t *body = d->data + main_prefix + 12;
	TASK_CHECK(rtp[1] == RTP_PTYPE_MPEGTS);
	TASK_CHECK(ntohs(*(const uint16_t *)(rtp + 2)) == TEST_SEQ);
	TASK_CHECK(ntohl(*(const uint32_t *)(rtp + 8)) == (TEST_FLOW_ID | 1u));
	TASK_CHECK(memcmp(body, want, TS_PACKET_SIZE) == 0);

	free(a);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

/* Advanced<->Advanced: 32-bit sequence and the R retransmit bit. */
static int wire_advanced_profile(void)
{
	test_clock_reset();
	capture_reset();
	struct rist_ctx *api;
	struct rist_sender *s = make_sender(RIST_PROFILE_ADVANCED, &api);
	TASK_CHECK(s != NULL);
	struct rist_peer *a = make_peer(s, 10);
	TASK_CHECK(a != NULL);
	a->remote_supports_advanced = true;
	a->is_advanced = true;

	uint32_t seq32 = 0x10000u + TEST_SEQ;
	struct rist_buffer *buf = cache_packet(s, seq32, (uint16_t)TEST_SEQ, 0x63);
	TASK_CHECK(buf != NULL);
	uint8_t want[TS_PACKET_SIZE];
	make_ts_packet(want, 0x63);

	rist_retry_enqueue(s, seq32, a);
	TASK_CHECK(rist_retry_dequeue(s) > 0);

	const struct captured *d = &cap.per_fd[0];
	/* RTP(12) + extension(4) + flow id(4, ports are set) + TS(188). */
	const size_t adv_header = RIST_ADV_HEADER_MIN + RIST_ADV_FLOW_ID_SIZE;
	TASK_CHECK(d->len == adv_header + TS_PACKET_SIZE);
	const uint8_t *p = d->data;
	TASK_CHECK(p[0] == RIST_ADV_RTP_FLAGS);
	TASK_CHECK(p[1] == RIST_ADV_PT);
	uint32_t wire_seq = ((uint32_t)ntohs(*(const uint16_t *)(p + 12)) << 16)
	                   | ntohs(*(const uint16_t *)(p + 2));
	TASK_CHECK(wire_seq == seq32);
	/* Retransmission mark and Flow-ID mark are both in ext flags (byte 14). */
	TASK_CHECK((p[14] & RIST_ADV_FLAG_R) != 0);
	TASK_CHECK((p[14] & RIST_ADV_FLAG_I) != 0);
	const uint8_t *body = p + adv_header;
	TASK_CHECK(memcmp(body, want, TS_PACKET_SIZE) == 0);

	free(a);
	rist_destroy(api);
	rist_retry_clock_set(0);
	return 0;
}

const struct task_case task_cases[] = {
	{ "fanout", "per_receiver_counts",      fanout_counts_are_per_receiver },
	{ "fanout", "send_failure_not_counted", fanout_send_failure_not_counted },
	{ "fanout", "off_mode_merge_and_cap",   fanout_off_mode_merges_but_caps },
	{ "fanout", "normal_spacing_single",     fanout_normal_spacing_single },
	{ "fanout", "normal_spacing_multi",      fanout_normal_spacing_multi },
	{ "fanout", "aggressive_spacing_multi",  fanout_aggressive_spacing_multi },
	{ "fanout", "rtt_clamped_to_bounds",     fanout_rtt_clamped_to_configured_bounds },

	{ "lifecycle", "peer_destroy_scrubs",        lifecycle_peer_destroy_scrubs },
	{ "lifecycle", "recreated_peer_clean",       lifecycle_recreated_peer_is_clean },
	{ "lifecycle", "membership_change_isolates", lifecycle_membership_change_isolates_state },
	{ "lifecycle", "cache_eviction_reuse",       lifecycle_cache_eviction_and_reuse },
	{ "lifecycle", "state_reclaimed_with_cache", lifecycle_state_reclaimed_with_cache },

	{ "wire", "simple",                wire_simple_profile },
	{ "wire", "main",                  wire_main_profile },
	{ "wire", "advanced_context_main", wire_advanced_context_main_peer },
	{ "wire", "advanced",              wire_advanced_profile },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
