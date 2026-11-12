# Pressure-aware stroking

## The width parameter

Every vertex of an `agg::pressure_path` carries three values: a position
`(x, y)` and a **full stroke width `w`** (not a half width). All coordinates
and widths must be finite and widths must be non-negative; a violating
`move_to`, `line_to`, `curve3` or `curve4` throws `std::invalid_argument`.

For a curve segment the center position `P(t)` and the width `w(t)` are
expanded with the **same Bernstein basis**:

* `curve4`: four control triples, one cubic basis for `P`, `w`;
* `curve3`: three control triples, one quadratic basis;
* `line_to`: linear interpolation of both position and width.

The first control triple of a segment is inherited from the end of the
previous segment (its `x`, `y` **and** `w`), exactly as an ordinary path
storage inherits the current point. Interpolating only between the two end
widths would discard the pressure stored on the curve control points, so
that is deliberately not done.

## The two boundaries

For each regular segment the converter builds two offset curves

```
Q+(t) = P(t) + w(t)/2 * N(t)
Q-(t) = P(t) - w(t)/2 * N(t)
```

where `N(t)` is the unit **left normal** of the centerline tangent
`P'(t)`. Both boundaries therefore react to pressure changes: even when
the centerline is perfectly straight, a large control-point width makes
the sides bulge, and a zero-width endpoint collapses to a single tip.

When a centerline end derivative is zero, the tangent at that end is the
one-sided limit tangent of the curve. A zero-speed point strictly inside a
segment, a constant centerline, or a zero-length line is reported with
`std::invalid_argument` instead of being replaced by an arbitrary
direction.

## Boundary error and `tolerance`

`tolerance` (finite, strictly positive) bounds the **two-way distance**
between the output polyline and the true boundary of each side:

1. every point of the true boundary arc is within `tolerance` of the
   output polyline; and
2. every output vertex is within `tolerance` of the true boundary arc.

The check is geometric, not a check of centerline flatness, and it is not
satisfied by sampling a fixed number of parameter points: segments are
subdivided adaptively from the measured point-to-chord deviation on both
sides, then re-verified on a dense parameter grid and refined wherever the
budget is violated. Round caps are bounded by the same `tolerance` through
the sagitta of their approximating semicircle. If the required precision
cannot be reached within the subdivision depth guard, the converter throws
rather than silently emitting an out-of-tolerance result.

The supported working range is coordinate magnitudes up to 4096, widths up
to 512, and tolerances between 0.001 and 1.

## Joins and caps

Adjacent segments share the width at their common endpoint, but width
derivatives, tangents and curvatures may jump there.

* **bevel join** connects the two boundary endpoints directly on each side.
* **miter_join_revert** intersects the tangent lines of the actual offset
  curves `Q+`/`Q-` (never centerlines translated by a constant width). The
  miter is kept only while its distance from the center joint is at most
  the local half width times `miter_limit`; otherwise that side reverts to
  a bevel. Zero boundary derivative, parallel tangents or a zero local
  half width also fall back to a bevel. `miter_limit` must be finite and at
  least 1.
* **butt cap** joins the two sides at the endpoint.
* **square cap** extends by the local half width along the endpoint
  tangent.
* **round cap** is the outer semicircle centered on the endpoint with the
  local half width as radius.

An open subpath becomes one closed outline. A closed subpath produces two
independently closed boundaries of opposite winding; the implicit closing
segment interpolates position and width linearly and its seam uses the
same join rules, with no caps. The non-zero fill rule keeps the interior
hole empty. A path whose widths are all zero produces no geometry.

## Source lifecycle

The converter borrows the source pointer-style (it never owns or mutates
it). `rewind(path_id)` rebuilds the outlines from the current source, so
changing `tolerance`, cap or join and rewinding regenerates them; calling
`attach` or editing the source and then rewinding reads the new input.
Outputs are ordinary `move_to` / `line_to` / closing `end_poly` / `stop`
commands that any existing fill rasterizer accepts.
