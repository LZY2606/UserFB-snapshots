# Diagnostic defect patches

These three patches inject **reversible, degenerate-but-benign** defects into
geometry-central's flip-based geodesic code. They exist to demonstrate that
the regression tests in `task_tests/geodesic_network.cpp` detect each failure
mode through **content assertion failures** (`TASK_CHECK` -> the case returns
1 -> the driver exits with code 4), not through compile errors, crashes, or
early process exits.

They are **not** applied during normal runs (`bash task_support/run.sh
baseline|target` builds the pristine sources).

## Applying / reverting

```sh
git apply task_tests/defects/<name>.patch   # enable the defect
bash task_support/run.sh target             # observe the assertion failure
git apply -R task_tests/defects/<name>.patch # revert; tree is clean again
```

Each patch touches exactly one hunk in `src/surface/flip_geodesics.cpp`,
compiles cleanly, and lets every routine return normally.

## The three defects

### 1. `skip_valid_shortening.patch` — valid shortenings are skipped

`FlipEdgeNetwork::iterativeShorten()` pops the smallest wedge, confirms it is
valid and unobstructed, and then simply does not call `locallyShortenAt()`.
The queue drains, the loop terminates normally, and every API keeps
returning well-formed results — the path just never gets shorter.

Detected by: `chart/folded_strip_unfolding`
(`net.nFlips > 0` fails; the final-length and monotonicity expectations would
fail as well).

### 2. `trace_position_offset.patch` — retraced positions shifted by a finite offset

`FlipEdgeNetwork::getPathPolyline()` post-processes every retraced
`SurfacePoint`, moving edge points 5% along their edge and face points 10%
toward their face centroid. Points stay on the same mesh elements, so
validation, element bookkeeping and 3D conversion all keep working; only the
geometry is wrong, by ~0.05 at the generated mesh scale (tolerance is 1e-6).

Detected by: `chart/folded_strip_unfolding`
(`std::fabs(sp.tEdge - tExpect) < TOL` fails, as do the independent
atlas-position and 3D cross-checks).

### 3. `rewind_stale_metric.patch` — rewind restores connectivity but a wrong metric

The rewind record stores `oldLen * 1.02` instead of `oldLen`. `rewind()`
still undoes every flip combinatorially (connectivity and original-edge flags
are perfectly restored), but every flipped edge comes back with a length 2%
off — an inconsistent metric that later computations would silently inherit.

Detected by: `rewind/rewind_reinitialize_alignment`
(`std::fabs(netA.tri->edgeLengths[e] - expect) < TOL_EXACT` fails, where
`expect` is recomputed independently from the generated vertex positions).
The `chart` and `network` groups still complete normally first, confirming
the defect only affects the rewind path.
