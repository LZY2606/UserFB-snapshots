// Target tests for the direct isosurface intersection.  No main(): the C
// driver in task_support/driver.c runs the registered cases.  Analytic
// trilinear polynomials are built from their eight corner values so that the
// expected roots and gradients are known independently of the solver.
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <array>

#include "task_support/check.h"

#include "isosurface.h"

namespace {

// Corner values (x-fastest order i + 2*j + 4*k) of a trilinear cell given its
// eight mixed coefficients:
// F(u,v,w) = c000 + c100 u + c010 v + c001 w + c110 uv + c101 uw
//          + c011 vw + c111 uvw
std::vector<double> cell_corners(double c000, double c100, double c010, double c001,
                                 double c110, double c101, double c011, double c111) {
    return {
        c000,
        c000 + c100,
        c000 + c010,
        c000 + c100 + c010 + c110,
        c000 + c001,
        c000 + c100 + c001 + c101,
        c000 + c010 + c001 + c011,
        c000 + c100 + c010 + c001 + c110 + c101 + c011 + c111
    };
}

bool nearly(double a, double b, double eps) { return std::abs(a-b) <= eps; }

bool vec_nearly(const vec3 &a, const vec3 &b, double eps) {
    return nearly(a.x,b.x,eps) && nearly(a.y,b.y,eps) && nearly(a.z,b.z,eps);
}

const Material test_mat = {1.0f, {1.0f,0.0f,0.0f,0.0f}, {0.2f,0.5f,0.7f}, 25.0f};

// ---------------------------------------------------------------- roots group

// Along the diagonal ray u=v=w=t the chosen coefficients make the residual
// F - iso = (t-1/4)(t-1/2)(t-3/4): three distinct roots in one cell.
int roots_three_distinct() {
    const double a = 0.25, b = 0.5, c = 0.75;
    const double s2 = -(a+b+c);                 // -1.5
    const double s1 = a*b + a*c + b*c;          // 0.6875
    const double s0 = -a*b*c;                   // -0.09375
    std::vector<double> vals = cell_corners(s0, s1, 0, 0, s2, 0, 0, 1.0);
    IsoGrid grid({0,0,0}, {1,1,1}, {2,2,2}, vals, 0.0, test_mat);

    // Non-unit direction: D = k(1,1,1).  Local coordinate grows by k per t,
    // so the ray parameters of the same roots scale by 1/k.
    const double k = 2.5;
    const vec3 O{0,0,0}, D{static_cast<float>(k), static_cast<float>(k), static_cast<float>(k)};
    const double expect_t[3] = {a/k, b/k, c/k};
    double tmin = 0.0;
    for (int r = 0; r < 3; ++r) {
        IsoHit hit;
        TASK_CHECK(grid.nearest(O, D, tmin, 1.0/k, hit));
        TASK_CHECK(nearly(hit.t, expect_t[r], 1e-6));
        const double tl = k*hit.t; // local diagonal parameter
        const vec3 p{static_cast<float>(tl), static_cast<float>(tl), static_cast<float>(tl)};
        TASK_CHECK(vec_nearly(hit.point, p, 1e-6));
        // Analytic gradient on the diagonal for these coefficients.
        vec3 g{static_cast<float>(s1 + s2*tl + tl*tl),
               static_cast<float>(s2*tl + tl*tl),
               static_cast<float>(tl*tl)};
        TASK_CHECK(vec_nearly(hit.normal, g.normalized(), 1e-5));
        TASK_CHECK(nearly(hit.normal.norm(), 1.0, 1e-6));
        TASK_CHECK(hit.cell[0] == 0 && hit.cell[1] == 0 && hit.cell[2] == 0);
        tmin = hit.t + 1e-5;
    }
    IsoHit none;
    TASK_CHECK(!grid.nearest(O, D, tmin, 1.0/k, none));
    return 0;
}

// F(t,t,t) = (t-0.4)^2 (t-1.5): a tangent double root inside the cell while
// both endpoint values are negative, so endpoint sign flips alone would miss
// it.  The tangency at t=0.4 must still be reported.
int roots_tangent_double_same_sign() {
    const double r1 = 0.4, r2 = 1.5;
    const double c3 = 1.0;
    const double c2 = -(r2 + 2*r1);
    const double c1 = 2*r1*r2 + r1*r1;
    const double c0 = -r1*r1*r2;
    std::vector<double> vals = cell_corners(c0, c1, 0, 0, c2, 0, 0, c3);
    TASK_CHECK(vals[0] < 0 && vals[7] < 0); // same-sign endpoints
    IsoGrid grid({0,0,0}, {1,1,1}, {2,2,2}, vals, 0.0, test_mat);

    const vec3 O{0,0,0}, D{1,1,1};
    IsoHit hit;
    TASK_CHECK(grid.nearest(O, D, 0.0, 1.0, hit));
    TASK_CHECK(nearly(hit.t, r1, 1e-6));
    TASK_CHECK(nearly(hit.point.x, r1, 1e-6));
    vec3 g{static_cast<float>(c1 + c2*r1 + r1*r1),
           static_cast<float>(c2*r1 + r1*r1),
           static_cast<float>(r1*r1)};
    TASK_CHECK(vec_nearly(hit.normal, g.normalized(), 1e-5));
    // Past the tangency there is no further root inside the cell.
    IsoHit none;
    TASK_CHECK(!grid.nearest(O, D, r1 + 1e-4, 1.0, none));
    return 0;
}

// A tangential double root reached with one direction component exactly zero:
// F(u,v,w) = c000 + c100 u + c001 w + c101 uw restricted to the ray
// (u,v,w) = (t, 0.5, t) is (t-0.4)^2, a tangent root at t = 0.4 whose
// gradient is checked analytically.
int roots_quadratic_tangency() {
    // (t-0.4)^2 = t^2 - 0.8 t + 0.16, and on the slice v = 0.5 the field
    // becomes c000 + (c100+c001) t + c101 t^2.
    const double c101 = 1.0, c100 = -0.8, c001 = 0.0, c000 = 0.16;
    std::vector<double> vals = cell_corners(c000, c100, 0, c001, 0, c101, 0, 0);
    IsoGrid grid({0,0,0}, {1,1,1}, {2,2,2}, vals, 0.0, test_mat);
    const vec3 O{0, 0.5f, 0}, D{1, 0, 1}; // zero y component, non-unit length
    TASK_CHECK(std::abs(D.norm() - std::sqrt(2.0f)) < 1e-6f);
    IsoHit hit;
    TASK_CHECK(grid.nearest(O, D, 0.0, 1.0, hit));
    TASK_CHECK(nearly(hit.t, 0.4, 1e-6));
    TASK_CHECK(nearly(hit.point.x, 0.4, 1e-6));
    TASK_CHECK(nearly(hit.point.z, 0.4, 1e-6));
    // dF/du = c100 + c101 w = -0.8 + t ; dF/dw = c101 u = t ; dF/dv = 0
    vec3 g{static_cast<float>(-0.8 + 0.4), 0.0f, 0.4f};
    TASK_CHECK(vec_nearly(hit.normal, g.normalized(), 1e-5));
    TASK_CHECK(nearly(hit.normal.norm(), 1.0, 1e-6));
    // The tangent ray never crosses the surface, so strictly past the double
    // root there is nothing left to find.
    IsoHit none;
    TASK_CHECK(!grid.nearest(O, D, 0.4 + 1e-4, 1.0, none));
    // And the same surface crossed transversely (w fixed at 0.5): two simple
    // roots of (u-0.4)^2 = 0... instead check a shifted transverse ray for
    // which the residual is linear, confirming root ordering is t-ordered.
    const vec3 across{1,0,0};
    IsoHit a1;
    TASK_CHECK(grid.nearest({0,0.5f,0.4f}, across, 0.0, 1.0, a1));
    TASK_CHECK(nearly(a1.t, 0.4, 1e-6));
    TASK_CHECK(!grid.nearest({0,0.5f,0.4f}, across, a1.t + 1e-4, 1.0, none));
    return 0;
}

int roots_constructor_validation() {
    std::vector<double> ok(8, 0.0);
    auto fails = [&](const vec3 &o, const std::array<double,3> &sp,
                     const std::array<int,3> &cnt,
                     const std::vector<double> &values, double iso) {
        bool threw = false;
        try { IsoGrid g(o, sp, cnt, values, iso, test_mat); (void)g; }
        catch (const std::invalid_argument&) { threw = true; }
        return threw;
    };
    TASK_CHECK(fails({0,0,0}, {1,1,1}, {1,2,2}, ok, 0.0));   // count < 2
    TASK_CHECK(fails({0,0,0}, {0,1,1}, {2,2,2}, ok, 0.0));   // non-positive spacing
    TASK_CHECK(fails({0,0,0}, {1,-1,1}, {2,2,2}, ok, 0.0));  // negative spacing
    TASK_CHECK(fails({0,0,0}, {1,1,1}, {2,2,2}, std::vector<double>(7,0.0), 0.0)); // length
    std::vector<double> bad = ok; bad[3] = std::nan("");
    TASK_CHECK(fails({0,0,0}, {1,1,1}, {2,2,2}, bad, 0.0));  // non-finite scalar
    TASK_CHECK(fails({0,0,0}, {1,1,1}, {2,2,2}, ok, std::numeric_limits<double>::infinity()));
    TASK_CHECK(fails({std::numeric_limits<float>::infinity(),0,0}, {1,1,1}, {2,2,2}, ok, 0.0));
    bool good = true;
    try { IsoGrid g({0,0,0}, {1,1,1}, {2,2,2}, ok, 0.0, test_mat); (void)g; }
    catch (...) { good = false; }
    TASK_CHECK(good);
    // Deep copy: mutating the source vector must not change the grid.
    std::vector<double> src = cell_corners(-0.09375, 0.6875, 0, 0, -1.5, 0, 0, 1.0);
    IsoGrid grid({0,0,0}, {1,1,1}, {2,2,2}, src, 0.0, test_mat);
    src.assign(8, 123.0);
    IsoHit hit;
    TASK_CHECK(grid.nearest({0,0,0}, {1,1,1}, 0.0, 1.0, hit));
    TASK_CHECK(nearly(hit.t, 0.25, 1e-6));
    return 0;
}

// A thin sliver: along the diagonal the residual is
// (t-0.3)(t-0.7)(t+2) = t^3 + t^2 - 1.79 t + 0.42.  Both cell endpoints are
// positive, yet the field dips below zero twice inside the cell, so an
// endpoint sign test alone would report no surface at all.
int roots_same_sign_sliver() {
    const double c111 = 1.0, c110 = 1.0, c100 = -1.79, c000 = 0.42;
    std::vector<double> vals = cell_corners(c000, c100, 0, 0, c110, 0, 0, c111);
    // Evaluate the diagonal polynomial at both endpoints.
    auto poly = [&](double t) { return c000 + c100*t + c110*t*t + c111*t*t*t; };
    TASK_CHECK(poly(0.0) > 0.0 && poly(1.0) > 0.0);
    IsoGrid grid({0,0,0}, {1,1,1}, {2,2,2}, vals, 0.0, test_mat);

    const vec3 O{0,0,0}, D{1,1,1};
    IsoHit first;
    TASK_CHECK(grid.nearest(O, D, 0.0, 1.0, first));
    TASK_CHECK(nearly(first.t, 0.3, 1e-6));
    IsoHit second;
    TASK_CHECK(grid.nearest(O, D, first.t + 1e-5, 1.0, second));
    TASK_CHECK(nearly(second.t, 0.7, 1e-6));
    // tmin exactly on a root is included; tmax exactly on a root is excluded
    // because the interval is half-open [tmin, tmax).
    IsoHit at_min;
    TASK_CHECK(grid.nearest(O, D, 0.3, 1.0, at_min));
    TASK_CHECK(nearly(at_min.t, 0.3, 1e-6));
    IsoHit at_max;
    TASK_CHECK(!grid.nearest(O, D, 0.0, 0.3, at_max));
    // Occlusion agrees.
    TASK_CHECK(grid.occluded(O, D, 0.0, 0.5));
    TASK_CHECK(!grid.occluded(O, D, 0.0, 0.3));
    return 0;
}

// ------------------------------------------------------------ traversal group

// Large grid that is empty (scalar 0) for a long stretch and carries a small
// active patch only near its far corner.  The traversal must skip the empty
// bricks instead of scanning cells.
int traversal_empty_space_skipping() {
    constexpr int N = 18;                 // 17 cells/axis -> 5 bricks/axis
    std::vector<double> values(N*N*N, 0.0);
    for (int k = 14; k < N; ++k)
        for (int j = 14; j < N; ++j)
            for (int i = 14; i < N; ++i)
                values[i + N*(j + N*k)] = 1.0;
    IsoGrid grid({0,0,0}, {1,1,1}, {N,N,N}, values, 0.5, test_mat);

    const auto bd = grid.brick_dims();
    const long total_bricks = 1L*bd[0]*bd[1]*bd[2];
    const auto cd = grid.cell_dims();
    const long total_cells = 1L*cd[0]*cd[1]*cd[2];

    QueryStats st;
    IsoHit hit;
    const vec3 O{0.5f,0.5f,0.5f}, D{1,1,1};
    TASK_CHECK(grid.nearest(O, D, 0.0, 30.0, hit, &st));
    TASK_CHECK(hit.t > 13.0 && hit.t < 14.0); // patch starts at coordinate 14
    // The diagonal crosses exactly one brick per axis step: 5 of 125 bricks.
    std::printf("EMPTY-SKIP grid: bricks=%ld cells=%ld, visited bricks=%ld, root-solved cells=%ld\n",
                total_bricks, total_cells, st.bricks_visited, st.cells_evaluated);
    TASK_CHECK(st.bricks_visited <= 6);
    TASK_CHECK(st.bricks_visited < total_bricks / 10);
    TASK_CHECK(st.cells_evaluated <= 8);
    TASK_CHECK(st.cells_evaluated < total_cells / 100);

    // Occlusion query shares the production traversal and its statistics.
    QueryStats ost;
    TASK_CHECK(grid.occluded(O, D, 0.0, 30.0, &ost));
    TASK_CHECK(ost.bricks_visited == st.bricks_visited);
    // A bounded interval ending before the patch cannot be occluded.
    QueryStats est;
    TASK_CHECK(!grid.occluded(O, D, 0.0, 10.0, &est));
    TASK_CHECK(est.cells_evaluated == 0);
    // A ray entirely outside the box visits nothing at all.
    QueryStats mst;
    TASK_CHECK(!grid.nearest({-5,-5,-5}, {0,1,0}, 0.0, 2.0, hit, &mst));
    TASK_CHECK(mst.bricks_visited == 0 && mst.cells_evaluated == 0);
    return 0;
}

// Shared-face identity, reverse rays, outer-boundary ownership and a ray that
// crosses three grid planes simultaneously.  Grid sizes are deliberately not
// multiples of the 4-cell brick.
int traversal_cell_identity() {
    // 2 x 1 x 1 cells (counts 3,2,2): a partial brick on every axis.
    const int nx = 3, ny = 2, nz = 2;
    std::vector<double> values(nx*ny*nz);
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                values[i + nx*(j + ny*k)] = static_cast<double>(i); // F = x
    IsoGrid grid({0,0,0}, {1,1,1}, {nx,ny,nz}, values, 1.0, test_mat);

    // Hit on the shared internal face x = 1: smallest x-fastest id cell (0,0,0)
    // owns it, regardless of traversal direction.
    IsoHit fwd;
    TASK_CHECK(grid.nearest({0.5f,0.5f,0.5f}, {1,0,0}, 0.0, 2.0, fwd));
    TASK_CHECK(nearly(fwd.t, 0.5, 1e-6));
    TASK_CHECK(fwd.cell[0] == 0 && fwd.cell[1] == 0 && fwd.cell[2] == 0);
    TASK_CHECK(nearly(fwd.point.x, 1.0, 1e-6));
    IsoHit rev;
    TASK_CHECK(grid.nearest({1.5f,0.5f,0.5f}, {-1,0,0}, 0.0, 2.0, rev));
    TASK_CHECK(rev.cell[0] == 0 && rev.cell[1] == 0 && rev.cell[2] == 0);
    TASK_CHECK(nearly(rev.point.x, 1.0, 1e-6));
    TASK_CHECK(fwd.normal.x > 0 && nearly(fwd.normal.norm(), 1.0, 1e-6));

    // Outer box face x = 2 belongs to the unique inner cell (1,0,0).
    IsoHit outer;
    TASK_CHECK(grid.nearest({0.5f,0.5f,0.5f}, {1,0,0}, 0.0, 2.0, outer));
    (void)outer;
    IsoGrid grid2({0,0,0}, {1,1,1}, {nx,ny,nz}, values, 2.0, test_mat);
    IsoHit boundary;
    TASK_CHECK(grid2.nearest({0.5f,0.5f,0.5f}, {1,0,0}, 0.0, 2.0, boundary));
    TASK_CHECK(nearly(boundary.point.x, 2.0, 1e-6));
    TASK_CHECK(boundary.cell[0] == 1 && boundary.cell[1] == 0 && boundary.cell[2] == 0);

    // 5 x 4 x 3 cells (counts 6,5,4): diagonal ray crosses three planes at
    // once when it reaches vertex (1,1,1); identity stays with cell (0,0,0).
    const int CX = 6, CY = 5, CZ = 4;
    std::vector<double> sum_values(CX*CY*CZ);
    for (int k = 0; k < CZ; ++k)
        for (int j = 0; j < CY; ++j)
            for (int i = 0; i < CX; ++i)
                sum_values[i + CX*(j + CY*k)] = i + j + k;
    IsoGrid sum_grid({0,0,0}, {1,1,1}, {CX,CY,CZ}, sum_values, 3.0, test_mat);
    IsoHit corner;
    TASK_CHECK(sum_grid.nearest({0,0,0}, {1,1,1}, 0.0, 4.0, corner));
    TASK_CHECK(nearly(corner.t, 1.0, 1e-6));
    TASK_CHECK(corner.cell[0] == 0 && corner.cell[1] == 0 && corner.cell[2] == 0);
    TASK_CHECK(vec_nearly(corner.normal, vec3{1,1,1}.normalized(), 1e-5));

    // Ray with two zero direction components.  F = i+j+k = 3 along it means
    // 0.2 + (0.2+t) + 0.2 = 3 -> t = 2.4, world y = 2.6.
    IsoHit axis_hit;
    TASK_CHECK(sum_grid.nearest({0.2f,0.2f,0.2f}, {0,1,0}, 0.0, 4.0, axis_hit));
    TASK_CHECK(nearly(axis_hit.t, 2.4, 1e-6));
    TASK_CHECK(nearly(axis_hit.point.y, 2.6, 1e-6));
    TASK_CHECK(vec_nearly(axis_hit.normal, vec3{1,1,1}.normalized(), 1e-5));
    IsoHit axis_miss;
    TASK_CHECK(!sum_grid.nearest({0.2f,0.2f,0.2f}, {0,0,1}, 0.0, 0.1, axis_miss));
    return 0;
}

// Bounded shadow intervals and tmin/tmax semantics, including a surface that
// lies behind the light and therefore must not shade.
int traversal_occlusion_intervals() {
    const int n = 4;
    std::vector<double> values(n*n*n);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                values[i + n*(j + n*k)] = static_cast<double>(k); // F = z
    IsoGrid grid({0,0,0}, {1,1,1}, {n,n,n}, values, 1.5, test_mat);

    const vec3 point{0.5f,0.5f,0.2f}, dir{0,0,-1}; // box z in [0,3], ray leaves box
    TASK_CHECK(!grid.occluded(point, dir, 0.001, 2.0)); // surface at larger z, behind ray
    const vec3 down{0,0,1};
    TASK_CHECK(grid.occluded(point, down, 0.001, 2.0));       // surface at z=1.5 inside range
    TASK_CHECK(!grid.occluded(point, down, 0.001, 1.0));      // interval ends at z=1.2, short
    // Light closer than the surface: nothing may occlude.
    TASK_CHECK(!grid.occluded(point, down, 0.001, 0.5));
    // Degenerate / invalid intervals are rejected.
    TASK_CHECK(!grid.occluded(point, down, 1.0, 1.0));
    TASK_CHECK(!grid.occluded(point, down, -1.0, 1.0));
    TASK_CHECK(!grid.occluded(point, vec3{0,0,0}, 0.0, 1.0));
    return 0;
}

// ---------------------------------------------------------------- render group

// A planar isosurface at z = -6 joins the default spheres on one nearest-hit
// chain: it wins where the ray meets it, the sphere wins off to the side.
int render_scene_chain() {
    const int n = 3;
    std::vector<double> values(n*n*n);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                values[i + n*(j + n*k)] = k - 1.0; // world z + 6, iso 0 at z=-6
    Material iso_mat = red_rubber;
    iso_mat.diffuse_color = {0.1f, 0.8f, 0.2f};
    IsoGrid grid({-1,-1,-7}, {1,1,1}, {n,n,n}, values, 0.0, iso_mat);

    Scene scene = make_default_scene();
    scene.iso = &grid;

    vec3 pt, N;
    Material mat;
    TASK_CHECK(scene_intersect({0,0,0}, {0,0,-1}, scene, pt, N, mat));
    TASK_CHECK(nearly(pt.z, -6.0, 1e-5));
    TASK_CHECK(mat.diffuse_color.y > 0.7f); // isosurface material, not a sphere

    vec3 spt, sN;
    Material smat;
    const vec3 sphere_dir = vec3{-3,0,-16}.normalized(); // misses the [-1,1] xy patch
    TASK_CHECK(scene_intersect({0,0,0}, sphere_dir, scene, spt, sN, smat));
    // The grid is absent along this ray, so the chain must return exactly the
    // sphere the legacy tracer picks (the glass sphere here).
    auto [ref_hit, ref_pt, ref_n, ref_m] = scene_intersect({0,0,0}, sphere_dir);
    TASK_CHECK(ref_hit);
    TASK_CHECK(nearly(spt.z, ref_pt.z, 1e-6));
    TASK_CHECK(smat.diffuse_color.x == ref_m.diffuse_color.x &&
               smat.diffuse_color.y == ref_m.diffuse_color.y &&
               smat.diffuse_color.z == ref_m.diffuse_color.z);
    TASK_CHECK(nearly(sN.norm(), 1.0, 1e-5));

    // The same scene without a grid behaves like the legacy default scene.
    Scene plain = make_default_scene();
    vec3 p2, n2;
    Material m2;
    TASK_CHECK(scene_intersect({0,0,0}, {0,0,-1}, plain, p2, n2, m2));
    auto [legacy_hit, legacy_pt, legacy_n, legacy_m] = scene_intersect({0,0,0}, {0,0,-1});
    TASK_CHECK(legacy_hit);
    TASK_CHECK(nearly(p2.z, legacy_pt.z, 1e-6));
    return 0;
}

int render_shadow_bounds() {
    const int n = 3;
    std::vector<double> values(n*n*n);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                values[i + n*(j + n*k)] = k - 1.0; // surface at z=-6
    IsoGrid grid({-1,-1,-7}, {1,1,1}, {n,n,n}, values, 0.0, red_rubber);
    Scene scene = make_default_scene();
    scene.iso = &grid;

    const vec3 point{0,0,-5.5f};
    // Light behind the surface: the finite shadow ray to the light is blocked.
    const vec3 blocked_dir = (vec3{0,0,-8} - point).normalized();
    TASK_CHECK(scene_occluded(point, blocked_dir, (vec3{0,0,-8}-point).norm(), scene));
    // Light in front of the surface: the surface sits beyond the light and
    // must not shade it.
    const vec3 clear_dir = (vec3{0,0,-5.8f} - point).normalized();
    TASK_CHECK(!scene_occluded(point, clear_dir, (vec3{0,0,-5.8f}-point).norm(), scene));
    return 0;
}

// Real recursive pixels: finite colours, background preserved, and the
// grid-bearing scene matches the legacy result for rays that never touch it.
int render_recursive_pixels() {
    const int n = 3;
    std::vector<double> values(n*n*n);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                values[i + n*(j + n*k)] = k - 1.0;
    Material shiny = mirror;
    shiny.diffuse_color = {0.7f, 0.2f, 0.6f};
    IsoGrid grid({-1,-1,-7}, {1,1,1}, {n,n,n}, values, 0.0, shiny);
    Scene scene = make_default_scene();
    scene.iso = &grid;
    Scene plain = make_default_scene();

    const vec3 dirs[4] = {
        vec3{0,0,-1}.normalized(),
        vec3{0.3f,0.2f,-1}.normalized(),
        vec3{-3,0,-16}.normalized(),
        vec3{0,1,0}.normalized()
    };
    for (const vec3 &d : dirs) {
        vec3 a = cast_ray({0,0,0}, d, plain);
        vec3 b = cast_ray({0,0,0}, d);                 // legacy entry point
        TASK_CHECK(a.x == b.x && a.y == b.y && a.z == b.z);
        TASK_CHECK(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z));
        vec3 c = cast_ray({0,0,0}, d, scene);         // recursive trace with the grid
        TASK_CHECK(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z));
    }
    // The central ray hits the reflective isosurface and must not return the
    // sky background colour.
    vec3 central = cast_ray({0,0,0}, vec3{0,0,-1}, scene);
    TASK_CHECK(!(nearly(central.x, 0.2, 1e-6) &&
                 nearly(central.y, 0.7, 1e-6) &&
                 nearly(central.z, 0.8, 1e-6)));
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"roots",     "three_distinct_roots",        roots_three_distinct},
    {"roots",     "tangent_double_same_sign",    roots_tangent_double_same_sign},
    {"roots",     "quadratic_tangent_gradient",  roots_quadratic_tangency},
    {"roots",     "same_sign_sliver_two_roots",   roots_same_sign_sliver},
    {"roots",     "constructor_validation",      roots_constructor_validation},
    {"traversal", "empty_space_skipping",        traversal_empty_space_skipping},
    {"traversal", "cell_identity_and_boundary",  traversal_cell_identity},
    {"traversal", "occlusion_intervals",         traversal_occlusion_intervals},
    {"render",    "scene_nearest_hit_chain",     render_scene_chain},
    {"render",    "shadow_finited_interval",     render_shadow_bounds},
    {"render",    "recursive_pixels",            render_recursive_pixels}
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
