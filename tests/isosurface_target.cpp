#include "task_support/check.h"
#include "renderer.h"
#include "isosurface.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <functional>

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static std::vector<double> make_scalar(int nx, int ny, int nz,
                                       const std::function<double(int,int,int)>& f) {
    std::vector<double> a(size_t(nx)*ny*nz);
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                a[(k*ny + j)*nx + i] = f(i, j, k);
    return a;
}

// Independent trilinear evaluation in cell-local coordinates.
static double tri_eval(const double c[8], double u, double v, double w) {
    return c[0]*(1-u)*(1-v)*(1-w) + c[1]*u*(1-v)*(1-w) +
           c[2]*(1-u)*v*(1-w)     + c[3]*u*v*(1-w)     +
           c[4]*(1-u)*(1-v)*w     + c[5]*u*(1-v)*w     +
           c[6]*(1-u)*v*w         + c[7]*u*v*w;
}

static bool close(float a, double b, double eps) {
    return std::abs(double(a) - b) <= eps;
}
static bool close3(const vec3& a, const vec3& b, double eps) {
    return close(a.x,b.x,eps) && close(a.y,b.y,eps) && close(a.z,b.z,eps);
}

// ===========================================================================
// roots group: analytic trilinear polynomials with known roots/gradients
// ===========================================================================

// One diagonal cell embedded in a bigger grid. Ray in local coords is
// u(t) = (t,t,t), so F(t,t,t) is a plain cubic whose coefficients we pick.
static std::vector<double> diagonal_cell_grid(int n, const double c[8]) {
    return make_scalar(n, n, n, [&](int i, int j, int k) {
        if (i <= 1 && j <= 1 && k <= 1)
            return c[i + 2*j + 4*k];
        return -5.0; // far from the 0 isovalue outside the crafted cell
    });
}

// G(t) = (t-.2)(t-.5)(t-.8) = t^3 - 1.5 t^2 + .66 t - .08 : three roots.
static int roots_three_roots(void) {
    // c7=1, c3=-1.5, c1=.66, c0=-.08 (all other monomial coeffs zero)
    const double c[8] = {-.08, .58, -.08, -.92, -.08, .58, -.08, .08};
    const int n = 6;
    auto scalar = diagonal_cell_grid(n, c);
    IsoGrid grid({0,0,0}, {1,1,1}, n, n, n, scalar, 0.0, ivory);

    const vec3 O{0,0,0}, D{1,1,1};
    IsoHit hit;
    TASK_CHECK(grid.intersect(O, D, 0.f, 1.f, hit));
    TASK_CHECK(close(hit.t, 0.2, 1e-6));
    TASK_CHECK(close3(hit.point, vec3{.2f,.2f,.2f}, 1e-6));

    // Analytic gradient of the trilinear field at the first root.
    const double u = .2;
    double g[3] = {
        .66 + (-1.5)*u + u*u,       // c1 + c3*u1 + c7*u1*u2
        (-1.5)*u + u*u,             // c3*u0 + c7*u0*u2
        u*u                          // c7*u0*u1
    };
    double gn = std::sqrt(g[0]*g[0]+g[1]*g[1]+g[2]*g[2]);
    TASK_CHECK(close3(hit.normal,
        vec3{float(g[0]/gn),float(g[1]/gn),float(g[2]/gn)}, 1e-5));
    TASK_CHECK(std::abs(hit.normal.norm()-1.f) < 1e-5f);
    TASK_CHECK(hit.cell[0]==0 && hit.cell[1]==0 && hit.cell[2]==0);

    // Probe all three roots through finite intervals; sign changes alone at
    // the endpoints would not be enough to prove they all exist.
    TASK_CHECK(!grid.occluded(O, D, 0.f, .1f));
    TASK_CHECK( grid.occluded(O, D, 0.f, .3f));       // root .2
    TASK_CHECK(!grid.occluded(O, D, .21f, .3f));
    TASK_CHECK( grid.occluded(O, D, .21f, .6f));      // root .5
    TASK_CHECK(!grid.occluded(O, D, .51f, .6f));
    TASK_CHECK( grid.occluded(O, D, .51f, .9f));      // root .8
    TASK_CHECK(!grid.occluded(O, D, .81f, .95f));

    // Independent residual check of the crafted polynomial.
    for (double t : {.2, .5, .8})
        TASK_CHECK(std::abs(tri_eval(c, t, t, t)) < 1e-10);
    return 0;
}

