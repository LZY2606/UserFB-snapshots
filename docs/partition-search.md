# Partition motion search (optional)

`TinyH264Encoder`/`SoftwareEncoder` can optionally code each P macroblock
as one of the four P partition shapes instead of always `P_16x16`:

| `mb_type` | shape | sub-partitions |
|-----------|-------|----------------|
| 0 `P_L0_16x16` | one 16×16 | – |
| 1 `P_L0_L0_16x8` | two 16×8 (top, bottom) | – |
| 2 `P_L0_L0_8x16` | two 8×16 (left, right) | – |
| 3 `P_8x8` | four 8×8 quadrants | each quadrant: 8×8, 8×4, 4×8 or 4×4 |

Enable it per encoder:

```cpp
TinyH264Encoder<> encoder;
encoder.setSize(width, height);
encoder.setQp(26);
encoder.setPartitionSearch(true);   // default: false
```

With it **off** (the default) the bitstream is byte-for-byte identical to
before this feature existed, and `setMotionSearchAlgorithm()`'s
Fast/Exhaustive choice still governs the 16×16 search exactly as before.
Enabling it does not reset the reference picture, so it can be toggled
mid-sequence; it also applies to the `encodeFrameRgb888()`/
`encodeFrameRgb666()`/`encodeFrameRgb565()`/`encodeFrameYuv422()`
overloads (they funnel through the same planar-YUV P-frame path). The
hardware backend (`HwEncoderP4`) rejects `setPartitionSearch(true)` and
returns `false`, since it has no shape-search control.

Scope is unchanged from the existing P encoder: single reference frame,
progressive 8-bit YUV 4:2:0, CAVLC, integer-pel motion search only (no
sub-pixel refinement, no multi-reference, no B pictures), and the same
±`setMotionSearchRange()` window and size ceiling.

## Search rules

The search is exhaustive and deterministic. Every candidate partition
position is scored as

```
cost = raw luma SAD + L * (syntax bits)
```

where `L = 1 + floor(current picture QP / 6)` and the syntax bits are the
signed Exp-Golomb (`se(v)`) lengths of both `mvd_l0` components, plus the
`mb_type` code length and, for a `P_8x8` region, its `sub_mb_type` code
length. (Only `mb_type`, `sub_mb_type` and `mvd_l0` are counted.)

- **MVD per candidate.** A candidate's MVD is its displacement relative to
  its *own* position in the current picture, minus the H.264 MV predictor
  for that partition (clause 8.4.1.3, the same `predictMvGeneral()` the
  decoder uses, including the 16×8/8×16 one-sided shortcuts). Because the
  predictor depends on the partitions already selected earlier in this
  macroblock, the cost of a later partition is computed only after the
  earlier ones are fixed.
- **Tie-breaks.** Within one partition (fixed predictor) candidates are
  ranked by cost, then by raw SAD, then by `|dx|+|dy|`, then by `mvY`, then
  by `mvX`.
- **Candidate order / greedy region choice.** `16x8` runs top then bottom,
  `8x16` left then right, `P_8x8` runs its four quadrants top-left,
  top-right, bottom-left, bottom-right, and within a quadrant the
  sub-partitions follow the H.264 syntax order (8×4: top then bottom;
  4×8: left then right; 4×4: raster). A `P_8x8` quadrant's *shape* is
  chosen greedily: each candidate `sub_mb_type` is tried starting from the
  state left by the already-selected quadrants, so only previously chosen
  partitions' MVs influence the predictor.
- **Shape comparison.** A shape's total is the sum of its partitions'
  costs plus its own codeword bits (`mb_type`, plus the four
  `sub_mb_type`s for `P_8x8`). Shapes are compared by this total; a tie
  favours the smaller `mb_type`, and within a `P_8x8` region the smaller
  `sub_mb_type`. Every big shape (16×16, 16×8, 8×16, 8×8) is evaluated
  starting from the same state (the macroblock's own prior state), and a
  rejected shape's trial MVs never leak into a later candidate or into a
  neighbouring macroblock.
- **Edge handling.** All reference fetches use the same edge-replicated
  (`clampedSample()`) fetch as motion compensation, so search cost and
  reconstruction agree at picture borders.

## Macroblock coding

Once a shape wins, the whole macroblock is coded as one unit: a single
`mb_skip_run`/`mb_type`/`sub_mb_type` list/`mvd_l0` list, one
`coded_block_pattern` and one `mb_qp_delta`, followed by the whole-MB
luma+chroma residual (CAVLC). The partitions share the macroblock's
CAVLC `nC` and QP state - they are not coded as independent
macroblocks. The selected partition MVs populate the per-4×4 MV/refIdx
state (so later macroblocks' predictors and the deblocking filter see
them) and the reconstructed prediction plane is driven by the selected
partitions.

The existing P-slice Intra fallback is retained: it compares the winning
partitions' total raw SAD against the Intra candidate, and if Intra wins,
the Intra macroblock overwrites the prediction plane and is written
instead - the partition writer re-runs motion compensation for every
selected partition, so a losing trial prediction can never survive into
the final reconstruction.

`P_Skip` is used only when *every* selected partition equals the derived
skip MV (not just the first) *and* the whole-macroblock residual at that
MV quantizes to zero.

## Decoder

The decoder saves and processes the full legal P_8x8 sub-partition list
(each macroblock expands to at most 16 partitions). This was previously
treated as at most 4 and could overflow; the I/P decode behaviour for
streams that don't use P_8x8 is unchanged.

See `test/native/test_partition_search_task.cpp` for the independent
search/syntax/reference verification, and `examples/PartitionSearch` for
a minimal call example.
