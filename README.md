[![Build Status](https://github.com/mozilla/nestegg/actions/workflows/build.yml/badge.svg)](https://github.com/mozilla/nestegg/actions/workflows/build.yml)

See INSTALL for build instructions.

Licensed under an ISC-style license.  See LICENSE for details.

## Incremental frame delivery

`nestegg_read_packet` allocates the whole block and returns it only once the
BlockGroup has been read in full.  For a streaming player that feeds a
bounded downstream queue, `include/nestegg/nestegg-events.h` adds an
incremental reader that delivers frame fragments as they are read and hands
the block's final metadata to the consumer afterwards in a separate commit
event:

```c
const nestegg_event * ev;

while ((r = nestegg_read_event(ctx, &ev)) == 1) {
  if (ev->type == NESTEGG_EVENT_DATA) {
    /* ev->data is borrowed until the next read; consume it now. */
    push_fragment(fragments[ev->packet_id], ev->data, ev->length);
  } else {
    /* ev->type == NESTEGG_EVENT_COMMIT: this packet_id is complete. */
    publish_packet(ev->packet_id, ev->track, ev->timestamp,
                   ev->duration, ev->keyframe);
  }
}
if (r < 0)
  handle_error();
```

Every block first produces `NESTEGG_EVENT_DATA` events in frame order, each
at most 1024 bytes and never spanning two frames; a frame with no payload
produces a single zero-length DATA event.  The final `NESTEGG_EVENT_COMMIT`
carries the block's duration, discard padding and reference block, which may
appear after the Block inside a BlockGroup.  Only after its COMMIT is a
packet id complete.  The event reader supports the unencrypted, unencoded
media the whole-packet reader does, less encryption, ContentEncoding and
BlockAdditions.  It is not mixed with `nestegg_read_packet` on one instance;
a successful seek discards uncommitted fragments and the packet id sequence
continues.
