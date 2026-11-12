#pragma once
// Participating-medium helpers: density lookup, optical path length, transmittance,
// and deterministic (fixed-quadrature) single scattering.
#include "renderer.h"

// One axis-aligned density grid. Vertices are x-fastest, i.e.
// index = (z * ny + y) * nx + x, with non-negative finite densities.
struct DensityGrid {
    vec3 box_min;
    vec3 box_max;
    int nx = 1, ny = 1, nz = 1;     // all >= 2
    std::vector<double> density;    // nx*ny*nz, non-negative finite
};

// Optional participating medium: the density grid plus its appearance scalars.
struct Volume {
    DensityGrid grid;
    double sigma = 0;               // extinction coefficient
    vec3 albedo = {0, 0, 0};         // single-scattering albedo per channel, [0,1]
};

// Throws std::invalid_argument if the grid or its scalars are malformed. Called
// before any rendering starts so a bad medium never surfaces from a deep
// recursive branch.
void validate_volume(const Volume &volume);

// Line integral of density over the segment between a and b, clipped to the
// grid box. The result is symmetric in a and b (direction reversal gives the
// same value). Returns 0 for zero-length segments and for segments that do not
// pass through the box's interior.
double density_line_integral(const DensityGrid &grid, const vec3 &a, const vec3 &b);

// T(a, b) = exp(-sigma * integral of density from a to b).
double transmittance(const Volume &volume, const vec3 &a, const vec3 &b);

// In-scattered radiance arriving at `origin` from direction `dir`, integrated
// over the segment [origin, origin + dir*t_end] (t_end >= 0), further clipped to
// the density box. Lights are RGB point-light powers placed outside the box.
// Only single scattering with entity shadows (same rule as the surface shadow
// test) is accumulated; there is no multiple scattering and no random sampling,
// so the result is deterministic.
vec3 single_scatter(const Volume &volume, const Scene &scene,
                    const vec3 &origin, const vec3 &dir, double t_end);

// Convenience wrapper around the three above, mirroring the cast_ray signature:
// returns the fog contribution plus T times the supplied surface radiance.
vec3 apply_medium(const Volume &volume, const Scene &scene,
                  const vec3 &orig, const vec3 &dir, double t_end,
                  const vec3 &surface_radiance);

// Distance at which a ray starting at `orig` leaves the density box (0 if it
// starts outside and points away). Used to bound fog segments that never hit a
// surface.
double medium_exit_distance(const Volume &volume, const vec3 &orig, const vec3 &dir);

// Medium policy plugged into the shared recursive caster in renderer.h.
struct MediumFog {
    const Volume *volume = nullptr;
    const Scene  *scene  = nullptr;
    bool active() const { return volume && volume->sigma > 0.0; }
    vec3 apply(const vec3 &orig, const vec3 &dir, double t_end, const vec3 &surface) const;
    float light_transmittance(const vec3 &point, const vec3 &light) const;
    double exit_distance(const vec3 &orig, const vec3 &dir) const;
};

// Fully medium-aware entry point. Same semantics as cast_ray(orig, dir, depth,
// scene), with fog integrated per segment.
vec3 cast_ray(const vec3 &orig, const vec3 &dir, int depth,
              const Scene &scene, const Volume &medium);
