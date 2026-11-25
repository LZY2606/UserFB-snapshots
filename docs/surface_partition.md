# SurfacePartition

`manifold::SurfacePartition` builds one common surface arrangement for up to
four closed `Manifold` solids.  It is intended for multi-material modeling:
both materials refer to the same subdivided interfaces instead of running
independent Booleans that can produce different tessellations.

The public entry point is in
[`include/manifold/surface_partition.h`](../include/manifold/surface_partition.h);
the implementation is in
[`src/surface_partition.cpp`](../src/surface_partition.cpp).

## Construction and ownership

```cpp
#include "manifold/surface_partition.h"

manifold::SurfacePartition partition({a, b, c});
if (!partition) {
  // partition.GetStatus() gives the explicit rejection reason.
}
```

The constructor reads each operand's `GetMeshGL64()` once and owns those
snapshots.  After construction returns, destroying the source `Manifold`
objects does not affect `Vertices()`, `Faces()`, source provenance, or repeated
calls to `Extract()`.

An initializer-list/vector entry accepts zero to four operands.  Zero operands
and empty `Manifold` operands are valid.  Five or more operands are rejected
with `Status::TooManyOperands`.  Operands in an error state, non-finite data,
and mismatched extra-property counts are also rejected explicitly.

## Occupancy masks and orientation

Bit `i` of a mask is one when the probe point is inside operand `i`.  Mask zero
is therefore the unbounded exterior.

Every `SurfacePartition::Face` is an arranged triangle stored exactly once:

- `vertices` indexes the shared `Vertices()` table.
- `sourceOperand` is the operand whose original surface contains the triangle.
- `sourceTriangle` is the triangle index in
  `SourceMeshes()[sourceOperand]`, i.e. the owned `GetMeshGL64()` snapshot.
- `sourceBarycentric[k]` is the three-weight barycentric coordinate of face
  corner `k` relative to the three source-triangle corners.
- `minusMask` is the occupancy on the negative geometric-normal side.
- `plusMask` is the occupancy on the positive geometric-normal side.

The vertex order follows the source triangle's outward orientation.  Thus the
source solid's interior is the negative-normal side:

```text
minusMask ^ plusMask == 1 << sourceOperand
minusMask & (1 << sourceOperand) != 0
plusMask  & (1 << sourceOperand) == 0
```

All other occupancy bits are identical on the two sides.

`Extract(mask)` returns the closed region whose exact occupancy is `mask`.
It selects triangles from the existing partition and reverses their order when
necessary; it never re-intersects the inputs or independently re-tessellates
the region.  Multiple connected components may remain in one `Manifold`.  A
valid bounded mask with zero volume returns an empty `Manifold`.

`Extract(0)` is rejected because mask zero is the unbounded exterior.  Use
`TryExtract(mask, result)` for a non-throwing form.

For an interface between two extractable masks, both extracted regions contain
the same three coordinate vertices.  One region uses the stored order and the
adjacent region uses the reverse order.  An outer shell has `plusMask == 0`;
its stored order is selected by its nonzero `minusMask`.

## Common arrangement

All pairwise triangle intersections are first represented as canonical
edge/face events.  Triple-surface intersections are canonical three-face
events and receive one global vertex ID.  Pair intersection segments are split
at those triple events before each source triangle's planar straight-line
graph is converted into cells and triangulated.  This makes the triple point a
single real vertex on all three source faces and all three pairwise
intersection curves.

Shells that do not intersect another operand—including a shell completely
contained in another solid—remain in the arrangement and are classified by
occupancy probes.  The implementation does not create all mask combinations
with sequential `Boolean`, `Split`, or `BatchBoolean` calls and does not weld
independent results by positional tolerance.

## Properties and provenance

All non-empty inputs must have the same number of extra scalar property
channels (`MeshGL64::numProp - 3`).  Output corner properties are interpolated
from the source triangle's face-corner properties using
`sourceBarycentric`.  Geometric positions and property vertices are kept
separate: nontrivial property seams remain duplicated property vertices while
`mergeFromVert`/`mergeToVert` identify their shared geometric vertex.

Extracted `MeshGL64` output preserves the source snapshot's:

- `runOriginalID`,
- `runTransform`,
- `runFlags`, when present, and
- `faceID`.

Untracked sources continue to use ID zero rather than being registered as new
original models.  Consequently an extraction is a relation-bearing Manifold in
the existing source chain, not a fresh anonymous primitive.

## Supported input range

The supported inputs are valid closed triangle solids, possibly non-convex,
with cavities and multiple components.  The documented geometric contract is:

- all coordinate absolute values at most `100`;
- shortest edge at least `1e-3`;
- longest-edge to shortest-altitude ratio at most `1000`;
- no self-intersections within an operand;
- surfaces may be disjoint, nested, or transverse;
- intersection lines do not pass through original vertices;
- original edges from different operands do not intersect;
- no coplanar overlap or tangency;
- no common point of surfaces from four distinct operands;
- unit cross-product normals of a transverse pair have magnitude at least
  `1e-4`;
- absolute determinant of the three unit normals at a triple-surface event is
  at least `1e-4`;
- separation between distinct events and unrelated edges is at least `1e-6`.

Unsupported or numerically under-conditioned cases are reported through
`GetStatus()` or `TryExtract()` rather than silently omitting interfaces.

## Example and tests

Build and run the example through the task runner, or build it against the
static library:

```bash
bash task_support/run.sh target
.task-build/example output.obj
```

The example writes one global OBJ vertex table and named groups
`mask_1` through `mask_7`.  Adjacent groups reuse the same vertex indices with
opposite face winding.

The required serial test suite is in `test/surface_partition.cpp`.  It
registers the `junctions`, `regions`, `properties`, and `reuse` groups and
checks triple-vertex identity, absence of T-junctions, closed bidirectional
edge topology, single vertex fans, analytic occupancy probes, directed volume,
barycentric reconstruction, property seams, provenance round-trips, source
lifetime independence, error recovery, and legacy binary Booleans.
