# LightTree: deterministic hierarchical point lights

`src/raytracer/lighttree.h/.c` adds a read-only bounding hierarchy for
caller-supplied point lights.  Each `PointLight` contains repository `v3`
`position` and `intensity` fields.  All error budgets and result channels use
`double`; the input vectors remain `float`-based `v3`.

The channel is deterministic and does not replace or wrap the random sampler.
It does not call `get_light_point`, consume random numbers, add specular
lighting, or alter area lights, ambient light, reflection, refraction, or path
weights.

## API

```c
LightTree *lighttree_build(const PointLight *lights, size_t count);
void lighttree_free(LightTree *tree);

void lighttree_eval(const LightTree *tree,
                    const v3 point,
                    const v3 normal,
                    const v3 diffuse,       /* [0,1] per channel */
                    double offset,          /* strictly positive */
                    double abs_tol,
                    double rel_tol,
                    LightResult *result);
```

`lighttree_build` copies the light array into a balanced spatial hierarchy.
Positions must be finite and intensities must be finite and non-negative.
`count == 0` returns a valid empty tree; evaluating it gives zero color,
bounds and counters, and `lighttree_free` safely releases it.

After construction the tree is immutable and may be shared.  All traversal
heaps, active-node marks, accumulated intervals and counters are local to the
single `lighttree_eval` call.

`LightResult` contains:

- `double color[3]`: the current lower estimate (see below);
- `double lower[3]`, `upper[3]`: the global inclusion interval;
- `size_t bound_evals`: conservative node-bound calculations;
- `size_t leaf_evals`: exact per-light leaf evaluations;
- `size_t shadow_queries`: actual calls to `is_light_blocked`.

## Exact leaf term

For light `i`, channel `c`, and shading point `p`, the unoccluded reference
term is

```text
diffuse[c] * intensity_i[c] * max(0, n . wi) / (offset + distance_i^2).
```

Each non-coincident evaluated leaf constructs the same finite shadow segment
used by the production renderer and calls

```c
is_light_blocked(&ray, distance, transmission, NULL);
```

with `transmission` initialized to `{1,1,1}` and a `NULL` emitter object.
An opaque blocker makes the contribution zero.  Otherwise the mutated `v3` is
the colored transmission; the implementation clamps it component-wise to
`[0,1]`.  Coincident lights contribute zero and do not issue a zero-length
shadow query.

## Interval basis

Every hierarchy node stores an axis-aligned bounding box and summed RGB power.
For an unresolved node `N`, visibility is treated conservatively:

```text
lower_N[c] = 0
upper_N[c] = diffuse[c] * power_N[c] * ||n|| / (offset + d_min(N,p)^2)
```

where `d_min` is the Euclidean nearest distance from `p` to the bounding box.
For a unit normal, `||n||` is one; retaining the computed length also covers
ordinary float-normal rounding.  Since

```text
max(0, n . wi) <= ||n||
```

and every point in the box is at least `d_min` away, the bound encloses every
leaf geometric factor.  If the whole box is robustly in the negative normal
half-space, its upper bound is zero.  The half-space test has a small
scale-dependent conservative band so tangent/rounding-sensitive boxes are not
culled.

The query maintains a cut of active hierarchy nodes:

```text
lower[c] = sum of exactly evaluated transmitted leaf contributions
upper[c] = lower[c] + sum of upper bounds of unresolved cut nodes
```

No visibility result or representative light is generalized to a cluster.
A node is either retained with lower bound zero or expanded.  Node bounds are
formed from power and geometry alone; they do not wait for shadow results for
all lights.

At every iteration the query checks the **global** condition

```text
unresolved[c] <= abs_tol + rel_tol * lower[c]
```

for every RGB channel.  It expands the active node with the highest normalized
global urgency in a violating channel.  Thus a large local relative error is
not treated as a global stopping certificate, and RGB imbalance is handled
channel by channel.  Long-double/Kahan accumulation and direct scans of the
active cut reduce cancellation when many small terms coexist with a bright
light.

The returned `color` equals `lower`.  Consequently it lies in the interval
and its distance to the upper endpoint is the unresolved width, which the
global stopping test bounds by `abs_tol + rel_tol * lower[c]`.  The test suite
adds the specification's rounding allowance

