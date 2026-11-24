# `frame_sequence_model` — structured LZ4 frame-sequence regression

`tests/frame_sequence_model.c` is a deterministic, replayable model for
cross-call LZ4 frame decoder state: checksum sticky state, frame boundaries,
skippable frames, output-history/dictionary continuation, and
`LZ4F_resetDecompressionContext()`.

It is registered with `task_support/check.h` as one case in each required
group:

- `sequence`
- `integrity`
- `recovery`

No auxiliary C file is used, so `task_support/run.sh target` continues to
compile only `task_support/driver.c` and this test file.

## What the existing tests already cover

`tests/frametest.c` already provides broad randomized coverage, including:

- random input and output segment sizes;
- compressible noise and round-trip/content checks;
- sentinel bytes around output buffers;
- randomized `stableDst` and `skipChecksums` options;
- skippable-frame paths;
- error localization and crash/fuzz-style loops.

There are also shell-level checks such as
`tests/test-lz4-frame-concatenation.sh` and
`tests/test-lz4-skippable.sh`, plus `task_support/baseline.c`, which checks one
successful content-checksum round trip.

Those tests do not keep a stable, independently modeled sequence of normal,
skippable, and checksum-damaged frames and pin the specific state transition
asked for here: skip content verification on one frame, then prove whether the
sticky skip state survives frame end, truncation, an explicit checksum error,
or context reset when the next frame is decoded.

## Corpus and independent model

The generator uses fixed seed `1681` and creates exactly 120 bounded
sequences, plus independent fixed examples.

Each sequence owns:

- `FrameModel.raw`: the exact on-wire bytes of one frame;
- `FrameModel.payload`: expected decoded bytes for ordinary frames;
- a concatenated `Sequence.stream`;
- concatenated expected output, excluding skippable-frame payloads;
- parsed field boundaries used by the field-boundary chunk plan.

Frame coverage includes:

- ordinary frames produced by the real `LZ4F_compressFrame()` API;
- format-written skippable frames with varied magic and payload sizes;
- frames where only a checksum field is flipped;
- empty, tiny, short, and `66000`–`72000` byte inputs;
- highly repetitive and mixed/entropic data;
- both `LZ4F_blockLinked` and `LZ4F_blockIndependent`;
- block-checksum and content-checksum combinations.

The focused content-skip case uses a 20,000-byte repetitive linked frame and
asserts `first_block_uncompressed == 0`, so the required content-checksum
relationship is proven on a real compressed block rather than only on raw
blocks.

## Decode arrangement

Every modeled decode uses:

- one 40-byte output buffer, repeatedly refilled with `0xA5`;
- 16 prefix sentinel bytes and 32 suffix sentinel bytes;
- `LZ4F_decompressOptions_t.stableDst = 0`;
- deterministic single-byte, field-boundary, or fixed-random input chunks.

After every call the test validates:

- `srcSizePtr` is no larger than the offered chunk;
- `dstSizePtr` is no larger than the 40-byte capacity;
- the cursor advances only by the reported consumed amount;
- prefix/suffix sentinels are unchanged;
- on successful calls, unwritten output-buffer bytes retain the fill pattern;
- reported output bytes match the independent cumulative model;
- a zero return occurs exactly at a parsed frame boundary;
- every frame produces exactly one zero return when complete.

The step cap is:

```c
step_cap = sequence.stream.len + sequence.expected.len + 256;
```

Every continuing successful call must consume at least one input byte or
produce at least one output byte, so the linear input+output bound is
generous. The extra 256 calls allow state-machine transitions. A successful
call that neither consumes nor produces is treated as input exhaustion and
stops with the returned positive hint instead of spinning.

## Failure replay output

A failing `CHECK` first prints a `MODEL_FAILURE` block containing:

- fixed seed (`1681`);
- sequence number;
- current frame number;
- first modeled difference, when applicable;
- chunk mode, parsed field boundaries or exact random chunk sizes;
- checksum-switch transitions;
- the last decode step records, including offer, actual consumption,
  production, cumulative output, cursor, hint, and exact error code;
- `REPLAY_SEQUENCE` hex for the complete concatenated byte stream;
- separate `FRAME_HEX` blocks for every frame.

The normal success output is concise because diagnostics are printed only on
failure.

## Pinned checksum relationships

### Content checksum skip is frame-scoped

`integrity_content_skip_then_next_frame()` builds two content-checksummed
frames and enables their block checksums. Both content CRC fields are
corrupted by one bit; no block-checksum field is modified.

On the first decode call only, `skipChecksums=1`; every later call passes
`skipChecksums=0`. The internal sticky state remains enabled for the rest of
the first frame:

- the first corrupted frame reaches its zero-return boundary;
- its cumulative output at that boundary is the complete original payload;
- the next frame is decoded with no skip option;
- it fails with exactly `LZ4F_ERROR_contentChecksum_invalid` (enum value 18 in
  this build), not merely “some error”.

### Exact block and content errors

`integrity_one_block_path()` covers both first-block forms:

- compressed block, corrupted block CRC, default options →
  `LZ4F_ERROR_blockChecksum_invalid`;
