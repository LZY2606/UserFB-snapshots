#include "isosurface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();
constexpr double FACE_EPS = 1e-9;      // local-coordinate tolerance for shared faces

bool finite_vec(const vec3 &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Analytic real roots of c0 + c1 t + c2 t^2 + c3 t^3 = 0.  Roots are returned
// unsorted; multiple roots (tangencies) are kept.
void solve_cubic(const long double c[4], std::vector<double> &roots) {
    long double scale = 1.0L;
    for (int i = 0; i < 4; ++i) scale = std::max(scale, std::fabsl(c[i]));
    const long double eps = 1e-14L * scale;

    if (std::fabsl(c[3]) <= eps) {
        if (std::fabsl(c[2]) <= eps) {
            if (std::fabsl(c[1]) <= eps) return;
            roots.push_back(static_cast<double>(-c[0] / c[1]));
            return;
        }
        const long double disc = c[1]*c[1] - 4.0L*c[2]*c[0];
        if (disc < 0) return;
        const long double s = std::sqrt(std::max(0.0L, disc));
        roots.push_back(static_cast<double>((-c[1] + s) / (2.0L*c[2])));
        if (disc > 0)
            roots.push_back(static_cast<double>((-c[1] - s) / (2.0L*c[2])));
        return;
    }

    const long double a = c[3], b = c[2], cc = c[1], d = c[0];
    const long double shift = b / (3.0L*a);
    const long double p = cc/a - (b*b)/(3.0L*a*a);
    const long double q = 2.0L*b*b*b/(27.0L*a*a*a) - b*cc/(3.0L*a*a) + d/a;
    const long double disc = (q/2.0L)*(q/2.0L) + (p/3.0L)*(p/3.0L)*(p/3.0L);
    // A near-zero discriminant is a double (tangent) root; route it through the
    // three-angle path so the repeated root is reported rather than collapsed.
    if (disc > 1e-18L * (1.0L + q*q + p*p)) {
        const long double s = std::sqrt(disc);
        const long double x = std::cbrt(-q/2.0L + s) + std::cbrt(-q/2.0L - s);
        roots.push_back(static_cast<double>(x - shift));
        return;
    }
    const long double r = 2.0L*std::sqrt(std::max(0.0L, -p/3.0L));
    long double ang = 0.0L;
    if (std::fabsl(p) > 1e-20L) // p == 0 here means a triple root sitting at the origin
        ang = std::acos(std::min(1.0L, std::max(-1.0L,
            (3.0L*q/(2.0L*p))*std::sqrt(-3.0L/p))));
    for (int k = 0; k < 3; ++k) {
        const long double x = r*std::cos((ang - 2.0L*3.14159265358979323846264338327950288L*k)/3.0L);
        roots.push_back(static_cast<double>(x - shift));
    }
}

} // namespace

