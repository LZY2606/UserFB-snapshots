# Layer contract regression for nested Speex streams

Speex wideband (WB) frames embed a complete narrowband (NB) frame followed by
a high-band block; an ultra-wideband (UWB) frame embeds the NB frame, the WB
high block and a second high block in that order.  An NB decoder can therefore
play the low frequencies of a WB/UWB packet by skipping the high blocks, and a
WB decoder can play the two lower layers of a UWB packet.

`tests/layer_contract.c` is a self-contained regression test for that
contract.  It is deliberately independent of any production helper that could
share a bug with the code under test.

## What the test does

1. A C generator produces deterministic, low-amplitude PCM whose values depend
   on the **absolute sample index** (an LCG mapped to [-1000, 1000]).  No sine
   wave is used.
2. At least twelve frames per group are encoded back-to-back into one
   `SpeexBits` with the public `speex_encode_int()`.  VBR, VAD and DTX are
   explicitly disabled; quality is either fixed at 5 or cycled 1/3/8 per
   frame.  The reader is never reset between frames, and a legal terminator is
   appended at the end.
3. The finished byte stream is parsed by the test's **own MSB-first bit
   reader**, not by `SpeexBits`.  Layer headers are decoded with the bit-length
   tables below; payload bits are copied verbatim into a projected stream.
   There is no re-encoding and no call to `speex_mode_query()` or any other
   mode-length query function.
4. Two decoders with identical initial settings and persistent state decode
   the two streams in parallel: one reads the full high-layer stream, the
   other reads the independently extracted low-layer stream.  Enhancement is
   explicitly turned off on both.  Every frame compares:
   - the decoder return status,
   - the total number of decoded frames,
   - every `int16` sample of the frame (the first mismatching sample is
     reported, not just a final hash).

   The generated PCM is **never** compared with decoded audio: the codec is
   lossy.  Only two decoders fed bit-equivalent streams are compared.

## Bit layout and length tables (the independent reference)

All multi-bit fields are packed MSB first, exactly as in `libspeex/bits.c`.
Blocks are packed back-to-back with no byte alignment between them.

A frame is laid out as:

```
[ in-band request block ]?  NB block  [ WB high block ]?  [ UWB high block ]?
```

Twelve such frames are concatenated, then `speex_bits_insert_terminator()`
adds the terminator and pads to a byte boundary.

### Narrowband block

Header: 1 bit narrowband flag (always `0`) + 4 bits NB submode id = 5 bits.

| NB submode | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------------|---|---|---|---|---|---|---|---|
| block size (bits) | 43 | 119 | 160 | 220 | 300 | 364 | 492 | 79 |

Source: `libspeex/modes.c`, `SpeexSubmode.bits_per_frame` of
`nb_submode1..8`.  **The sizes include the 5-bit header**, i.e. the payload is
`bits_per_frame - 5` bits.  Submode 0 is the null/DTX block (5 header bits, no
payload); DTX is disabled in this test so it never appears.  Submodes 9-15 do
not exist as audio modes (14 marks an in-band request, 15 a terminator).

### High-band (WB and UWB) blocks

Header: 1 bit wideband flag (always `1`) + 3 bits sub-band submode id = 4
bits (`SB_SUBMODE_BITS` is 3, see `libspeex/modes.h`).

| SB submode | 1 | 2 | 3 | 4 |
|------------|---|---|---|---|
| block size (bits) | 36 | 112 | 192 | 352 |

Source: `libspeex/modes_wb.c`, `SpeexSubmode.bits_per_frame` of
`wb_submode1..4`.  The same values are mirrored by the static
`wb_skip_table` in `libspeex/nb_celp.c`, which is what the NB decoder uses to
jump over high blocks.  **The sizes include the 4-bit header**, matching the
decoder's arithmetic: it reads the header first, then advances by
`bits_per_frame - (SB_SUBMODE_BITS + 1)` bits.  Submode 0 marks a frame with
no high-band content; submodes 5-7 are invalid.

The generator verifies independently that every per-frame, per-layer and
per-message bit interval ends inside the number of valid bits emitted by the
corresponding `speex_encode_int()` call.

### In-band request blocks

An in-band request is itself a block that starts with the narrowband flag `0`,
followed by the 4-bit marker `14`, a 4-bit request id and the request payload.
It is inserted immediately before the NB block of the same frame, with no
padding.

| Request id | meaning | payload bits after the 9-bit header |
|------------|---------|-------------------------------------|
| 0-1 | reserved / enhancement | 1 |
| 2-7 | mode/VBR style requests | 4 |
| 8-9 | CHAR / stereo | 8 |
| 10-11 | max bit-rate etc. | 16 |
| 12-13 | acknowledge / user | 32 |
| 14-15 | (marker / terminator space) | 64 |

Source: the skip table in `speex_inband_handler()`,
`libspeex/speex_callbacks.c`.  The test uses:

