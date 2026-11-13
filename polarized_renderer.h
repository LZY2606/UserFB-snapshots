#pragma once
// Polarization-aware specular-only renderer.
//
// This path follows only the mirror reflection channel at smooth dielectric
// sphere interfaces.  Energy that is not reflected (transmission, absorption)
// leaves the path: there is no refraction ray and no volume transport.  Sphere
// intersection reuses the exact ray_sphere_intersect() implementation from
// geometry.h, which the scalar renderer also uses.
//
// Direction conventions
// ---------------------
// A camera ray direction D points *into* the scene, along the traced ray.
// Light physically propagates the other way: its Stokes state always carries
// k = -D.  At a hit the outward geometric normal decides which side the ray
// comes from; reflection never crosses the interface, so the ray stays in the
// same medium and n1/n2 are chosen from that side.

#include "polarization.h"
#include "geometry.h"
#include <array>
#include <vector>

namespace pol_render {

struct PolarSphere {
    Sphere sphere;                 // shared geometry, intersected by ray_sphere_intersect
    double n_outside = 1.0;        // refractive index on the outward side
    double n_inside  = 1.5;        // refractive index on the inward side
};

struct CameraRay {
    dvec3 origin{0, 0, 0};
    dvec3 dir{0, 0, -1};           // points into the scene; k of the returned light is -dir
};

struct Background {
    // One Stokes vector per RGB channel.  It is interpreted in the
    // deterministic world-axis projected frame of the propagation direction
    // (see PolarScene::reference_axis).
    std::array<pol::Stokes, pol::CHANNELS> stokes{};
};

class PolarScene {
public:
    std::vector<PolarSphere> spheres;
    Background background;
    int max_reflections = 8;       // number of actual surface reflection events

    // Deterministic transverse reference axis for any propagation direction k.
    // Rule: take the world axis a in {x, y, z} with the smallest |dot(k,a)|,
    // project it onto the plane perpendicular to k and normalize:
    //   e1 = normalize(a - k * dot(k, a));  e2 = cross(k, e1).
    // Background light and every top-level trace use this frame, so separate
    // traces and camera rolls never reuse a frame built for another ray.
    static dvec3 reference_axis(const dvec3& k);

    // Trace one camera ray and return the full Stokes state of the light
    // arriving at the camera.  The returned propagation direction is
    // k = -normalize(ray.dir) with e1 = reference_axis(k).  A miss returns
    // the background; a hit with no reflection budget left returns zero.
    pol::PolarState trace(const CameraRay& ray) const;

private:
    // Returns the light carried back along the ray (orig, D) expressed in the
    // caller frame (k = -D, e1_out).  depth counts reflection events already
    // performed by parent calls.
    pol::PolarState trace_impl(const dvec3& orig, const dvec3& D,
                               const dvec3& e1_out, int depth) const;
};

} // namespace pol_render