IsoGrid::IsoGrid(const vec3 &origin,
                 const std::array<double,3> &spacing,
                 const std::array<int,3> &counts,
                 const std::vector<double> &values,
                 double isovalue,
                 const Material &material)
    : origin_(origin), spacing_(spacing), counts_(counts),
      values_(values), isovalue_(isovalue), material_(material) {

    auto fail = [](const char *msg) { throw std::invalid_argument(std::string("IsoGrid: ") + msg); };

    if (!finite_vec(origin_)) fail("origin must be finite");
    if (!std::isfinite(isovalue_)) fail("isovalue must be finite");
    for (int axis = 0; axis < 3; ++axis) {
        if (counts_[axis] < 2) fail("vertex counts must each be at least 2");
        if (!std::isfinite(spacing_[axis]) || spacing_[axis] <= 0.0) fail("spacing must be positive and finite");
    }
    const size_t expected = static_cast<size_t>(counts_[0]) *
                            static_cast<size_t>(counts_[1]) *
                            static_cast<size_t>(counts_[2]);
    if (values_.size() != expected) fail("scalar data length does not match vertex counts");
    for (double v : values_)
        if (!std::isfinite(v)) fail("scalar values must be finite");

    // Bricks: up to BRICK_CELLS cells per axis, the last brick may be partial.
    const std::array<int,3> bd = brick_dims();
    bricks_.reserve(static_cast<size_t>(bd[0])*bd[1]*bd[2]);
    for (int bz = 0; bz < bd[2]; ++bz) {
        for (int by = 0; by < bd[1]; ++by) {
            for (int bx = 0; bx < bd[0]; ++bx) {
                Brick brick;
                brick.bmin = {std::min(BRICK_CELLS*bx, counts_[0]-1),
                              std::min(BRICK_CELLS*by, counts_[1]-1),
                              std::min(BRICK_CELLS*bz, counts_[2]-1)};
                brick.bmax = {std::min(BRICK_CELLS*(bx+1), counts_[0]-1),
                              std::min(BRICK_CELLS*(by+1), counts_[1]-1),
                              std::min(BRICK_CELLS*(bz+1), counts_[2]-1)};
                double vmin = std::numeric_limits<double>::infinity();
                double vmax = -std::numeric_limits<double>::infinity();
                for (int k = brick.bmin[2]; k <= brick.bmax[2]; ++k)
                    for (int j = brick.bmin[1]; j <= brick.bmax[1]; ++j)
                        for (int i = brick.bmin[0]; i <= brick.bmax[0]; ++i) {
                            const double v = values_[vertex_index(i,j,k)];
                            vmin = std::min(vmin, v);
                            vmax = std::max(vmax, v);
                        }
                brick.vmin = vmin;
                brick.vmax = vmax;
                brick.active = (isovalue_ >= vmin && isovalue_ <= vmax); // closed interval
                bricks_.push_back(brick);
            }
        }
    }
}

vec3 IsoGrid::cell_gradient(const int cell[3], double u, double v, double w) const {
    const int i = cell[0], j = cell[1], k = cell[2];
    const double a0 = values_[vertex_index(i,   j,   k)];
    const double a1 = values_[vertex_index(i+1, j,   k)];
    const double a2 = values_[vertex_index(i,   j+1, k)];
    const double a3 = values_[vertex_index(i+1, j+1, k)];
    const double a4 = values_[vertex_index(i,   j,   k+1)];
    const double a5 = values_[vertex_index(i+1, j,   k+1)];
    const double a6 = values_[vertex_index(i,   j+1, k+1)];
    const double a7 = values_[vertex_index(i+1, j+1, k+1)];

    const double du = (1-v)*(1-w)*(a1-a0) + v*(1-w)*(a3-a2)
                    + (1-v)*w*(a5-a4)     + v*w*(a7-a6);
    const double dv = (1-u)*(1-w)*(a2-a0) + u*(1-w)*(a3-a1)
                    + (1-u)*w*(a6-a4)     + u*w*(a7-a5);
    const double dw = (1-u)*(1-v)*(a4-a0) + u*(1-v)*(a5-a1)
                    + (1-u)*v*(a6-a2)     + u*v*(a7-a3);
    return vec3{static_cast<float>(du/spacing_[0]),
                static_cast<float>(dv/spacing_[1]),
                static_cast<float>(dw/spacing_[2])};
}

