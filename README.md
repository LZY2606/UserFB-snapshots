# Blend Weight Solver

This module adds **shared, whole-skeleton animation blend weight solving** to the libGDX `g3d` utilities. Given a set
of skinned point observations (each: a `NodePart`, a bind-space point, four bone slots with skin weights, and a desired
world point), it solves **one weight per animation clip** — the same weights apply to every bone. It never introduces
per-joint weights or joint rotations, and every candidate pose is a combination of the existing clips.

The implementation is pure JDK plus the bundled libGDX sources: no JNI and no OpenGL are loaded or required.

## Components

| File | Role |
| --- | --- |
| `gdx/src/com/badlogic/gdx/graphics/g3d/utils/BlendWeightSolver.java` | Public API: observations, preparation, no-write evaluation, simplex solver and result report. |
| `gdx/src/com/badlogic/gdx/graphics/g3d/utils/WeightedAnimationPose.java` | Package-private multi-clip forward pose (TRS sampling, blending, hierarchy and analytic Jacobian). Owned per preparation. |
| `gdx/src/com/badlogic/gdx/graphics/g3d/utils/BaseAnimationController.java` | New public `applyWeightedAnimations(...)`; it drives the **same** forward pose implementation as the solver. |
| `tests/BlendWeightSolverTest.java` | Five `Checks` groups (`forward`, `jacobian`, `simplex`, `integration`, `invalid`); no `main`. |
| `examples/BlendWeightFit.java` | Default-package demo with `main`; prints `BLEND_WEIGHT_FIT_DEMO_OK`. |

The existing `AnimationController` and the two-clip `applyAnimations(...)` blend path are unchanged and keep working.

## Forward model

For each node and each clip, channels are sampled exactly like the existing controller, except that the sample time is
**clamped to the channel's own first/last key time** instead of implicitly holding an end key:

* translation and scale channels: linear interpolation;
* rotation channels: shortest-arc slerp;
* a missing channel, or a clip without a track for that node, contributes the node's **rest TRS**.

Per node the clips are then blended with the shared weights:

* translation and scale are direct weighted sums;
* each sampled rotation is first aligned to the hemisphere of the node's **rest rotation**, then
  `q = normalize(sum_k w_k * q_k)`;
* the full affine hierarchy is rebuilt from the real parent/child relations and `Node.inheritTransform`.

All rest and sampled scales must be strictly positive and every quaternion finite/non-zero. The absolute dot product of
every sampled rotation with the node's rest rotation must be greater than `1e-6`; otherwise the hemisphere is ambiguous
and preparation throws `IllegalArgumentException`.

For observation `o`, with skin weights `s_j`, slots `j`, bind point `b` and instance transform `T`, the modeled world
point is

```
p(w) = T * sum_j s_j * (G_j(w) * inverseBind_j * b)
```

where `G_j(w)` is the bone node's global transform and the slots index the part's `invBoneBindTransforms` order.

`Evaluation` returns:

* `worldPoints[3*o + axis]` — world point per observation (`axis` 0=x, 1=y, 2=z);
* `jacobian[o][3*k + axis]` — analytic derivative `d p_o(axis) / d w_k`; rows belong to observations in preparation
  order and the three axes of one clip are contiguous. The Jacobian is derived analytically (including the normalized
  quaternion blend and the full hierarchy); no perturbation, re-sampling or finite differences are used internally, and
  evaluation never writes to nodes, palettes or playback state.

## Usage

