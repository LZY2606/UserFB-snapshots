# CPU 3D Gaussian splat rendering (`tglRenderSplats`)

This note describes the data layout used by the splat entry point and the
approximations it makes.  The implementation lives in `src/splat.c`, the public
interface in `include/GL/splat.h`.

## 1. Data layout

### `TGLSplat` (one Gaussian)

| field          | size    | meaning                                                        |
|----------------|---------|----------------------------------------------------------------|
| `mean[3]`      | 3 doubles | world space position `(x, y, z)`                             |
| `covariance[6]`| 6 doubles | symmetric 3x3 world covariance, order `xx, xy, xz, yy, yz, zz` |
| `rgb[3]`       | 3 doubles | linear color `(r, g, b)`, every component in `[0,1]`          |
| `opacity`      | 1 double  | scalar opacity in `[0,1]`                                      |

The covariance stores only the six unique entries of a symmetric matrix;
`Cov_ij = Cov_ji`.  It must be positive (semi)definite.  All scalar inputs must
be finite (`isfinite`).  Nothing here is expanded into triangles and a point is
never treated as a fixed circular sprite: each splat keeps a full directional
covariance.

### `TGLSplatView`

| field          | meaning                                                                 |
|----------------|-------------------------------------------------------------------------|
| `view[16]`     | rigid world-to-camera transform, right handed, camera looks down `-Z`  |
| `projection[16]` | finite perspective or orthographic projection, NDC z in `[-1,1]`     |
| `x, y`         | viewport lower left corner in target pixels                            |
| `width,height` | viewport size in pixels, both at least 1                               |

Both matrices are ordinary OpenGL matrices stored in **column major** order, so
the element at column `c`, row `r` is at index `c*4 + r`.  The view matrix uses
its upper 3x3 block as the rotation and its last column as the translation.
The view is fully self contained: the current TinyGL model/projection matrix
state and GL viewport are not consulted.

The whole viewport rectangle must lie inside the destination `ZBuffer`
(`0 <= x`, `0 <= y`, `x + width <= xsize`, `y + height <= ysize`).

### `TGLSplatStats`

* `projected_count` — splats whose center passes the participation rules
  (`w > 0` and NDC `z` strictly inside `(-1,1)`).
* `tile_refs` — number of `(splat, tile)` bucket references actually built.
* `pixel_tests` — number of Mahalanobis `q` evaluations actually performed.

### Frame buffer assumptions

Only the default 32 bit RGBA build (`TGL_FEATURE_RENDER_BITS == 32`) is
supported.  The packed pixel is `0x00RRGGBB`; the high 8 bits are preserved and
never used.  The 16 bit depth buffer is read for the opaque occlusion test and
is never modified by a splat call.

## 2. Rendering pipeline

1. **Validation and allocation.**  Every input is validated and all temporary
   storage is allocated before any pixel is touched, so a rejected call leaves
   color and depth unchanged.  A `count` of zero (with possibly `NULL` points)
   is legal and changes nothing.
2. **Camera space.**  The mean becomes `m_c = R m + t` and the full covariance
   becomes `Sigma_c = R Sigma R^T`.
3. **Projection.**  The mean is multiplied by the projection matrix to clip
   coordinates and divided by `w` to NDC.  A splat participates only when its
   center has `w > 0` and NDC `z` strictly in `(-1,1)`.  Centers may lie outside
   the viewport in x/y; their tails can still paint.
4. **Screen Jacobian.**  The 2x3 Jacobian `J` of the divide-by-w screen mapping
   is taken with respect to camera position, using TinyGL's floating point
   viewport scales `a = (width - .5)/2`, `b = (height - .5)/2`.  Clip `w`, not
   clip `z`, is used for the division.
5. **Screen covariance.**  `C = J Sigma_c J^T`, a full 2x2 matrix (the
   off-diagonal term is kept), then `0.3` pixels^2 of variance is added to each
   diagonal element as a low pass filter.  `C` must stay positive definite.
6. **Pixel coverage.**  Pixel centers are at `(x+.5, y+.5)`.  With
   `q = delta^T C^-1 delta`, only pixels with `q <= 9` participate and
   `alpha = min(0.99, opacity * exp(-q/2))`.
7. **Tiling.**  The viewport is split into 16x16 pixel tiles.  Each splat
   creates references for the tiles overlapped by its axis aligned
   `3*sqrt(Cxx)` by `3*sqrt(Cyy)` bounds (clipped to the viewport).  References
   inside a tile are sorted by camera distance, ties broken by input index.
8. **Compositing.**  Splats blend front to back.  Per pixel the renderer keeps
   accumulated RGB and a transmittance `T` (starting at 1), accumulating
   `Csum += T*alpha*rgb` and `T *= 1-alpha` without intermediate quantization.
   A splat only blends where its center fixed-point depth passes the existing
   `>=` depth test.  At the end each pixel is written once as
   `Csum + T*background`, clamped to `[0,1]` and quantized with
   `floor(255*v + .5)`.

## 3. Approximations and limits

* **Center-only clipping.**  Participation is decided from the Gaussian center
  alone (`w > 0`, strict NDC z range).  A Gaussian that straddles the near or
  far plane is not split or volume-clipped.
* **Center depth.**  Every pixel of a splat compares the same center depth
  against the depth buffer; depth variation across the footprint is ignored.
  Splat centers are never written back into the depth buffer, so splats only
  occlude each other through back-to-front alpha blending, not through depth.
* **2D truncation.**  The Gaussian is cut at three standard deviations
  (`q <= 9`); the faint tail beyond it is discarded.
* **Alpha ceiling.**  Per-pixel alpha is capped at `0.99`, so a splat can never
  become fully opaque in one layer.
* **Low pass term.**  A fixed `0.3` px^2 diagonal variance limits sub-pixel
  aliasing; it does not rotate with the splat and does not affect the
  off-diagonal term.
* **Axis aligned tile bounds.**  Tiling uses axis aligned 3-sigma extents,
  which are conservative for rotated ellipses; some referenced pixels fail the
  exact `q` test.  Counts report the real references and real q tests, not
  estimates, and a pixel never scans the whole batch.
* **Fixed point depth.**  Depth is converted to TinyGL's 16 bit, 14 fractional
  bit viewport representation, inheriting that format's quantization and the
  library's `>=` comparison direction.
* **Single precision target buffer.**  All splat math is double precision, but
  the final result is quantized to 8 bits per channel; blending layers are kept
  in double precision until the single final write.
* **32 bit RGBA only.**  The entry point assumes the default 32 bit build and a
  viewport fully contained in the target; other pixel formats are not handled.
* **Resource use.**  Working memory is `O(count + viewport_pixels + tile_refs)`
  and is freed at return.  No view or input is cached between calls, so repeated
  calls always re-composite from the frame buffer's current contents.
* **Validity.**  Non-finite data, non-positive-definite covariance, out of range
  color/opacity, an out of target viewport, or allocation failure make the call
  return non zero without modifying the target.