// G(t) = (t-.4)^2 (t+1): double root, positive on both cell endpoints.
static int roots_double_same_sign(void) {
    // c7=1, c3=.2, c1=-.64, c0=.16
    const double c[8] = {.16, -.48, .16, -.28, .16, -.48, .16, .72};
    const int n = 6;
    auto scalar = diagonal_cell_grid(n, c);
    IsoGrid grid({0,0,0}, {1,1,1}, n, n, n, scalar, 0.0, glass);

    TASK_CHECK(tri_eval(c, 0, 0, 0) > 0.0); // endpoints same sign ...
    TASK_CHECK(tri_eval(c, 1, 1, 1) > 0.0); // ... yet a root hides inside

    IsoHit hit;
    TASK_CHECK(grid.intersect({0,0,0}, {1,1,1}, 0.f, 1.f, hit));
    TASK_CHECK(close(hit.t, .4, 1e-6));

    double g[3] = { -.64 + .2*.4 + .4*.4, .2*.4 + .4*.4, .4*.4 };
    double gn = std::sqrt(g[0]*g[0]+g[1]*g[1]+g[2]*g[2]);
    TASK_CHECK(close3(hit.normal,
        vec3{float(g[0]/gn),float(g[1]/gn),float(g[2]/gn)}, 1e-5));
    return 0;
}

// G(t) = (t-.5)^2, a pure quadratic tangency: the ray grazes the surface.
static int roots_tangent(void) {
    // c7=0, c3=1, c1=-1, c0=.25
    const double c[8] = {.25, -.75, .25, .25, .25, -.75, .25, .25};
    const int n = 6;
    auto scalar = diagonal_cell_grid(n, c);
    IsoGrid grid({0,0,0}, {1,1,1}, n, n, n, scalar, 0.0, red_rubber);

    IsoHit hit;
    TASK_CHECK(grid.intersect({0,0,0}, {1,1,1}, 0.f, 1.f, hit));
    TASK_CHECK(close(hit.t, .5, 1e-6));
    TASK_CHECK(close3(hit.point, vec3{.5f,.5f,.5f}, 1e-6));
    double g[3] = { -1.0 + .5, .5, 0.0 };
    double gn = std::sqrt(g[0]*g[0]+g[1]*g[1]);
    TASK_CHECK(close3(hit.normal,
        vec3{float(g[0]/gn),float(g[1]/gn),0.f}, 1e-5));
    return 0;
}

// Linear analytic field: gradient direction, reverse rays, non-unit D.
static int roots_gradient_and_reverse(void) {
    const int n = 6;
    auto scalar = make_scalar(n, n, n, [](int i, int j, int k) {
        (void)j; (void)k; return double(i) - 2.5; // surface plane x = 2.5
    });
    IsoGrid grid({0,0,0}, {1,1,1}, n, n, n, scalar, 0.0, mirror);

    IsoHit hit;
    // Non-unit direction; it must not be normalized internally.
    TASK_CHECK(grid.intersect({0,1.2f,.7f}, {3.0f,0,0}, 0.f, 10.f, hit));
    TASK_CHECK(close(hit.t, 2.5/3.0, 1e-6));
    TASK_CHECK(close3(hit.normal, vec3{1,0,0}, 1e-5));

    // Reverse ray: t is measured along the given direction, normal still
    // points toward increasing scalar.
    TASK_CHECK(grid.intersect({4.5f,1.2f,.7f}, {-2.0f,0,0}, 0.f, 10.f, hit));
    TASK_CHECK(close(hit.t, 2.0/2.0, 1e-6));
    TASK_CHECK(close3(hit.point, vec3{2.5f,1.2f,.7f}, 1e-6));
    TASK_CHECK(close3(hit.normal, vec3{1,0,0}, 1e-5));

    // Field decreasing in x: normal flips.
    auto scalar2 = make_scalar(n, n, n, [](int i, int j, int k) {
        (void)j; (void)k; return 2.5 - double(i);
    });
    IsoGrid grid2({0,0,0}, {1,1,1}, n, n, n, scalar2, 0.0, mirror);
    TASK_CHECK(grid2.intersect({0,1,1}, {1,0,0}, 0.f, 10.f, hit));
    TASK_CHECK(close3(hit.normal, vec3{-1,0,0}, 1e-5));
    return 0;
}

