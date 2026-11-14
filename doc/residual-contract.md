# Residual contract: independent bitstream-driven decoder regression

`tests/residual_contract.c` is a content-level regression contract for the
native FLAC decoder's residual reading, inverse prediction and stereo
reconstruction paths. It exists so that an internal re-organization of
bit reading or inverse prediction can be made without changing the
production algorithm or any public interface: every decoded integer is
checked against an independently derived expectation, not against a
frozen decoder output.

## What the test owns

Everything required to build a legal native FLAC stream is implemented in
the test file itself:

- an MSB-first bit writer (`BitBuf`) emits frame headers, subframe
  headers, warm-up samples, Rice/Rice2 residuals, escape partitions and
  frame footers directly as bits;
- CRC-8 (x^8 + x^2 + x + 1) and CRC-16 (x^16 + x^15 + x^2 + 1) are
  computed by local byte-wise routines (`own_crc8`, `own_crc16`), not
  taken from libFLAC;
- expected PCM is reconstructed with wide-integer recurrences written in
  the test (`ref_fixed_restore`, `ref_lpc_restore`), including explicit
  floor division (`sar_floor`) for the negative LPC scaling path.

The production bitwriter, residual encoder and stream encoder are never
used to construct the reference streams, and no decoder output is frozen
as an oracle. The single exception is the `library_roundtrip` case,
which deliberately keeps a library-internal encode/decode round trip as
a compatibility reference.

The STREAMINFO block carries a zero MD5 (so MD5 checking is disabled in
the decoder); frame header CRC-8 and frame footer CRC-16 are always
computed correctly. Scope is limited to 16/24-bit, two-channel material
with block sizes between 64 and 256 samples. No corrupted input and no
overflow-seeking data is used.

## Groups

### residuals

Two cases build fixed-predictor subframes from explicit per-channel
residual plans, one with PARTITIONED_RICE and one with
PARTITIONED_RICE2. Each frame uses four partitions that mix ordinary
Rice parameters with escape partitions:

- a width-0 escape partition means every residual in that partition is
  zero;
- a non-zero escape width is interpreted as plain two's complement;
- positive, negative and zero residuals all occur;
- long unary runs (e.g. 206 unary zeros) deliberately span machine words
  inside the bitreader.

The predictor order is subtracted only from the first partition; later
partitions carry their full length. The generator records each
partition's start/end bit position (`PartRec`) together with the residual
array, and the decoder callback checks the decoded residuals, the
partition parameters/escape widths and the final PCM. Warm-up samples
and residuals are constructed as distinct data.

### prediction

Four cases check inverse prediction with independently computed wide
integer recurrences (the production `restore_signal`/fixed helpers are
not used for expectations):

- `fixed_orders`: fixed predictors of orders 0..4 over several frames;
- `lpc_mixed_coefficients`: LPC filters with positive and negative
  coefficients and a negative, non-aligned prediction sum so that
  floor(-infinity) scaling differs from truncation toward zero;
- `lpc_wide_accumulator`: large coefficients whose worst-case
  pre-shift prediction exceeds 32 bits, forcing the decoder's wide
  accumulator path while the actual signal stays within 24 bits (no
  32-bit mastering material);
- `wasted_bits`: subframes with different wasted-bit counts, verifying
  that reconstruction happens at the effective bit width first and the
  wasted bits are shifted back in before stereo restoration.

Data is chosen so that a wrong coefficient sign, historical ordering or
scaling position changes the PCM; comparison is per sample.

### stream

Five cases pack the subframes into sequences of legal frames whose
prediction style and partition parameters change between frames. All
four channel assignments are covered — independent, left-side,
right-side and mid-side — with the side channel carrying one extra bit.
The mid-side material includes a negative odd mid value and both
positive and negative odd side values, exercising the parity bit in the
mid reconstruction. The final case is the library-internal round trip.

Each stream is decoded four ways: normal reads through
`FLAC__stream_decoder_process_single`, normal reads through
`process_until_end_of_stream`, and the same two entry points with a read
callback that alternately delivers 1, 3 and 7 bytes. After a
`FLAC__stream_decoder_reset()` (which rewinds the read cursor via the
seek callback) the stream is replayed and must produce identical
results. The write callback verifies sample numbers, every integer PCM
value, the absence of decoder errors and the exact sample count. A
mismatch reports the frame, channel, partition and the first differing
sample; absence of an error alone is never treated as success.

## Running

```sh
bash task_support/run.sh baseline   # builds libFLAC and the existing checks
bash task_support/run.sh target     # additionally builds and runs this contract
bash task_support/mutants.sh        # target + baseline, then two defect copies
```

The build uses the bundled native FLAC sources, statically linked, with
Ogg support and multithreading disabled. No external media, FFmpeg or
other decoder is needed at run time.

`mutants.sh` copies the tree, applies one source defect per copy
(flipping the sign of non-zero escape residuals, and removing the
mid-side parity bit), rebuilds, and requires each mutant to compile,
reach the content comparison and then be rejected by these cases with a
non-zero exit and a `FAIL` line. A build failure, crash, source-only
inspection or zero executed cases does not count as detection.
