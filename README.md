# Analytic Beam Film

This project adds an **analytic beam transport** mode to the path tracer. A
screen pixel is treated as a continuous bundle of pinhole rays rather than a
set of sampled rays. When the bundle hits a finite planar mirror it is split
by the actually visible screen region, and the reflected bundle is integrated
in the original screen coordinates. There is no supersampling, no centre or
corner ray approximation and no fixed-resolution intermediate buffer, so the
output resolution does not change with geometric detail.

## Scope and limitations

Supported scenes contain only `Mesh` objects whose triangles use:

* `EMIT` materials with constant, finite, non-negative emission;
* `IDEAL_MIRROR` materials with constant, finite, non-negative reflectance in
  `[0,1]` per channel and zero emission.

Textures, `Sphere`s, `DIFF` and `SPEC` materials are rejected with
`std::invalid_argument` before any film is produced. Zero-area triangles are
ignored at load time. The reflection depth limit `D` is in `[0,4]`.

## Coordinates and rays

* A pixel is `[x,x+1] x [y,y+1]` with area one and **no half-pixel shift**.
* `Camera::get_ray_continuous(double x, double y)` is the deterministic,
  jitter-free extension of `get_ray(x, y, false, Xi)`. At integer coordinates
  it returns exactly the same ray; arbitrary real coordinates follow the same
  affine screen formula.
* A beam segment is represented by an unnormalized affine ray family
  `D(x,y) = d0 + dx*x + dy*y` from a (possibly virtual) eye. Planar
  reflection keeps the family affine, including the chirality change.

## Geometry and visibility

* Triangle screen footprints are exact polygons obtained by clipping the
  affine barycentric half planes against the current convex domain.
* Overlapping, non-coplanar triangles split the domain by the affine
  footprint edges and by geometrically reconstructed depth equality contours,
  so a front/back swap *inside* one pixel is integrated instead of decided
  from a single centre distance.
* Equal-depth ties are resolved by Scene add-slot, then original OBJ face
  order. Triangles touching only along edges or at vertices contribute no
  area twice. Duplicate faces keep distinct identities.
* Each reflected segment carries the mirror departure plane as a *near
  plane*. Triangles straddling the virtual eye are clipped to the valid side;
  triangles lying on the departure plane (the mirror itself) cannot re-win at
  zero depth. The virtual-eye-inside-box and box-straddling-the-near-plane
  cases are handled conservatively.

## KD tree integration

The original KD tree gains a conservative beam traversal
(`KDNode::collect_beam`) using half-space tests on leaf bounding boxes: it may
over-collect (false positives are removed by exact clipping) but can never
drop a contributing face. No alternative tree is built and no triangle list
is exported. Each `BeamFilm`/`BeamSubResult` reports:

* `nodes_visited` – KD nodes entered during the query;
* `candidate_faces` – faces handed out by leaves;
* `clipped_faces` – faces that went through exact screen clipping.

## Outputs and accounting

`BeamPixel` owns, per pixel:

* `beauty` – sum of the `D+1` `by_bounce[k]` entries;
* `by_bounce[k]` – unnormalized radiance reaching an emitter after exactly
  `k` mirror reflections;
* `emitter_by_bounce[k]`, `emitter_area`, `miss_area`, `truncated_area`.

For every pixel `emitter_area + miss_area + truncated_area == 1`: a region
that hits a mirror after `D` reflections is truncated, a region whose rays
escape is a miss, and edge/point contacts have zero area. Radiance only is
transported (no distance attenuation or projected-area conversion) and the
mirror reflectance accumulates per channel.

`BeamSubResult` is the same accounting over an arbitrary convex screen
polygon; splitting a domain into convex pieces and adding the results
reproduces the whole exactly.

## Public API

* `BeamTracer(Scene&, Camera&)`
  * `BeamFilm render_frame(int D)` – whole frame;
  * `BeamSubResult query(const std::vector<ScreenPoint>&, int D)` – convex
    sub-domain.
* `Renderer::render_beam(int D)` – renderer-level entry. A failed render
  leaves the previous film untouched and separate `Renderer` instances never
  share state.
* `beamfilm_write_ppm` / `beamfilm_write_csv`.

`Scene::trace_ray` also supports `IDEAL_MIRROR` deterministically with the
same geometry and identity pipeline, so a single ray inside an analytic
region can be cross-checked against the area integration. The original
random `DIFF`/`SPEC` behaviour is unchanged.

## Example and tests

* `examples/beamfilm.cpp <output-dir>` is windowless. It writes OBJ scene
  files, renders through `Renderer::render_beam`, and produces
  `image.ppm` plus `channels.csv`
  (`y,x,bounce,r,g,b,emitter_area,miss_area,truncated_area`, 17 significant
  digits; the area columns are filled on the `bounce=0` row only). The scene
  shows a fine slit in a slanted mirror, an emitter outside the direct camera
  view seen through the mirror, and a two-reflection path.
* `task_tests/beamfilm.cpp` contains the `events`, `transport` and `film`
  groups registered with `task_support/check.h`. Expected areas and RGB
  values are hand-computed; finite renderers are never used as a reference.

Build and run everything with:

```
bash task_support/run.sh target
```