// ===========================================================================
// traversal group
// ===========================================================================

// Hits lying on shared cell faces: identity is the minimum x-fastest cell;
// a hit on the outer box face belongs to the unique inner cell.
static int trav_identity_face(void) {
    const int n = 6;
    auto scalar = make_scalar(n, n, n, [](int i, int j, int k) {
        (void)j; (void)k; return double(i) - 2.0; // plane x = 2
    });
    IsoGrid grid({0,0,0}, {1,1,1}, n, n, n, scalar, 0.0, ivory);

    IsoHit hit;
    // Hit point (2,3,.3): shared faces in x (cells 1/2) and y (2/3).
    TASK_CHECK(grid.intersect({0,1,.3f}, {1,1,0}, 0.f, 10.f, hit));
    TASK_CHECK(close3(hit.point, vec3{2,3,.3f}, 1e-6));
    TASK_CHECK(hit.cell[0]==1 && hit.cell[1]==2 && hit.cell[2]==0);
    TASK_CHECK(close3(hit.normal, vec3{1,0,0}, 1e-5));

    // Outer box boundary x = 5: unique inner cell i = nx-2. The ray's y is
    // kept off grid planes so j is unambiguous.
    auto scalar_b = make_scalar(n, n, n, [](int i, int j, int k) {
        (void)j; (void)k; return double(i) - 5.0;
    });
    IsoGrid gridb({0,0,0}, {1,1,1}, n, n, n, scalar_b, 0.0, ivory);
    TASK_CHECK(gridb.intersect({0,2.3f,.3f}, {1,0,0}, 0.f, 10.f, hit));
    TASK_CHECK(close3(hit.point, vec3{5,2.3f,.3f}, 1e-6));
    TASK_CHECK(hit.cell[0]==n-2 && hit.cell[1]==2 && hit.cell[2]==0);
    return 0;
}

// Long empty run, one small active patch far away: the walk must skip empty
// bricks instead of scanning the grid.
static int trav_skip_empty(void) {
    const int nx = 40, ny = 7, nz = 5; // non-multiple-of-4 dimensions
    auto scalar = make_scalar(nx, ny, nz, [](int i, int j, int k) {
        (void)k;
        // small patch around j in {2,3}, transition between i 32 and 33
        if (i >= 33 && (j == 2 || j == 3)) return 5.0;
        return -5.0;
    });
    IsoGrid grid({0,0,0}, {1,1,1}, nx, ny, nz, scalar, 0.0, glass);

    QueryStats st{};
    IsoHit hit;
    TASK_CHECK(grid.intersect({-2,2.5f,2.5f}, {1,0,0}, 0.f, 100.f, hit, &st));
    TASK_CHECK(close(hit.t, 34.5, 1e-6)); // cell i=32, local u=.5 -> x 32.5

    const int total_bricks = ((nx-1+3)/4)*((ny-1+3)/4)*((nz-1+3)/4);
    const int total_cells  = (nx-1)*(ny-1)*(nz-1);
    printf("TRAV skip-empty: bricks_visited=%llu cells_solved=%llu "
           "(grid has %d bricks, %d cells)\n",
           st.bricks_visited, st.cells_solved, total_bricks, total_cells);
    TASK_CHECK(st.bricks_visited < (unsigned long long)total_bricks);
    TASK_CHECK(st.cells_solved <= 4);          // only the one active brick
    TASK_CHECK(st.cells_solved >= 1);

    // Ray missing the patch in y: every brick on its path is inactive.
    QueryStats st2{};
    TASK_CHECK(!grid.occluded({-2,5.5f,2.5f}, {1,0,0}, 0.f, 100.f, &st2));
    printf("TRAV miss-ray:   bricks_visited=%llu cells_solved=%llu\n",
           st2.bricks_visited, st2.cells_solved);
    TASK_CHECK(st2.cells_solved == 0);
    TASK_CHECK(st2.bricks_visited >= 1);
    return 0;
}

