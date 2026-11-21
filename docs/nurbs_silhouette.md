# NURBS Silhouette Extraction (`gte::NURBSSilhouette`)

CPU silhouette (contour) extraction for NURBS surfaces under an orthographic
camera, living in `GTE/Mathematics/NURBSSilhouette.h` and
`GTE/Mathematics/NURBSSilhouette.cpp`.  Only C++17 standard library and the
existing Geometric Tools headers are used; the old evaluators, meshes and
sample interfaces are unchanged.

## API

```cpp
std::vector<std::shared_ptr<gte::NURBSSurface<3, double>>> surfaces = ...;
gte::Vector3<double> D{ 0.0, 0.0, 1.0 };          // object-to-eye, unit length
gte::NURBSSilhouette::Options options;            // tolerances and budget
gte::NURBSSilhouette silhouette(surfaces, D, options);
if (!silhouette.IsValid()) { /* see silhouette.GetError() */ }
gte::NURBSSilhouette::Result result;
if (!silhouette.Compute(result)) { /* invalid input; result untouched */ }
bool everythingResolved = result.complete;
```

* The constructor takes **shared ownership** of the surface list and
  validates the camera, the surfaces and the tolerances.  Illegal input
  (non-unit view direction, degree outside {2,3}, non-clamped or periodic
  knots, interior knot multiplicity larger than degree-1, non-positive or
  non-finite weights, non-positive tolerances, zero budget) is rejected
  **before** any work: `IsValid()` is false and `Compute` returns false
  without modifying the caller's result object.
* The result owns its data.  It stays valid after the input surfaces and
  the `NURBSSilhouette` object itself are destroyed.

### Supported surfaces

* 3D surfaces, double precision, degree 2 or 3 independently in u and v.
* Clamped (open), non-periodic knot vectors; interior knots of multiplicity
  at most degree-1, so the surface is at least C1.
* Strictly positive, finite weights.  No trimmed surfaces.
* The surface must be regular (`S_u x S_v != 0`).  This is checked
  numerically during extraction; a vanishing normal is reported as an
  unresolved region (`SurfaceDegenerate`) rather than silently accepted.

## What is computed

For each surface the silhouette is the zero set of

```
f(u,v) = (S_u(u,v) x S_v(u,v)) . D
```

in the parametric domain.  Surface boundary curves are **not** silhouettes
unless the zero set runs into them.  All surfaces occlude, and the front
part of a surface occludes its own back part.

Each output `Curve` contains samples in connected order.  Every sample
carries the surface index, the parameters `(u,v)`, the position (exactly
`S(u,v)` of the source surface, so back-substitution reproduces the output)
and the oriented unit tangent: the parametric direction `(-f_v, f_u)`
mapped through the surface first derivative and normalized.  `closed`
distinguishes closed loops from open branches; open branches end only on
the domain boundary or at an explicitly unresolved region.

## Extraction and connection rules

1. **Element split.**  Each surface is split into its Bezier elements using
   its knot partition (knot insertion on the homogeneous control net).  On
   an element, `w^2 * f` (with `w` the weight function) is a polynomial;
   its Bernstein coefficients are computed exactly from the control net.
2. **Exclusion.**  A region quadtree (de Casteljau splits, so the Bernstein
   bounds stay exact) discards every parameter region whose coefficients
   have a constant sign — such regions provably contain no zero.  This is a
   true parametric exclusion test, not sampling on a fixed grid.
3. **Seeds.**  Regions that cannot be excluded are refined until a Newton
   iteration on `f` converges to a seed point on the zero set.  Interior
   closed loops are found exactly the same way as boundary-reaching
   branches; nothing is traced from the parameter boundary inward.
4. **Tracing.**  Branches are traced from the seeds with an adaptive
   predictor-corrector method in the parameter domain, following
   `(-f_v, f_u)`, across knot elements.
5. **Connection.**  Seeds lying on an already traced branch (including the
   same parametric event reached from two neighboring knot elements) are
   consumed, so every parametric event is connected exactly once.
   Connection is decided in the parameter domain of each surface: different
   parameters that map to the same spatial point are never welded together.

## Tolerances

* `spatialTolerance` — 3D distances.  A ray-surface intersection occludes
  only when its distance along `+D` exceeds this tolerance; the silhouette
  point itself (and its tangent contact with its own surface) never counts.
* `parameterTolerance` — parameter-domain resolution: smallest region,
  trace step and boundary-crossing location.
* `angleTolerance` — every reported sample satisfies
  `|Dot(N,D)|/|N| <= angleTolerance` with `N = S_u x S_v` evaluated at the
  sample parameters.
* `regionBudget` — total number of region nodes (exclusion quadtrees and
  ray-cast quadtrees together).  Exhaustion is reported through unresolved
  regions (`BudgetExhausted`), never silently treated as empty space.

## Visibility

Visibility is decided along the continuous parameter of each curve.  From a
silhouette point `X` the ray `X + t*D` is tested against every surface
(including its own); `X` is occluded when an intersection with
`t > spatialTolerance` exists.  Ray-surface intersection uses the same
Bernstein exclusion quadtree, resolved by minimizing the squared distance
from the ray line to the surface over each leaf cell, so tangential
contacts (the ray just touches its own surface at `t = 0`) are classified
correctly instead of being reported as occluders or left unresolved.

Occlusion state changes between samples are located by bisection along the
traced branch and the intervals are split there — endpoint or midpoint
visibility of a polyline segment is never used to represent the whole
segment.  The full silhouette is always kept; hidden parts are reported as
`Hidden` intervals and never joined across an occluded gap.

`Curve::intervals` partitions the normalized chord-length parameter
`t in [0,1]` of the samples (`Curve::parameter` gives the parameter of each
sample).  On closed curves one interval may wrap across the seam, reported
with `t0 > t1`.

## Unresolved states

Extraction is conservative: a region that can neither be excluded nor
resolved is reported in `Result::unresolved` (with surface index, UV box
and reason) and `Result::complete` is false.  Nothing unresolved is treated
as empty space, and unresolved rays are never defaulted to visible —
affected intervals are marked `Unknown`.

| Reason | Meaning |
| --- | --- |
| `ZeroSetGradientVanishes` | `grad f` vanishes on/next to the zero set; the branch cannot be traced reliably. |
| `NonIsolatedZeroSet` | `f` vanishes on a whole region (e.g. a plane viewed along its tangent direction). |
| `BudgetExhausted` | The region work budget ran out before the region was excluded or resolved. |
| `RayUnresolved` | A visibility ray could not be classified. |
| `OcclusionBoundaryUnresolved` | An occlusion state change could not be located. |
| `SurfaceDegenerate` | The surface normal vanished (input outside the regular-C1 scope). |

## Example

`examples/nurbs_silhouette.cpp` writes an SVG drawing of a sphere contour
with two occluding rectangles in front of it: the full silhouette as a thin
gray line, visible intervals as a thick green line and hidden intervals as
a dashed red line.

```
.task-build/example out.svg
```

## Tests

`tests/nurbs_silhouette.cpp` registers the groups `topology`, `visibility`
and `failure` with the task driver.  Results are checked against the
analytic sphere silhouette `u^2+v^2 = 1`, against the analytic projection
intervals of the occluding rectangles, and against back-substitution into
independently constructed surfaces.  Run everything with

```
bash task_support/run.sh target
```
