#include "volume.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr double PI = 3.14159265358979323846;

struct DVec3 { double x=0, y=0, z=0; };

inline DVec3 to_d(const vec3 &v) { return {double(v.x), double(v.y), double(v.z)}; }
inline vec3  to_f(const DVec3 &v) { return {float(v.x), float(v.y), float(v.z)}; }

inline DVec3 add(const DVec3 &a, const DVec3 &b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline DVec3 sub(const DVec3 &a, const DVec3 &b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline DVec3 mul(const DVec3 &a, double s)       { return {a.x*s, a.y*s, a.z*s}; }
inline double dot(const DVec3 &a, const DVec3 &b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline double norm(const DVec3 &a) { return std::sqrt(dot(a, a)); }

// ---------------------------------------------------------------- grid access

inline size_t vertex_index(const DensityGrid &g, int x, int y, int z) {
    return (size_t(z) * size_t(g.ny) + size_t(y)) * size_t(g.nx) + size_t(x);
}

// Trilinear interpolation at a point known to lie inside (or on the boundary
// of) the box. Coordinates are clamped so rounding at the far face cannot
// index past the last vertex.
double density_at(const DensityGrid &g, const DVec3 &p) {
    const double fx = (p.x - g.box_min.x) / (g.box_max.x - g.box_min.x);
    const double fy = (p.y - g.box_min.y) / (g.box_max.y - g.box_min.y);
    const double fz = (p.z - g.box_min.z) / (g.box_max.z - g.box_min.z);
    const double ux = std::min(std::max(fx, 0.0), 1.0) * (g.nx - 1);
    const double uy = std::min(std::max(fy, 0.0), 1.0) * (g.ny - 1);
    const double uz = std::min(std::max(fz, 0.0), 1.0) * (g.nz - 1);
    const int ix = std::min(int(ux), g.nx - 2);
    const int iy = std::min(int(uy), g.ny - 2);
    const int iz = std::min(int(uz), g.nz - 2);
    const double wx = ux - ix, wy = uy - iy, wz = uz - iz;
    const size_t i000 = vertex_index(g, ix,   iy,   iz);
    const size_t i100 = vertex_index(g, ix+1, iy,   iz);
    const size_t i010 = vertex_index(g, ix,   iy+1, iz);
    const size_t i110 = vertex_index(g, ix+1, iy+1, iz);
    const size_t i001 = vertex_index(g, ix,   iy,   iz+1);
    const size_t i101 = vertex_index(g, ix+1, iy,   iz+1);
    const size_t i011 = vertex_index(g, ix,   iy+1, iz+1);
    const size_t i111 = vertex_index(g, ix+1, iy+1, iz+1);
    const double c00 = g.density[i000]*(1-wx) + g.density[i100]*wx;
    const double c10 = g.density[i010]*(1-wx) + g.density[i110]*wx;
    const double c01 = g.density[i001]*(1-wx) + g.density[i101]*wx;
    const double c11 = g.density[i011]*(1-wx) + g.density[i111]*wx;
    const double c0  = c00*(1-wy) + c10*wy;
    const double c1  = c01*(1-wy) + c11*wy;
    return c0*(1-wz) + c1*wz;
}

// Clip the ray p + t*d with t >= 0 to the closed box [lo, hi]. Returns false
// when the positive-length overlap is empty. Zero direction components fall
// out of the slab test naturally: they either keep the interval or, when the
// origin is outside that slab, make the ray miss.
bool clip_ray_to_box(const DVec3 &p, const DVec3 &d, const DVec3 &lo, const DVec3 &hi,
                     double &t_lo, double &t_hi) {
    t_lo = 0.0;
    t_hi = std::numeric_limits<double>::infinity();
    const double comp[3] = {d.x, d.y, d.z};
    const double org[3]  = {p.x, p.y, p.z};
    const double mn[3]   = {lo.x, lo.y, lo.z};
    const double mx[3]   = {hi.x, hi.y, hi.z};
    for (int a = 0; a < 3; ++a) {
        if (comp[a] == 0.0) {
            if (org[a] < mn[a] || org[a] > mx[a]) return false;
            continue;
        }
        double t0 = (mn[a] - org[a]) / comp[a];
        double t1 = (mx[a] - org[a]) / comp[a];
        if (t0 > t1) std::swap(t0, t1);
        t_lo = std::max(t_lo, t0);
        t_hi = std::min(t_hi, t1);
        if (t_lo >= t_hi) return false; // empty or only a point (grazing)
    }
    return std::isfinite(t_hi) && t_hi > t_lo;
}

// Collect the interior cell-boundary parameters of a segment. Coordinates are
// linear in t, so each axis contributes its (n-1) plane crossings; sorting the
// union yields pieces that each lie inside one voxel cell.
void collect_cell_cuts(const DensityGrid &g, const DVec3 &org, const DVec3 &d,
                       double t0, double t1, std::vector<double> &cuts) {
    cuts.clear();
    cuts.push_back(t0);
    cuts.push_back(t1);
    const double o[3] = {org.x, org.y, org.z};
    const double v[3] = {d.x, d.y, d.z};
    const double mn[3] = {g.box_min.x, g.box_min.y, g.box_min.z};
    const double mx[3] = {g.box_max.x, g.box_max.y, g.box_max.z};
    const int    n[3]  = {g.nx, g.ny, g.nz};
    for (int axis = 0; axis < 3; ++axis) {
        if (v[axis] == 0.0) continue;
        for (int k = 1; k < n[axis]; ++k) {
            const double plane = mn[axis] + (mx[axis] - mn[axis]) * double(k) / double(n[axis] - 1);
            const double t = (plane - o[axis]) / v[axis];
            if (t > t0 && t < t1) cuts.push_back(t);
        }
    }
    std::sort(cuts.begin(), cuts.end());
}

// 8-point Gauss-Legendre on [-1, 1].
constexpr int GL_N = 8;
constexpr double GL_X[GL_N] = {
    -0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
     0.1834346424956498,  0.5255324099163290,  0.7966664774136267,  0.9602898564975363};
constexpr double GL_W[GL_N] = {
    0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
    0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763};

// Fit the density along one cell piece as a cubic polynomial in the local
// parameter u in [0,1] and return its antiderivative coefficients. The
// trilinear field restricted to a line is exactly cubic, so four samples
// determine it exactly. This gives the optical path from the piece start to
// any fraction of the piece without relying on a linear approximation (which
// would be wrong for oblique rays crossing a cell).
struct Cubic { double c0, c1, c2, c3; };

Cubic fit_piece_cubic(const DensityGrid &g, const DVec3 &pa, const DVec3 &pb) {
    // Newton form through nodes 0, 1/3, 2/3, 1.
    static const double x[4] = {0.0, 1.0/3.0, 2.0/3.0, 1.0};
    double y[4];
    for (int i = 0; i < 4; ++i) {
        const DVec3 p = add(pa, mul(sub(pb, pa), x[i]));
        y[i] = density_at(g, p);
    }
    double t[4] = {y[0], y[1], y[2], y[3]};
    const double a0 = t[0];
    double a1 = 0, a2 = 0, a3 = 0;
    for (int order = 1; order < 4; ++order) {
        for (int i = 0; i + order < 4; ++i)
            t[i] = (t[i + 1] - t[i]) / (x[i + order] - x[i]);
        if (order == 1) a1 = t[0];
        if (order == 2) a2 = t[0];
        if (order == 3) a3 = t[0];
    }
    // Convert the Newton basis 1, u, u(u-1/3), u(u-1/3)(u-2/3) to monomial form.
    //   u(u-1/3)            = u^2 - u/3
    //   u(u-1/3)(u-2/3)     = u^3 - u^2 + 2u/9
    Cubic c;
    c.c0 = a0;
    c.c1 = a1 - a2 / 3.0 + 2.0 * a3 / 9.0;
    c.c2 = a2 - a3;
    c.c3 = a3;
    return c;
}

// Optical path of the piece from 0 to u: integral_0^u (c0 + c1 s + c2 s^2 +
// c3 s^3) ds, in units of the piece's true length.
double cubic_path_to(const Cubic &c, double u) {
    return c.c0 * u + c.c1 * u * u / 2.0 + c.c2 * u * u * u / 3.0
         + c.c3 * u * u * u * u / 4.0;
}

// Optical path of a single cell piece pa -> pb. The trilinear density along a
// straight line is cubic per coordinate, so the 8-point rule is exact (up to
// floating-point roundoff) even for oblique rays; the endpoints alone would not
// be enough.
double piece_optical_path(const DensityGrid &g, const DVec3 &pa, const DVec3 &pb) {
    const DVec3 mid = mul(add(pa, pb), 0.5);
    const DVec3 half = mul(sub(pb, pa), 0.5);
    const double len = 2.0 * norm(half);
    if (!(len > 0.0)) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < GL_N; ++i) {
        const DVec3 p = add(mid, mul(half, GL_X[i]));
        sum += GL_W[i] * density_at(g, p);
    }
    return 0.5 * len * sum;
}

// Core line integral in double precision, with the segment given as a->b.
double line_integral_d(const DensityGrid &grid, const DVec3 &a, const DVec3 &b) {
    const DVec3 d = sub(b, a);
    if (norm(d) == 0.0) return 0.0; // zero-length segment

    const DVec3 lo = to_d(grid.box_min), hi = to_d(grid.box_max);
    double t0, t1;
    if (!clip_ray_to_box(a, d, lo, hi, t0, t1)) return 0.0; // miss or grazing
    // The segment only spans t in [0, 1]; clipping to the box may extend past
    // either end, so intersect with that domain.
    t0 = std::max(t0, 0.0);
    t1 = std::min(t1, 1.0);
    if (!(t1 > t0)) return 0.0;

    std::vector<double> cuts;
    collect_cell_cuts(grid, a, d, t0, t1, cuts);

    double integral = 0.0;
    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
        const double ta = cuts[k], tb = cuts[k + 1];
        if (!(tb > ta)) continue; // touching pieces contribute nothing
        integral += piece_optical_path(grid, add(a, mul(d, ta)), add(a, mul(d, tb)));
    }
    return integral;
}

// ---------------------------------------------------------------- validation

void require_finite(double v, const char *what) {
    if (!std::isfinite(v)) throw std::invalid_argument(std::string("non-finite ") + what);
}

void require_finite_vec(const vec3 &v, const char *what) {
    require_finite(v.x, what); require_finite(v.y, what); require_finite(v.z, what);
}

} // namespace