void IsoGrid::cell_roots(const int cell[3],
                         double u0, double v0, double w0,
                         double du, double dv, double dw,
                         double tlo, double thi,
                         std::vector<double> &roots,
                         QueryStats &stats) const {
    ++stats.cells_evaluated;
    const int i = cell[0], j = cell[1], k = cell[2];
    // Corner order: linear id = di + 2*(dj + 2*dk).
    const long double a[8] = {
        values_[vertex_index(i,   j,   k)],
        values_[vertex_index(i+1, j,   k)],
        values_[vertex_index(i,   j+1, k)],
        values_[vertex_index(i+1, j+1, k)],
        values_[vertex_index(i,   j,   k+1)],
        values_[vertex_index(i+1, j,   k+1)],
        values_[vertex_index(i,   j+1, k+1)],
        values_[vertex_index(i+1, j+1, k+1)]
    };
    const long double iso = isovalue_;
    const long double c000 = a[0] - iso;
    const long double c100 = a[1] - a[0];
    const long double c010 = a[2] - a[0];
    const long double c001 = a[4] - a[0];
    const long double c110 = a[3] - a[2] - a[1] + a[0];
    const long double c101 = a[5] - a[4] - a[1] + a[0];
    const long double c011 = a[6] - a[4] - a[2] + a[0];
    const long double c111 = a[7] - a[6] - a[5] + a[4] - a[3] + a[2] + a[1] - a[0];

    // Solve in local time s = t - tlo, so that (u,v,w) = (u0,v0,w0) +
    // (du,dv,dw) * s holds exactly at s = 0.
    const long double U0 = u0, V0 = v0, W0 = w0, DU = du, DV = dv, DW = dw;
    // Trilinear field after substituting u(s), v(s), w(s).
    const long double b00 = c000 + c100*U0 + c010*V0 + c110*U0*V0;
    const long double b10 = c100*DU + c110*V0*DU;
    const long double b01 = c010*DV + c110*U0*DV;
    const long double b11 = c110*DU*DV;
    const long double r0 = c001 + c101*U0 + c011*V0 + c111*U0*V0;
    const long double r1 = c101*DU + c111*V0*DU;
    const long double r2 = c011*DV + c111*U0*DV;
    const long double r3 = c111*DU*DV;
    long double coef[4];
    coef[0] = b00 + r0*W0;
    coef[1] = b10 + b01 + r0*DW + (r1+r2)*W0;
    coef[2] = b11 + (r1+r2)*DW + r3*W0;
    coef[3] = r3*DW;

    std::vector<double> raw;
    solve_cubic(coef, raw);
    long double scale = 1.0L;
    for (long double c : coef) scale = std::max(scale, std::fabsl(c));
    const long double tol = 1e-9L * scale;
    const double slo = 0.0;
    const double shi = thi - tlo;
    const double span = std::max(1.0, shi);
    for (double r : raw) {
        if (!std::isfinite(r)) continue;
        // Pull roots sitting just outside the cell span back onto it.
        if (r < slo && r >= slo - 1e-10*span) r = slo;
        if (r > shi && r <= shi + 1e-10*span) r = shi;
        if (r < slo || r > shi) continue;
        long double q = coef[0] + r*(coef[1] + r*(coef[2] + r*coef[3]));
        if (std::fabsl(q) > tol*(1.0L + std::fabsl(static_cast<long double>(r))*
                                      std::fabsl(static_cast<long double>(r)))) continue;
        roots.push_back(tlo + r); // back to global ray parameter
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end(),
                            [](double x, double y) {
                                return std::abs(x-y) <= 1e-12*std::max(1.0, std::abs(x));
                            }), roots.end());
}

