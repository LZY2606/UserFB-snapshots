# Distance-field regression checks

These checks separate **bad distance values in the texture** from
**reconstruction errors that eat sharp corners**, without any font files,
extensions or GPU backends. Only `core` is compiled.

## Running

```sh
sh tests/check-distance-field.sh
```

The script compiles the core sources together with `distance-field-test.cpp`,
runs every check against the unmodified library, then builds three temporary
mutated copies of `core` and confirms each one is rejected. Exit status is
non-zero on any unexpected result. Override the compiler with e.g.
`CXX=g++ sh tests/check-distance-field.sh`.

Reconstruction images are written (P5 binary PGM) to
`tests/distance-field-checks/pgm/`:

- `gen.arrow.pgm` — asymmetric shape with four distinct sharp corners,
  reconstructed from an MSDF (corners survive the median reconstruction);
- `gen.hole.pgm` — outer contour with a reversed inner contour (a hole).

## What is verified

### Generation (`generateSDF`, `generateMSDF`, `generateMTSDF`)

Shapes are built directly from `Shape` / `Contour` / `EdgeHolder`:

1. `gen.arrow` — asymmetric quadrilateral with sharp corners;
2. `gen.hole` — outer contour plus a reversed inner contour (a hole);
3. `gen.curve` — a closed contour where a quadratic and a cubic Bézier meet.

The reference is fully independent of the production scanline, distance
selectors and rasterization code:

- straight edges use exact point-to-segment distance;
- curves are flattened by adaptive de Casteljau half-splitting. A degree-n
  Bézier stays within `(n-1)/n` of the longest edge of its control polygon,
  so each emitted chord has a guaranteed curve-to-polyline bound; subdivision
  continues until that bound is <= 2e-4 shape units, which is the printed
  `ref bound` (0 for purely linear shapes);
- inside/outside is an independent ray-cast **winding number**, summed over
  contours so the hole has winding 0;
- numeric comparisons only use samples at least 0.60 shape units from the
  boundary and within the mapped distance window, so the subdivision bound is
  always far smaller than the quantities being compared.

Checks performed:

- `generateSDF`: texture value must equal `0.5 + d / rangeWidth` (inside is
  negative) within `2 * refBound / rangeWidth + 4e-6`; sign must match the
  winding. The field is compared **unclamped** — the generator does not clamp
  values to [0,1];
- `generateMSDF` / `generateMTSDF`: each RGB channel is allowed to be a
  pseudo-distance, but `median(R, G, B) < 0.5` must agree with the winding;
- `generateMTSDF` alpha: checked against the **true** signed distance exactly
  like the SDF, so an RGB pseudo-distance leaking into alpha is caught.

### Reconstruction (`renderSDF`)

Hand-built 2x2 and 3x2 float textures are used; the reference bilinear weights
are computed per output-pixel center from scratch (channels interpolated
first, median second), then either hard-thresholded or converted with the
documented ranged coverage formula, including the public range rescaling
`range * (outW + outH) / (sdfW + sdfH)` and edge clamping. Distance-field
values are never clamped to [0,1] before the numeric assertions.

Coverage:

- 2x2 and 3x2 textures, one-/three-/four-channel overloads;
- an RGB texture deliberately chosen so that
  `median(interpolated RGB) != interpolate(corner medians)` — an
  implementation that performs median before interpolation fails on it;
- a vertically asymmetric texture checked under both `Y_UPWARD` and
  `Y_DOWNWARD` (via a negative row stride);
- downscaling, non-integer upscaling, non-square output and a non-default
  `sdThreshold` (0.35).

## Injected regressions

In temporary build copies (deleted afterwards) the script applies, one at a
time, and requires a concrete failing sample for each:

| Mutation | File | What breaks |
| --- | --- | --- |
| RGB median replaced by arithmetic mean | `core/render-sdf.cpp` | anti-order MSDF reconstruction |
| pixel-center `+0.5` half-pixel offset removed | `core/render-sdf.cpp` | every resampled output position |
| sampled Y position interpreted backwards | `core/bitmap-interpolation.hpp` | Y-axis orientation / row mapping |

The tolerances are derived from the printed reference bound plus float32
rounding (observed errors are around 1e-7 versus tolerances around 1e-4), so
they cannot be loosened enough to hide these regressions while still passing.
