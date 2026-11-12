# Indexed distance field checks

These checks exercise the spatial-pruning entry points added alongside the legacy per-edge
generators:

- `msdfgen::ShapeDistanceIndex` — an owning, reusable snapshot of a `Shape` plus a bounding-volume
  hierarchy.
- `msdfgen::generateSDFIndexed` / `msdfgen::generateMTSDFIndexed`.
- `ShapeDistanceIndex::sdfDistance` / `mtsdfDistance`, which return the pre-correction `double`
  and `MultiAndTrueDistance` values for direct comparison.

Run everything with:

```sh
bash checks/run.sh target
```

The script compiles `checks/target.cpp` with `checks/driver.cpp` and every `core/*.cpp`, runs the
four registered groups (`distance`, `endpoints`, `contours`, `pruning`) and then runs the existing
square distance-field baseline (`baseline.cpp`).

## Case groups

- **distance** — pre-correction `double` distances and full `float` fields match the legacy
  per-edge entry points, including non-default range, mirrored/non-uniform projection, non-square
  bitmaps, and values outside `[0,1]` (the field is deliberately never clamped to a constant).
- **endpoints** — independently hand-computed straight-edge true distances and a sharp-corner
  perpendicular (pseudo) distance, the unbounded tangent ridge, and a zero-length endpoint tangent.
- **contours** — multi-color corners, mixed quadratic/cubic/linear edges, a reversed inner hole and
  overlapping same-winding contours (both `overlapSupport` modes), Y-axis orientation, and an index
  that keeps working after the source `Shape` is edited.
- **pruning** — independence of nearby vs. jumped queries and of alternating projections, exact
  statistics semantics, linear storage, and the required leaf-visit reductions on two 128x128
  fields. The actual counters and allocation sizes are printed; wall-clock time is intentionally
  not used to claim a speedup.

## Why the pruning is conservative

The true signed distance (the SDF value and the MTSDF alpha channel) is governed only by the finite
curves, so a node whose tight curve bounding box is farther than the current nearest-true-distance
radius cannot change it and may be pruned.

The three MTSDF color channels are different. They use the *perpendicular* pseudo-distance, whose
domain is not limited to the curve. Around every endpoint the selector extends two tangent lines:

- at the start point, the reversed start tangent and the corner bisector with the previous edge;
- at the end point, the end tangent and the corner bisector with the next edge.

A point past an endpoint still contributes a perpendicular value (the cross product with the
tangent direction) whenever it lies inside the corresponding half-plane gates. Along an endpoint
tangent ridge that value stays exactly zero at **arbitrary** distance — the probes in
`endpoints/tangent_ridge_survives_far_pruning` check this out to 1e5 units. A pruner that only used
finite curve boxes would drop the responsible edge far away and change a color channel (and hence
the median) even though the alpha distance is large.

The index therefore never bounds the pseudo-domain with a finite box. Instead, at each candidate
edge it applies the *same* geometry as `PerpendicularDistanceSelectorBase`:

1. the outer corner-bisector half-plane gate and the inner tangent half-plane gate;
2. the exact perpendicular cross-product value, compared against independently maintained per
   contour / per channel negative and positive extrema;
3. the finite curve box, which only decides true-distance (alpha) relevance.

The tracked extrema are updated with exactly the values the selector records (including its
`|perpendicular| < |true distance|` condition), so they are never tighter than the selector's.
`overlapSupport=true` still routes edges through the existing directed `OverlappingContourCombiner`
merge rules — pruning happens inside each contour's candidate set, never by globally keeping a
single nearest edge.

All pruning state (the combiners, per-edge caches, radii and statistics) is per-query working
storage held outside the immutable index. A query that jumps far away, or two generations using
different projections interleaved on one index, can never reuse a stale minimum.

## Statistics

`DistanceQueryStats` counts only real work:

- `nodeTests` — bounding-box / node decisions actually evaluated;
- `leafEdgeVisits` — candidate edges handed to an edge selector;
- `exactDistanceCalls` — actual `EdgeSegment::signedDistance` calls (estimates are never counted).

Index construction is not part of generation statistics. The hierarchy over `E` edges stores at
most `2E-1` nodes (printed as nodes per edge), so index memory grows linearly with edge count and
never precomputes answers for a target image size.

## Generated images

`pruning/colored_contour_mtsdf_prunes_half_and_reconstructs` writes two 128x128 PPM images:

- `checks/mtsdf-reconstruction.ppm` — the MTSDF shape reconstructed from the median of the three
  color channels;
- `checks/smooth-sdf.ppm` — the single-channel SDF of the 512-edge smooth contour.

They are plain binary P6 PPMs and are regenerated on every run.
