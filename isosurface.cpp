#include "isosurface.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kPi  = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Polynomial root solving. All real roots of a degree <= 3 polynomial are
// returned; tangential double roots are kept (the near-zero discriminant
// branch always goes through the trigonometric three-root formula).
// ---------------------------------------------------------------------------
void solve_polynomial(double a, double b, double c, double d,
                      std::vector<double>& roots) {
    const double scale = 1.0 + std::abs(a) + std::abs(b) +
                               std::abs(c) + std::abs(d);
    if (std::abs(a) <= 1e-14 * scale) {            // degree <= 2
        if (std::abs(b) <= 1e-14 * scale) {        // degree <= 1
            if (std::abs(c) > 1e-14 * scale)
                roots.push_back(-d / c);
            return;
        }
        const double disc = c*c - 4.0*b*d;
        if (disc < 0.0) return;
        const double s = std::sqrt(std::max(0.0, disc));
        roots.push_back((-c - s) / (2.0*b));
        if (disc > 0.0) roots.push_back((-c + s) / (2.0*b));
        return;
    }

    // Normalize to x^3 + p x^2 + q x + r, then depress with x = z - p/3.
    const double p = b / a;
    const double q = c / a;
    const double r = d / a;
    const double P = q - p*p / 3.0;
    const double Q = r - p*q / 3.0 + 2.0*p*p*p / 27.0;
    const double disc = (Q*0.5)*(Q*0.5) + (P/3.0)*(P*P/9.0);
    const double disc_scale = std::max(1.0,
        std::max(std::abs((Q*0.5)*(Q*0.5)), std::abs((P/3.0)*(P*P/9.0))));

    if (disc > 1e-12 * disc_scale) {               // one real root
        const double sq = std::sqrt(disc);
        const double u = -Q*0.5 + sq;
        const double v = -Q*0.5 - sq;
        const double z = std::cbrt(u) + std::cbrt(v);
        roots.push_back(z - p/3.0);
        return;
    }

    // Three real roots (the disc ~ 0 case gives a repeated root).
    if (P >= 0.0) {
        roots.push_back(-p/3.0);
        return;
    }
    const double sq = std::sqrt(-P/3.0);
    double arg = (3.0*Q) / (2.0*P) * std::sqrt(-3.0/P);
    arg = std::max(-1.0, std::min(1.0, arg));
    const double ang = std::acos(arg) / 3.0;
    for (int k = 0; k < 3; ++k) {
        const double z = 2.0 * sq * std::cos(ang - 2.0*kPi*k/3.0);
        roots.push_back(z - p/3.0);
    }
}

// ---------------------------------------------------------------------------
// Generic slab DDA over a regular axis split into blocks of `block` cells.
// ---------------------------------------------------------------------------
struct AxisDDA {
    double o = 0;          // world origin of the grid
    double s = 1;          // world spacing
    double D = 0;          // ray direction component
    double O = 0;          // ray origin component
    int    nblocks = 0;    // number of blocks on this axis
    int    block = 1;      // cells per block (4 for bricks, 1 for cells)
    int    index = 0;      // current block index
    double tnext = kInf;   // ray time of the next block boundary

    void init(double tstart) {
        if (nblocks <= 0) { index = 0; tnext = kInf; return; }
        const double q = (O + tstart*D - o) / s; // continuous vertex coord
        index = int(std::floor(q / block));
        index = std::max(0, std::min(nblocks - 1, index));
        recompute();
    }
    void recompute() {
        if (D == 0.0) { tnext = kInf; return; }
        if (D > 0.0) {
            if (index + 1 >= nblocks) { tnext = kInf; return; }
            const double boundary = double((index + 1) * block);
            tnext = (o + boundary*s - O) / D;
        } else {
            if (index <= 0) { tnext = kInf; return; }
            const double boundary = double(index * block);
            tnext = (o + boundary*s - O) / D;
        }
    }
    void advance() {
        index += D > 0.0 ? 1 : -1;
        recompute();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Construction and validation
// ---------------------------------------------------------------------------
IsoGrid::IsoGrid(const vec3& origin,
                 const vec3& spacing,
                 int nx, int ny, int nz,
                 const std::vector<double>& scalar,
                 double isovalue,
                 const Material& material)
    : origin_(origin), spacing_(spacing),
      nx_(nx), ny_(ny), nz_(nz), iso_(isovalue), material_(material) {
    auto bad = [](const char* msg) {
        throw std::invalid_argument(std::string("IsoGrid: ") + msg);
    };
    if (nx < 2 || ny < 2 || nz < 2 || nx > 100000 ||
        ny > 100000 || nz > 100000)
        bad("vertex counts must each lie in [2, 100000]");
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(spacing_[a]) || spacing_[a] <= 0.0f)
            bad("spacing must be finite and positive on every axis");
        if (!std::isfinite(origin_[a]))
            bad("origin must be finite");
    }
    if (!std::isfinite(isovalue))
        bad("isovalue must be finite");