void validate_volume(const Volume &volume) {
    const DensityGrid &g = volume.grid;
    require_finite_vec(g.box_min, "grid box minimum");
    require_finite_vec(g.box_max, "grid box maximum");
    if (!(g.box_min.x < g.box_max.x && g.box_min.y < g.box_max.y && g.box_min.z < g.box_max.z))
        throw std::invalid_argument("density grid box must have positive extent on all axes");
    if (g.nx < 2 || g.ny < 2 || g.nz < 2)
        throw std::invalid_argument("density grid needs at least 2 vertices per axis");
    const size_t expected = size_t(g.nx) * size_t(g.ny) * size_t(g.nz);
    if (g.density.size() != expected)
        throw std::invalid_argument("density array size does not match nx*ny*nz");
    for (double rho : g.density)
        if (!std::isfinite(rho) || rho < 0.0)
            throw std::invalid_argument("density values must be finite and non-negative");
    require_finite(volume.sigma, "sigma");
    if (volume.sigma < 0.0) throw std::invalid_argument("sigma must be non-negative");
    require_finite_vec(volume.albedo, "albedo");
    for (int c = 0; c < 3; ++c)
        if (volume.albedo[c] < 0.f || volume.albedo[c] > 1.f)
            throw std::invalid_argument("albedo channels must lie in [0,1]");
}

