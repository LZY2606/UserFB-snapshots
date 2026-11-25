# Queued, non-blocking RTMP writes

The synchronous `RTMP_SendPacket()` path serializes a packet and pushes it
through `WriteN()` immediately.  On a non-blocking socket, `send()` returning
`EAGAIN` is treated as a fatal error: the connection is closed inside the
library even though the caller could simply wait for the socket to become
writable again.

The queued write API adds an event-loop friendly alternative for plaintext
RTMP publishing.  Packets are first serialized into library-owned snapshots
(`RTMP_QueuePacket`), then flushed from a writable-event callback
(`RTMP_WriteQueue`).  Backpressure never tears the connection down.

## API

```c
#include <rtmp.h>

int RTMP_QueuePacket(RTMP *r, const RTMPPacket *packet);
int RTMP_WriteQueue(RTMP *r);
int RTMP_HasQueuedPackets(RTMP *r);
```

`RTMP_QueuePacket()` returns `RTMP_QUEUE_OK` (0) once an independent snapshot
of the packet has been appended to the internal FIFO, or `RTMP_QUEUE_ERROR`
(-1) on an invalid argument or allocation failure.  On failure nothing is
queued and no half packet remains.

Only `RTMP_PACKET_TYPE_AUDIO`, `RTMP_PACKET_TYPE_VIDEO` and
`RTMP_PACKET_TYPE_INFO` (AMF0 metadata) packets are accepted; every other
type is rejected at submission time and must keep using
`RTMP_SendPacket()`.

A successful submission only means the library took a snapshot.  The caller
may immediately overwrite or free its `RTMPPacket`, its `m_body` and the
18 header bytes reserved before `m_body`; the wire bytes are not affected.
The caller's `m_headerType` and body bytes are never modified.  Per-channel
header compression is applied to a private working copy, not to the supplied
packet.

`RTMP_WriteQueue()` drives the FIFO with a non-blocking `send()` and returns
one of three distinguishable states:

| Return value                | Meaning                                                        |
| --------------------------- | -------------------------------------------------------------- |
| `RTMP_QUEUE_DRAINED` (0)    | every queued byte has been written, the FIFO is empty          |
| `RTMP_QUEUE_WOULDBLOCK` (1) | `EAGAIN`/`EWOULDBLOCK`; data remains queued, connection is fine |
| `RTMP_QUEUE_FAILED` (-1)    | another send error; the connection has gone through `RTMP_Close` |

`EINTR` is retried internally.  A positive short write consumes exactly that
many bytes, so a split landing inside a basic header, an extended timestamp
or a media fragment resumes correctly; each byte is consumed once.  On
`EAGAIN` the function returns immediately instead of busy-looping, leaving
the unsent snapshot untouched.  Calling `RTMP_WriteQueue()` on an empty
queue returns `RTMP_QUEUE_DRAINED`.

`RTMP_HasQueuedPackets()` returns non-zero while snapshots remain queued.
`RTMP_Close()` frees every snapshot that was not sent; after a subsequent
`RTMP_Init()` no old byte can reach a new connection.

The queued path uses the exact same chunk serializer as
`RTMP_SendPacket()`, so the on-wire chunk format (basic/message headers,
fmt-3 continuation headers, extended timestamps, per-channel compression
history) is identical between the two entry points.

## Scope and usage constraints

This API is intentionally narrow:

- Plaintext RTMP only, on a connection whose handshake has completed.
  RTMPT, TLS/RTMPS, RTMPE and the remote-method-call
  (`RTMP_PACKET_TYPE_INVOKE`) queue are not covered; keep using the
  synchronous entry points for those.
- Single-threaded use.  Submitting packets from inside a callback that is
  already running on the same `RTMP` handle is not supported.
- Do **not** change `m_outChunkSize` while packets are queued.  Set the
  outgoing chunk size before the first `RTMP_QueuePacket()` call and leave
  it fixed until the queue has drained.  (Changing it mid-stream would give
  later packets a different split size than the snapshots already queued.)
- Do **not** mix `RTMP_SendPacket()` (or helpers built on it, such as
  `RTMP_Write`) into a stream being driven through the queue.  Both entry
  points advance the same per-channel compression history, so interleaving
  them would make a queued packet inherit headers from a packet that has not
  been serialized yet.  Use one entry point for the whole stream.
- The socket should be set non-blocking (`O_NONBLOCK`) so that backpressure
  surfaces as `EAGAIN` rather than blocking inside `RTMP_WriteQueue()`.

## Writable-event loop example

```c
#include <poll.h>
#include <fcntl.h>
#include <rtmp.h>

static int pump_writable(RTMP *r)
{
    /* Called whenever the socket is reported writable.  One call moves
     * as many queued bytes as the kernel currently accepts. */
    for (;;)
    {
        int rc = RTMP_WriteQueue(r);

        if (rc == RTMP_QUEUE_DRAINED)
            return 0;                   /* nothing left to watch */

        if (rc == RTMP_QUEUE_WOULDBLOCK)
            return 1;                   /* wait for the next POLLOUT */

        /* RTMP_QUEUE_FAILED: RTMP_Close() has already run, give up */
        return -1;
    }
}

void publish_loop(RTMP *r, RTMPPacket *packets, int npackets)
{
    int fd = RTMP_Socket(r);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    for (int i = 0; i < npackets; i++)
    {
        if (RTMP_QueuePacket(r, &packets[i]) != RTMP_QUEUE_OK)
            return;                     /* bad type or out of memory */
        /* packets[i] and its body may be reused/freed from here on */
    }

    while (RTMP_HasQueuedPackets(r))
    {
        struct pollfd pfd = { fd, POLLOUT, 0 };

        if (poll(&pfd, 1, -1) < 0)
            continue;                   /* EINTR: poll again */

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;                      /* RTMP_WriteQueue will report FAILED */

        if (pump_writable(r) < 0)
            break;
    }
}
```

New packets may be queued while earlier ones are still waiting; the FIFO
preserves submission order and each channel's compression history is updated
in that same order, so a packet is always serialized against the packet that
precedes it on the wire rather than against whatever happens to have been
fully sent first.

## Test hook

`RTMP_SetDebugSend(RTMPDebugSend *fn)` optionally replaces the plaintext
`send()` used by both write paths.  The callback has the same contract as
`send()` (positive byte count, zero, or -1 with `errno` set) and lets tests
script short writes, `EINTR` and repeated `EAGAIN` deterministically.  Pass
`NULL` to restore the real socket send.  It is intended for testing, not for
production use.
