/* librist. SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-receiver, per-cached-packet retransmission state.
 *
 * The legacy retry accounting lived in struct rist_buffer as a single
 * transmit_count / retry_queued / last_retry_request tuple, so one
 * receiver consuming a retransmission spent the budget of every other
 * receiver sharing the same cached media packet. The records here hang
 * off the cached packet (struct rist_buffer) and key the accounting by
 * the receiving peer's retry generation, giving every (peer, packet)
 * pair its own success count, duplicate-merge flag and spacing stamp.
 *
 * Lifecycle invariants:
 *  - records are allocated on the first NACK for a packet and freed
 *    when that packet leaves the sender recovery cache, so the state is
 *    reclaimed together with the media cache;
 *  - a record stores a generation id, never a peer pointer, so a
 *    destroyed peer leaves no dangling reference and a recreated peer
 *    (new generation) never inherits the old endpoint's counts;
 *  - the success count is only incremented after a datagram was
 *    actually sent and is hard-limited to the peer's max_retries, so
 *    it can neither overflow nor reopen once the cap is reached.
 */

#ifndef RIST_RETRY_STATE_H
#define RIST_RETRY_STATE_H

#include "common/attributes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rist_buffer;
struct rist_peer;

struct rist_buffer_retry {
	/* Retry generation of the peer this record belongs to. */
	uint64_t peer_generation;
	/* Completed retransmissions for this (peer, packet) pair. The
	 * enqueue/dequeue gates keep this <= the peer's max_retries. */
	uint32_t success_count;
	/* NTP ticks of the most recent *accepted* NACK; rejected
	 * duplicate NACKs never update it. 0 until the first acceptance. */
	uint64_t last_accepted;
	/* A retry for this pair is sitting in the retry queue and has
	 * not been dequeued yet; further NACKs merge into it. */
	bool outstanding;
};

/* Return the accounting record for (buffer, peer), creating an empty
 * one when create is true and no record exists yet. NULL means no
 * record (or allocation failure). */
RIST_PRIV struct rist_buffer_retry *rist_retry_state_get(struct rist_buffer *buffer,
                                                         struct rist_peer *peer,
                                                         bool create);

/* Free all per-peer records attached to a media packet. Called from
 * every site that releases a sender recovery-cache entry. */
RIST_PRIV void rist_retry_state_discard_buffer(struct rist_buffer *buffer);

/* Process-wide, never-reused identity for a cached packet, assigned in
 * rist_new_buffer(). Lets a queued retry detect that its ring slot was
 * evicted and reused even when 16-bit sequence numbers collide. */
RIST_PRIV uint64_t rist_retry_next_cache_serial(void);

/* Injectable clock for the retransmission path, in NTP ticks.
 *
 * The production enqueue/dequeue code reads the time exclusively
 * through rist_retry_clock_now(); with no override installed it is
 * exactly timestampNTP_u64(). Tests install a deterministic value with
 * rist_retry_clock_set()/advance() so RTT spacing can be driven
 * without sleeping. Passing 0 restores the real clock. */
RIST_PRIV uint64_t rist_retry_clock_now(void);
RIST_PRIV void rist_retry_clock_set(uint64_t ticks);
RIST_PRIV void rist_retry_clock_advance(uint64_t ticks);

#endif /* RIST_RETRY_STATE_H */
