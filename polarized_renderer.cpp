#include "polarized_renderer.h"

#include <algorithm>
#include <cmath>

namespace {

pol::Vec3 to_d(const vec3& v) {
    return {static_cast<double>(v.x), static_cast<double>(v.y),
            static_cast<double>(v.z)};
}

vec3 normalized_f(vec3 v) {
    float n = v.norm();
    return n > 0.0f ? v * (1.0f / n) : vec3{0.0f, 0.0f, -1.0f};
}

PolarizedTraceResult zero_hit_state(const vec3& D, int events) {
    pol::Vec3 k = -to_d(normalized_f(D));
    PolarizedTraceResult result;
    result.hit = true;
    result.reflection_events = events;
    result.state = pol::make_world_state(pol::StokesRGB{}, k);
    return result;
}

} // namespace

vec3 polarized_camera_ray(int x, int y, int width, int height,
                          float fov, double roll) {
    double dx = static_cast<double>(x) + 0.5 - width / 2.0;
    double dy = -(static_cast<double>(y) + 0.5) + height / 2.0;
    double dz = -height / (2.0 * std::tan(static_cast<double>(fov) / 2.0));

    double c = std::cos(roll);
    double s = std::sin(roll);
    double rx = c * dx - s * dy;
    double ry = s * dx + c * dy;

    return normalized_f(vec3{
        static_cast<float>(rx),
        static_cast<float>(ry),
        static_cast<float>(dz)
    });
}

pol::Vec3 polarized_camera_reference(const vec3& D, double roll) {
    vec3 dn = normalized_f(D);
    pol::Vec3 k{-dn.x, -dn.y, -dn.z};
    pol::Vec3 rolled_up{-std::sin(roll), std::cos(roll), 0.0};
    return pol::projected_reference_axis(k, rolled_up);
}

PolarizedTraceResult PolarizedRenderer::trace_internal(
        const vec3& origin, const vec3& direction, int reflections_left) const {
    vec3 D = normalized_f(direction);

    int nearest = -1;
    float nearest_t = 1e30f;
    for (int i = 0; i < static_cast<int>(spheres.size()); ++i) {
        const PolarizedSphere& ps = spheres[i];
        auto [hit, t] = ray_sphere_intersect(origin, D, ps.center, ps.radius);
        if (hit && t < nearest_t) {
            nearest_t = t;
            nearest = i;
        }
    }

    pol::Vec3 k_out = -to_d(D);
    if (nearest < 0) {
        PolarizedTraceResult result;
        result.hit = false;
        result.reflection_events = 0;
        result.state = pol::make_world_state(background, k_out);
        return result;
    }

    // The ray has hit a sphere.  If the budget is exhausted, the task contract
    // is to return zero rather than silently falling back to background.
    if (reflections_left <= 0) return zero_hit_state(D, 0);

    const PolarizedSphere& ps = spheres[nearest];
    vec3 P = origin + D * nearest_t;
    vec3 Nf = (P - ps.center).normalized();
    pol::Vec3 N = pol::normalize(to_d(Nf));

    // Existing shared reflection geometry for the next sphere query.
    vec3 D_reflected = normalized_f(reflect(D, Nf));

    PolarizedTraceResult incoming =
        trace_internal(P, D_reflected, reflections_left - 1);

    // Interface s axis.  cross(N, k_out) is exactly the requested ordering;
    // use the published deterministic perpendicular axis at normal incidence.
    pol::Vec3 s_cross = pol::cross(N, k_out);
    pol::Vec3 s0;
    if (pol::norm2(s_cross) > 1e-24) {
        s0 = pol::normalize(s_cross);
    } else {
        s0 = pol::world_reference_axis(k_out);
    }

    // Recursive state travels along k_in = -D_reflected and carries its own
    // world reference.  Rebase the same physical beam to e1=s, e2=p_in.
    pol::PolarizationState in_s = pol::change_basis(incoming.state, s0);

    // k_out points to the caller and decides which side n1 describes.
    double side = pol::dot(N, k_out);
    double ci = std::max(0.0, std::min(1.0, std::abs(side)));
    bool caller_is_outside = side >= 0.0;
    double n1 = caller_is_outside ? ps.n_out : ps.n_in;
    double n2 = caller_is_outside ? ps.n_in : ps.n_out;

    pol::Mueller M = pol::reflection_mueller(n1, n2, ci);
    pol::StokesRGB reflected = pol::apply(M, in_s.stokes);

    // The matrix coordinates are (s, p_in) for input and (s, p_out) for
    // output, with p = cross(k, s).  Carrying e1=s with the new k makes
    // e2() evaluate to p_out automatically.
    pol::PolarizationState out_s;
    out_s.k = k_out;
    out_s.e1 = in_s.e1;
    out_s.stokes = reflected;

    // Return to this internal caller's fixed world projection basis.  The
    // public entry may perform one more rebase to a caller-supplied axis.
    pol::Vec3 world_e1 = pol::world_reference_axis(k_out);
    pol::PolarizationState out_world = pol::change_basis(out_s, world_e1);

    PolarizedTraceResult result;
    result.hit = true;
    result.state = out_world;
    result.reflection_events = incoming.reflection_events + 1;
    return result;
}

PolarizedTraceResult PolarizedRenderer::trace_ray_with_count(
        const vec3& origin, const vec3& D,
        const pol::Vec3* output_e1) const {
    PolarizedTraceResult result = trace_internal(origin, D, max_reflections);
    if (output_e1) {
        result.state = pol::change_basis(result.state, *output_e1);
    }
    return result;
}

pol::PolarizationState PolarizedRenderer::trace(
        const vec3& D, const pol::Vec3* output_e1) const {
    return trace_with_count(D, output_e1).state;
}

PolarizedTraceResult PolarizedRenderer::trace_with_count(
        const vec3& D, const pol::Vec3* output_e1) const {
    return trace_ray_with_count(camera_origin, D, output_e1);
}

PolarizedTraceResult PolarizedRenderer::trace_pixel(
        int x, int y, int width, int height,
        const pol::Vec3* output_e1) const {
    vec3 D = camera_ray(x, y, width, height);
    return trace_with_count(D, output_e1);
}

pol::PolarizationState trace_polarized(
        const PolarizedScene& scene,
        const vec3& origin,
        const vec3& D,
        const pol::Vec3* output_e1) {
    PolarizedRenderer renderer(scene);
    return renderer.trace_ray(origin, D, output_e1);
}
