# Independent fill regression

This directory contains a self-contained regression suite for PlutoVG
solid fills, fill rules and clipping. It only drives the product through its
public `plutovg_canvas` / `plutovg_path` / `plutovg_surface` entry points;
no product source under `source/` is modified or included by the reference.

## Files

- `fill_reference.c` - named scenes, product drawing, per-pixel assertions,
  SRC_OVER byte check and fill/fill_preserve/fill_path consumption checks.
- `ref_geometry.h`, `ref_geometry.c` - the independent analytic reference.
  It stores its own line segments and cubic control points as doubles,
  solves horizontal-ray crossings directly (cubics are split at their
  y-extrema and bisected on each monotone piece), accumulates directed
  winding and transverse parity, and integrates fill intervals over pixel
  area. It never calls the product flatten/rasterize/contains/extents code.
- `ppm_image.[ch]` - dependency-free binary PPM writer for the side-by-side
  reference / product / amplified-difference diagnostic.
- `check_fill.sh` - builds the real library in a temp directory and runs the
  suite. `--mutations` builds three isolated broken source copies and
  requires numeric assertion failures from the same suite.

## How the curve error band is derived

`rshape_curve_band()` reproduces, independently in the test, the product's
fixed-point pipeline: `roundf(x*64)` 26.6 grid quantization of every on/off
curve point, the FreeType smooth rasterizer's cubic stop rule
(`gray_render_cubic` perpendicular-distance and dot-product tests with the
`alpha max + beta min` hypot), and integer midpoint subdivision to the
required depth. It then measures the true double-precision Bezier against
that chord polygon in the L-infinity norm and adds the 1/128 px grid
rounding bound. The result is asserted to stay within 1/4 px per scene and is
never adjusted from observed pixels.

Coverage bounds erode/dilate the true region by that L-infinity band; the
product byte must lie inside them plus a 2-byte rounding margin. Clip layers
propagate these intervals through the product's exact `a*b/255` convention
using monotone multiplication rather than treating an ideal-area product as
a single exact byte.

## Run

```sh
bash regress/check_fill.sh
bash regress/check_fill.sh --mutations
```

The original `devcheck/baseline.c` is rebuilt and must keep passing.