bool IsoGrid::traverse(const vec3 &O, const vec3 &D,
                       double tmin, double tmax,
                       QueryStats &stats, IsoHit *out) const {
    if (!finite_vec(O) || !finite_vec(D) || D.norm() == 0.0f) return false;
    if (!std::isfinite(tmin) || !std::isfinite(tmax) || tmin < 0.0 || tmin >= tmax) return false;

    const double d[3] = {D.x, D.y, D.z};
    const double o[3] = {O.x, O.y, O.z};

    // Slab intersection of the ray with the grid box; no surface outside it.
    double tA = tmin, tB = tmax;
    for (int axis = 0; axis < 3; ++axis) {
        const double lo = origin_[axis];
        const double hi = origin_[axis] + (counts_[axis]-1)*spacing_[axis];
        if (std::abs(d[axis]) < 1e-300) {
            if (o[axis] < lo || o[axis] > hi) return false;
            continue;
        }
        double t1 = (lo - o[axis]) / d[axis];
        double t2 = (hi - o[axis]) / d[axis];
        if (t1 > t2) std::swap(t1, t2);
        tA = std::max(tA, t1);
        tB = std::min(tB, t2);
    }
    if (!(tA < tB)) return false;
    const double tend = tB; // already <= tmax

    const std::array<int,3> bd = brick_dims();

    // ---- brick DDA, seeded at the box entry point ----
    int b[3];
    int stepb[3];
    double tnextb[3], dtb[3];
    {
        const double p[3] = {o[0] + d[0]*tA, o[1] + d[1]*tA, o[2] + d[2]*tA};
        for (int axis = 0; axis < 3; ++axis) {
            const double fv = (p[axis] - origin_[axis]) / spacing_[axis];
            int idx = static_cast<int>(std::floor(fv / BRICK_CELLS));
            idx = std::clamp(idx, 0, bd[axis]-1);
            b[axis] = idx;
            stepb[axis] = d[axis] >= 0.0 ? 1 : -1;
            if (std::abs(d[axis]) < 1e-300) {
                dtb[axis] = INF;
                tnextb[axis] = INF;
            } else {
                dtb[axis] = BRICK_CELLS*spacing_[axis] / std::abs(d[axis]);
                const int boundary_cell = (d[axis] >= 0.0) ? BRICK_CELLS*(idx+1)
                                                           : BRICK_CELLS*idx;
                const double boundary_world = origin_[axis] + boundary_cell*spacing_[axis];
                tnextb[axis] = (boundary_world - o[axis]) / d[axis];
            }
        }
    }

    std::vector<double> roots;
    double cur = tA;
    while (b[0] >= 0 && b[0] < bd[0] &&
           b[1] >= 0 && b[1] < bd[1] &&
           b[2] >= 0 && b[2] < bd[2]) {
        ++stats.bricks_visited;
        const Brick &brick = brick_at(b[0], b[1], b[2]);

        if (brick.active) {
            // ---- cell DDA inside this brick ----
            int c[3];
            int stepc[3];
            double tnextc[3], dtc[3];
            {
                const double p[3] = {o[0] + d[0]*cur, o[1] + d[1]*cur, o[2] + d[2]*cur};
                for (int axis = 0; axis < 3; ++axis) {
                    double fv = (p[axis] - origin_[axis]) / spacing_[axis];
                    int idx = static_cast<int>(std::floor(fv));
                    idx = std::clamp(idx, brick.bmin[axis], brick.bmax[axis]-1);
                    c[axis] = idx;
                    stepc[axis] = d[axis] >= 0.0 ? 1 : -1;
                    if (std::abs(d[axis]) < 1e-300) {
                        dtc[axis] = INF;
                        tnextc[axis] = INF;
                    } else {
                        dtc[axis] = spacing_[axis] / std::abs(d[axis]);
                        const int boundary_cell = (d[axis] >= 0.0) ? idx+1 : idx;
                        const double boundary_world = origin_[axis] + boundary_cell*spacing_[axis];
                        tnextc[axis] = (boundary_world - o[axis]) / d[axis];
                    }
                }
            }

            double cell_cur = cur;
            while (c[0] >= brick.bmin[0] && c[0] <= brick.bmax[0]-1 &&
                   c[1] >= brick.bmin[1] && c[1] <= brick.bmax[1]-1 &&
                   c[2] >= brick.bmin[2] && c[2] <= brick.bmax[2]-1) {
                double chi = std::min({tnextc[0], tnextc[1], tnextc[2], tend});
                if (cell_cur < chi) {
                    roots.clear();
                    // Parametric local cell coordinates along the ray.
                    const double u0 = (o[0] + d[0]*cell_cur - (origin_[0] + c[0]*spacing_[0])) / spacing_[0];
                    const double v0 = (o[1] + d[1]*cell_cur - (origin_[1] + c[1]*spacing_[1])) / spacing_[1];
                    const double w0 = (o[2] + d[2]*cell_cur - (origin_[2] + c[2]*spacing_[2])) / spacing_[2];
                    const double duv[3] = {d[0]/spacing_[0], d[1]/spacing_[1], d[2]/spacing_[2]};
                    cell_roots(c, u0, v0, w0, duv[0], duv[1], duv[2],
                               cell_cur, chi, roots, stats);
                    // Roots are t-ordered; take the first one still inside the
                    // requested interval.
                    const double *chosen = nullptr;
                    for (const double &r : roots) {
                        if (r >= tmin && r < tmax) { chosen = &r; break; }
                    }
                    if (chosen != nullptr) {
                        const double r = *chosen;
                        const double pw[3] = {o[0] + d[0]*r,
                                              o[1] + d[1]*r,
                                              o[2] + d[2]*r};
                        const double local[3] = {
                            (pw[0] - (origin_[0] + c[0]*spacing_[0])) / spacing_[0],
                            (pw[1] - (origin_[1] + c[1]*spacing_[1])) / spacing_[1],
                            (pw[2] - (origin_[2] + c[2]*spacing_[2])) / spacing_[2]};
                        // Canonical owning cell: smallest x-fastest linear id
                        // among the cells containing the hit point.  A point on
                        // a lower face is shared with the backward neighbour
                        // (smaller id); an upper face already belongs to this
                        // cell by the smallest-id rule.  Only face neighbours
                        // are inspected -- a constant amount of work.
                        int own[3] = {c[0], c[1], c[2]};
                        for (int axis = 0; axis < 3; ++axis) {
                            if (local[axis] <= FACE_EPS && c[axis] > 0)
                                own[axis] = c[axis] - 1;
                        }
                        const double lu = std::clamp(local[0], 0.0, 1.0);
                        const double lv = std::clamp(local[1], 0.0, 1.0);
                        const double lw = std::clamp(local[2], 0.0, 1.0);
                        vec3 grad = cell_gradient(own, lu, lv, lw);
                        vec3 nrm = grad.normalized();
                        if (out != nullptr) {
                            out->t = r;
                            out->point = vec3{static_cast<float>(pw[0]),
                                              static_cast<float>(pw[1]),
                                              static_cast<float>(pw[2])};
                            out->normal = nrm;
                            out->cell = {own[0], own[1], own[2]};
                            out->material = material_;
                        }
                        return true;
                    }
                }
                if (chi >= tend) break;
                // Cross every grid plane met at chi in one step; simultaneous
                // crossings share a face/edge/vertex whose canonical owner is
                // resolved when the hit is reported.
                const double tie = 1e-12*std::max(1.0, std::abs(chi));
                cell_cur = chi;
                for (int axis = 0; axis < 3; ++axis) {
                    if (std::abs(tnextc[axis] - chi) <= tie) {
                        c[axis] += stepc[axis];
                        tnextc[axis] += dtc[axis];
                    }
                }
            }
        }

        double bcross = INF;
        for (int axis = 0; axis < 3; ++axis) bcross = std::min(bcross, tnextb[axis]);
        if (!(bcross < tend)) break;
        const double tie = 1e-12*std::max(1.0, std::abs(bcross));
        cur = bcross;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(tnextb[axis] - bcross) <= tie) {
                b[axis] += stepb[axis];
                tnextb[axis] += dtb[axis];
            }
        }
    }
    return false;
}

