# Ray Differentials and Texture Footprints

This note describes the screen-space ray differentials and mip-mapped texture
filtering added to the `TheRestOfYourLife` renderer. The feature is **off by
default** and lives entirely on the existing production path; no separate
integrator is used, and no neighboring camera rays are ever traced to estimate
a footprint.

## Coordinate conventions

The screen coordinates are the pixel coordinates used by `camera::sample_ray`
and `pixel00_loc`: the center of pixel `(i, j)` is at `(i, j)`, one full pixel
apart horizontally and vertically. Every differential is a derivative with
respect to one of these two screen axes:

| Quantity            | Meaning                                  |
|---------------------|------------------------------------------|
| `ray::do_dx/dy`     | `d origin / d screen_x`, `d origin / d screen_y` |
| `ray::dd_dx/dy`     | `d direction / d screen_x`, `d direction / d screen_y` |
| `hit_record::dpdx/dpdy` | `d hit_point / d screen_x/y`        |
| `hit_record::dndx/dndy` | `d unit_normal / d screen_x/y`      |
| `texture_footprint::dudx,dvdx,dudy,dvdy` | `d(u,v) / d screen_x/y` |

Consequences of these conventions:

- **Direction is not normalized.** As with ordinary rays in this codebase, the
  differentiated direction is a free vector; `dd_*` differentiates that same
  unnormalized direction. Specular materials differentiate the normalization
  explicitly before reflecting or refracting.
- **Ray time is held constant.** Neighboring screen coordinates reuse the same
  lens sample and the same time, so a moving sphere's center `C(time)` does not
  move with the screen coordinate. Its velocity therefore never enters the
  screen-space surface derivatives; the sphere still moves for the central ray.
- **The lens sample is held constant.** With a fixed point in the unit defocus
  disk, the ray origin does not vary with the screen coordinate, so
  `do_dx = do_dy = 0`; the direction changes by exactly one pixel delta
  (`dd_dx = pixel_delta_u`, `dd_dy = pixel_delta_v`). The sub-pixel jitter
  only selects where the ray is evaluated, not the derivatives.
- **The scale is one full pixel.** Because adjacent screen coordinates differ
  by one pixel, the differential magnitude is independent of
  `samples_per_pixel`; increasing the sample count never shrinks the footprint.

## How the footprint is produced

1. `camera::enable_differentials = true` makes `sample_ray` tag each ray and
   set the four ray derivatives. The default `false` leaves rays byte-for-byte
   on the old path, including the sequence of random numbers consumed.
2. Each primitive differentiates its own intersection:
   - **Sphere:** the implicit surface
     `|O + tD - C|² - R² = 0` is differentiated at fixed time, giving
     `dt/ds`, then `dP/ds` and the unit normal derivative `dn/ds = dP/ds / R`.
     The `1/R` factor is the curvature term — the sphere is not treated as a
     flat, constant-normal plane. UV derivatives come from differentiating the
     `atan2`/`acos` mapping directly, so they are local across the `u = 0/1`
     meridian seam rather than jumping by a whole wrap.
   - **Quad:** the plane equation `n·P = D` gives `dt/ds` and `dP/ds`; the
     normal is constant so `dn/ds = 0`, and the planar `u,v` coordinates are
     linear in `P`.
   - **`translate` / `rotate_y`:** differentials are transformed together with
     the ray (in) and the hit point and normal (out); translation leaves
     direction derivatives unchanged. BVH and list traversal preserve the full
     record without re-transforming or dropping it.
3. Ideal specular events propagate the footprint into `skip_pdf_ray`, following
   the exact reflection/refraction branch chosen for the central ray:
   - perfect metal reflection,
   - dielectric reflection (including total internal reflection),
   - dielectric refraction, differentiating the incident normalization, the
     curved-surface normal change, and the interior/exterior index ratio.
   No extra random numbers are drawn for auxiliary directions, and the
   reflection and refraction branches are never chosen twice.
4. `lambertian::scatter` and `diffuse_light::emitted` pass the hit's
   `texture_footprint` to the texture query, which `mip_image_texture` turns
   into a level of detail.

## LOD selection

For a valid footprint, the footprint size in original (level-0) texels is

```
rho_x = |(width  * dudx, height * dvdx)|
rho_y = |(width  * dudy, height * dvdy)|
rho   = max(1, rho_x, rho_y)
LOD   = clamp(log2(rho), 0, top_level)
```

Each level is bilinearly sampled (texel centers on half-integers, U wraps, V
clamps and grows upward), and the two bracketing levels are trilinearly
blended by the fractional LOD. The mip pyramid is built at construction from
2×2 linear means; an axis that has reached one texel repeats its edge texel
down to the 1×1 level.

## Invalidation scope

A footprint is an all-or-nothing chain from the camera to the texture query.
It is explicitly invalidated (and its storage zeroed) when:

- the incoming ray has `has_differentials == false`;
- a ray is tangent to a surface (the implicit-function denominator vanishes),
  or any intermediate derivative is non-finite;
- a **sphere pole** is hit — only the UV footprint is undefined there, while
  the position and normal derivatives remain valid;
- a **volume scattering** event occurs (`constant_medium`), which has no
  surface differential;
- a material breaks directional coherence: non-zero metal fuzz, Lambertian
  (diffuse) scattering, or isotropic phase scattering;
- refraction reaches the critical-angle band, where the mapping is not
  differentiable. The central ray still scatters; only the derivatives clear.

Invalid UV footprints and the ordinary three-argument `value(u,v,p)` calls
sample level 0 bilinearly. A non-finite derivative is treated as invalid and
is never used to index the mip levels. Reusing a `hit_record` can never retain
a previous hit's flag or values: every primitive clears the record before
filling it, and misses clear it as well.
