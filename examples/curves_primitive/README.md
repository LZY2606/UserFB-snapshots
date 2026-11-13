# Curves(Bezier) renderer.

Use Nakamaru and Ohno method to find an intersection between ray and Bezier curve. 

# Round tube mode (swept-sphere union)

`round_tube.h` adds a second, exact curve primitive next to the legacy
line-segment approximation.  A primitive is the union of all closed balls

    { X : |X - C(u)| <= r(u) },  u in [0, 1]

where four 3D control points define the cubic Bezier centerline `C(u)` and
four positive values define the radius `r(u)` in the same Bernstein basis.
The union naturally includes the spherical caps at `u = 0` and `u = 1`
(constant centerlines degenerate to concentric balls, i.e. one ball with
radius `max_u r(u)`).

Only the *outer boundary* of the union is intersected: sphere patches hidden
inside other balls of the same curve (self fold-back, a wide part covering a
narrow part, overlapping parameter ranges) produce no hit.  Different
primitives are still intersected individually through the BVH; no scene-wide
boolean union is performed.

## Radii vs. legacy widths

The legacy mode treats the four thickness values as *widths* and multiplies
them by 0.5 (and only uses the first/last value).  In round-tube mode the
four values are used **directly as radii** of the swept spheres, all four
participating through the cubic Bernstein basis.

## Interface

Everything works for both `float` and `double` and plugs straight into
`nanort::BVHAccel<T>`:

    round_tube::Geometry<T>      conservative primitive bboxes for Build()
    round_tube::Pred<T>          SAH split predicate
    round_tube::Intersection<T>  t, prim_id, u, position, normal (real3<T>)
    round_tube::Intersector<T>   ray vs. swept-sphere-union

`Geometry`/`Pred` take the control-point array (12 coordinates per curve),
`Intersector` takes the control points plus the 4 radii per curve, in the
same layout as the legacy example.  Bounding boxes are control-polygon boxes
expanded by `max(r_i)`, which conservatively contains the whole swept sphere
(`C(u)` stays in the control hull, `r(u) <= max r_i`).  `prim_id` is always
the input curve index; production queries must go through `BVHAccel::Traverse`
(BVH filtering), never a scene-wide loop.

Rays are `O + t D` with `D` not necessarily normalized; `t` stays on that
parameter axis.  The hit returned is the earliest true union boundary in
`[min_t, max_t)`:

* if `min_t` is already inside the tube, the *exit* boundary is reported
  (never `min_t` itself or an internal sphere crossing), including later
  re-entries of the same curve;
* a tangential external touch counts as a hit;
* `prim_ids_range` filtering is honored, and with `cull_back_face` only
  boundaries with `normal . D < 0` are accepted - a rejected exit does not
  stop the search for later entries;
* `u` back-substitutes into the boundary sphere (`|position - C(u)| = r(u)`)
  and `normal` is the unit vector from `C(u)` to `position`;
* intersector state is fully reset in `PrepareTraversal`, so one instance can
  be reused across interleaved directions, windows and misses.

## Numerical method and stopping criteria

All solving is done in double precision (also for `T = float`).

The union boundary along the ray is the zero set of
`m(t) = min_u (|O + tD - C(u)|^2 - r(u)^2)`.  Interior minimizers satisfy the
envelope system `h = 0`, `dh/du = 0`, which eliminates `t = -B(u)/A(u)` into a
degree-10 polynomial `E(u)`; the two cap spheres give quadratics.  Roots of
`E(u)` are isolated by recursive Bernstein subdivision (convex-hull
discard, never fixed surface sampling) with a relative coefficient zero-band
of `1e-12` and a minimum interval width of `1e-12`, then polished by Newton
steps.  Every candidate `t` is validated as a true union boundary by
evaluating `m(t)` exactly (stationary points of a degree-5 polynomial in
`u`): candidates with `m(t) < -1e-9 * S^2` are rejected as internal, where
`S = max(1, control bbox diagonal, max radius control)`.  Envelope
candidates must also satisfy the residual `|A(u) t + B(u)| <= 1e-7 * scale`.

## Error bounds and supported ranges

With `S = max(1, control-point bbox diagonal, max radius control)`:

* ordinary (non-tangential) hit position error: `<= 3e-4 * S` (float),
  `<= 2e-7 * S` (double);
* unit normal error: `<= 2e-3` (float), `<= 2e-5` (double);
* tangential hits (analytic geometry): position error `<= 1e-4 * S`, normal
  unbounded at the exact contact;
* rays staying at least `1e-3 * S` away from the true tube are always
  classified as misses (the tube is never fattened to hide misses).

Supported input range: finite coordinates with control components in
`[-64, 64]`, radius controls in `[0.02, 4]`, direction length in
`[0.25, 4]`, query windows `0 <= min_t < max_t <= 100`.  Repeated control
points and constant centerlines are legal.

## Test reference and its convergence

`task_tests/target.cpp` checks straight constant-radius material against the
closed-form analytic capsule.  Varying-radius and curved material is checked
against an independent brute-force reference: `m(t)` is evaluated by a dense
`u` scan (1024 samples) plus golden-section refinement of every
local-minimum bracket, and the `t` intervals with `m(t) <= 0` are found by a
dense `t` scan (8000 samples) plus 80 bisection steps per sign change.
Convergence basis: `m` is continuous (a minimum of continuous functions), so
bisection on a sign-changing bracket converges to a true boundary crossing;
the test geometries have boundary features orders of magnitude wider than
the scan pitch, so no interval can be skipped.  Reported hits are
additionally certified a posteriori: `u` back-substitution
(`|position - C(u)| = r(u)`), unit normal from `C(u)`, and `m(t) >= -eps`.

## Usage

    ./curves_primitive                       # legacy PNG mode (unchanged)
    ./curves_primitive --round-tubes out.ppm # headless 64x64 P3 round-tube render

The round-tube mode uses the same `genFur` control data as the default mode
with the four thickness values as radii, shades with `|normal|` of the true
union boundary, needs no window and downloads nothing.

# TODO

* [x] Use accurate ray - curve intersection(Nakamaru and Ohno method)
* [x] Support accurate variable width (round tube / swept-sphere mode)
