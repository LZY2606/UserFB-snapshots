#include "polarized_renderer.h"
#include <cmath>

namespace pol_render {

dvec3 PolarScene::reference_axis(const dvec3& k) {
    const dvec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    int best = 0;
    double best_abs = std::abs(k.x);
    if (std::abs(k.y) < best_abs) { best = 1; best_abs = std::abs(k.y); }
    if (std::abs(k.z) < best_abs) { best = 2; best_abs = std::abs(k.z); }
    const dvec3 a = axes[best];
    dvec3 e = a - k * (a * k); // project the world axis onto the k-perpendicular plane
    return e.normalized();
}

pol::PolarState PolarScene::trace(const CameraRay& ray) const {
    dvec3 D = ray.dir.normalized();
    dvec3 k_out = D * -1.0;                 // returned light propagates toward the camera
    dvec3 e1 = reference_axis(k_out);       // fresh deterministic frame for this ray
    return trace_impl(ray.origin, D, e1, 0);
}

pol::PolarState PolarScene::trace_impl(const dvec3& orig, const dvec3& D,
                                       const dvec3& e1_out, int depth) const {
    dvec3 k_out = D * -1.0;
    pol::PolarState result;
    result.k = k_out;
    result.e1 = e1_out;
    for (auto& st : result.s) st = pol::Stokes{0.0, 0.0, 0.0, 0.0};

    // Sphere intersection through the shared float implementation.
    const PolarSphere* hit = nullptr;
    float nearest = 1e30f;
    vec3 forig = to_vec3(orig);
    vec3 fdir = to_vec3(D);
    for (const PolarSphere& ps : spheres) {
        auto [found, t] = ray_sphere_intersect(forig, fdir, ps.sphere);
        if (found && t < nearest) {
            nearest = t;
            hit = &ps;
        }
    }

    if (hit == nullptr) {
        // Miss: background in its own deterministic world-projected frame,
        // rebased into the caller's output frame.
        dvec3 e1_bg = reference_axis(k_out);
        pol::PolarState bg;
        bg.k = k_out;
        bg.e1 = e1_bg;
        bg.s = background.stokes;
        return pol::rebase(bg, k_out, e1_out);
    }

    if (depth >= max_reflections)
        return result; // hit surface but the reflection budget is exhausted: zero

    dvec3 P = orig + D * static_cast<double>(nearest);
    dvec3 C = to_dvec3(hit->sphere.center);
    dvec3 N = (P - C).normalized(); // outward geometric normal

    // The ray direction versus the outward normal fixes the incident side.
    // Reflection stays in the incident medium, so no medium is crossed.
    bool inside = (D * N) > 0.0;
    double n1 = inside ? hit->n_inside  : hit->n_outside;
    double n2 = inside ? hit->n_outside : hit->n_inside;

    // Shared s axis for both interface frames; deterministic fallback at
    // normal incidence where cross(N, k_out) vanishes.  The second axis of
    // each frame is the p direction, p_out = cross(k_out, s) and
    // p_in = cross(k_in, s); PolarState::e2() rebuilds it on demand, so the
    // frames are fully described by (k_out, s) and (k_in, s).
    dvec3 sc = cross(N, k_out);
    dvec3 s = (sc.norm() < 1e-12) ? pol::perpendicular_axis(k_out)
                                  : sc.normalized();

    dvec3 Dr = reflect(D, N).normalized(); // reflected ray direction, still traced away from camera
    dvec3 k_in = Dr * -1.0;

    // Light coming back along the reflected ray, in a deterministic frame...
    dvec3 e1_child = reference_axis(k_in);
    pol::PolarState child = trace_impl(P, Dr, e1_child, depth + 1);
    // ...rotated into the interface s/p basis before the Mueller matrix acts.
    pol::PolarState incoming = pol::rebase(child, k_in, s);

    double ci = std::abs(D * N);
    if (ci > 1.0) ci = 1.0;
    pol::Mueller M = pol::reflection_mueller(n1, n2, ci);

    pol::PolarState reflected;
    reflected.k = k_out;
    reflected.e1 = s;
    for (int ch = 0; ch < pol::CHANNELS; ++ch)
        reflected.s[ch] = pol::apply_mueller(M, incoming.s[ch]);

    // Back into the reference frame the caller asked for.
    return pol::rebase(reflected, k_out, e1_out);
}

} // namespace pol_render
