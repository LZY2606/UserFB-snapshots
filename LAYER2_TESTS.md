# Layer II fixed-point contract tests

`tests/layer2_contract.c` is an independent regression for the MPEG Audio
**Layer II** branch that lives in `minimp3.h` alongside the Layer III decoder.
The production algorithm, the public `mp3dec_*` interface and the existing
PCM/PSNR test (`minimp3_test.c`, `task_support/baseline.sh`) are unchanged.

## Build and run

```
bash task_support/build.sh          # compiles .task-build/target
.task-build/target digits           # grouped quantization contract
.task-build/target scfsi            # scalefactor / SCFSI contract
.task-build/target frames           # full-frame, public-interface contract
bash task_support/baseline.sh       # original Layer III + Layer II PCM run
bash task_support/target.sh         # all three groups, mutations, then baseline
```

Every group prints `BEGIN <group>`, ends with `DONE <group> checks=<n>` (a
non-zero number of assertions) and `PROCESS_COMPLETE`, and returns 0. A failed
comparison prints `CONTENT_MISMATCH <file>:<line> <message>` and exits 42.

## The independent model

The test defines `MINIMP3_IMPLEMENTATION` and `MINIMP3_NO_SIMD`, then includes
`minimp3.h` and `task_support/check.h`; it implements only
`int task_group(const char *)` and defines no `main`.

All expectation values are produced by a small, self-contained model in the
test file:

- its own bit writer `model_put` and bit reader `model_get` (MSB-first,
  byte-crossing); the production `get_bits` is never called by the model;
- independent copies of the ISO 11172-3 bit-allocation tables; the production
  tables are only consulted read-only through `L12_subband_alloc_table`, which
  is format data, not decoding logic;
- its own grouped codeword pack/unpack (`model_pack_group`,
  `model_unpack_group`) and its own scalefactor math
  `2*pow(2,-index/3)/levels` in `double`;
- no production output is ever stored and re-used as an expected value.

The production code is exercised only through the entry points under test:
`L12_dequantize_granule`, `L12_read_scalefactors`, `L12_apply_scf_384`,
`L12_subband_alloc_table` and the public `mp3dec_decode_frame`.

### Read-only observation point

`task_support/layer2_observe.inc`, included by `minimp3.h` only when
`MINIMP3_LAYER2_OBSERVE=1`, snapshots the `2 x 32 x 12` subband values of each
384-sample block right after `L12_apply_scf_384` and before the synthesis
filterbank. The macro defaults to `0`, so a normal build adds no state, no
callback and no symbols (verified: an ordinary compile contains no
`layer2_obs` references).

## What is actually covered

### `digits` — grouped quantization (allocation levels 17, 18, 19)

- Asserts the three levels use radix `mod = 3, 5, 9` and codeword widths
  `5, 7, 10` bits, and that every legal triple fits the width.
- Iterates **every legal triple** (`mod^3` of them) per level. Each triple is
  packed bit-by-bit from the explicit, de-centered samples using the
  production mixed-radix ordering (first transmitted sample = least
  significant digit).
- Plain (non-grouped) bands frame the grouped band; for start offsets
  `0..7` the stream is padded so reads cross byte boundaries.
- The same bytes are handed to the real `L12_dequantize_granule`. Checks
  localize, per level/group/digit/offset:
  - sample order and the centered integer value of each digit,
  - the exact bit position of every codeword and the exact number of bits
    consumed,
  - that zero-allocation subbands and the unused mono channel stay exactly
    zero.
- Only legal codewords are modeled; malformed-codeword tolerance is
  intentionally left undefined.

### `scfsi` — scalefactor selection information

- A dedicated bit writer generates all four SCFSI reuse patterns with
  deliberately different per-segment 6-bit indices; the 2-bit SCFSI codes and
  the 6-bit scalefactor fields are written and read back on separate streams,
  matching how `L12_read_scale_info` and `L12_read_scalefactors` consume them.
- Asserts exactly how many 6-bit fields are read (`3, 2, 1, 2`), including the
  stale-value reuse when a mask bit is absent, and the exact bit consumption
  (integer counts must match precisely).
- Compares every float scale against the `double` formula
  `2*pow(2,-index/3)/levels` with tolerance `2e-6*(1+|expected|)`. The plain
  denominator is `2^ba - 1`; the grouped denominator is the radix `3/5/9`.
- Covers indices `0, 1, 2, 62, 63`, grouped and plain rebuild, and both a mono
  scenario and a joint-stereo scenario: inside the boundary each channel has
  its own codeword and its own scales; outside the boundary the codeword is
  shared while two scale sets are retained. Left and right scales are compared
  explicitly, never reduced to "both energies are non-zero".

### `frames` — full frames through the public decoder

- Builds a contiguous stream of explicitly planned MPEG-1 Layer II frames:
  mono, plain stereo and joint stereo (tight and wide boundary), each with
  grouped and plain quantization and all four SCFSI patterns, plus a
  same-header stereo chain that exercises frame-to-frame matching.
- Header, bit allocation, SCFSI codes, scalefactor fields and payload are all
  generated from a plan and independently read back from the finished bytes.
- The frames are accepted by the real `mp3dec_decode_frame`. Checks cover the
  returned 1152 samples, consumed bytes, channel count, sample rate, bitrate,
  non-zero PCM, and an untouched output guard region.
- The observation point compares every pre-synthesis subband value against the
  independent model for each of the three 384-sample blocks, including the
  joint-stereo exterior bands (left raw samples reused on the right with the
  separately parsed right scalefactor).
- The synthesis filterbank has no separate reference, and Layer III short
  windows, bit reservoir and backend selection are out of scope.

## Difference from the original PCM/PSNR test

`baseline.sh` decodes three Layer III and three Layer II frames and folds the
final PCM into a single hash / PSNR number. It answers "did the whole pipeline
drift", but a regression can only be localized somewhere in the combined
bitstream parse, dequantization, scaling, synthesis and PCM conversion, and a
small internal change can be hidden by the aggregate.

These contract tests never re-score PCM. They feed hand-planned fields and
compare the internal, fixed-point quantities directly. The **first difference**
is therefore reported at the exact failing construct:

- a specific grouped codeword — level, granule group, digit and its bit
  position/consumed bit count (`digits`);
- a specific scalefactor slot, SCFSI pattern and 6-bit field, with exact field
  counts and bit consumption (`scfsi`);
- a specific frame, 384-block, channel, subband and moment before synthesis
  (`frames`).

## Mutation detection

`task_support/mutations` contains two injected faults, both of which must
compile and run to a content assertion (never crash or fail to build):

- `radix.patch` — splits grouped triples with the wrong divisor. It is caught
  by `digits` (and the grouped paths of the other groups) with
  `CONTENT_MISMATCH` and exit 42.
- `reuse.patch` — makes an SCFSI pattern read one scalefactor too many. It is
  caught by `scfsi` and `frames` (but not `digits`, which does not parse
  scalefactors), again with `CONTENT_MISMATCH` and exit 42.

All test code, material generation and examples are plain C99; no audio is
downloaded and no packages are installed.
