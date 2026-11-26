# 3D Gaussian splat overlay (`tglRenderSplats`)

## Purpose

`src/splat.c` adds a CPU-only entry point that draws a batch of oriented,
scaled 3D Gaussian splats on top of the contents of a TinyGL `ZBuffer`.
Typical use is an offline preview of a scanned point cloud over an
already rasterised opaque triangle scene:

1. draw the opaque triangles with the normal TinyGL pipeline (this fills
   `pbuf` and `zbuf`);
2. call `tglRenderSplat()` to alpha-composite the point cloud over the
   existing colours.

The depth buffer is read for occlusion but is never written, and the
current GL matrix/viewport state is not consulted. Only the default
32 bit RGBA build (`TGL_FEATURE_RENDER_BITS == 32`) is supported.

## Data layout (`include/GL/splat.h`)

### `TGLSplat`

| field          | meaning |
| -------------- | ------- |
| `mean[3]`      | world space centre |
| `covariance[6]`| symmetric 3x3 world covariance, upper triangle packed as `xx, xy, xz, yy, yz, zz` |
| `rgb[3]`       | linear colour channels, each in `[0,1]` |
| `opacity`      | scalar opacity in `[0,1]` |

The covariance must be finite and symmetric positive definite (the three
leading principal minors must all be strictly positive). A semi-definite
or otherwise non-invertible input is rejected before any pixel is
touched.

### `TGLSplatView`

`view[16]` and `projection[16]` are 4x4 matrices in OpenGL **column
major** order: element (row r, column c) is stored at index `4*c + r`.

* `view` is the rigid world-to-camera transform, right handed, camera
  looking down `-Z`;
* `projection` is any finite perspective or orthographic projection that
  maps eye space to NDC with `z` in `[-1,1]`;
* `x, y, width, height` define the viewport in target pixels and must lie
  completely inside the destination buffer. Non zero `x,y` and partial
  edge tiles are handled.

### `TGLSplatStats`

Optional instrumentation (may be `NULL`):

* `projected_count` - points passing the centre rule;
* `tile_refs` - number of generated (point, tile) references;
* `pixel_tests` - number of Mahalanobis `q` evaluations actually
  performed.

## Projection pipeline

All math runs in double precision.

1. **Camera space.** The mean is transformed by the rigid view matrix and
   the full covariance by its rotation: `Sc = R S R^T` (the translation
   does not affect the covariance).
2. **Clip space.** `clip = P * (mc, 1)`, with `w = clip[3]`.
3. **Centre rule.** A point participates only if `w > 0` and the centre
   NDC depth `clip[2]/w` is strictly inside `(-1,1)`. Gaussians are not
   volume-clipped against the near/far planes; crossing a plane is handled
   purely through this centre rule. The centre `x/y` are *not* culled, so
   an off-screen ellipse whose tail reaches the viewport still draws.
4. **Screen centre.** TinyGL's floating point viewport convention is
   used, with `a = (width-0.5)/2`, `b = (height-0.5)/2`:
   `X = x + a*(ndc.x+1)`, `Y = y + b*(1-ndc.y)`.
5. **EWA covariance.** The 2x3 Jacobian `J` of the divide-by-w viewport
   map is derived analytically from the projection rows (the true
   projection `w` is used, never clip-z), and
   `C = J * Sc * J^T`. A low pass variance of `0.3` pixels^2 is added to
   both diagonal entries; the off-diagonal entry is preserved.
6. If any intermediate value is non finite or `C` is not invertible, the
   whole call fails before the frame buffer is modified.

## Pixel evaluation

For pixel `(px,py)` the sample point is the pixel centre
`(px+0.5, py+0.5)`. With `delta` against the splat centre and
`C^-1` the inverse screen covariance,

```
q     = delta^T C^-1 delta
alpha = min(0.99, opacity * exp(-q/2))
```

Only pixels with `q <= 9` (the 3-sigma ellipse) contribute. The ellipse
is bounded per axis by `3*sqrt(Cxx)` / `3*sqrt(Cyy)` for tile bucketing;
the exact quadratic still decides each pixel.

## Tiling and ordering

* The viewport is split into 16x16 pixel tiles (partial tiles on the
  right/bottom edges are clipped, never read out of bounds).
* Each projected splat is referenced by every tile its axis aligned
  3-sigma bounds intersect; `tile_refs` counts those pairs.
* The projected points are sorted back-to-front by camera distance
  `-mean_camera.z`, nearest first, with the original input index breaking
  distance ties. References inside each tile inherit that global order,
  so no pixel ever scans the whole point set - only splats referenced by
  its tile, and only pixels inside each splat's bounds, are visited.

## Compositing and depth

* Within a tile every pixel starts with accumulated colour `Csum = 0` and
  transmittance `T = 1`. For each splat that passes the quadratic and the
  occlusion test: `Csum += T*alpha*rgb`, then `T *= 1-alpha`. No
  quantisation happens between layers.
* Occlusion uses a centre-depth approximation. The centre NDC depth is
  mapped with TinyGL's own viewport depth formulas
  (`zsize = 2^(ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS)`, fixed point depth
  shifted by `ZB_POINT_Z_FRAC_BITS`) and compared against the pixel's
  stored `zbuf` value with the library's `>=` convention. If
  `depth_test` is disabled on the buffer, every splat passes, matching
  the triangle rasteriser. Splat centres are never written to `zbuf`.
* After a tile is processed, each touched pixel is written once:
  `final = clamp(Csum + T*original_colour, 0, 1)`, quantised with
  `floor(255*v + 0.5)`. The unused top 8 bits of the 32 bit pixel are
  preserved. Pixels outside the viewport, and pixels no splat reached, are
  left byte-for-byte unchanged.

## Approximation limits

* Each splat is a planar 2D Gaussian in the screen plane: the EWA
  projection linearises the divide-by-w mapping at the splat centre, so
  very large splats that span a strong perspective gradient differ from
  an exact curved projection.
* Depth is evaluated only at the splat centre. A splat is treated as one
  depth for all of its pixels; it is not split where it intersects opaque
  geometry, so a splat can be fully in front of or behind a surface per
  its centre even when its footprint crosses a silhouette.
* Gaussians crossing the near/far planes are accepted or rejected by the
  centre rule only; the volume is never clipped.
* The 3-sigma (`q <= 9`) cutoff and the per-axis 3-sigma tile bounds
  truncate the Gaussian tails; the `0.3` pixel^2 low pass term sets a
  minimum footprint.
* Alpha is capped at 0.99 to keep the transmittance strictly positive.
* Sorting is by centre distance only; intersecting splats are composited
  in that global order rather than interleaved per pixel.
* All accumulation is double precision; the only rounding is the final
  8 bit per channel quantisation.

## Failure semantics

Inputs are fully validated and all temporary buffers are allocated
before any frame buffer write. Invalid viewport geometry, `NULL`
arguments, non finite data, out-of-range colour/opacity, a non positive
definite covariance, non finite projection intermediates, or allocation
failure make the function return non zero with the destination
(including `zbuf`) untouched. `count == 0` is legal, leaves the output
unchanged and returns 0. Repeated calls always recomposite from the
current destination colours; nothing is cached between calls.
