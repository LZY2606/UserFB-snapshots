# Adaptive codebook regression contract

`tests/codebook_contract.c` is a C regression suite that explains, sample by
sample, where audible differences in the fixed-point iLBC adaptive codebook
come from. It is registered through `task_support/check.h`
(`task_cases` / `task_case_count`), defines no `main`, and every comparison
goes through `TASK_CHECK`; each case returns 0 on success and the driver in
`task_support/driver.c` returns 4 on the first failed check.

Production algorithms, the public API and the pre-existing tests are
unchanged. The only new production-facing file is this test; the only other
change is ignoring the local `.task-build/` build directory.

## How to run

```sh
bash task_support/run.sh baseline        # existing encode/decode demo
bash task_support/run.sh target          # builds libilbc.a + the contract
bash tests/check_codebook_mutants.sh     # the two seeded degradations
```

Everything is offline and deterministic. Both scripts were run unchanged.
They use `mktemp` for logs and mutant sandboxes; on a normal Linux/macOS
shell that works directly. In this sandbox the system temporary directory is
not writable, so a workdir-local `mktemp` shim (kept only under the ignored
`.task-build/` directory, never shipped) redirected those temporary paths
into the working tree; with it on `PATH` both scripts completed:
`run.sh target` exits 0 with `TARGET_COMPLETE`, and
`check_codebook_mutants.sh` exits 0 after rejecting both variants with
exit code 4 each. No script content was modified.

## Reference model independence

The expected ("want") values are re-derived from the bitstream fields and the
published constant tables; they are never copied from production output and
no whole-segment hash is used as an oracle.

The reference deliberately does **not** call any of these production
routines to compute an expectation:

- `WebRtcIlbcfix_GetCbVec` / `WebRtcIlbcfix_CreateAugmentedVec`
- `WebRtcIlbcfix_FilteredCbVecs`
- `WebRtcIlbcfix_GainDequant`
- `WebRtcIlbcfix_CbConstruct`
- `WebRtcIlbcfix_DecodeResidual`

Those functions are invoked only as the "got" side that the independent
model is compared against. Only constant tables are read by the model:
`WebRtcIlbcfix_kAlpha`, `WebRtcIlbcfix_kCbFiltersRev`,
`WebRtcIlbcfix_kGain[3]` and, indirectly, the LSF/state tables.

Two production pieces are reused by explicit design:

1. `WebRtcIlbcfix_StateConstruct` for the scalar start-state block in the
   residual group (the task allows the original StateConstruct for that
   block). The adaptive completion around it is rebuilt by the model.
2. LSF dequantization and LSF/LSP interpolation
   (`SimpleLsfDeQ`, `LsfCheck`, `DecoderInterpolateLsp`), always on an
   independent decoder instance whose `lsfdeqold` history advances in lock
   step with the decoder under test. The adaptive residual itself still
   comes from the model.

Numeric semantics are written out instead of relying on implementation
behaviour:

- signed 16x16 products are computed in `int32_t` (they cannot reach
  2^31, so there is no product wrap);
- `sar32` performs an arithmetic right shift that rounds toward minus
  infinity for negative dividends;
- `add_wrap32` makes the two's-complement 32-bit accumulation explicit,
  matching the production `-fwrapv` build;
- `wrap16` keeps the low 16 bits and re-signs (a wrap, never a
  saturation); the only saturations modelled are the MA-filter clamps in
  `ref_ma_q12`, which mirror `WebRtcSpl_FilterMAFastQ12`;
- the MA convolution is a finite 8-tap sum with explicit zero extension
  outside the memory buffer.

## The three groups

### vectors — codebook memory mechanics (5 cases)

Bounded arrays in ordinary time order (oldest sample at index 0) describe the
codebook memory, with separate positive-mixing and all-negative fixtures.

- `short23_memory85`: 23-sample start-state vectors over 85-sample memory,
  both filtered halves, boundary and interior indexes, negative memory.
- `long40_sections`: 40-sample vectors over 147-sample memory covering all
  four `GetCbVec` sections (plain/augmented x unfiltered/filtered), plus
  augmented vectors at every legal lag 20..39 on both raw and filtered
  memory.
- `overlap_two_truncations`: the four-sample short-period overlap keeps two
  signed scalings and two int16 truncations before the add. The case proves
  the fixture contains a sample where a single merged truncation would
  differ, then checks both products and the combined value.
