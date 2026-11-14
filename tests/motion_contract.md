# Motion compensation pixel contract (`tests/motion_contract.c`)

Independent, pixel-level regression for the MPEG-1 motion compensation path.
It is C99 + libc + libm only, has no `main`, registers its cases through
`task_support/check.h`, and is compiled by the fixed driver:

```
bash task_support/run.sh target
```

All material is generated deterministically inside the test: no video is
downloaded and no external tool is called. The production decoder and the
`task_support` driver are not modified.

## Two halves that never share code

- **Bitstream writer** — a small MSB-first writer that emits a legal MPEG-1
  elementary stream (sequence header, GOP, picture/slice headers, macroblock
  syntax, sequence end). Its motion / DCT-size / macroblock codewords were
  derived by walking the VLC tree independently; it does not copy any decoder
  table, motion-vector decoder, or block macro.
- **Reference model** — keeps its own per-picture planes keyed by picture
  identity and renders the explicitly listed macroblock types and *final*
  half-pel displacements. Expected pixels are computed from the MPEG-1
  rounding rules, never from a hash of library output.

The reference model applies the required rounding order:

1. Luma half-pel: round once over the two/four-point sum
   (`(a+b+1)>>1`, `(a+b+c+d+2)>>2`).
2. Bidirectional: average the two *already rounded* predictions with a second
   rounding (`(fwd+back+1)>>1`).
3. Chroma: truncate the luma half-pel component toward zero first
   (`/2`, so `-3/2 == -1`), then run chroma's own half-pel sampling.

Every decoded frame is snapshotted (`memcpy`, because the decoder reuses its
frame buffers) and compared byte-for-byte on Y, Cb and Cr, including its 25 fps
timestamp.

### Fixed-memory note

`plm_buffer_create_with_memory` has no load callback, so the decoder's EOF
flush branch never runs. To release the last reference picture the writer
appends, after the real pictures, a fully decodable neutral intra *sentinel*
frame followed by a bare picture start code and the sequence end code. The
sentinel is valid but is never returned (the bare start code ends the loop
first), so it takes part in no assertion. Output order follows the decoder's
reference-delay rule: B pictures pass through immediately, I/P references are
released when the next reference is decoded.

## Groups

### `stream` — legal stream plumbing and identity
- `coding_and_display_order`: I,P,B,B coding order versus the different
  emission/display order; every frame and timestamp is checked.
- `reference_planes_distinct`: the two reference pictures differ on Y, Cb and
  Cr, and Cb differs from Cr, so reference rotation or a swapped chroma plane
  cannot pass silently.

### `prediction` — half-pel and bidirectional rounding
- `luma_halfpel_and_chroma_trunc`: forward vectors cover horizontal, vertical
  and 2-D half-pel with positive and negative odd components.
- `bidirectional_rounding`: forward-only, backward-only and bidirectional
  macroblocks; it also asserts that an odd-sum bidirectional pixel exists so
  the second rounding is genuinely exercised.

### `state` — vector state carried into pixels
- `fcode_residual_accumulation`: f_code = 2 residual bits, differential
  accumulation and sign changes.
- `modular_vector_wrap`: positive and negative modular range wrap (64 px wide).
- `fullpel_conversion`: full-pel vectors shifted to half-pel internally.
- `skipped_mbs_and_slice_reset`: P skipped macroblocks reset the vector to
  zero, B skipped macroblocks inherit the previous direction/vectors, and a
  new slice resets all predictors.

All vectors obey per-edge sign rules so every sampling footprint stays inside
the reference image; corrupt-stream out-of-range behaviour is intentionally
not part of this contract.

## Hand-worked samples

- **Luma half-pel.** P3 mb(0,0), vector `(3,0)`. At output x6 the footprint
  straddles the 8 px DC-block edge: `(I0(7,0)=20 + I0(8,0)=24 + 1) >> 1 = 22`
  (`plane[6] == 22`).
- **Chroma truncate toward zero.** Luma `(-3,3)` becomes chroma `(-1,1)`. At
  Cb output (16,0) the 2-D footprint straddles the 108/116 column edge:
  `(108+116+108+116+2) >> 2 = 112`.
- **f_code = 2 residual.** P mb(1,0) final `(9,0)`: delta 4 -> m_code 2,
  residual 1 -> `((2-1)<<1)+1+1 = 4`; predictor 5 + 4 = 9.
- **Bidirectional.** The two directional predictions are rounded separately
  and only then averaged with `+1`; the test asserts the exact averaged byte.

## The two weak points (regression targets)

Both are compile-clean changes; the contract must fail on content, not on a
build error or crash.

1. **Chroma truncate-toward-zero division.** Replacing the four chroma
   `motion / 2` sites with arithmetic shift (floor for negatives) changes only
   Cb/Cr on negative odd components. Measured: Y mismatches 0, Cb/Cr > 0.
2. **Bidirectional final `+1` rounding.** Removing the outer `+1` from the
   four interpolate cases changes every odd-sum averaged pixel. Measured:
   ~195 luma mismatches (plus the corresponding few chroma pixels).

On the unmodified decoder every case passes with zero per-plane mismatches;
under either mutation the run exits non-zero with a `planes_match` failure.
