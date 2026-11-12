# Stochastic queries on `VolumeGridDensity`

`volumes/volumegrid.h` adds two stochastic point queries to the existing
density grid, built on a shared, read-only per-brick majorant index.  The
classic fixed-step `tau()`, `Density()`, `sigma_t()` and `IntersectP()`
are unchanged.

## Types

- `GridTrackingStats` — diagnostic counters, reset to zero at the start of
  every call that receives one:
  - `brick_visits` — majorant bricks entered by the traversal (including
    empty bricks that were skipped without any work);
  - `density_evaluations` — actual trilinear density evaluations;
  - `null_events` — virtual collisions rejected by `SampleDistance()`'s
    accept/reject test.  `RatioTransmittance()` performs no rejection, so
    its candidate events are never counted here.
- `GridDistanceSample` — result of `SampleDistance()`: `hit`, the ray
  parameter `t`, and the world-space `position`.  On escape `hit == false`
  and `t == ray.maxt` (`position` then unspecified).

## Methods

```cpp
GridDistanceSample SampleDistance(const Ray &ray, float extinction,
    RNG &rng, GridTrackingStats *stats = NULL) const;
Spectrum RatioTransmittance(const Ray &ray, RNG &rng,
    GridTrackingStats *stats = NULL) const;
```

- `SampleDistance()` samples the first real collision of the
  non-homogeneous Poisson process with rate `extinction * density` along
  the ray (delta tracking).  The exponential free path is measured in
  **world-space length**; reported `t` values are always parameters of the
  caller's ray `O + t D`, so multiply by `|D|` to recover world distances.
- `RatioTransmittance()` returns a single-sample unbiased estimate of
  `exp(-tau)` along the clipped segment (ratio tracking), using the
  region's RGB extinction `sig_a + sig_s`.  Every channel is non-negative.
- The ray is clipped to the intersection of its own `[mint, maxt]` with
  the grid bound before tracking; `maxt` may be `+INFINITY`.
- Vacuum (zero extinction), non-intersecting rays and zero-length segments
  return escape / all-ones transmittance immediately and consume **no**
  RNG values.  Bricks whose majorant is zero are skipped the same way.
- Any invertible affine `VolumeToWorld` (translation, non-uniform scale,
  shear, …) is supported; the walk happens in volume space with an
  exact world-length rescaling.

## Brick majorants

The index is built once in the constructor and never allocates or mutates
during queries.  Each brick covers at most 4 interpolation intervals
(gaps between adjacent density centers) per axis, including the outermost
half-cells, and its bound is the maximum over all samples that participate
in an interpolation inside the brick — including neighbor samples just
outside the brick — so it always bounds the trilinear interpolant.  Brick
boundaries are crossed without reusing the previous brick's bound and
without processing any boundary twice.

## Errors

The constructor throws `std::invalid_argument` for negative or non-finite
density values, non-positive grid dimensions, degenerate extents,
negative/non-finite extinction coefficients, and non-finite or singular
`VolumeToWorld` transforms.  The queries throw `std::invalid_argument`
for malformed rays (non-finite origin/direction, zero direction,
`mint > maxt`, NaN limits) and for negative or non-finite `extinction`.

## Testing and example

- `checks/target_tests.cpp` — regression suites `freeflight`, `ratio`,
  `majorant`, `affine`, `legacy` (run via `bash checks/target.sh`).
  Expectations come from analytic escape probabilities / first-collision
  CDFs (uniform media), analytic exponentials (colored transmittance) and
  an independent high-precision line integral of `sigma_t()` (varying
  media); statistical checks print mean and standard error with
  standard-error-based tolerances.
- `examples/grid_transmittance_ppm.cpp` — windowless PPM renderer whose
  pixel colors are averages of real `RatioTransmittance()` samples.
