# Seek-origin regression tests

`tests/seek_origin_test.c` (with `tests/seek_origin_support.h`) is an
independent C regression for the decoder's sample-accuracy path when a
WavPack file carries a non-zero 40-bit sample origin. Player recordings are
cut from a longer programme by whole blocks, so the interface counts from
zero while the compressed header keeps the programme's absolute coordinate.
These tests exercise that split directly: the production code is untouched,
nothing touches audio hardware or the network, and the only inputs are
materials produced through the public encoding API.

## How to run

```
bash task_support/run.sh baseline    # six-channel lossless + hybrid round trip, integer tests
bash task_support/run.sh target      # the new origin / lanes / restart groups
bash task_support/variants.sh        # two content-mutating variants must be rejected
```

`run.sh target` builds `task_support/driver.c` with the test file and runs it;
`tests/seek_origin_test.c` defines no `main` and registers its cases through
`task_cases` / `task_case_count`, and every comparison is a `TASK_CHECK`.

> Sandbox note: on this macOS session the runner's bare `mktemp` targets the
> per-user Darwin temp directory, which the sandbox does not allow writing.
> The commands above were run with a small `mktemp` shim earlier in `PATH`
> that points at a writable directory. No runner or variant script was
> modified.

## Independent catalog

`tests/seek_origin_support.h` walks each encoded image by physical block
length alone and records, per block: the byte interval in its own file, the
40-bit raw sample origin, the sample count, the flags, and its sub-stream
index inside the multichannel group. It also reconstructs each group's
relative start and total frames. Everything there is decoded from the raw
little-endian bytes:

* `so_block_index` / `so_total_samples` read the 40-bit `block_index` and
  `total_samples` fields themselves (including the all-ones "unknown" rule)
  instead of using `GET_BLOCK_INDEX` / `GET_TOTAL_SAMPLES`;
* `so_block_verify` / `so_block_fix_checksum` implement the block-checksum
  format (seed `0xffffffff`, multiplier 3 over 16-bit words) directly, so the
  relabelling step can re-seal blocks without trusting the library helper;
* `so_catalog_consistent` checks that the recorded byte intervals tile the
  file, that every audio block in a group shares the group's origin and
  sample count, and that sub-streams are numbered from zero.

Expected PCM never comes from a decoded header or a macro: the pre-encode
`int32_t` array is retained on the material (`SoMaterial.pcm`) and every test
comparison reads that array. Each channel of each frame is a distinct value
(`so_mark`), so a one-frame preroll mistake cannot hide.

## Relabelling

`so_relabel_shift` moves every audio block's stored 40-bit origin by the same
delta, leaves the compressed payload untouched, updates the header, and
recomputes the trailing checksum item in place (checksums stay enabled). The
total-length field is either preserved (known) or set to the all-ones
"unknown" form. Relabelled materials are used only for mono and stereo, as
the task requires. Origins exercised include zero, a small non-zero value
(12345), a value that crosses the 32-bit boundary (`0xfffffff0`), and a value
already above 32 bits (`2^32 + 12345` / `2^32 + 999`); known and unknown
totals are both covered, always inside the format's range.

Before any seek, every material is checked for the real physical catalog, the
full frame count, and a straight sequential decode of the whole material.

## Groups

### origin

Six cases (mono and stereo) with out-of-order targets that cross block and
group boundaries. Each seek verifies the returned cursor and every decoded
integer, and the error counter is required to stay at zero. The cursor is
read after the seek but asserted after decoding, so a wrong PCM result is
reported first.

`origin/cross32_unknown_mono` is the mono unknown-total case; the remaining
five cover zero, small, cross-32 and over-32 origins with known or unknown
totals.

### lanes

