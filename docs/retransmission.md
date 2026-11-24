# Sender retransmission (NACK) accounting

This note describes how a libRIST sender accounts for retransmissions of one
cached media packet when several independent receivers request it, and what the
`congestion_control_mode` setting (`OFF` / `NORMAL` / `AGGRESSIVE`) means for
duplicate NACK handling.

## Retry counts are per receiver and per cached packet

The sender keeps one recovery cache of recently transmitted media packets.
Each cached packet carries a small table of retransmission records, one entry
per receiver that has NACKed it. A record holds:

- `success_count` — how many retransmissions of **this packet** were actually
  sent to **this receiver**;
- `outstanding` — whether a request for this (receiver, packet) pair is queued
  and has not been dequeued yet;
- `last_accepted` — the timestamp of the most recently *accepted* request, used
  for RTT spacing.

Consequences:

- The original (primary) transmission of a media packet never increments a
  retry count; only retransmissions do.
- One receiver spending its retry budget has no effect on any other receiver.
  With `max_retries = 1`, receiver A exhausting its single retry does not
  prevent receiver B from receiving its own first retransmission of the same
  cached packet.
- `max_retries` is an inclusive cap on successful retransmissions per
  (receiver, packet) pair. The counter saturates instead of overflowing, so a
  pair that reached the cap can never become eligible again.
- A failed send (transport error, bandwidth/age rejection) is not counted as a
  success and clears the outstanding flag, so a later NACK can retry the packet.

## Duplicate NACK merging and spacing

All three modes share one rule: while a request for a given (receiver, packet)
pair has not been processed yet, repeated NACKs for the same pair merge into
that single pending request — duplicates are never queued twice. This gives
identical behaviour whether the sender currently has one peer or several.

After the pending request has been handled, the modes differ in how soon a
further NACK for the same pair is accepted:

- **NORMAL** — a new request is accepted only once at least one RTT interval
  has elapsed since the last accepted request. The interval is the measured
  RTT clamped into `recovery_rtt_min` / `recovery_rtt_max`.
- **AGGRESSIVE** — same rule with twice the clamped RTT interval, i.e. at most
  one retransmission per two RTTs for a given pair.
- **OFF** — no RTT spacing at all: duplicate requests are accepted as fast as
  they arrive (only the unhandled-merge rule above still applies). OFF does
  **not** disable the retry cap: successful retransmissions remain limited to
  `max_retries` per (receiver, packet) pair, and requests past the cap are
  refused when the queue is drained.

Spacing is always measured from the last *accepted* request. A NACK rejected as
too soon does not move the timestamp, so a burst of premature duplicates cannot
keep pushing the next eligible slot forward.

## Lifecycle

- Retransmission records are allocated on the first NACK for a packet and
  freed when that packet leaves the recovery cache, so the accounting memory is
  reclaimed together with the media cache. A new packet reusing a cache slot
  starts with empty records even when its low 16-bit sequence number matches
  the previous occupant's.
- A queued request is bound to the exact cached packet it was created for
  (ring slot plus a per-packet identity stamp). If the packet is evicted before
  the request is sent, the stale request is dropped; it can never be delivered
  as a different packet that reused the slot, including on a 16-bit sequence
  wrap.
- Records key off a per-peer generation id rather than a peer pointer.
  Destroying a peer therefore leaves no dangling references (its queued
  requests are scrubbed), and a recreated endpoint starts with no inherited
  counts. Other peers' pending requests keep working while members are added
  or removed.

For deterministic testing, the retry path reads the time through an internal
clock entry point that defaults to the real monotonic clock but can be
overridden (`rist_retry_clock_set` / `rist_retry_clock_advance`); no sleep or
wall-clock timing is needed to exercise spacing.