// Partial bricks, non-unit direction, non-cubic spacing: compare against the
// independent analytic plane x+y+z = isovalue.
static int trav_partial_brick_nonunit(void) {
    const int nx=7, ny=5, nz=3;
    const vec3 org{-1,-1,-1}, sp{.5f,.7f,1.3f};
    auto scalar = make_scalar(nx, ny, nz, [&](int i, int j, int k) {
        double x = org.x + i*sp.x, y = org.y + j*sp.y, z = org.z + k*sp.z;
        return x + y + z;
    });
    const double iso = 1.2;
    IsoGrid grid(org, sp, nx, ny, nz, scalar, iso, red_rubber);

    const vec3 O{.5f,-.5f,.5f}, D{2.3f,-.4f,.9f}; // non-unit
    const double t_an = (iso - (O.x+O.y+O.z)) / (D.x+D.y+D.z);
    IsoHit hit;
    TASK_CHECK(grid.intersect(O, D, 0.f, 100.f, hit));
    TASK_CHECK(close(hit.t, t_an, 1e-6));
    TASK_CHECK(close3(hit.point,
        vec3{float(O.x+t_an*D.x),float(O.y+t_an*D.y),float(O.z+t_an*D.z)},
        1e-6));
    TASK_CHECK(close3(hit.normal,
        vec3{1.f/std::sqrt(3.f),1.f/std::sqrt(3.f),1.f/std::sqrt(3.f)},
        1e-5));

    // Construction must reject invalid arguments.
    bool threw = false;
    try {
        IsoGrid bad(org, sp, 1, ny, nz, scalar, iso, red_rubber);
    } catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    threw = false;
    try {
        IsoGrid bad(org, {1.f,-1.f,1.f}, nx, ny, nz, scalar, iso, red_rubber);
    } catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    threw = false;
    try {
        std::vector<double> short_data(3, 0.0);
        IsoGrid bad(org, sp, nx, ny, nz, short_data, iso, red_rubber);
    } catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    return 0;
}

// Zero direction components, origin inside the box, simultaneous three-plane
// crossings, and rays that never meet the surface.
static int trav_axes_and_diagonal(void) {
    const int n = 6;
    auto scalar = make_scalar(n, n, n, [](int i, int j, int k) {
        (void)j; (void)k; return double(i) - 2.5; // plane x = 2.5
    });
    IsoGrid grid({0,0,0}, {1,1,1}, n, n, n, scalar, 0.0, ivory);
    IsoHit hit;

    // Zero y/z components, axis-aligned hit.
    TASK_CHECK(grid.intersect({.5f,1.2f,.7f}, {1,0,0}, 0.f, 10.f, hit));
    TASK_CHECK(close(hit.t, 2.0, 1e-6));
    TASK_CHECK(hit.cell[0]==2);

    // Origin inside the box, ray pointing away from the surface: clean miss.
    TASK_CHECK(!grid.intersect({1.5f,1.5f,1.5f}, {-1,0,0}, 0.f, 10.f, hit));
    TASK_CHECK( grid.intersect({1.5f,1.5f,1.5f}, { 1,0,0}, 0.f, 10.f, hit));
    TASK_CHECK(close(hit.t, 1.0, 1e-6));

    // Zero x component and no crossing: must terminate, not loop.
    TASK_CHECK(!grid.intersect({1.2f,.5f,.5f}, {0,1,0}, 0.f, 10.f, hit));
    TASK_CHECK(!grid.intersect({1.2f,.5f,.5f}, {0,0,1}, 0.f, 10.f, hit));

    // Diagonal ray crosses three grid planes at every step simultaneously.
    TASK_CHECK(grid.intersect({.5f,.5f,.5f}, {1,1,1}, 0.f, 10.f, hit));
    TASK_CHECK(close(hit.t, 2.0, 1e-6));
    TASK_CHECK(close3(hit.point, vec3{2.5f,2.5f,2.5f}, 1e-6));
    TASK_CHECK(hit.cell[0]==2 && hit.cell[1]==2 && hit.cell[2]==2);
    return 0;
}

