# Motion compensation pixel contract

`tests/motion_contract.c` is an independent pixel-level regression for the
MPEG-1 motion compensation path in `pl_mpeg.h`. It does **not** change the
decoder and adds no SIMD backend.

Run the fixed driver exactly as the task does:

```sh
bash task_support/run.sh target
```

It compiles `task_support/driver.c tests/motion_contract.c` with C99 + the
standard library + `-lm`, runs the registry in `stream` / `prediction` /
`state` order, and then runs the original solid-colour baseline. The file
contains no `main()`; cases are registered through `task_cases` and
`task_case_count` from `task_support/check.h`.

## How the test is built

Everything is deterministic and in memory; there are no downloaded clips and
no ffmpeg.

- Fixed geometry: **48×48 luma, 24×24 Cb/Cr (4:2:0)**, i.e. 3×3 macroblocks.
  Interior macroblocks leave a margin so every declared vector's full sampling
  footprint (including odd neighbours) stays inside the reference planes.
- A small MSB-first bit writer emits a legal elementary stream: sequence
  header (48×48, 24 fps), picture headers, slice start codes, macroblock
  address increments / types / motion VLCs, and intra DC blocks. Standard
  ISO/IEC 11172 codewords are encoded in the constructor; the expectations do
  **not** reuse the library's VLC traversal tables, its differential motion
  decoder, or `PLM_BLOCK_SET`.
- Intra references are DC-only 8×8 blocks with deliberately uneven values
  (`mc_blocks`), so no IDCT/AC regression is mixed in.
- The independent reference model keeps planes **by picture identity**
  (`MC_ID_A`, `MC_ID_C`, `MC_ID_P2`, `MC_ID_P4`, …). Every macroblock states
  its explicit type and **final** displacement (`fh/fv/bh/bv`, in half-luma
  units). The model samples the named identity directly and never simulates
  pointer rotation.
- `mc_run_clip()` decodes through the public
  `plm_buffer_create_with_memory` → `plm_video_create_with_buffer` →
  `plm_video_decode` API and compares the complete padded Y/Cb/Cr planes byte
  for byte. Each returned frame is compared immediately, before the next
  decode can reuse its buffers. Frame timestamps are checked against
  `display_index / 24`.

A decodable all-intra trailer plus a bare picture start code is appended to
every clip. The trailer flushes the last checked reference through pl_mpeg's
normal delayed-reference output, so no EOF/flush path is required.

## Groups and scenarios

### `stream`

| Case | Construction | Checks |
| --- | --- | --- |
| `stream_intra_uneven_dc` | one I picture from a rich DC grid | entry decode, all planes |
| `stream_p_zero_copy` | I=A then zero-MV P | reference copy plumbing |
| `stream_ipb_display_order` | coding order `I0 A, P2, B1, P4, B3`; display order `I0, B1, P2, B3, P4` | all five frames, timestamps, both refs kept and reused (`B1` uses A+P2, `P4`/`B3` keep referencing P2/P4) |

### `prediction`

All vectors below are final half-luma displacements from the named identity.

- `pred_luma_half_pel_axes` — nine forward MBs from A:
  `(0,0) (±1,0) (0,+3) (+3,+3) (−3,−3) (0,−1) (+5,−3) (−5,−5)`.
  Covers horizontal-only, vertical-only and 2-D half-pel and positive/
  negative odd components.
- `pred_chroma_trunc_toward_zero` — forward MBs `(+2,0) (0,−2)` for chroma's
  own half-pel and `(−1,0) (−3,0) (0,−1) (−1,−1)` for the truncation rule,
  over a high-contrast chroma DC grid.
- `pred_bidir_final_rounding` — two distinct all-intra refs A/C with forward-
  only, backward-only and bidirectional MBs, including zero-odd, horizontal-
  only, vertical-only and 2-D odd bidirectional forms.

### `state`

These encode the differential symbols explicitly (`auto_deltas = 0`) while
the reference still uses the hand-computed finals.

- `state_differential_accumulation`: deltas `+3`, then `(+2,+2)` ⇒ finals
  `3`, `(5,2)`.