- id 10 (`SPEEX_INBAND_MAX_BITRATE`) with a 16-bit payload and **no
  registered handler**: the default path must skip exactly 16 bits and leave
  the audio identical to a message-free stream;
- id 8 (`SPEEX_INBAND_CHAR`) with an 8-bit payload and a custom handler that
  reads only that character and records the call order.  The projection keeps
  the identical message bytes, and the handler must fire exactly once, before
  the corresponding audio frame, in frame order, on both decoders.

### Terminator

The terminator is a `0` flag followed by the 4-bit value `15`;
`speex_bits_insert_terminator()` then fills the rest of the byte with ones.
When the decoder reaches it, `speex_decode_int()` returns non-zero.  The test
makes one extra decode call per stream after the twelfth frame and requires
that call to fail: a correct stream must not synthesize a thirteenth silence
frame.

## Why high layers are skipped one call late

The NB decoder (`nb_decode()` in `libspeex/nb_celp.c`) searches for the next
NB block at the start of each call.  A high block trailing frame *N* is only
jumped over when the decoder looks for frame *N+1*.  Consequently the
`SpeexBits` cursor is **not** guaranteed to sit at the next frame boundary
immediately after a successful `speex_decode_int()`.  The test therefore never
asserts cursor alignment after individual decode calls; frame identity is
established solely from the independently parsed bit intervals and the
sequential decode results.

This is exactly why a one-bit mistake in the skip arithmetic leaves the first
frame intact and shifts every later frame onto the wrong bit: the wrong offset
is first consumed while searching for the second frame.

## Test groups

| Group | Source | Decoder | Projection | Quality | In-band traffic |
|-------|--------|---------|------------|---------|-----------------|
| `wb-nb-fixed` | WB | NB | drop WB block | fixed 5 | none |
| `wb-nb-changing` | WB | NB | drop WB block | 1/3/8 cycle | none |
| `uwb-nb-fixed` | UWB | NB | drop WB and UWB blocks | fixed 5 | none |
| `uwb-nb-changing` | UWB | NB | drop WB and UWB blocks | 1/3/8 cycle | none |
| `uwb-wb-fixed` | UWB | WB | keep WB block, drop UWB block | fixed 5 | none |
| `uwb-wb-changing` | UWB | WB | keep WB block, drop UWB block | 1/3/8 cycle | none |
| `controls-ignored` | UWB | NB | drop both high blocks, drop nothing of the message | fixed 5 | 16-bit id-10 request per frame, unregistered |
| `controls-callback` | UWB | NB | drop both high blocks, keep the CHAR message | fixed 5 | 8-bit id-8 CHAR per frame with ordered callback |

Each group contains twelve frames in one continuous `SpeexBits`.

## Failure diagnostics

Every failure line has the form

```
FAIL group=<name> frame=<n> layer=<nb|wb|uwb|msg|pcm|frame|term> bit=<position> kind=<KIND> <detail>
```

- `TRUNC` — the decoder returned an error (truncated/corrupt stream) or the
  decoded frame count differs; the reported bit position is where the failing
  decode started.  This distinguishes a broken stream from merely wrong audio.
- `PCM` — both decoders returned success but an `int16` sample differs; the
  detail names the first mismatching sample index and both values.
- `RANGE` — the independent parser found a header with an invalid submode id
  or a layer interval crossing the valid encoded bit count.
- `TERM` — a frame was produced after the terminator instead of ending the
  stream.
- `CALLBACK` — the CHAR handler did not run exactly once before its audio
  frame, ran in the wrong order, or saw the wrong character.

## Mutation harness

`tests/run_layer_contract.sh` first builds and runs the test against the
pristine tree and requires all eight `PASS <group>` lines.  It then copies the
whole source tree into separate temporary directories and applies one patch
per copy:

- `devcheck/mutations/skip.patch` — the NB high-block skip advances one bit
  too few (`advance -= SB_SUBMODE_BITS` instead of `SB_SUBMODE_BITS + 1` in
  `nb_celp.c`);
- `devcheck/mutations/control.patch` — an unregistered 16-bit in-band request
  is skipped as 15 bits instead of 16 in `speex_callbacks.c`.

Each mutated tree is compiled with the unmodified `devcheck/build.sh` and the
same test binary is run.  A mutation counts as caught only when the modified
library **compiles successfully** and the test then fails at runtime with one
of the assertion kinds above (bit position, frame count or PCM).  A compile
failure, a missing assertion, or any source-text inspection aborts the
harness.  The patches never touch the working tree; only the temporary copies
are modified, and the copies are removed on exit.  Successful detection prints
`DETECTED_skip` and `DETECTED_control`.

## Running

```sh
bash devcheck/target.sh
```

This runs the full layer-contract orchestration and then the existing
`devcheck/baseline.sh` smoke tests.  Everything is plain C and shell; no audio
device is required.
