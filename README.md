[![Build Status](https://github.com/mozilla/nestegg/actions/workflows/build.yml/badge.svg)](https://github.com/mozilla/nestegg/actions/workflows/build.yml)

See INSTALL for build instructions.

Licensed under an ISC-style license.  See LICENSE for details.

## Fragment events (`nestegg-events.h`)

Besides `nestegg_read_packet()`, nestegg provides a pull based event API for
pushing compressed data into a fixed capacity downstream queue without ever
allocating a whole block or frame.  `nestegg_read_event()` returns:

- `1` with a valid `const nestegg_event *`,
- `0` at normal end of stream (the pointer is set to `NULL`),
- `-1` on error (the pointer is set to `NULL`; end/error state is sticky).

Each block emits one or more `NESTEGG_EVENT_DATA` fragments first.  Frames
keep their original lace order; every fragment is at most 1024 bytes,
fragments never span frames, `frame_index` is zero based, and `first`/`last`
mark the fragments of one frame.  A zero length frame produces one DATA
event with `length == 0` and `first == last == 1`.  All events of one block
share a monotonically increasing `packet_id`.

The `data` pointer is borrowed and only valid until the next
`nestegg_read_event()`, seek or `nestegg_destroy()`, so copy the bytes into
the downstream queue immediately.  A single `NESTEGG_EVENT_COMMIT` closes the
packet after the whole `BlockGroup` (including metadata stored after the
`Block`) has been parsed.  Only then should a consumer treat the
`packet_id` as a complete packet and read the final `duration`,
`discard_padding`, `reference`, `keyframe` and the `has_*` presence flags.

```c
#include <nestegg/nestegg.h>
#include <nestegg/nestegg-events.h>
#include <string.h>

/* Provided by the player: accept one complete, committed packet. */
void player_submit_packet(unsigned int track, uint64_t timestamp_ns,
                         int keyframe, uint64_t duration_ns,
                         const unsigned char *data, size_t length);

#define QUEUE_CAPACITY (64u * 1024u)  /* fixed downstream byte budget */

struct downstream_queue {
    unsigned char bytes[QUEUE_CAPACITY];
    size_t used;
};

/* Copy the borrowed fragment before the next nestegg_read_event() call.
   Returns -1 when the fixed queue is full; the player would decode/drain
   some bytes and retry instead of growing the queue. */
static int
queue_push_fragment(struct downstream_queue *q, const nestegg_event *ev)
{
    if (ev->length > sizeof(q->bytes) - q->used)
        return -1;
    memcpy(q->bytes + q->used, ev->data, ev->length);
    q->used += ev->length;
    return 0;
}

static void
queue_commit_packet(struct downstream_queue *q, const nestegg_event *ev)
{
    /* ev carries the final BlockGroup metadata now. */
    player_submit_packet(ev->track, ev->timestamp, ev->keyframe,
                         ev->has_duration ? ev->duration : 0,
                         q->bytes, q->used);
    q->used = 0;
}

/* Returns 0 on normal end, -1 on error or queue overflow. */
int
play_events(nestegg *ctx)
{
    struct downstream_queue q;
    const nestegg_event *ev;

    memset(&q, 0, sizeof(q));

    for (;;) {
        int r = nestegg_read_event(ctx, &ev);
        if (r < 0)
            return -1;       /* parse/IO error; ev is NULL, state sticky */
        if (r == 0)
            return 0;        /* finished on a complete block/group edge */

        if (ev->type == NESTEGG_EVENT_DATA) {
            if (queue_push_fragment(&q, ev) != 0)
                return -1;   /* fixed queue full: drain it and try again */
        } else { /* NESTEGG_EVENT_COMMIT: packet_id is now complete */
            queue_commit_packet(&q, ev);
        }
    }
}
```