- `state_fcode_residual_bits`: `f_code=2` (`r_size=1`); `+5` is m_code 3 with
  residual 0, `+2` is m_code 1 with residual 1; the next MB adds `+2,−1` ⇒
  final `(7,1)`.
- `state_full_pel_scaling`: `full_pel=1`, wire codes `(2,1)` ⇒ effective
  final `(4,2)`.
- `state_differential_wrap`: positive `14 + 3 = 17 > 15 ⇒ −15`; after an
  intra reset, negative `-14, -2 ⇒ -16` (inclusive bound), then `-1` ⇒
  `-17 < -16 ⇒ +15`.
- `state_p_skipped_clear_vectors`: MB0 has `+5`; address increment 3 creates
  two consecutive skipped P MBs which must be colocated (gap clears the
  vector); the next coded delta `+2` starts from 0.
- `state_b_skipped_inherit`: MB0 is bidirectional `F(3,1)/B(2,1)`; increment
  2 skips MB1 which must repeat that direction and those vectors; MB2's
  `(−3,−1)` differential is relative to the inherited `(3,1)` and lands on
  `(0,0)`.
- `state_slice_start_reset`: three slices; slice 1 ends at `−4`, slice 2
  starts with `+5` and must yield final `+5`, not `−4 + 5 = 1`.

## Rounding contract and hand samples

For a half-pel component `m`, the integer base is `floor(m/2)` and the odd
fraction is `m − 2·floor(m/2)`, which is 0 or 1 for negative odd values too
(e.g. `m=−1` gives base `−1`, odd 1).

Luma (and each single-reference predictor) rounds **once**:

- two taps: `(a + b + 1) >> 1`
- four taps: `(a + b + c + d + 2) >> 2`

Bidirectional prediction then averages the **two already-rounded
predictors** with a second rounding: `(Pf + Pb + 1) >> 1`.

Chroma first derives its motion in half-chroma units with division
**truncated toward zero**: `c = trunc_zero(m/2)` (C99 `m / 2`), and only then
applies chroma's own two/four-tap sampling.

Hand example — chroma negative odd (MB4, Cb block at plane index 200).
Neighbouring DC blocks are left `220`, centre `40`, luma `mh = −1`:

- correct: `c = trunc_zero(−1/2) = 0` ⇒ the block is the flat centre `40`;
- negative floor: `floor(−1/2) = −1` ⇒ first chroma column averages the left
  and centre blocks: `(220 + 40 + 1) >> 1 = 130`.

The mutant therefore produces `Cb[200] = 130` against expected `40`.

Hand example — final bidirectional average (centre MB4, zero MVs). A's four
luma blocks are DC `10`, C's are DC `11`:

- correct: `(10 + 11 + 1) >> 1 = 11`;
- dropping the last `+1`: `(10 + 11) >> 1 = 10`.

The mutant therefore produces `Y[784] = 10` against expected `11`.

## The two guarded weak points

Both are known-compilable degeneracies; the suite must fail a **content**
assertion (not the build, a crash, or a relaxed tolerance) on either.

1. **Chroma floor division for negatives.** Replacing the chroma
   `motion_h / 2, motion_v / 2` derivations with an arithmetic shift/floor
   makes `pred_chroma_trunc_toward_zero` and `pred_luma_half_pel_axes` fail
   Cb/Cr byte comparisons. Applying the change only to the bidirectional
   interpolate call site is caught by `pred_bidir_final_rounding`.
2. **Missing `+1` in the final bidirectional average.** Removing the outer
   `+1` from any of the four final-average forms (zero/horizontal/vertical/2-D
   odd) is caught by `pred_bidir_final_rounding`; the zero-odd centre MB is
   the stable `10` vs `11` sentinel.

Verified locally by mutating `pl_mpeg.h`, running the fixed driver (non-zero
exit with a `..._count == 0` pixel assertion), and reverting.

## Out of scope

EOF tail flushing, reverse playback/seeking and corrupt-stream recovery are
intentionally not expanded here; vectors are kept in-bounds so those paths are
not exercised by these cases.
