# Poisson seamless cloning (`plugins/poisson_clone.hpp`)

Gradient-domain compositing for CImg. Instead of pasting source pixels through
a mask with `draw_image()` (which keeps a visible brightness seam along the
mask boundary), `poisson_clone()` reconstructs the selected pixels so that the
*gradients* of the source are transplanted while the values on the selection
boundary stay attached to the destination.

## Interface

```cpp
#include "plugins/poisson_clone.hpp"   // includes CImg.h

cimg_library::PoissonCloneResult r = cimg_library::poisson_clone(
    dst, src, mask, offset_x, offset_y, mixed, tolerance, max_iterations);
```

- `dst`, `src`: 2-D (`depth == 1`) `CImg<double>` with the same, non-zero
  number of channels. Pixel values are treated as linear floating-point
  intensities: no gamma conversion, no clamping, no rounding.
- `mask`: single-channel `CImg<unsigned char>` with the same width/height as
  `src`. Non-zero pixels form the selection.
- `offset_x`, `offset_y`: translation of the source selection onto the
  destination. Selected pixels landing outside `dst` are clipped and do not
  take part in the equations. Mask holes count as boundary.
- `mixed`: guidance-field choice per edge (directed pair of 4-neighbors):
  - `false` (normal): the guidance is `src[p] - src[q]`; if either endpoint
    maps outside the source image, the target difference `dst[p] - dst[q]`
    of the *original* `dst` is used instead.
  - `true` (mixed): for each edge the source-difference vector and the
    target-difference vector (over *all* channels) are compared by Euclidean
    norm and the larger one is used for every channel; ties resolve to the
    source. The target difference always comes from the input `dst`, never
    from the updated result.
- `tolerance`: relative-residual target, must lie in `(0,1)`.
- `max_iterations`: solver iteration cap, must be non-zero.

`PoissonCloneResult` contains:

- `image`: composited `CImg<double>` with the same dimensions as `dst`.
  Unselected pixels are copied through unchanged; the inputs are never
  modified. If the selection does not intersect `dst`, `image` is a plain
  copy of `dst` and both counters are zero.
- `iterations`: maximum number of solver rounds actually executed over the
  channels.
- `relative_residual`: 2-norm of the residuals of all free equations (after
  elimination of fixed and pinned constraints), recomputed from the returned
  image, divided by `max(1, 2-norm of the right-hand side)`.

## Equations

Unknowns are the selected pixels that land inside `dst`. Each unknown is
linked to its in-destination 4-neighbors; for every edge `(p,q)` the
equations impose `image[p] - image[q] = guidance(p,q)` summed over the
neighbors, with out-of-selection neighbors contributing their fixed `dst` value.
Connected components without any fixed neighbor (e.g. a fully selected
destination) have no Dirichlet boundary: the component's first pixel in
destination row order is then pinned to its original `dst` value, and no
other pinning is applied. All channels share the selection numbering and the
sparse (CSR) adjacency; memory grows linearly with the number of selected
pixels and no dense pixel-count-squared matrix is built. The system is solved
per channel with Jacobi-preconditioned conjugate gradients.

## Failure behavior

- `CImgArgumentException` is thrown when:
  - any input value is non-finite (NaN/inf in `dst` or `src`);
  - dimensions are illegal: non-2-D images, zero size, channel-count
    mismatch or zero channels, or a mask that is not single-channel with the
    source width/height;
  - `tolerance` is not in the open interval `(0,1)`;
  - `max_iterations` is zero.
- `CImgInstanceException` is thrown when the iteration cap is reached before
  the tolerance is met.

## Tests and demo

`task_tests/target.cpp` registers the groups `equations`, `mixed`,
`components` and `scale` through `register_task_tests(Registry&)`; run them
with `bash task_support/target.sh` (which also re-runs the baseline). The
`scale` group writes `task_tests/poisson_clone_demo.ppm`, a small rendering
comparing a seamless clone against a plain masked paste.