    const size_t expected = size_t(nx) * size_t(ny) * size_t(nz);
    if (scalar.size() != expected)
        bad("scalar length does not match nx*ny*nz");
    values_.reserve(scalar.size());
    for (double v : scalar) {
        if (!std::isfinite(v)) bad("scalar values must be finite");
        values_.push_back(v);                       // deep copy
    }

    // Brick acceleration: up to 4 cells per axis per brick.
    nbx_ = (nx_ - 1 + 3) / 4;
    nby_ = (ny_ - 1 + 3) / 4;
    nbz_ = (nz_ - 1 + 3) / 4;
    bricks_.resize(size_t(nbx_) * nby_ * nbz_);
    for (int bz = 0; bz < nbz_; ++bz) {
        const int k0 = 4*bz, k1 = std::min(4*(bz+1), nz_ - 1);
        for (int by = 0; by < nby_; ++by) {
            const int j0 = 4*by, j1 = std::min(4*(by+1), ny_ - 1);
            for (int bx = 0; bx < nbx_; ++bx) {
                const int i0 = 4*bx, i1 = std::min(4*(bx+1), nx_ - 1);
                double vmin = std::numeric_limits<double>::infinity();
                double vmax = -vmin;
                for (int k = k0; k <= k1; ++k)
                    for (int j = j0; j <= j1; ++j)
                        for (int i = i0; i <= i1; ++i) {
                            const double v = vertex_value(i, j, k);
                            vmin = std::min(vmin, v);
                            vmax = std::max(vmax, v);
                        }
                Brick& br = bricks_[brick_index(bx, by, bz)];
                br.vmin = vmin;
                br.vmax = vmax;
                // Closed interval: a brick holding the isovalue on one of
                // its grid points must remain active.
                br.active = (iso_ >= vmin && iso_ <= vmax);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Trilinear cell work
// ---------------------------------------------------------------------------
namespace {

// Corner values in x-fastest local order: q = ii + 2*jj + 4*kk.
struct TriCell {
    double v[8];
    double c[8]; // monomial coefficients in local (u0,u1,u2)

    explicit TriCell(const double corners[8]) {
        for (int q = 0; q < 8; ++q) v[q] = corners[q];
        c[0] = v[0];
        c[1] = v[1] - v[0];
        c[2] = v[2] - v[0];
        c[4] = v[4] - v[0];
        c[3] = v[3] - v[2] - v[1] + v[0];
        c[5] = v[5] - v[4] - v[1] + v[0];
        c[6] = v[6] - v[4] - v[2] + v[0];
        c[7] = v[7] - v[6] - v[5] + v[4] - v[3] + v[2] + v[1] - v[0];
    }
    double value(double u0, double u1, double u2) const {
        return c[0] + c[1]*u0 + c[2]*u1 + c[4]*u2 +
               c[3]*u0*u1 + c[5]*u0*u2 + c[6]*u1*u2 +
               c[7]*u0*u1*u2;
    }
    // Analytic gradient in local coordinates.
    void grad_local(double u0, double u1, double u2, double g[3]) const {
        g[0] = c[1] + c[3]*u1 + c[5]*u2 + c[7]*u1*u2;
        g[1] = c[2] + c[3]*u0 + c[6]*u2 + c[7]*u0*u2;
        g[2] = c[4] + c[5]*u0 + c[6]*u1 + c[7]*u0*u1;
    }
};

// Multiply polynomial p (degree <= 3) by (alpha + beta*t).
void poly_mul_linear(double p[4], double alpha, double beta) {
    double out[4] = {0,0,0,0};
    for (int d = 0; d < 4; ++d) {
        out[d]     += p[d] * alpha;
        if (d < 3) out[d+1] += p[d] * beta;
    }
    for (int d = 0; d < 4; ++d) p[d] = out[d];
}

} // namespace

bool IsoGrid::solve_cell(int i, int j, int k,
                         const vec3& Of, const vec3& Df,
                         float t0, float t1,
                         std::vector<float>& out_roots,
                         bool& poly_solved) const {
    poly_solved = false;
    double corners[8];
    double vmin =  std::numeric_limits<double>::infinity();
    double vmax = -vmin;
    for (int kk = 0; kk < 2; ++kk)
        for (int jj = 0; jj < 2; ++jj)
            for (int ii = 0; ii < 2; ++ii) {
                const double val = vertex_value(i+ii, j+jj, k+kk);
                corners[ii + 2*jj + 4*kk] = val;
                vmin = std::min(vmin, val);
                vmax = std::max(vmax, val);
            }
    // Trilinearity keeps the field inside the corner range.
    if (iso_ < vmin || iso_ > vmax) return false;
    poly_solved = true;

    const TriCell cell(corners);
    const double O[3] = {Of.x, Of.y, Of.z};
    const double D[3] = {Df.x, Df.y, Df.z};
    const double sp[3] = {spacing_.x, spacing_.y, spacing_.z};
    const double or_[3] = {origin_.x, origin_.y, origin_.z};

    double alpha[3], beta[3];
    const int origin_idx[3] = {i, j, k};
    for (int a = 0; a < 3; ++a) {
        alpha[a] = (O[a] - or_[a]) / sp[a] - origin_idx[a];
        beta[a]  = D[a] / sp[a];
    }

    // Build G(t) = F(alpha + beta*t) - isovalue.
    double poly[4] = {0.0, 0.0, 0.0, 0.0};
    for (int q = 0; q < 8; ++q) {
        if (cell.c[q] == 0.0) continue;
        double factor[4] = {1.0, 0.0, 0.0, 0.0};
        if (q & 1) poly_mul_linear(factor, alpha[0], beta[0]);
        if (q & 2) poly_mul_linear(factor, alpha[1], beta[1]);
        if (q & 4) poly_mul_linear(factor, alpha[2], beta[2]);
        for (int d = 0; d < 4; ++d) poly[d] += cell.c[q] * factor[d];
    }
    poly[0] -= iso_;

    std::vector<double> roots;
    solve_polynomial(poly[3], poly[2], poly[1], poly[0], roots);
    if (roots.empty()) return false;

    const double tol_t = 1e-10 *
        std::max(1.0, std::max(std::abs(double(t0)), std::abs(double(t1))));
    const double field_scale = 1.0 + std::abs(vmax) + std::abs(vmin) +
                               std::abs(iso_);
    std::sort(roots.begin(), roots.end());

    double last_kept = -kInf;
    bool any = false;
    for (double r : roots) {
        if (r < double(t0) - tol_t || r > double(t1) + tol_t) continue;
        r = std::max(double(t0), std::min(double(t1), r));
        // Residual sanity gate against solver noise on degenerate cases.
        const double u[3] = {alpha[0] + beta[0]*r,
                             alpha[1] + beta[1]*r,
                             alpha[2] + beta[2]*r};
        if (std::abs(cell.value(u[0], u[1], u[2]) - iso_) > 1e-8*field_scale)
            continue;
        if (any && std::abs(r - last_kept) <= 8.0*tol_t) continue;
        last_kept = r;
        any = true;
        out_roots.push_back(float(r));
    }
    return any;
}

// ---------------------------------------------------------------------------
// Ray walk: brick events first, cell DDA inside active bricks.
// ---------------------------------------------------------------------------
bool IsoGrid::trace(const vec3& Of, const vec3& Df,
                    float tmin, float tmax,
                    IsoHit* hit, QueryStats* stats) const {
    QueryStats local_stats;
    if (!stats) stats = &local_stats;

    const double O[3] = {Of.x, Of.y, Of.z};
    const double D[3] = {Df.x, Df.y, Df.z};
    const double sp[3] = {spacing_.x, spacing_.y, spacing_.z};
    const double or_[3] = {origin_.x, origin_.y, origin_.z};
    const int ncells[3] = {nx_ - 1, ny_ - 1, nz_ - 1};

    // Ray vs grid box: slab intersection.
    double t_enter = double(tmin), t_leave = double(tmax);
    for (int a = 0; a < 3; ++a) {
        const double lo = or_[a];
        const double hi = or_[a] + ncells[a]*sp[a];
        if (D[a] == 0.0) {
            if (O[a] < lo || O[a] > hi) return false;
            continue;
        }
        double ta = (lo - O[a]) / D[a];
        double tb = (hi - O[a]) / D[a];
        if (ta > tb) std::swap(ta, tb);
        t_enter = std::max(t_enter, ta);
        t_leave = std::min(t_leave, tb);
    }
    if (!(t_enter < t_leave)) return false;

    AxisDDA brick_axis[3];
    const int nb[3] = {nbx_, nby_, nbz_};
    for (int a = 0; a < 3; ++a) {
        brick_axis[a].o = or_[a];
        brick_axis[a].s = sp[a];
        brick_axis[a].D = D[a];
        brick_axis[a].O = O[a];
        brick_axis[a].nblocks = nb[a];
        brick_axis[a].block = 4;
        brick_axis[a].init(t_enter);
    }

    double t = t_enter;
    const double end_eps = 1e-12 *
        std::max(1.0, std::max(std::abs(t_enter), std::abs(t_leave)));

    while (t < t_leave - end_eps) {
        double brick_end = t_leave;
        for (int a = 0; a < 3; ++a)
            brick_end = std::min(brick_end, brick_axis[a].tnext);
        const int bx = brick_axis[0].index;
        const int by = brick_axis[1].index;
        const int bz = brick_axis[2].index;
        ++stats->bricks_visited;

        if (bricks_[brick_index(bx, by, bz)].active) {
            // Cell DDA limited to this brick's ray interval [t, brick_end].
            AxisDDA cell_axis[3];
            for (int a = 0; a < 3; ++a) {
                cell_axis[a].o = or_[a];
                cell_axis[a].s = sp[a];
                cell_axis[a].D = D[a];
                cell_axis[a].O = O[a];
                cell_axis[a].nblocks = ncells[a];
                cell_axis[a].block = 1;
                cell_axis[a].init(t);
            }
            double tc = t;
            while (tc < brick_end - end_eps) {
                double cell_end = brick_end;
                for (int a = 0; a < 3; ++a)
                    cell_end = std::min(cell_end, cell_axis[a].tnext);

                const int ci[3] = {cell_axis[0].index,
                                   cell_axis[1].index,
                                   cell_axis[2].index};
                std::vector<float> roots;
                bool poly_solved = false;
                const bool had_roots = solve_cell(ci[0], ci[1], ci[2], Of, Df,
                               float(tc), float(cell_end), roots,
                               poly_solved);
                if (poly_solved) ++stats->cells_solved;
                if (had_roots) {
                    const double r = roots.front(); // sorted, earliest first
                    if (r >= double(tmin) - end_eps && r < double(tmax)) {
                        // Identify the owning cell: among the up to 8 cells
                        // containing the hit point, take the smallest
                        // x-fastest linear index. Constant work.
                        double q[3];
                        for (int a = 0; a < 3; ++a)
                            q[a] = (O[a] + r*D[a] - or_[a]) / sp[a];
                        int owner[3];
                        for (int a = 0; a < 3; ++a) {
                            int base = int(std::floor(q[a]));
                            base = std::max(0, std::min(ncells[a]-1, base));
                            owner[a] = base;
                        }
                        // Shared grid plane: the neighbor on the low side
                        // also contains this point.
                        const double plane_eps = 1e-8;
                        for (int a = 0; a < 3; ++a) {
                            const double m = std::round(q[a]);
                            if (m > 0 && m < ncells[a] &&
                                std::abs(q[a] - m) <= plane_eps) {
                                int candidate[3] = {owner[0], owner[1],
                                                    owner[2]};
                                candidate[a] = int(m) - 1;
                                const int lin_own =
                                    (owner[2]*ncells[1] + owner[1])*ncells[0] +
                                    owner[0];
                                const int lin_cand =
                                    (candidate[2]*ncells[1] + candidate[1])*
                                        ncells[0] + candidate[0];
                                if (lin_cand < lin_own)
                                    owner[a] = candidate[a];
                            }
                        }

                        // Normal from the owning cell's trilinear field.
                        double corners[8];
                        for (int kk = 0; kk < 2; ++kk)
                            for (int jj = 0; jj < 2; ++jj)
                                for (int ii = 0; ii < 2; ++ii)
                                    corners[ii + 2*jj + 4*kk] =
                                        vertex_value(owner[0]+ii,
                                                     owner[1]+jj,
                                                     owner[2]+kk);
                        const TriCell owner_cell(corners);
                        const double u[3] = {q[0] - owner[0],
                                             q[1] - owner[1],
                                             q[2] - owner[2]};
                        double gl[3];
                        owner_cell.grad_local(u[0], u[1], u[2], gl);
                        double gw[3] = {gl[0]/sp[0], gl[1]/sp[1],
                                        gl[2]/sp[2]};
                        const double gn = std::sqrt(gw[0]*gw[0] +
                                                    gw[1]*gw[1] +
                                                    gw[2]*gw[2]);

                        hit->t = float(r);
                        hit->point = vec3{float(O[0] + r*D[0]),
                                          float(O[1] + r*D[1]),
                                          float(O[2] + r*D[2])};
                        hit->normal = vec3{float(gw[0]/gn),
                                           float(gw[1]/gn),
                                           float(gw[2]/gn)};
                        hit->cell[0] = owner[0];
                        hit->cell[1] = owner[1];
                        hit->cell[2] = owner[2];
                        hit->material = material_;
                        return true;
                    }
                }

                // Advance every axis whose block ends here together, so a
                // ray crossing two or three planes at once never skips one.
                const double eps_ev = 1e-12 *
                    std::max(1.0, std::max(std::abs(tc),
                                           std::abs(cell_end)));
                for (int a = 0; a < 3; ++a) {
                    if (cell_axis[a].D != 0.0 &&
                        cell_axis[a].tnext <= cell_end + eps_ev)
                        cell_axis[a].advance();
                }
                tc = cell_end;
            }
        }

        const double eps_ev = 1e-12 *
            std::max(1.0, std::max(std::abs(t), std::abs(brick_end)));
        for (int a = 0; a < 3; ++a) {
            if (brick_axis[a].D != 0.0 &&
                brick_axis[a].tnext <= brick_end + eps_ev)
                brick_axis[a].advance();
        }
        t = brick_end;
    }
    return false;
}

bool IsoGrid::intersect(const vec3& O, const vec3& D,
                        float tmin, float tmax,
                        IsoHit& hit, QueryStats* stats) const {
    if (!std::isfinite(tmin) || !std::isfinite(tmax) ||
        !(tmin >= 0.0f) || !(tmin < tmax))
        throw std::invalid_argument("IsoGrid::intersect: require 0<=tmin<tmax");
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(O[a]) || !std::isfinite(D[a]))
            throw std::invalid_argument("IsoGrid::intercept: O and D finite");
    }
    if (D.x == 0.0f && D.y == 0.0f && D.z == 0.0f)
        throw std::invalid_argument("IsoGrid::intersect: D must be non-zero");
    return trace(O, D, tmin, tmax, &hit, stats);
}

bool IsoGrid::occluded(const vec3& O, const vec3& D,
                       float tmin, float tmax,
                       QueryStats* stats) const {
    if (!std::isfinite(tmin) || !std::isfinite(tmax) ||
        !(tmin >= 0.0f) || !(tmin < tmax))
        throw std::invalid_argument("IsoGrid::occluded: require 0<=tmin<tmax");
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(O[a]) || !std::isfinite(D[a]))
            throw std::invalid_argument("IsoGrid::occluded: O and D finite");
    }
    if (D.x == 0.0f && D.y == 0.0f && D.z == 0.0f)
        throw std::invalid_argument("IsoGrid::occluded: D must be non-zero");
    IsoHit scratch;
    return trace(O, D, tmin, tmax, &scratch, stats);
}