double density_line_integral(const DensityGrid &grid, const vec3 &a, const vec3 &b) {
    return line_integral_d(grid, to_d(a), to_d(b));
}

double transmittance(const Volume &volume, const vec3 &a, const vec3 &b) {
    return std::exp(-volume.sigma * density_line_integral(volume.grid, a, b));
}

// --------------------------------------------------------------- scattering

namespace {

vec3 scatter_integral(const Volume &volume, const Scene &scene,
                      const DVec3 &origin, const DVec3 &dir, double t_end) {
    const DensityGrid &g = volume.grid;
    if (volume.sigma <= 0.0 || t_end <= 0.0) return {0, 0, 0};

    const DVec3 lo = to_d(g.box_min), hi = to_d(g.box_max);
    double t0, t1;
    if (!clip_ray_to_box(origin, dir, lo, hi, t0, t1)) return {0, 0, 0};
    t1 = std::min(t1, t_end);
    if (!(t1 > t0)) return {0, 0, 0};

    std::vector<double> cuts;
    collect_cell_cuts(g, origin, dir, t0, t1, cuts);

    const double sigma = volume.sigma;
    const vec3 albedo = volume.albedo;

    vec3 total = {0, 0, 0};
    double acc_opt = 0.0; // optical path from origin to the start of the piece

    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
        const double ta = cuts[k], tb = cuts[k + 1];
        if (!(tb > ta)) continue;
        const DVec3 pa = add(origin, mul(dir, ta));
        const DVec3 pb = add(origin, mul(dir, tb));
        if (!(norm(sub(pb, pa)) > 0.0)) continue;

        // Optical path over this piece (exact for the trilinear field).
        const double piece_len = norm(sub(pb, pa));
        const Cubic cubic = fit_piece_cubic(g, pa, pb);
        const double piece_opt = piece_len * cubic_path_to(cubic, 1.0);

        // Integrate the single-scatter source with the specified 8-point
        // Gauss-Legendre rule over this traversed cell interval. The view
        // transmittance to a node is exp(-sigma * optical path from the origin
        // to that node); inside the piece that partial path follows the exact
        // cubic, not a linear interpolation of the endpoints, which matters for
        // oblique rays through a non-uniform cell.
        double contrib[3] = {0, 0, 0};
        for (int i = 0; i < GL_N; ++i) {
            const double frac = 0.5 * (GL_X[i] + 1.0);
            const DVec3 p = add(origin, mul(dir, ta + (tb - ta) * frac));
            const double rho = density_at(g, p);
            if (rho <= 0.0) continue;
            const double view_opt = acc_opt + piece_len * cubic_path_to(cubic, frac);
            const double t_view = std::exp(-sigma * view_opt);

            for (size_t li = 0; li < scene.lights.size(); ++li) {
                const DVec3 light = to_d(scene.lights[li]);
                const DVec3 to_light = sub(light, p);
                const double dist = norm(to_light);
                if (!(dist > 0.0)) continue;

                // Same shadow rule as the surface: an object closer than
                // the light blocks it; objects behind the light do not.
                const vec3 node = to_f(p);
                const vec3 ldir = to_f(mul(to_light, 1.0 / dist));
                auto [hit, shadow_pt, trashnrm, trashmat] =
                    scene_intersect(node, ldir, scene);
                if (hit && (shadow_pt - node).norm() < float(dist)) continue;

                // Light-side transmittance; transmission only, never
                // scattering.
                const double t_light = std::exp(-sigma * line_integral_d(g, p, light));
                const vec3 power = li < scene.light_powers.size()
                                       ? scene.light_powers[li]
                                       : vec3{1, 1, 1};
                const double falloff = 1.0 / (4.0 * PI * dist * dist);
                const double w = GL_W[i] * t_view * rho * sigma * t_light * falloff;
                for (int c = 0; c < 3; ++c)
                    contrib[c] += w * double(albedo[c]) * double(power[c]);
            }
        }
        // For x in [-1, 1], d(view length) = (tb - ta)/2 * dx.
        for (int c = 0; c < 3; ++c)
            total[c] += float(contrib[c] * 0.5 * (tb - ta));
        acc_opt += piece_opt;
    }
    return total;
}

} // namespace

