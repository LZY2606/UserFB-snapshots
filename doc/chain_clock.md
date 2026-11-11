# Chained-stream clock and pre-roll regression test

`tests/chain_clock.cc` (driven through `tests/run_chain_clock.sh`) is a
regression test for the chained-encoding code in `src/opusenc.c`.  It exists
because the obvious check — "EOS granule minus pre-skip equals the expected
length of each chain" — can pass while the audio near a chain seam is wrong:
the granule arithmetic can be perfect while the packet that was supposed to
provide pre-roll overlap is the wrong packet.

The production encoder and the public API are not touched.  The test only
adds a C++ test program, a shell runner, and this document.  No Python, no
audio devices.

## What is encoded

Each case builds a single mapping-family-0 stereo Ogg Opus stream of six
logical links (chains) through the callback API, with every written page
byte captured.  Serials start at 17001 and increase by one per chain, the
bitrate is 96000, and the frame size is 20 ms or 60 ms.

* `queued-*`: segment lengths 333, 0, 1001, 2209, 5003, 457 input frames.
  Everything (including the zero-length and tiny links) is queued with
  `ope_encoder_chain_current()` inside one encoder and flushed by a single
  final `ope_encoder_drain()`, so all packetization happens with the full
  chain list known — this exercises overlap-packet reuse.
* `streamed-*`: segment lengths 94001, 15, 0, 44117, 5003, 457.  The first
  segment exceeds the default decision delay, so chaining continues after
  encoding has already started.

Both are run at 44100 and 48000 Hz input, giving 8 case groups named
`queued|streamed-RATE-MS`, selected with `--case`.

The input signal is generated continuously over the whole input timeline:
frame `i` (0-based, absolute), channel `c` is
`trunc(6000*sin(i*(0.013+c*0.007)) + 2500*sin(i*0.071))` as int16.  Each
group is encoded three ways: whole segments as int16, int16 fragmented into
a 1/7/251/13-frame chunk cycle, and the same int16 values scaled by exactly
1/32768 written as float with the same fragmentation.  Zero-length segments
still perform a zero-frame write.  All three must produce byte-identical
page streams; that only proves the write API is fragmentation-agnostic, it
says nothing about correctness of the bytes.

## Independent reference, part 1: container and clock

The test parses the Ogg layer itself — capture pattern, version, header
type flags, little-endian granule/serial/page-number fields, lacing values,
packet boundaries across pages — and verifies, without borrowing anything
from `src/ogg_packer.c`:

* page lengths (header + segment table + payload) and the Ogg CRC
  (polynomial 0x04c11db7, computed over each page with the CRC field
  zeroed),
* serials are exactly 17001..17005, each in one contiguous run, page
  numbers restart at 0 and increment per chain,
* BOS on the first page of each chain, EOS exactly on the last, and the
  continued-packet flag set exactly when the previous page of that chain
  ended mid-packet (such a page must carry granule -1),
* each chain starts with a well-formed 19-byte OpusHead (version 1, 2
  channels, the case's input rate, zero gain, mapping family 0) alone on
  the BOS page, followed by a well-formed OpusTags packet alone on the
  next page, both with granule 0,
* on every normal audio page, the page granule equals the chain's decoded
  sample position so far; on the final page of a chain the granule (the
  chain's EOS granule) may crop the tail, so it must land inside the last
  packet's span.  Packet durations are always read from the actual Opus
  packets with `opus_packet_get_nb_samples()` — in 60 ms mode the final
  drain may shorten the last packet, so the configured frame size is never
  assumed.

The clock rule under test: the audible end point for cumulative input `N`
is `ceil(N*48000/rate)` on the 48 kHz granule clock, and a chain's length
is the difference of *adjacent cumulative* end points — never a per-segment
rounding.  For every chain, `EOS granule - pre-skip` (both read from the
file: the EOS page granule and the chain's own OpusHead pre-skip) must
equal that length, zero-length chains included.  When all six chains pass,
the program prints `CLOCK_OK <case> chains=6`.

## Independent reference, part 2: the sound reference

This is what catches "the clock is right but the pre-roll packet is wrong".

The packet callback record preserves the raw bytes and the production order
of every packet the encoder emits.  At a chain boundary the encoder
re-commits the previous chain's tail packet — and, when overlap requires
it, the chaining keyframe before it — into the new chain.  Those reused
packets are aligned back onto the single compressed timeline by skipping
any callback packet identical to one of the last few produced packets (the
chaining overlap is never more than a couple of packets; this is a bounded
local window, not whole-file content deduplication, and reused packets
never occupy global time twice).  Every remaining — i.e. newly produced —
compressed packet is fed exactly once, in production order, to one
persistent libopus decoder.  That decoder's output, after dropping the
first chain's pre-skip, is the reference timeline of the whole input.

Each chain is also decoded on its own: a fresh decoder state per chain
(zero-length chains included — their decode and crop still run), every
audio packet decoded, with raw packet durations, actual decodable length,
and head/tail cropping cross-checked.  The chain's audible PCM is exactly
its own decode cropped to `[pre-skip, EOS granule)`.

Chain `k`'s audible PCM is then compared sample-by-sample against the
reference slice `[E_k, E_{k+1})`, where `E_k` are the cumulative
`ceil(N*48000/rate)` end points.

## Tolerances

The comparison is done on normalized float PCM (`opus_decode_float`):

* maximum absolute error per sample: **0.04**
* RMS error over all valid samples of all six chains: **0.003**

The encoding is lossy, so the chains are not compared against the original
sines, and a chain decoded from a fresh decoder state is not required to be
bit-identical to the reference timeline.  The tolerances exist to absorb
exactly one thing: the small, decaying divergence at the start of a chain,
where a fresh decoder begins at the prediction-disabled chaining keyframe
instead of with the reference decoder's full history.  In practice the
largest observed deviation is about 0.026 (20 ms frames), and RMS stays
below 0.002.  A wrong pre-roll packet — e.g. the current packet substituted
for the previous chain's tail packet — produces errors around 0.1 at the
seam, an order of magnitude above the tolerance.

Failures are reported as `FAIL <case> <TAG> ...` with the group name, the
chain, and the packet or sample range involved (e.g.
`FAIL queued-48000-20 PCM_REFERENCE chain=3 sample=0 channel=0 |diff|=0.1229 ...`),
never a bare assertion.

## Mutation regression

`tests/run_chain_clock.sh` first requires the pristine library to pass all
8 groups, then applies the two patches in `devcheck/mutations/` to isolated
copies of the library sources (production files are never modified), builds
the unchanged test against each copy, and runs the same 8 groups:

* `floor-clock.patch` rounds the cumulative end point down instead of up.
  At 44100 Hz the per-chain sample lengths come out wrong and the
  `CLOCK_LENGTH` check fails; at 48000 Hz `ceil` and `floor` coincide, so
  those groups must still pass.  The runner prints `DETECTED_CLOCK_FLOOR`.
* `wrong-preroll.patch` keeps the chain clock and cropping intact but
  commits the current packet instead of the reused previous-chain pre-roll
  packet.  All six chains' duration checks pass (`CLOCK_OK`), and the sound
  reference then catches the substituted packet (`PCM_REFERENCE`).  In one
  60 ms group the substituted packet is the drain-shortened final packet,
  which also makes the last chain's granule clock inconsistent, so the
  granule cross-check reports that group instead.  The runner prints
  `DETECTED_WRONG_PREROLL`.

A detection only counts if the variant compiles and the real test binary
fails the real assertions; compile failures, zero-case runs, or
name-matching shortcuts are treated as runner errors.
