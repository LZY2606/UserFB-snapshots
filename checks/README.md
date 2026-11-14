# Perspective texture checks

These checks cover the perspective-correct texture path.  Run them from the
repository root:

```sh
bash checks/target.sh
```

`target.sh` compiles the C core, builds and runs `checks/target_tests.cpp`
(five groups: `orthographic`, `perspective`, `clipped`, `states`,
`compatibility`, all registered through `checks/suite.h`), and then runs the
original reproduction in `checks/baseline.sh`.  No SDL, OpenMP, model files
or texture downloads are needed.  The RGB coordinate texture and the small
screen meshes are generated in-process.

## What is checked

Every reference is computed independently of the rasterizer:

1. object vertices are projected with a local double-precision 4x4 matrix;
2. optional clipping is done by a local homogeneous Sutherland-Hodgman /
   recursive tessellation pass that mirrors the library's clip tree, so the
   same integer vertices are formed;
3. vertices are snapped to the existing integer screen convention;
4. coverage and barycentrics come from explicit screen-space edge functions;
5. each covered pixel is evaluated with the rational quotient
   `u = (sum lambda_i * u_i / w_i) / (sum lambda_i / w_i)`, where `w_i` is
   the homogeneous clip-space `w`;
6. the source texel is looked up with the existing fixed-point repeat/nearest
   addressing and the existing vertex-colour modulation;
7. depth is checked against the affine interpolation of integer window `z`.

Colour is only asserted away from nearest-sampling ties and away from the
`s`/`t` repeat breakpoint.  A tie (a UV within 1e-3 texel of the sampling
breakpoint) is counted but never treated as a colour error.

Groups:

* `orthographic` - the original reproducer: same xy/UV with varying depth
  versus flat depth must be pixel-identical; colour, coverage and depth
  match the reference; a flat/minuscule depth gradient and changing clip `z`
  with fixed xy/w do not move the texture.
* `perspective` - strong `w` variation, a second triangle in the same frame,
  a projection switch, the old 8-pixel scanline tails (span widths 8..15
  sweep every `width mod 8`), and a per-axis `<= 1 texel` bound against the
  independent quotient for every covered pixel of a hard triangle.
* `clipped` - side clip (one vertex outside), two-vertices-outside side clip,
  and a near-plane clip with a large `w` spread; every clipped sub-triangle
  is checked against the same rational formula.
* `states` - blend on/off pass-through equivalence, lit texture modulation,
  `TGL_NO_DRAW_COLOR` transparent texels (must discard and not write depth,
  so a later background passes through the hole), polygon stipple holes (same
  depth rule), and the plain untextured colour/depth path.
* `compatibility` - display-list replay matches direct drawing; a list
  replayed under a different projection uses the new projection; binding a
  second texture before replay uses the new texture; flat/smooth untextured
  rendering still works.

## Precision

For each covered pixel the sampled texel is the rational interpolation
rounded by the existing nearest rule.  In a broad randomized sweep (strong
perspective, `w` from ~1 to ~10, hundreds of thousands of samples) the only
disagreements with the double-precision per-pixel quotient are UVs that land
within ~1e-6 texel of a nearest-sampling tie; there are no off-tie
disagreements.  The `8 pixel` scanline chunking of the old path is gone:
the new scanline loop evaluates the rational quotient per pixel from
double-precision plane coefficients, so there is no piecewise approximation
to justify.

## Visual comparison (before/after)

`checks/perspective_ppm.c` writes a 128x64 PPM with two panels that use the
same screen geometry and UVs but different vertex depth.  Under an
orthographic projection `w` is constant, so the panels must be identical.

* `checks/ppm/perspective_before.ppm` - generated from the pre-fix code.
  The left/right panels differ on 3448 of 8192 pixels.
* `checks/ppm/perspective_after.ppm` - generated from the fixed code.
  The left/right panels differ on 0 pixels, and the two images differ on
  3565 pixels overall.

Rebuild it with:

```sh
cc -std=c99 -O2 -Iinclude checks/perspective_ppm.c libTinyGL.a -lm
./a.out checks/ppm/perspective_after.ppm
```