- raw/uncompressed block, corrupted block CRC, default options →
  `LZ4F_ERROR_blockChecksum_invalid`;
- corrupted content CRC, default options →
  `LZ4F_ERROR_contentChecksum_invalid`.

The assertions compare `LZ4F_getErrorCode()` to the exact enum. Truncation is
kept separate and is not counted as checksum detection.

### Preserved upstream difference: compressed block CRC versus `skipChecksums`

The current implementation has an intentional-looking asymmetry:

- Raw/uncompressed block CRC verification is inside
  `if (!dctx->skipChecksum)` at `lib/lz4frame.c:1821`, so a bad raw-block CRC
  can be skipped. The fixed raw case decodes and byte-compares the full
  payload with skip enabled.
- Compressed-block CRC verification at `lib/lz4frame.c:1871` is not gated by
  `dctx->skipChecksum`; under the normal (non-fuzzing) build it returns
  `ERROR_blockChecksum_invalid` even when `skipChecksums=1`.

Both paths are fixed small examples in `integrity_one_block_path(..., skip=1)`
and are recorded here as **current behavior**. This regression does not
require an upstream fix, and “skip and restore the complete payload” must not
be generalized to a corrupted compressed-block checksum: that case is asserted
to remain an error.

### Exhaustion hints are not detection

`integrity_truncation_hint_is_not_detection()` cuts a valid frame at
`header_size + 2`, leaving a two-byte partial block header. The partial call
and a zero-input probe both return a positive hint and no error. This matches
the API meaning: the hint says more input is needed; a truncated prefix is not
required to fail immediately.

## Recovery and context equivalence

The `recovery` group exercises both required starting states:

1. an unfinished large linked frame, started with `skipChecksums=1`;
2. a frame that has already returned
   `LZ4F_ERROR_contentChecksum_invalid` with skip disabled.

Each path calls `LZ4F_resetDecompressionContext()` on the original context,
then decodes another complete multi-frame sequence containing an ordinary
linked/block-checksum frame, a skippable frame, and an independent frame.

The reused context is compared with a freshly created context step by step:

- offered chunk;
- actual consumption;
- produced bytes;
- cumulative output;
- cursor;
- returned hint;
- completion count;
- exact error code.

Final end position and complete output are also compared against both the
fresh context and the independent model. After that, a corrupted-content frame
is decoded with default options and must again be rejected with
`LZ4F_ERROR_contentChecksum_invalid`.

## Logic-mutation verification

Two temporary production mutations were tested in throwaway copies only; the
normal repository source remains unpatched.

### Mutation 1: ignore content-checksum comparison

Temporary change in the copy:

```diff
- RETURN_ERROR_IF(readCRC != resultCRC, contentChecksum_invalid);
+ RETURN_ERROR_IF(0 && readCRC != resultCRC, contentChecksum_invalid);
```

The copy still compiles and runs. The new test rejects it with:

```text
MODEL_FAILURE seed=1681 sequence=6 frame=1 first_difference=result.error_code vs seq.expected_error: actual=0 expected=18
FAIL tests/frame_sequence_model.c:1111 model_check_ok_
```

### Mutation 2: reset does not clear sticky skip state

Temporary change in the copy:

```diff
-    dctx->skipChecksum = 0;
+    /* mutation: retain previous skip state */
```

The copy still compiles and runs. In the full driver order it is first rejected
by the integrity next-frame assertion; the recovery group also contains an
unfinished-with-skip → reset → bad-content assertion for the same state:

```text
MODEL_FAILURE seed=1681 sequence=1000 frame=1 first_difference=result.error_code vs (int)LZ4F_ERROR_contentChecksum_invalid: actual=0 expected=18
FAIL tests/frame_sequence_model.c:1178 model_check_ok_
```

## Commands and observed results

### Baseline

Command:

```sh
bash task_support/run.sh baseline
```

Observed result:

```text
LZ4_BASELINE_ROUNDTRIP_OK
```

### Target

Command:

```sh
bash task_support/run.sh target
```

Observed result:

```text
CASE sequence seed_1681_structured_sequences checks=2419
GROUP sequence cases=1 checks=2419 COMPLETE
CASE integrity checksum_scope_and_error_codes checks=73
GROUP integrity cases=1 checks=73 COMPLETE
CASE recovery reset_equivalence_and_recheck checks=8037
GROUP recovery cases=1 checks=7159 COMPLETE
TARGET_COMPLETE cases=3
LZ4_BASELINE_ROUNDTRIP_OK
```

The test also compiles cleanly under:

```sh
cc -std=c99 -O1 -g -fsanitize=address,undefined \
   -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror \
   -I. -Ilib task_support/driver.c tests/frame_sequence_model.c \
   .task-build/*.o -o .task-build/target-warn
```

In the local macOS sandbox used for this task, bare `mktemp` tried to use a
Darwin user temporary directory that Seatbelt rejected. The `run.sh target`
result above was obtained with a repository-local executable `mktemp` shim
prepended to `PATH`; `task_support/run.sh` and the test itself were not changed.
An unrestricted development environment can run the documented command
directly.
