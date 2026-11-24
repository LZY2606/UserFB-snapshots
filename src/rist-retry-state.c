/* librist. SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-receiver, per-cached-packet retransmission state.
 * See rist-retry-state.h for the design and lifecycle invariants.
 */

#include "rist-private.h"
#include "rist-retry-state.h"
#include "proto/rist_time.h"

#include <stdlib.h>

/* 0 means "no override": the production path then reads the real
 * monotonic clock. A non-zero value is the deterministic NTP tick
 * count used by tests driving RTT spacing without sleeping. */
static uint64_t injected_clock;

/* Monotonic identity source for cached media packets. */
static uint64_t cache_serial_next;

uint64_t rist_retry_next_cache_serial(void)
{
	return ++cache_serial_next;
}

uint64_t rist_retry_clock_now(void)
{
	return injected_clock != 0 ? injected_clock : timestampNTP_u64();
}

void rist_retry_clock_set(uint64_t ticks)
{
	injected_clock = ticks;
}

void rist_retry_clock_advance(uint64_t ticks)
{
	injected_clock += ticks;
}

struct rist_buffer_retry *rist_retry_state_get(struct rist_buffer *buffer,
                                               struct rist_peer *peer,
                                               bool create)
{
	if (RIST_UNLIKELY(!buffer || !peer))
		return NULL;

	const uint64_t generation = peer->retry_generation;
	for (size_t i = 0; i < buffer->retry_state_len; i++) {
		if (buffer->retry_states[i].peer_generation == generation)
			return &buffer->retry_states[i];
	}

	if (!create)
		return NULL;

	struct rist_buffer_retry *grown =
		realloc(buffer->retry_states,
		        (buffer->retry_state_len + 1) * sizeof(*grown));
	if (RIST_UNLIKELY(!grown))
		return NULL;
	buffer->retry_states = grown;

	struct rist_buffer_retry *record = &grown[buffer->retry_state_len];
	record->peer_generation = generation;
	record->success_count = 0;
	record->last_accepted = 0;
	record->outstanding = false;
	buffer->retry_state_len++;
	return record;
}

void rist_retry_state_discard_buffer(struct rist_buffer *buffer)
{
	if (RIST_UNLIKELY(!buffer))
		return;
	free(buffer->retry_states);
	buffer->retry_states = NULL;
	buffer->retry_state_len = 0;
}
