# Receiver-side RTP retransmission (RTX, RFC 4588) recovery

The "liveMedia" library can now recover lost RTP packets of a video stream from an
SSRC-multiplexed retransmission stream (RFC 4588), i.e. one that arrives on the
*same* RTP session (same destination port) as the primary stream, but with its own
payload type and SSRC.

## SDP

Recovery is configured entirely from the SDP description; no application code
changes are needed.  After `MediaSession` parsing and `MediaSubsession::initiate()`,
the recovery path is connected automatically, and the application keeps reading the
video from the subsession's `readSource()`, exactly as before.

A complete declaration looks like this (the `rtpmap`, `fmtp` and `ssrc-group`
lines may appear in any order):

```
v=0
o=- 1 1 IN IP4 192.0.2.1
s=camera
t=0 0
c=IN IP4 192.0.2.1
m=video 5004 RTP/AVP 96 97
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1
a=rtpmap:97 rtx/90000
a=fmtp:97 apt=96
a=ssrc-group:FID 1111 2222
```

- The primary payload format (here `96`, H.264) is the first format in the `m=` line.
- `a=rtpmap:97 rtx/90000` declares the retransmission payload format; its clock
  frequency must match the primary stream's.
- `a=fmtp:97 apt=96` binds the RTX payload format to the primary payload format.
  (This line is recorded separately and does *not* pollute the primary codec's
  `fmtp` attributes.)
- `a=ssrc-group:FID 1111 2222` gives the primary stream's SSRC and the
  retransmission stream's SSRC.

## Scope

Supported:

- One video subsession with a single primary/RTX payload-type pair and a single
  `FID` SSRC group; unicast; H.264 or H.265.
- The primary payload type is the first entry of the `m=` line; `apt` must name it.
- Identical duplicate declarations are allowed.  Contradictory declarations,
  multiple RTX mappings for one primary stream, or invalid values make the
  subsession's `initiate()` fail explicitly.
- SDPs without RTX declarations behave exactly as before.  Incomplete declarations
  (`rtx` without `apt`, or without `FID`) simply leave recovery disabled; the
  primary video is still received normally.

Not supported (out of scope): sending NACKs, the retransmission *sender* side,
separate-session RTX (RFC 4588 "session multiplexing"), dynamic SSRC discovery,
and SRTP.

## Reception behavior

- Only packets matching the configured RTX payload type *and* RTX SSRC are
  accepted, and only after a valid primary-stream packet (from the FID's primary
  SSRC) has been seen.  CSRC lists, header extensions, and padding are processed
  by the normal RTP rules first; the payload then begins with the 2-byte
  network-order "original sequence number" (OSN), followed by the original codec
  payload.
- Recovered packets enter the existing reordering/framing machinery using the
  primary SSRC, the primary payload type, and the OSN; the original RTP timestamp
  and marker bit are preserved.  The RTX packet's own sequence number never
  advances the primary stream's reordering cursor, and the RTX SSRC never triggers
  a "primary SSRC changed" reset.  Duplicates (late original, repeated RTX, or an
  already-delivered OSN) are delivered only once.
- Statistics are accounted separately: primary packets are recorded under the
  primary SSRC with their own sequence numbers; RTX network packets are recorded
  under the RTX SSRC with their outer sequence numbers, so reception reports keep
  reflecting the actual loss on the primary stream.  Recovered packets take their
  presentation time from the primary stream's existing RTP/RTCP time mapping
  without recording an extra primary-stream reception or disturbing its jitter.
- Malformed packets (short OSN, truncated header extension, bad padding, wrong
  payload-type/SSRC combination) are dropped without affecting subsequent framing.

## Tests

`test/rtx-recovery.cpp` (run via `sh task_support/build.sh target`) builds a real
`MediaSession` from a hand-written SDP and feeds hand-written RTP/RTCP packets
over loopback UDP and RTP-over-TCP (interleaved), covering: SDP
parsing/validation, loss and recovery of an H.264 FU fragment across the
65535/0 sequence-number wrap, late originals and duplicate retransmissions,
RTCP SR time mapping, per-SSRC reception statistics, malformed-packet handling,
and isolation between two subsessions that share payload types and OSNs.