vec3 single_scatter(const Volume &volume, const Scene &scene,
                    const vec3 &origin, const vec3 &dir, double t_end) {
    return scatter_integral(volume, scene, to_d(origin), to_d(dir), t_end);
}

vec3 apply_medium(const Volume &volume, const Scene &scene,
                  const vec3 &orig, const vec3 &dir, double t_end,
                  const vec3 &surface_radiance) {
    if (t_end <= 0.0) return surface_radiance;
    const vec3 end = to_f(add(to_d(orig), mul(to_d(dir), t_end)));
    const double T = std::exp(-volume.sigma * line_integral_d(volume.grid, to_d(orig), to_d(end)));
    return single_scatter(volume, scene, orig, dir, t_end) + surface_radiance * float(T);
}

// -------------------------------------------------- recursive-caster policy

double medium_exit_distance(const Volume &volume, const vec3 &orig, const vec3 &dir) {
    const DVec3 o = to_d(orig), d = to_d(dir);
    double t0, t1;
    if (!clip_ray_to_box(o, d, to_d(volume.grid.box_min), to_d(volume.grid.box_max), t0, t1))
        return 0.0;
    return t1;
}

vec3 MediumFog::apply(const vec3 &orig, const vec3 &dir, double t_end,
                      const vec3 &surface) const {
    return apply_medium(*volume, *scene, orig, dir, t_end, surface);
}

float MediumFog::light_transmittance(const vec3 &point, const vec3 &light) const {
    return float(transmittance(*volume, point, light));
}

double MediumFog::exit_distance(const vec3 &orig, const vec3 &dir) const {
    return medium_exit_distance(*volume, orig, dir);
}

vec3 cast_ray(const vec3 &orig, const vec3 &dir, int depth,
              const Scene &scene, const Volume &medium) {
    validate_volume(medium);
    MediumFog fog{&medium, &scene};
    return cast_ray_impl(orig, dir, depth, scene, fog);
}