// occluded() honors the finite [tmin, tmax) shadow interval.
static int trav_occlusion_bounds(void) {
    const int n = 6;
    auto scalar = make_scalar(n, n, n, [](int i, int j, int k) {
        (void)j; (void)k; return double(i) - 2.5;
    });
    IsoGrid grid({0,0,0}, {1,1,1}, n, n, n, scalar, 0.0, ivory);

    TASK_CHECK(!grid.occluded({0,1,1}, {1,0,0}, 0.f, 2.0f));   // before
    TASK_CHECK( grid.occluded({0,1,1}, {1,0,0}, 0.f, 3.0f));   // spans root
    TASK_CHECK( grid.occluded({0,1,1}, {1,0,0}, 2.4f, 3.0f));  // window on it
    TASK_CHECK(!grid.occluded({0,1,1}, {1,0,0}, 2.6f, 3.0f));  // past it
    // Half-open interval: ending exactly at the root does not count.
    TASK_CHECK(!grid.occluded({0,1,1}, {1,0,0}, 0.f, 2.5f));
    return 0;
}

// ===========================================================================
// render group
// ===========================================================================

// Sphere-like trilinear field used by the render cases.
static IsoGrid make_sphere_grid(Material mat) {
    const vec3 c{3,0,-9};
    const double R2 = 1.5*1.5;
    const int n = 5;
    auto scalar = make_scalar(n, n, n, [&](int i, int j, int k) {
        vec3 p{1.f+i, -2.f+j, -11.f+k};
        double dx=p.x-c.x, dy=p.y-c.y, dz=p.z-c.z;
        return R2 - (dx*dx+dy*dy+dz*dz);
    });
    return IsoGrid({1,-2,-11}, {1,1,1}, n, n, n, scalar, 0.0, mat);
}

// The isosurface joins the nearest-hit chain with spheres and the floor.
static int render_iso_hit_and_material(void) {
    IsoGrid grid = make_sphere_grid(mirror);
    Scene scene;
    scene.scene_spheres.assign(std::begin(spheres), std::end(spheres));
    scene.scene_lights.assign(std::begin(lights), std::end(lights));
    scene.iso = &grid;

    vec3 dir = vec3{3,0,-9}.normalized();
    auto [hit, point, N, mat] = scene_intersect({0,0,0}, dir, scene);
    TASK_CHECK(hit);
    TASK_CHECK(mat.albedo[1] == mirror.albedo[1]); // iso won the nearest chain
    TASK_CHECK(std::abs(N.norm()-1.f) < 1e-5f);

    vec3 color = cast_ray({0,0,0}, dir, scene);
    TASK_CHECK(std::isfinite(color.x) && std::isfinite(color.y) &&
               std::isfinite(color.z));
    TASK_CHECK(!close3(color, vec3{.2f,.7f,.8f}, 1e-6)); // not background

    // A Scene without an iso behaves exactly like the default scene.
    const Scene& def = default_scene();
    for (vec3 d : {dir, vec3{0,1,0}, vec3{-3,0,-16}.normalized()}) {
        vec3 a = cast_ray({0,0,0}, d);
        vec3 b = cast_ray({0,0,0}, d, def);
        TASK_CHECK(a.x==b.x && a.y==b.y && a.z==b.z);
    }
    vec3 sky = cast_ray({0,0,0}, {0,1,0});
    TASK_CHECK(std::abs(sky.y-.7f) < 1e-5f);
    return 0;
}

