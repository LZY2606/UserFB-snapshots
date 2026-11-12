# Continuous weighted corridor straightening

## What it does

`dtNavMeshQuery::findWeightedPath()` straightens a polygon corridor that has
already been selected (for example by `findPath()`). The corridor itself is
never searched again. Instead of pinning every portal crossing to its
midpoint, each crossing is allowed to move freely along its portal segment
and is optimized so that more of the travel stays inside cheap polygons.

Given an ordered corridor of `n` ground polygons, there are `n-1` portal
segments. The returned polyline always contains `n+1` points:

```
p0 (start), p1 ... p_{n-1} (one point per portal), p_n (end)
```

All crossing positions are retained, including coincident ones; only the
geometric turning points are never dropped, so the per-polygon cost
attribution is always available.

## The objective

For segment `i`, from `p_i` to `p_{i+1}`, the weight is the filter's area
cost of `path[i]`:

```
w_i = filter->getAreaCost(areaOf(path[i]))
```

The cost is the full 3D weighted length

```
U = sum_i w_i * |p_{i+1} - p_i|
```

using the real three-dimensional segment lengths (a tilted portal is measured
with its vertical component, not its horizontal projection). The custom
`dtQueryFilter::getCost()` is deliberately not used; only `getAreaCost()` is.
All weights must be finite and strictly positive.

The start and end positions are used verbatim and are never snapped onto a
polygon or a portal; placing them inside the first and last polygons is the
caller's responsibility.

## Portals

Each portal is built from the real `from -> to` link between consecutive
corridor polygons, using the same edge vertices and the same `bmin`/`bmax`
clipping as `dtNavMeshQuery::getPortalPoints()`. A clipped cross-tile link
therefore yields the clipped segment, never the unclipped full tile edge.
A crossing is always expressed as a point on that segment.

## The certificate and the two costs

`dtWeightedPathResult::dual` holds `n` vectors `q_i`, one per polygon, each
satisfying the norm constraint

```
|q_i| <= w_i
```

These vectors certify a lower bound on the global minimum. With portal `j`
endpoints `a_j`, `b_j` the exact lower bound is

```
D = q_{n-1} . p_n - q_0 . p_0
    + sum_j min( (q_{j-1} - q_j) . a_j, (q_{j-1} - q_j) . b_j )
```

For any feasible polyline, weak duality gives `D <= optimum <= U`. The
published values are:

- `upperCost` — the exact weighted length `U` of the returned polyline.
- `lowerCost` — the certificate bound `D` recomputed from the published
  points and vectors, independently verifiable by the caller.

The numerical floor on vector feasibility and cost recomputation is a single
global slack of `1e-8 * max(1, U, max(w_i))`; it is applied once, never
accumulated per portal. A zero-length segment produces a finite zero vector
and contributes zero cost — it never creates a NaN direction.

## Return status

| Status | Meaning |
| --- | --- |
| `DT_SUCCESS` | `upperCost - lowerCost <= absoluteTolerance`. The published points and vectors are complete and feasible either way. |
| `DT_IN_PROGRESS` | The iteration budget was exhausted before the certified gap met the tolerance. Complete feasible points, vectors and the true upper/lower costs are still returned; success is never claimed on a small parameter change or a stalled coordinate. |
| `DT_FAILURE \| DT_INVALID_PARAM` | A null/bad argument, non-finite or non-positive tolerance, non-finite endpoint, non-finite/non-positive area cost, a failed filter, a polygon that is not ground (off-mesh connections are rejected), an invalid reference, or two consecutive polygons with no link between them. |
| `DT_FAILURE \| DT_BUFFER_TOO_SMALL` | The points or dual array is too small for `n+1` points or `n` vectors. No truncated evidence is published; counts and costs are cleared. |
| `DT_FAILURE \| DT_OUT_OF_MEMORY` | A temporary `dtAlloc` allocation failed. |

`maxIterations` is a hard upper bound and the loop is bounded. With zero
iterations the initial midpoint polyline and its (feasible) certificate are
returned together with `DT_IN_PROGRESS`, unless the corridor has no portals,
in which case the single segment is provably optimal and `DT_SUCCESS` is
returned even with a zero budget.

## Example

```cpp
dtNavMeshQuery query;
query.init(&navMesh, 2048);

dtQueryFilter filter;
filter.setAreaCost(SLOW_AREA, 4.0f);

const dtPolyRef* corridor = ...;      // connected ground polys, e.g. from findPath
const int corridorSize = ...;
const float startPos[3] = ...;        // inside the first polygon
const float endPos[3] = ...;          // inside the last polygon

const int nPoints = corridorSize + 1;
double points[3 * nPoints];
double dual[3 * corridorSize];

dtWeightedPathResult result;
result.points = points;
result.maxPoints = nPoints;
result.dual = dual;
result.maxDual = corridorSize;

const dtStatus status = query.findWeightedPath(
    startPos, endPos, corridor, corridorSize,
    &filter,
    1e-5,    // absolute tolerance on upperCost - lowerCost
    20000,   // iteration budget
    &result);

if (dtStatusSucceed(status))
{
    // result.upperCost is within 1e-5 of the certified global minimum.
}
```

The same query is available on a corridor object through
`dtPathCorridor::findWeightedPath()`, which forwards the corridor's current
position, target and path verbatim. The corridor's own path is not modified
and no crossing solution is cached, so moving the position or target and
calling again always consumes the fresh endpoint coordinates.