```text
2e-6 * max(1, |exact_reference[c]|)
```

when comparing against an independent per-light reference.

When both tolerances are zero, a deterministic recursive traversal visits the
whole hierarchy and evaluates every leaf exactly.  Repeated calls with
identical inputs produce identical values and counters; ordering and heap ties
are deterministic.  Counters are incremented at the corresponding operations
rather than inferred after a full scan.

## Renderer integration

`render.h` exposes:

```c
void render_set_point_lights(const PointLight *lights, size_t count,
                             double offset, double abs_tol, double rel_tol);
void render_clear_point_lights(void);
```

The setter builds a private snapshot tree and frees the previously installed
tree after replacement.  The point-light offset is independent of the old
area-light attenuation setting.

In `cast_ray`, only a front-facing hit (`is_outside`) fetches the actual
texture color and calls `lighttree_eval`.  `LightResult.color` is added to
`obj_color` before multiplication by the inherited path weight `kr` and before
the existing camera-distance attenuation.  Therefore texture color, path
weights and camera distance affect the new diffuse term in the same places as
the legacy direct term.  No Phong/Blinn specular term is generated for these
lights.

With no tree installed, or after `render_clear_point_lights`, the new code
performs no texture query or random call, so colors and the `rand()` stream
follow the old path exactly.

## Limitations

- The bounds are deterministic and conservative, not unbiased or
  Monte-Carlo estimates.
- An upper bound intentionally ignores visibility and can therefore be loose;
  a very small tolerance may expand many or all leaves and issue one shadow
  query per evaluated leaf.
- Zero tolerance is exact with respect to the float geometry/shadow seam and
  double-precision evaluation, subject to the stated `2e-6` comparison margin.
- `LightTree` itself is read-only, but `is_light_blocked` uses the renderer's
  existing global object/BVH state.  Concurrent queries are safe only under
  the same assumptions as concurrent production shadow rays against that
  immutable scene.
- The channel adds diffuse direct illumination only.

## Build and run the required tests

From the repository root, with only `cc`, `ar`, `bash` and standard C11/LIBM
dependencies:

```sh
bash task_support/run.sh target
```

This builds the sources, links `task_support/driver.c` with
`tests/lighttree_test.c`, requires completed `bounds`, `visibility` and
`transport` groups, and then runs the retained production baseline.

The test file exports `task_cases` and `task_case_count` and has no `main`.
It checks:

- empty trees and safe free;
- independent per-light reference inclusion;
- global absolute/relative tolerance;
- zero-tolerance full leaf expansion;
- duplicate coordinates and cross-magnitude RGB intensities;
- deterministic repeat queries and snapshot copying;
- partial occlusion by real sphere/BVH geometry;
- colored transparent transmission;
- 4096 distant dim lights plus a separated near bright light with work below
  the required `n/8` leaf/shadow and `n/2` bound limits;
- `cast_ray` texture coloring, camera-distance attenuation, replacement and
  clearing;
- the old emissive-object transport and random-stream control.

## Example

Build the target harness once to produce `.task-build/libscene.a`, then:

```sh
cc -std=c11 -O2 -DUNBOUND_OBJECTS \
   -I. -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -Ilib/cJSON \
   examples/lighttree_demo.c .task-build/libscene.a -lm \
   -o .task-build/lighttree_demo
.task-build/lighttree_demo
```

The example constructs real receiver and colored-filter spheres plus 4096
lights, performs a query, and prints RGB color, lower/upper bounds and the
three work counters.

## Checked degradation

A compile-time mutation seam is included in `lighttree.c`: building with

```sh
-DLIGHTTREE_DROP_COLORED_TRANSMISSION
```

keeps opaque shadows but forces colored transmission to one.  It still
compiles and runs; its colors differ from the filtered reference.  The
assertion that detects it is the zero-tolerance global interval check in the
`transport/colored_transmission` case:

```c
TASK_CHECK(interval_matches_reference(&result, &reference, 0.0, 0.0));
```

followed by the explicit per-channel red/green/blue comparisons.  Run the
packaged mutation check with:

```sh
bash task_support/mutation_check.sh
```

It expects the mutant test binary to fail on that transport interval assertion
and reports `MUTATION_DETECTED`.