```java
Animation[] clips = {clipA, clipB};
float[] times = {0.5f, 1.0f};                 // fixed sample seconds per clip

BlendWeightSolver.Observation obs = new BlendWeightSolver.Observation();
obs.part = skinnedPart;                       // must belong to the target ModelInstance
obs.bindPoint.set(0.3f, -0.4f, 0.6f);
obs.setBoneSlots(new int[] {0, 1, 2, 3});     // indices into part.invBoneBindTransforms
obs.setSkinWeights(new float[] {0.35f, 0.25f, 0.25f, 0.15f}); // >= 0, sum = 1
obs.target.set(worldTargetX, worldTargetY, worldTargetZ);
obs.weight = 1f;                              // strictly positive

// 1) Prepare: clips, times, observations are validated and snapshotted.
BlendWeightSolver.Preparation preparation =
    BlendWeightSolver.prepare(instance, clips, times, new BlendWeightSolver.Observation[] {obs});

// 2) No-write evaluation: world points and the analytic Jacobian.
BlendWeightSolver.Evaluation evaluation = preparation.evaluate(new double[] {0.5, 0.5});

// 3) Solve on the simplex and apply through the regular node/palette pipeline.
BlendWeightSolver.Result result = BlendWeightSolver.solveAndApply(
    preparation,
    new double[] {0.5, 0.5},   // initial: finite, non-negative, summing to one
    new double[] {0.5, 0.5},   // prior:     same simplex
    0.001,                     // lambda > 0
    1e-9,                      // tolerance > 0 for the projected-gradient residual
    400);                      // maxIterations >= 0
```

The controller entry uses the identical forward pose and commits it to the instance:

```java
baseAnimationController.applyWeightedAnimations(clips, times, new double[] {0.7, 0.3});
```

After a solve, ordinary animation playback can take over again (e.g. `AnimationController#update`); nothing about the
solver keeps persistent state on the instance. Two instances may be prepared and solved interleaved — every preparation
owns its snapshots and scratch arrays.

## Objective and stopping conditions

The objective, minimized on the probability simplex `{ w >= 0, sum w = 1 }`, is

```
F(w) = 0.5 * sum_o observationWeight_o * ||p_o(w) - target_o||^2
       + 0.5 * lambda * ||w - prior||^2
```

with `lambda > 0` and both `initial` and `prior` on the simplex. Each iteration is a projected-gradient step with
backtracking (Armijo) line search:

* every evaluated point is projected onto the simplex (Duchi-style projection) — negatives are never just clipped and
  renormalized from an unconstrained solve;
* a step is accepted only when `F` does not increase;
* the report contains independently owned `weights`, `objective`, `acceptedIterations`,
  `projectedGradientResidual = ||w - ProjectSimplex(w - gradientF)||_2` and `converged`;
* `converged` is `true` **only** when the residual is at most the caller's positive `tolerance`. The problem is not
  assumed convex, so no global optimum is guaranteed;
* if the iteration budget is exhausted (or no acceptable step exists), the last feasible point is returned **and
  applied**, with `converged == false`.

Repeated clips, rank-deficient Jacobians, zero skin weights and boundary optima (some weight exactly zero) are all
supported. Candidate, objective and gradient arithmetic uses `double`; only the final node TRS and palette are stored
as `float`. Committed world points agree with the no-write candidate evaluation within `2e-4`.

## Validation and failure semantics

`prepare(...)` and `solveAndApply(...)` throw `IllegalArgumentException` for:

* duplicate node tracks inside a clip, or non-strictly-ascending key times;
* a node part, bone or animated node that does not belong to the target `ModelInstance`;
* bone slots out of range, wrong slot/weight dimensions;
* non-finite values, negative/non-summing skin weights, non-positive observation weights;
* a singular (non-invertible) `instance.transform`;
* non-positive rest/sampled scales, zero quaternions, or rotation samples whose rest-rotation dot product is at most
  `1e-6` in absolute value;
* simplex, `lambda`, `tolerance` or iteration-count violations for the optimizer.

On failure nothing is committed: the model, any previously obtained report, the preparation and the caller's arrays
stay unchanged. A successfully applied pose goes through `ModelInstance#calculateTransforms()`, so the real
`Node#calculateBoneTransforms` palette and the solver's candidate evaluation describe the same pose.

## Build and test

```bash
bash task_support/run.sh target    # compiles the new production files, runs the five test groups and the demo
bash task_support/run.sh baseline  # keeps the existing local-matrix / inverse-bind / world-transform numeric checks
```

The target run requires the five group banners `GROUP <name> checks=<n> COMPLETE`, the `TARGET_COMPLETE groups=5`
line and the demo's `BLEND_WEIGHT_FIT_DEMO_OK` marker.
