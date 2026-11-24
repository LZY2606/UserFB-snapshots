# Field-time and dual-prime notes

`tests/field_prediction_cases.c` generates all of its tiny 4:2:0 MPEG-2
elementary streams in C and decodes them through the public
`mpeg2_init` / `mpeg2_buffer` / `mpeg2_parse` API with `mpeg2_accel(0)`.
The expected pixels are computed independently from decoded reference
textures; the test does not call libmpeg2's motion macros or its compressed
motion-vector parser.

## Field model

A reference is addressed as

```text
R[picture identity][parity][field row][column]
```

where parity 0 is the top field (even frame rows) and parity 1 is the bottom
field (odd frame rows).  A field plane starts at `R + parity * stride` and
successive rows inside that field are two frame rows apart, so MC uses a
doubled line stride.  This is why field selection, parity offset and doubled
stride have to be explained together; changing only one can still land on a
different but accidentally similar DC block.

Half samples use MPEG-2's rounding:

```text
avg2(a,b)  = (a + b + 1) >> 1
avg4(...)  = (a + b + c + d + 2) >> 2
```

For the frame-picture dual-prime derivation, for destination field `p`

```text
m       = p == top ? (top_field_first ? 1 : 3)
                   : (top_field_first ? 3 : 1)
other_x = floor2(m * mx + (mx > 0)) + dm_x
other_y = floor2(m * my + (my > 0)) + dm_y + (p == top ? -1 : +1)
```

`floor2(z)` is explicit floor division by two.  In particular, negative odd
values round away from zero here: `floor2(-3) = -2` and `floor2(-9) = -5`,
matching an arithmetic right shift.  The main vector samples the same-parity
field; `other_*` samples the opposite-parity field with the `| 1` / `& ~1`
row adjustment.  Each route is half-pixel sampled first; only then are the
two predictions averaged.  This is not a B-picture forward/backward average.

For field pictures, `field_select = 0/1` selects top/bottom parity through
`motion->ref2[]`.  For a second non-B P field, the opposite-parity slot is
repointed from the previous frame to `current_fbuf`, i.e. the already
reconstructed first field of the current field pair.  The same-parity slot
still points into the committed previous reference.

## Hand-computable double-field example

The DC texture used by the tests has a top/left luma value `T = 116` and the
corresponding bottom/left value `B = 132`.

For a top field using frame/field dual-prime main vector `(1,1)`, zero
dmvector and top-first coefficient `m=1`:

```text
other_x = floor2(1*1 + 1) + 0 = 1
other_y = floor2(1*1 + 1) + 0 - 1 = 0
```

The main same-parity half sample stays inside the constant DC block and is
`116`.  The derived route starts on the opposite/bottom field and is `132`.
Therefore the first reconstructed top sample is

```text
avg2(116, 132) = (116 + 132 + 1) >> 1 = 124.
```

Now decode the second, bottom P field.  Its main route still references the
committed bottom field and yields `132`.  Its opposite/top route uses the
just reconstructed top field, whose corresponding value is `124`:

```text
normal expectation = avg2(132, 124)
                   = (132 + 124 + 1) >> 1
                   = 128.
```

If the production branch that repoints the opposite slot to the current first
field is disabled, that route instead reads the old top value `116`:

```text
degenerated value = avg2(132, 116) = 124.
```

The mutation helper reports exactly this picture/field identity and the first
coordinate `x=0, frame y=1`, rather than failing to compile, crashing, timing
out, or merely changing a vector field.

## Coverage relative to the old baseline

`support/ipb.m2v` and `support/baseline.c` are useful scalar/SIMD video
baseline coverage, but its six fixtures are ordinary frame pictures.  It does
not specifically exercise:

- TOP/BOTTOM field buffer offsets and doubled field strides in
  `mpeg2_init_fbuf()`;
- `f_motion.ref2[]` field-select mapping;
- the `second_field && coding_type != B_TYPE` replacement of the opposite
  reference with `current_fbuf`;
- `motion_fr_dmv()` frame dual-prime coefficient selection by
  `top_field_first`, dmvector derivation and `MOTION_DMV` averaging;
- `motion_fi_field()` field prediction;
- `motion_fi_16x8()` and its independent `pmv[0]` / `pmv[1]` predictors;
- predictor continuation between MBs and reset in `slice_init()`;
- `motion_fi_dmv()` with `dmv_offset = -1/+1`;
- field arrival order versus interlaced display order;
- byte-for-byte equivalence of whole, one-byte and irregular chunk feeds.

The new cases stay in canonical 4:2:0 streams whose motion footprints remain
inside the coded reference area.  They are content/field-time regressions,
not another scalar-versus-SIMD or IDCT/quantization suite.
