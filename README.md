# HalfedgeCatmullClark — direct limit-surface queries

`cc_Limit.h` adds a direct query API for the **infinite** Catmull–Clark limit
surface of a control cage. Unlike a fixed-depth subdivision, the returned
position, analytic derivatives and normal do not depend on any preview
resolution: each query subdivides only the local neighbourhood of the queried
face until its carrying sub-patch is fully regular, then evaluates the exact
uniform bicubic B-spline patch of the limit surface.

## Accepted cages

Finite coordinates, boundary-free, consistently oriented combinatorial
2-manifold quad meshes, vertex valences 3–12, all crease sharpnesses zero.
Multiple connected components and adjacent extraordinary vertices are
supported. Self-intersections and degenerate limit surfaces are not handled.

## Face parameter convention

For face `f`, starting at the halfedge `faceToHalfedgeIDs[f]`, the four
corners correspond, in traversal order, to

| corner | (u, v) |
| ------ | ------ |
| 0      | (0, 0) |
| 1      | (1, 0) |
| 2      | (1, 1) |
| 3      | (0, 1) |

Both parameters live on the closed interval `[0, 1]`. UVs stored on the cage
are not used. At an extraordinary corner sampled exactly (for example
`u = v = 0` when corner 0 has valence other than 4), parametric derivatives
are not defined: `derivatives_defined` is 0, `du`/`dv` are zero, and the
position and an oriented tangent-plane normal are returned.

## Usage

`cc_Limit.h` is a single-header library. Define `CC_LIMIT_IMPLEMENTATION` in
exactly one translation unit before including it; elsewhere include it
normally. It builds on the types in `CatmullClark.h` and uses only the C
standard library.

```c
/* In one translation unit: */
#define CC_LIMIT_IMPLEMENTATION
#include "cc_Limit.h"

/* Anywhere: */
#include "CatmullClark.h"
#include "cc_Limit.h"

void inspect_face(const cc_Mesh *cage, int32_t face)
{
    cc_LimitSurface *surface = cc_limit_create(cage);
    if (!surface) return;                 /* unsupported cage */

    cc_LimitSample sample;
    if (cc_limit_sample(surface, face, 0.35, 0.70, &sample) == 0) {
        /* sample.position[3], sample.du[3], sample.dv[3],
           sample.normal[3], sample.derivatives_defined */
    }

    /* Bad face ids, non-finite or out-of-range parameters and null outputs
       fail with a non-zero return and leave the output untouched. */
    cc_limit_sample(surface, face, 2.0, 0.0, &sample); /* returns non-zero */

    cc_limit_release(surface);
}
```

`cc_limit_create` copies the geometry and topology it needs, so the source
`cc_Mesh` may be edited or released afterwards; multiple
`cc_LimitSurface` objects are independent and may be used interleaved.

## How the limit is evaluated

1. The carrying face is cropped with a fixed local margin (no whole-model
   high-resolution mesh is ever built).
2. Catmull–Clark face/edge/vertex rules are applied locally and the child
   quadrant containing `(u, v)` is selected. Its rotation/scaling maps the
   parent chart to the child chart; the forward Jacobian is accumulated so
   the final B-spline derivatives are converted back to the original `(u, v)`
   parameters.
3. Refinement stops once every vertex of the carrying face's full 4×4
   B-spline control stencil is regular, and the exact uniform bicubic
   B-spline patch is evaluated. Analytic basis-function derivatives are used
   throughout — differences are never taken from positions.

Samples as close as `2^-20` to an extraordinary vertex are supported through
the parameter chain. Across a shared edge, the two incident faces produce the
same limit position and the same oriented unit normal; derivatives are
expressed in each face's own chart.

## Files

- `cc_Limit.h` — single-header limit-surface library (declarations plus,
  under `CC_LIMIT_IMPLEMENTATION`, the implementation).
- `checks/test_limit.c` — checks for regular patches, extraordinary regions
  and chart/lifecycle behaviour (run by `task_support/run.sh target`).
- `CatmullClark.h` — the existing finite-depth subdivision library, which is
  left unchanged.