Three stereo hybrid materials that genuinely need the correction file, with
two independent readers (`.wv` and `.wvc`), each keeping its own read ledger.
Targets are chosen so the block starts at different byte positions in the two
files (block zero sits at offset zero in both, so it is skipped as evidence).
For each target the context is first parked in a different block, then the
seek is measured: the `.wv` ledger must have touched the `.wv` block offset
and the `.wvc` ledger must have touched the `.wvc` block offset. The `.wv`
position is never accepted as the correction position.

The tests also decode the same hybrid material with the correction lane absent
and require the PCM to differ, proving the correction lane is actually needed.
Evidence from the run:

```
WVC_IGNORED_DIFF material=lanes/known_zero_stereo differing_samples=794
LANES_TARGET material=lanes/known_zero_stereo target=350 wv_block=818 wvc_block=1236 wv_trace_hit=1 wvc_trace_hit=1
LANES_TARGET material=lanes/known_zero_stereo target=200 wv_block=528 wvc_block=740 wv_trace_hit=1 wvc_trace_hit=1
```

### restart

* `same_block_backward` — backward and forward seeks inside one block.
* `cross_block_jump` — jumps over block and group boundaries.
* `consecutive_no_read` — several seeks back to back with no decoding
  between them.
* `end_short_then_start` — a short read at the end (asking for more than
  remains), reaching the end by reading (never by seeking to the length), then
  returning to the start for a full read.
* `sixch_nonfirst_preroll` — a zero-origin six-channel material whose groups
  have several sub-streams; seeks land on frames that need non-first
  sub-stream preroll.
* `interleaved_two_instances` — a stereo and a six-channel context used in
  alternation, with per-instance ledgers, to catch state bleeding.

Reads alternate chunk sizes 1, 7 and a cross-block size; every output buffer
is surrounded by sentinels on both sides. Only valid targets are sought; EOF
is expressed by reading. After a failed seek the tests do not read further.

## Activated paths

`origin` activates the non-zero-origin relabel path: `find_sample` bounds,
`SET_BLOCK_INDEX(... - initial_index)` and the preroll skip `sample -
sample_index`. `lanes` activates the separate `.wvc` search
(`find_sample` on `wvc_in`) and the matching correction header load. `restart`
activates the same-block rewrite, consecutive seeks, EOF/short-read handling,
and the multi-sub-stream preroll loop.

## First-difference evidence

On the original library, `run.sh target` passes:

```
GROUP origin cases=6 checks=12612 COMPLETE
GROUP lanes cases=3 checks=4542 COMPLETE
GROUP restart cases=6 checks=3788 COMPLETE
TARGET_COMPLETE cases=15
```

`variants.sh` applies two content mutations in temporary copies and re-runs
the same tests. Both are rejected by a real assertion failure (exit status 4),
not by a compile error, crash or return-code difference:

* `origin.patch` (one frame less preroll at a non-zero origin) fails first on
  a stereo material:

  ```
  SEEK_PCM_DIFF material=origin/small_known_stereo target_frame=350 channel=0 actual=-3468 expected=4451
  FAIL tests/seek_origin_test.c:60 check_pcm_block (...)
  ```

* `restart.patch` (multi-sub-stream preroll reduced to the first sub-stream)
  fails first on the six-channel material:

  ```
  SEEK_PCM_DIFF material=restart/sixch_nonfirst_preroll target_frame=63 channel=2 actual=462 expected=-10641
  FAIL tests/seek_origin_test.c:86 unpack_checked (...)
  ```

Each `SEEK_PCM_DIFF` line names the material, target frame, channel, actual
value and expected value before the failing assertion. The runner reports
`VARIANT_CONTENT_REJECTED origin status=4` and
`VARIANT_CONTENT_REJECTED restart status=4`.

## Scope

Production sources and the existing baseline tests were not modified. The
only additions are `tests/seek_origin_test.c` and
`tests/seek_origin_support.h` (plus `tests/SEEK_ORIGIN_TESTS.md`). Materials
are generated inside the tests through the public encoder; nothing is
downloaded.