bool IsoGrid::nearest(const vec3 &O, const vec3 &D,
                      double tmin, double tmax,
                      IsoHit &hit, QueryStats *stats) const {
    QueryStats local;
    QueryStats &s = stats ? *stats : local;
    return traverse(O, D, tmin, tmax, s, &hit);
}

bool IsoGrid::occluded(const vec3 &O, const vec3 &D,
                       double tmin, double tmax,
                       QueryStats *stats) const {
    QueryStats local;
    QueryStats &s = stats ? *stats : local;
    return traverse(O, D, tmin, tmax, s, nullptr);
}

// ---------------------------------------------------------------------------
// Scene-aware rendering: the isosurface joins spheres and the checkerboard on
// one nearest-hit chain; reflection/refraction carry the same Scene.
// ---------------------------------------------------------------------------

bool scene_intersect(const vec3 &orig, const vec3 &dir, const Scene &scene,
                     vec3 &pt, vec3 &N, Material &material) {
    bool found = false;
    float nearest_dist = 1e10f;

    if (std::abs(dir.y) > .001f) { // checkerboard, equation y = -4
        const float d = -(orig.y+4)/dir.y;
        const vec3 p = orig + dir*d;
        if (d>.001f && d<nearest_dist && std::abs(p.x)<10 && p.z<-10 && p.z>-30) {
            nearest_dist = d;
            pt = p;
            N = {0,1,0};
            material.diffuse_color = (int(.5f*pt.x+1000) + int(.5f*pt.z)) & 1 ? vec3{.3f,.3f,.3f} : vec3{.3f,.2f,.1f};
            found = true;
        }
    }

    for (const Sphere &s : scene.spheres) {
        auto [intersection, d] = ray_sphere_intersect(orig, dir, s);
        if (!intersection || d > nearest_dist) continue;
        nearest_dist = d;
        pt = orig + dir*nearest_dist;
        N = (pt - s.center).normalized();
        material = s.material;
        found = true;
    }

    if (scene.iso != nullptr) {
        IsoHit hit;
        if (scene.iso->nearest(orig, dir, .001, static_cast<double>(nearest_dist), hit)) {
            nearest_dist = static_cast<float>(hit.t);
            pt = hit.point;
            N = hit.normal;
            material = hit.material;
            found = true;
        }
    }
    return found;
}

