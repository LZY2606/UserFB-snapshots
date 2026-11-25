# MPEG4-GENERIC / AAC AU timing and fragmentation

This repository includes a local real-chain example:

```sh
sh task_support/run_example.sh
```

It creates a `MediaSession`, receives through the normal
`MPEG4GenericRTPSource` and `MultiFramedRTPSource` path, and sends two loopback
RTP packets containing tiny AAC-like AU byte patterns.

## SDP parameters

All parameter names are lower-cased by the existing SDP fmtp parser.  Missing
numeric parameters default to zero.

- `SizeLength`: bit width of each `AU-size`.
- `IndexLength`: bit width of the first AU's absolute `AU-Index`.
- `IndexDeltaLength`: bit width of later `AU-Index-delta` fields.
- `constantDuration`: duration of one sequence step, in **RTP clock ticks**
  (not milliseconds or AAC frames per second).
- `CTSDeltaLength`: `0` disables CTS; valid enabled widths are `1..32`.
  When enabled, every AU header contains a one-bit `CTS-flag`.  Only when that
  flag is `1` is a two's-complement signed `CTS-delta` of the configured width
  read.

Widths above 32 are rejected.  `DTSDeltaLength`,
`RandomAccessIndication`/`RAP`, and `StreamStateIndication` are not
implemented; a nonzero configuration is rejected rather than consumed as
following header or payload bits.

`AU_headers_length` is measured in **bits**.  Parsing stops at the exact
declared bit position; padding bits in the final byte are not interpreted as
another AU header.

## Clock and timing

The first AU in an RTP packet is anchored to that packet's mapped RTP
presentation time from the library's existing RTP/RTCP synchronization state.
It is not recomputed from a new wall-clock call per callback.

With nonzero `constantDuration`, the first AU index must be zero.  For later
headers:

```text
relativeSequence[n] = relativeSequence[n-1] + indexDelta[n] + 1
time[n] = anchor + relativeSequence[n] * constantDuration
```

If a non-first AU has `CTS-flag=1`, its time is instead:

```text
time = anchor + signedCTSDelta
```

and `constantDuration` is not added.  The first AU must have `CTS-flag=0`.
Tick arithmetic uses 32-bit RTP timestamp wrap, and each timeval is derived
directly from the anchor and total tick offset rather than by repeatedly
adding rounded microsecond durations.

When `constantDuration=0` and there is no explicit CTS, the historical
behavior is retained: enclosed AUs share the packet's presentation time.  The
implementation does not guess an AAC frame duration from the sample rate.

## Fragmentation

`AU-size` always denotes the complete access-unit size, even when the current
RTP payload contains only a fragment.  Consecutive sequence numbers and the
same AU timestamp/index/size identity are required for reassembly.  A lost
fragment, a different AU, or an overlong continuation discards only the
unfinished AU; later valid packets continue through the existing reordering
path.

The RTP marker bit reports completion of the packet's final AU.  Complete AUs
earlier in a packet are delivered individually even when the marker is zero
because a trailing AU is fragmented.  The presentation time captured for the
first fragment is retained for the single completed delivery.

## Tests

`test/aac-au-timing.cpp` provides:

- `int test_aac_au_headers()`
- `int test_aac_au_timing()`
- `int test_aac_fragment_timing()`

`sh task_support/run.sh target` links all three through
`task_support/target_main.cpp` and also builds/runs the existing interleaved
framing test.  The tests use an independent bit writer and real loopback RTP
`FramedSource` callbacks with a bounded event-loop timeout.

If the sandbox disables local UDP binding, allow local binding
(`sandbox.network.allowLocalBinding: true`) before running the target/example.
