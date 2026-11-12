# Replay protection across SRTP policy updates

This note describes how libSRTP preserves RTP replay protection when a
stream is rebuilt by `srtp_update()` / `srtp_stream_update()`, including
the conservative lower bound used when the replay window is resized, and
which stream owns which replay state when a wildcard template is updated.

It covers only the protection state reached through the normal public API
(`srtp_protect` / `srtp_unprotect` and the update calls). It does not
change key derivation, the public function signatures, or the bytes of a
protected packet.

## The problem

A policy update used to tear the stream down and build a brand new one,
copying back only the highest extended packet index (the ROC). The new
stream started with an empty received-index bitmask, so every packet that
had already been authenticated looked like an acceptable, not-yet-seen
packet again. Replaying an old SRTP packet across an update therefore
succeeded.

Keeping only the ROC, or only the highest index, is insufficient: the
replay decision also depends on *which* indices inside the window have
already been received.

## What state is preserved

For every existing stream the update now carries the complete RTP replay
state across the rebuild:

* the extended packet index (ROC plus sequence number) in `rdbx.index`;
* the whole received-index bitmask in `rdbx.bitmask`;
* the conservative replay floor in `rdbx.replay_floor` (see below);
* the stream `direction` (sender/receiver/unknown) and `pending_roc`.

The SRTCP replay database (`rtcp_rdb`) and the sender-side indices keep
their previous behavior and are copied verbatim; fixing RTP replay does
not clear or otherwise alter them.

## The conservative lower bound (`replay_floor`)

`replay_floor` is a single 48-bit extended index: the oldest packet index
that may still be accepted. It only ever moves forward.

The sliding bitmask is bounded: when a higher index arrives the mask
shifts, when the window shrinks some indices leave it, and an ROC reset
clears it. In every case the indices that are no longer represented are
"forgotten" by the mask. The floor remembers the boundary of that
forgetting so they can never become acceptable again.

The floor is raised, never lowered, when:

* the bitmask shifts forward as new high indices are received;
* the window is made smaller, dropping the indices below its new start;
* the bitmask is cleared by an ROC/rollover reset.

A candidate index that falls on an unmarked bit of the current window is
still rejected with `srtp_err_status_replay_old` if it is below the floor.

This needs no unbounded history. Memory stays a fixed-size bitmask plus
one integer; the floor is a boundary, not a set of every index ever seen.

### Shrinking and enlarging the window

Resizing keeps the highest index fixed and re-maps each set bit to the
same absolute packet index in the new mask.

* **Shrink.** Bits still inside the smaller window keep their meaning:
  received packets stay rejected and unmarked holes stay receivable.
  Everything below the new, smaller window start is forgotten, so the
  floor is raised to that start.
* **Grow.** The window reaches further back, but it is never filled in:
  no bit is set for an index that was never received, and any index below
  the floor stays closed. Growing therefore cannot reopen a region that an
  earlier, smaller window had already forgotten.

Worked example, with a highest received index of 1000:

1. window 128, receive index 1000; indices 980 and 900 are not received.
2. shrink to 64 (window 937..1000): the in-window hole 980 is still
   accepted once, the already-received 1000 is still a replay, and 900 is
   outside the window and remains old. The floor becomes 937.
3. enlarge to 256 (window 745..1000): 900 is now inside the mask's range
   but is below the floor, so it stays rejected; the still-unreceived 980
   remains an open hole.

The floor continues to advance with normal traffic, so an index that was
old stays old even after many later packets and further resizes.

Sessions that are never updated keep their existing behavior: the floor
starts at zero and only ratchets up as the ordinary window slides.

## Ownership of template and stream state

The wildcard template (`ssrc_any_inbound` / `ssrc_any_outbound`) is only a
prototype. Each real SSRC owns an independent stream with its own `rdbx`
and therefore its own bitmask, index and floor. Two SSRCs at the same
sequence number never share replay state.

On a template update:

* A new template is built from the new policy, but every stream that was
  cloned from the old template is rebuilt and then has **its own** saved
  RTP state restored into it. The per-SSRC bitmaps do not merge and do not
  reset.
* A stream is cloned out of the current (new) template the first time its
  SSRC appears. An SSRC that is seen only *after* the update therefore
  starts from a clean window under the new policy and inherits no other
  stream's received marks.
* A stream that was added explicitly with its own `ssrc_specific` policy
  and was never cloned from the template is not re-keyed by a template
  update; it keeps its own policy, keys and replay state.
* The template direction (inbound versus outbound) and each stream's
  direction are preserved.

Because the saved state includes the direction and the full `rdbx`, and
not merely the ROC or the highest index, rekeying (same key, replacement
key, or an MKI policy with multiple master keys) keeps packet-index replay
protection continuous for a given SSRC.

## Where the logic lives

* `crypto/replay/rdbx.[ch]`
  * `srtp_rdbx_copy_state()` - transfer index, bitmask and floor between
    windows of different sizes, raising the floor on a shrink.
  * `srtp_rdbx_resize()` - change window size without losing state.
  * `srtp_rdbx_check()` - consult the floor for in-window holes.
  * `srtp_rdbx_add_index()`, `srtp_rdbx_set_roc()`,
    `srtp_rdbx_set_roc_seq()` - maintain the floor when the mask moves or
    is cleared.
* `srtp/srtp.c`
  * `srtp_stream_save_rtp_state()` / `srtp_stream_restore_rtp_state()` -
    snapshot and restore a stream's RTP state around a rebuild, used by
    both the specific-SSRC update and the wildcard template update.

The behavior is exercised end to end with real `srtp_protect` /
`srtp_unprotect` traffic in `test/replay_update_test.c` (groups
`specific`, `template`, `resize`, `rollover` and `auth`).