bool scene_occluded(const vec3 &point, const vec3 &light_dir, float tmax, const Scene &scene) {
    for (const Sphere &s : scene.spheres) {
        auto [hit, d] = ray_sphere_intersect(point, light_dir, s);
        if (hit && d < tmax) return true;
    }
    if (std::abs(light_dir.y) > .001f) {
        const float d = -(point.y+4)/light_dir.y;
        const vec3 p = point + light_dir*d;
        if (d>.001f && d<tmax && std::abs(p.x)<10 && p.z<-10 && p.z>-30) return true;
    }
    if (scene.iso != nullptr &&
        scene.iso->occluded(point, light_dir, .001, static_cast<double>(tmax)))
        return true;
    return false;
}

vec3 cast_ray(const vec3 &orig, const vec3 &dir, const Scene &scene, int depth) {
    vec3 point, N;
    Material material;
    if (depth > 4 || !scene_intersect(orig, dir, scene, point, N, material))
        return {0.2f, 0.7f, 0.8f}; // background color

    const vec3 reflect_dir = reflect(dir, N).normalized();
    const vec3 refract_dir = refract(dir, N, material.refractive_index).normalized();
    const vec3 reflect_color = cast_ray(point, reflect_dir, scene, depth + 1);
    const vec3 refract_color = cast_ray(point, refract_dir, scene, depth + 1);

    float diffuse_light_intensity = 0, specular_light_intensity = 0;
    for (const vec3 &light : scene.lights) {
        const vec3 light_dir = (light - point).normalized();
        const float light_dist = (light - point).norm();
        // Shadow rays are bounded by the light: surface behind it cannot shade.
        if (scene_occluded(point, light_dir, light_dist, scene)) continue;
        diffuse_light_intensity  += std::max(0.f, light_dir*N);
        specular_light_intensity += std::pow(std::max(0.f, -reflect(-light_dir, N)*dir), material.specular_exponent);
    }
    return material.diffuse_color * diffuse_light_intensity * material.albedo[0]
         + vec3{1.f,1.f,1.f}*specular_light_intensity * material.albedo[1]
         + reflect_color*material.albedo[2]
         + refract_color*material.albedo[3];
}
