# Hierarchical point-light integrator

`src/raytracer/lighttree.{h,c}` adds a deterministic point-light channel to
the ray tracer.  Unlike the legacy area lights, it never calls
`get_light_point` and never consumes a random number: each shading point is
answered from a read-only spatial tree with a per-channel numeric interval
whose width is bounded by an inspectable, caller-supplied error budget.

## Exact reference

For point light *k* and channel *c*, the exact diffuse direct-light
contribution at shading point `p` with unit normal `n`, albedo `diffuse` and
denominator `offset` is

```
diffuse[c] * intensity_k[c] * max(0, n . wi_k) / (offset + |p - position_k|^2)
            * transmission_k[c]
```

where `transmission` is the colored result of the production shadow path
`is_light_blocked(ray, distance, transmission, NULL)`.  Transparent objects
multiply their material `kt` into `transmission` (clamped to `[0,1]`), an
opaque hit yields zero, and a light coincident with `p` contributes zero.
The exact answer is the per-channel sum of these contributions.

## How the interval is built

* `lighttree_build` snapshots the input lights (positions and intensities are
  copied) and constructs a binary median-split spatial tree.  Every node
  stores its axis-aligned bounding box and its aggregated per-channel power.
  The tree is immutable after construction and may be shared read-only; all
  per-query state (the active partition and its work counters) is local to one
  `lighttree_eval` call.
* Each unresolved node contributes a conservative interval `[0, upper]`.
  The lower bound is zero because visibility is not assumed coherent: a
  representative light's shadow result must never be promoted to a whole
  cluster.  The upper bound assumes every light is fully visible and combines
    - the squared distance from `p` to the closest point of the node box,
    - the half-space support of `n` over that box (clamped to 1),
    - the node's aggregated per-channel power,
    - the same `offset + distance^2` denominator.
  These bounds are computed without shadow results for any light in the node.
* Resolved leaves contribute their exact value on both interval ends using
  the real shadow path (emitter argument `NULL`).
* The integrator always refines the active node whose remaining per-channel
  interval is largest until, on every channel,

```
upper[c] - lower[c] <= abs_tol + rel_tol * lower[c]
                      + 2e-6 * max(1, exact[c])      (floating-point margin)
```

  or until every leaf has been evaluated.  With both tolerances zero every
  cluster is expanded and the interval collapses onto the exact leaf sum.

The returned `color` is the interval midpoint, so it always lies inside
`[lower, upper]`.  `bound_evals`, `leaf_evals` and `shadow_queries` count the
node bounds computed, exact leaf evaluations and production shadow-path
queries actually performed; nothing is scanned without being counted.
Repeating a query on the same tree yields identical values and counters.

## Guarantees and limitations

* The interval encloses the exact sum per channel; the budget is global (it
  is checked against the summed interval, not against each node), so uneven
  RGB power and many tiny terms cannot hide accumulated local error.
* Only diffuse direct lighting is added.  Legacy area lights, the Phong/Blinn
  specular term, ambient light, path tracing, reflection and refraction are
  unchanged, and the point-light offset is independent of the legacy
  attenuation offset.
* Conservative bounds trade tightness for safety: when most lights are dark
  and distant, whole clusters are accepted from their power aggregate, but a
  field of individually bright lights may still require many leaf
  evaluations to meet a tight tolerance.  No stochastic sampling is involved,
  so the same inputs always produce the same work.
* Inputs are validated: finite positions, finite non-negative intensities,
  unit normal, albedo in `[0,1]`, strictly positive offset and non-negative
  tolerances.  An empty light set returns an all-zero, safely freeable tree.

## Renderer binding

`render_set_point_lights(lights, count, offset, abs_tol, rel_tol)` copies the
lights into a new tree and atomically replaces the previous one;
`render_clear_point_lights()` removes the channel, after which output and the
random stream match the legacy renderer.  At every front-facing hit the
channel is evaluated with the actual textured color and geometric normal and
its `color` is added to the surface color, then passes through the existing
path weight (`kr`) and camera distance attenuation like every other term.

## Offline build and run

Only a C11 compiler, the standard library, libm and the sources in this
repository are required.  From the repository root:

```sh
# Task tests (bounds / visibility / transport) plus the production baseline
bash task_support/run.sh target

# Standalone demo: build the static library pieces the same way run.sh does,
# then link the example against them
cc -std=c11 -O2 -DUNBOUND_OBJECTS \
   -I. -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -Ilib/cJSON \
   examples/lighttree_demo.c .task-build/libscene.a -lm \
   -o .task-build/lighttree_demo
.task-build/lighttree_demo
```

The demo constructs a receiver sphere and a colored transparent filter,
installs 258 point lights, and prints the interval, midpoint color and work
counters for both a loose absolute budget and zero tolerance.

## Mutation test for the colored transmission

`tests/lighttree_test.c` contains a deliberately degraded transport
(`degraded_channel_eval`): it still walks the real shadow path and still
zeros fully occluded lights, but it discards the per-channel transmission
returned through the shadow intensity argument and treats every transparent
segment as perfectly clear.  It compiles and runs like the real path and
produces visibly different colors through the colored filter.

The content assertion that detects it is the zero-tolerance exactness check
used throughout the suite — at `abs_tol = rel_tol = 0` every channel must
equal the independent per-light reference within the `2e-6` rounding margin.
With the filter `kt = (.5, .25, .75)` the degraded build overestimates all
three channels (the green channel worst, by a factor of four), so the
`visibility/degraded_transmission_detected` case fails on that assertion;
the opaque-occlusion behavior alone cannot catch this defect.