- `gain_chain_weighted`: the three gains recurse from the previous stage's
  already quantized magnitude with the 1638 floor; each stage is compared
  separately and the final weighted 40- and 23-sample vectors are checked.
- `filter_zero_extension`: the finite convolution at every production filter
  length (63/85/125/147 samples), including the exact zero prefix.

### residual — whole-frame residual reconstruction (6 cases)

The model organizes an entire frame using absolute sample coordinates
(0 = oldest) and its own time-ordered memory, rather than replaying the
production pointer/`memmove` loops. The reverse-time consumption order of the
(memory, cb index, gain index) triples is documented at `model_residual`:

- triple 0 builds the adaptive start-state completion;
- triples 1..Nfor build the forward subframes oldest to newest;
- the remaining triples build the backward subframes from the start state
  outward; each vector is produced in reverse time and flipped once.

Coverage includes 20 ms and 30 ms, both `state_first` layouts, and early,
late and centered start blocks; every residual sample is compared. The
centered 30 ms case (`startIdx=3`, two forward and two backward subframes)
requests 160 backward samples and asserts that the 147-sample ceiling is
actually hit.

### stream — public `Decode` end to end (6 cases)

Deterministic, non-silent PCM (a scanning narrow energy burst plus LCG
noise) produces 240 continuous legal frames per mode. Packets are
byte-swapped and unpacked through the public format path, codebook indexes
are converted with `IndexConvDec`, and each frame is decoded through the
public `WebRtcIlbcfix_Decode`. The decoder's `prevResidual` is compared to
the model every frame; output length and `speechType` are checked, reset/
reuse is verified across fresh instances, and two instances run interleaved
on disjoint subsequences with separate model histories. Lossy PCM is not
required to equal the input and no stream hash is used.

## Measured coverage counts

These are the real counts from the deterministic stream (printed by the
`coverage_counts` case as `COVER` lines), not frame-count assumptions. Cell
entries are `state_first=0 / state_first=1` per `startIdx`.

20 ms, 240 frames:

- cells: start1 21/66, start2 47/63, start3 41/2 — all six cells non-empty;
- 40-sample subframe codebook sections (plain/augmented x unfiltered/
  filtered): 1449 / 262 / 351 / 98 — all four visited;
- start-state short-vector sections: 393 / 0 / 327 / 0 — only the legal
  plain and filtered halves are ever used;
- indexes changed by `IndexConvDec`: 301;
- frames with both a forward and a backward subframe: 110;
- frames whose raw backward memory exceeds 147 samples: 87.

30 ms, 240 frames:

- cells: start1 14/30, start2 12/28, start3 37/32, start4 40/2,
  start5 42/3 — all ten cells non-empty;
- subframe sections: 2209 / 426 / 790 / 175 — all four visited;
- start-state sections: 410 / 0 / 310 / 0;
- indexes changed by `IndexConvDec`: 310;
- centered frames: 151;
- frames whose raw backward memory exceeds 147 samples: 153, and the
  continuous stream run independently observes the clipped-memory path.

Current check totals: vectors 4429 checks / 5 cases, residual 3512 / 6,
stream 198110 / 6; 17 cases, `TARGET_COMPLETE`, driver exit 0.

## The two seeded degradations and their first differences

`tests/check_codebook_mutants.sh` builds two patched copies in isolated
sandboxes. Both must compile, link, reach the content comparison, print
`CONTENT_MISMATCH`, fail the `TASK_CHECK` and make the runner return 4;
build failures or crashes do not count.

- `tests/mutants/merged.patch` rewrites the four-point overlap as one merged
  product with a single truncation. First difference observed:

  `CONTENT_MISMATCH group=vectors case=long40_sections
  what=long_getcb_idx108 idx=17 got=243 want=242`

  The first augmented index (lag 20) already exposes a one-LSB rounding
  change inside the overlap; the unmutated two-truncation value is the
  reference.

- `tests/mutants/forward.patch` copies the backward codebook memory in
  forward order instead of reversed order. First difference observed:

  `CONTENT_MISMATCH group=residual case=20ms_start2_first0
  what=frame_residual idx=0 got=79 want=-3`

  The very first sample of the backward-predicted region is wrong because
  the reverse-time memory layout is corrupted.

Both mutants currently end with exit code 4 and the `MUTANT_CONTENT_REJECTED`
marker produced by the script; the unmutated build stays at exit 0.