// Surface beyond the light must not shadow: bounded shadow interval.
static int render_shadow_bounded(void) {
    IsoGrid grid = make_sphere_grid(ivory);
    const vec3 P{0,0,0};
    const vec3 L{1,0,-3};
    vec3 ldir = (L - P).normalized();
    float ldist = (L - P).norm();

    // The sphere surface starts ~8 units away, the light only ~3.16.
    TASK_CHECK(!grid.occluded(P, ldir, .001f, ldist));
    TASK_CHECK( grid.occluded(P, ldir, .001f, 20.f));

    // Full shading path: a floor point lit by a light placed between it and
    // the far isosurface. With bounded shadow rays the iso cannot darken it,
    // so the shaded pixel must match the no-iso scene exactly.
    const vec3 floor_point{0,-4,-20};
    const vec3 prim_dir = floor_point.normalized();
    const vec3 to_iso = vec3{3,0,-9} - floor_point;
    const vec3 light = floor_point + to_iso * 0.3f; // before the iso

    Scene scene;
    scene.scene_spheres.assign(std::begin(spheres), std::end(spheres));
    scene.scene_lights = {light};
    scene.iso = &grid;
    vec3 with_far_iso = cast_ray(P, prim_dir, scene);
    scene.iso = nullptr;
    vec3 without_iso = cast_ray(P, prim_dir, scene);
    TASK_CHECK(!close3(with_far_iso, vec3{.2f,.7f,.8f}, 1e-6)); // floor hit
    TASK_CHECK(close3(with_far_iso, without_iso, 1e-6));
    return 0;
}

// A real recursive framebuffer: reflection/refraction carry the same Scene.
static int render_recursive_framebuffer(void) {
    IsoGrid grid = make_sphere_grid(glass);
    Scene scene;
    scene.scene_spheres.assign(std::begin(spheres), std::end(spheres));
    scene.scene_lights.assign(std::begin(lights), std::end(lights));
    scene.iso = &grid;

    constexpr int w = 48, h = 36;
    constexpr float fov = 1.05f;
    int non_background = 0;
    for (int py = 0; py < h; ++py)
        for (int px = 0; px < w; ++px) {
            float dx = (px + .5f) - w/2.f;
            float dy = -(py + .5f) + h/2.f;
            float dz = -h/(2.f*std::tan(fov/2.f));
            vec3 d = vec3{dx,dy,dz}.normalized();
            vec3 col = cast_ray({0,0,0}, d, scene);
            TASK_CHECK(std::isfinite(col.x) && std::isfinite(col.y) &&
                       std::isfinite(col.z));
            if (!close3(col, vec3{.2f,.7f,.8f}, 1e-5)) ++non_background;
        }
    printf("RENDER framebuffer: %d/%d pixels hit scene geometry\n",
           non_background, w*h);
    TASK_CHECK(non_background > w*h/4);
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
extern const struct task_case task_cases[] = {
    {"roots",     "three_roots",            roots_three_roots},
    {"roots",     "double_same_sign",       roots_double_same_sign},
    {"roots",     "tangent",                roots_tangent},
    {"roots",     "gradient_and_reverse",   roots_gradient_and_reverse},
    {"traversal", "identity_face",          trav_identity_face},
    {"traversal", "skip_empty",             trav_skip_empty},
    {"traversal", "partial_brick_nonunit",  trav_partial_brick_nonunit},
    {"traversal", "axes_and_diagonal",      trav_axes_and_diagonal},
    {"traversal", "occlusion_bounds",       trav_occlusion_bounds},
    {"render",    "iso_hit_and_material",   render_iso_hit_and_material},
    {"render",    "shadow_bounded",         render_shadow_bounded},
    {"render",    "recursive_framebuffer",  render_recursive_framebuffer},
};
extern const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
