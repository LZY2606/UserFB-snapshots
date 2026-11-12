# Projective clip converter checks

This directory holds the fixed test harness for the perspective-aware
Bezier converter `agg::conv_projective_clip<VertexSource>`
(`agg-src/include/agg_conv_projective_clip.h`).

Run everything with:

```sh
bash checks/run.sh target
```

That compiles `checks/target.cpp` with the AGG sources and runs the four
registered groups `rational`, `clip`, `topology`, `render` in order, then
re-runs the pre-existing baseline (`checks/baseline.cpp`), which exercises
the untouched `conv_curve` + `conv_transform` pipeline.

## The visible domain

A user-space point `(x, y)` is lifted to homogeneous coordinates by the
snapshot of the `trans_perspective` matrix:

```
W  = w0*x + w1*y + w2
NX = sx*x + shx*y + tx
NY = shy*x + sy*y + ty
```

and projects to the device point `(NX / W, NY / W)`. A point belongs to the
visible domain only when **both** hold:

1. `W >= 1e-6` — the projective near-plane / horizon guard. Points with a
   smaller (in particular negative) weight are on the far side of the
   perspective horizon and are discarded;
2. the projected point lies inside the configured clip rectangle
   (`clip_box`, default `[0, 0, 256, 256]`).

Curves are kept in the original Bezier parameter space. Each user control
point is lifted to `(NX, NY, W)`, so the projected curve is a **rational**
Bezier `R(t) = (NX(t)/W(t), NY(t)/W(t))`. The converter never projects the
control points and then treats the result as an ordinary polynomial Bezier.
Visibility roots (`W = 1e-6` and the four rectangle inequalities, all
Bernstein polynomials in `t`) are isolated by de Casteljau subdivision, so
a short visible interval whose two ends are both outside the rectangle is
not skipped.

## The approximation tolerance

`approximation_tolerance(eps)` (default `0.25` device pixels) bounds the
two-way distance between every retained visible arc of the original
projected curve and its approximating polyline:

* every point of the true projected arc is within `eps` of the polyline, and
* every polyline vertex that belongs to a curve (rather than an added
  closing segment) is within `eps` of the true arc.

The bound follows from rational-Bezier convexity: with all homogeneous
weights positive, the projected curve is a convex combination of its
projected control points. Recursive subdivision continues until every
interior control point is within `eps/2` of the chord (perpendicular
distance **and** longitudinal overshoot are both checked). Midpoint-only
flatness is deliberately not used, so internal cusps and same-endpoint loops
keep subdividing instead of being accepted as a single chord.

Boundary intersections carry the same `eps` guarantee. When a closed
contour is cut, the extra segments that re-close it run along the boundary
of the convex visible polygon (the clip rectangle intersected with the
near-plane half-plane). Those closing segments are exempt from the arc
distance comparison because they are not part of the original curve.

`clip_box` requires four finite, strictly increasing boundaries and
`approximation_tolerance` requires a finite positive value; otherwise the
setter throws `std::invalid_argument`.

## Test groups

* **rational** — matrix snapshot semantics, agreement with an independent
  homogeneous rational Bezier evaluator, quadratic and cubic curves,
  divergence from the naive "project controls then flatten" approach,
  tolerance scaling, and invalid-configuration exceptions.
* **clip** — far-side rejection, a box sliver and a horizon sliver whose
  endpoints are both outside the visible domain, boundary intersections
  sitting exactly on clip edges, and a custom clip box.
* **topology** — open paths split into separately `move_to`-started
  fragments, closed contours re-closed on the visible-domain boundary with
  orientation derived from the emitted geometry (including reflections),
  nested reversed contours keeping their hole, `rewind`/`attach`/matrix
  freshness, and non-modification of the source path.
* **render** — the output is fed to the real `rasterizer_scanline_aa` and
  `render_scanlines_aa_solid`; interior/exterior pixels well away from the
  antialiased edges are compared against independently constructed
  reference polygons. A small PPM of the three figures
  (`checks/projective_clip.ppm`, three 256x256 panels stacked vertically)
  is written for visual inspection.
