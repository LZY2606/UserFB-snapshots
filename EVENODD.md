# Even-odd fill mode (`SFT_EVEN_ODD`)

## Interface

`schrift.h` defines a new render flag:

```c
#define SFT_EVEN_ODD 0x02
```

It is OR-ed into `SFT.flags` and may be combined with the existing
`SFT_DOWNWARD_Y` (0x01). No public structure changes layout, no function
signature changes, and the flag only affects `sft_render`. With the flag
cleared, output is byte-for-byte identical to the legacy rasterizer.

## Semantics

When `SFT_EVEN_ODD` is set, all closed contours of a glyph are treated as one
planar curve set and filled with the even-odd (parity) rule: cast a horizontal
ray from a point and fill it exactly when the ray crosses an odd number of
edges.

- Two coincident, identically oriented contours cancel (coverage 0); three
  coincide again to full coverage.
- Every lobe of a self-intersecting contour contributes. For example the
  unit-square bowtie `(0,0),(1,1),(0,1),(1,0)` fills half of the sole
  covered pixel, which becomes 128.
- Contour orientation and the choice of start point do not matter.
- Edges or points that merely touch (zero-width or zero-height contact) add
  no area.
- Nested contours alternate fill / hole / fill, independent of winding.

Each output byte is the area of the intersection between the odd region and
the pixel square `[x,x+1] × [y,y+1]`, multiplied by 255 and rounded to the
nearest integer (half rounds up). Areas are integrated geometrically from
the transformed, curve-tessellated outline; the rasterizer does not average
winding numbers first and fold them to parity afterward, and it does not use
grid supersampling. Straight-edge material is exact up to one gray level of
rounding error.

Quadratic curves use the same device-space flatness criterion and recursion
limit as the default path. Unlike the default path, subdivision happens on
the transformed outline *before* any clipping, so control points outside the
output window keep their true positions; only the final area integration is
restricted to the window.

## Coordinate and window rules

Metrics (`advanceWidth`, `leftSideBearing`, `yOffset`, `minWidth`,
`minHeight`) are independent of the fill rule and of `SFT_EVEN_ODD`.
Fractional `xOffset`/`yOffset`, independent positive `xScale`/`yScale`, and
both Y-axis conventions are interpreted exactly as before.

A positive-size `SFT_Image` acts as a crop window anchored at the glyph's
local origin:

- If it is smaller than the metrics rectangle, the result is the
  same-width/height sub-rectangle taken from the first row and first column
  of the full bitmap.
- If it is larger, the pixels outside the metrics rectangle are written as
  zero and the covered region equals the corresponding part of the full
  bitmap.

## Resource and failure behavior

Temporary storage is organized per contour edge and per scanned pixel row;
no per-pixel edge lists and no high-resolution proxy bitmaps are allocated.
A single output-sized grayscale buffer holds the result and is copied into
the caller's image only after the whole image has been integrated, so an
out-of-memory or decode failure returns `-1` without leaving a partially
written destination. A failed render does not damage the `SFT_Font`; the
same font can be rendered again immediately.

## Example

`evenodd_pgm.c` builds a geometric TrueType font entirely in memory (no font
files are loaded) and writes the same glyph under both fill rules:

```sh
cc -std=c99 -O2 -I. evenodd_pgm.c -L. -lschrift -lm -o evenodd_pgm
./evenodd_pgm /tmp/pgm-out
# -> /tmp/pgm-out/winding.pgm and /tmp/pgm-out/evenodd.pgm (binary PGM, P5)
```

The glyph contains a self-intersecting bowtie and two exactly coincident
squares, which is enough to make the two fill rules visibly different.
