#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

// Deterministic failing allocator used by the failure-recovery checks.
static int task_alloc_calls;
static int task_alloc_fail_at;
static void* task_malloc(size_t n)
{
    if (task_alloc_fail_at > 0 && ++task_alloc_calls >= task_alloc_fail_at) {
        return 0;
    }
    return malloc(n);
}
static void* task_calloc(size_t a, size_t b)
{
    if (task_alloc_fail_at > 0 && ++task_alloc_calls >= task_alloc_fail_at) {
        return 0;
    }
    return calloc(a, b);
}
static void* task_realloc(void* p, size_t n)
{
    if (task_alloc_fail_at > 0 && ++task_alloc_calls >= task_alloc_fail_at) {
        return 0;
    }
    return realloc(p, n);
}
#define PAR_MALLOC(T, N) task_malloc(sizeof(T) * (N))
#define PAR_CALLOC(T, N) task_calloc(sizeof(T), N)
#define PAR_REALLOC(T, B, N) task_realloc(B, sizeof(T) * (N))
#define PAR_SHAPES_REALLOC(T, B, N, ON) task_realloc(B, sizeof(T) * (N))
#define PAR_FREE(B) free(B)

#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

#ifndef PAR_PI
#define PAR_PI (3.14159265359f)
#endif

// ---- callback surfaces -----------------------------------------------------

static void fn_plane(float const* uv, float* p, void* user)
{
    (void) user;
    p[0] = uv[0];
    p[1] = uv[1];
    p[2] = 0.0f;
}

// The bump surface mandated by the task: P = (u, v, h).
static void fn_bump(float const* uv, float* p, void* user)
{
    (void) user;
    float du = uv[0] - 0.25f;
    float dv = uv[1] - 0.25f;
    float r = hypotf(du, dv) / 0.18f;
    float h = r < 1.0f ? 0.2f * powf(1.0f - r * r, 3.0f) : 0.0f;
    p[0] = uv[0];
    p[1] = uv[1];
    p[2] = h;
}

static void fn_torus(float const* uv, float* p, void* user)
{
    (void) user;
    float theta = uv[0] * 2.0f * PAR_PI;
    float phi = uv[1] * 2.0f * PAR_PI;
    float minor = 0.3f;
    float beta = 1.0f + minor * cosf(phi);
    p[0] = cosf(theta) * beta;
    p[1] = sinf(theta) * beta;
    p[2] = minor * sinf(phi);
}

// Smooth bump straddling the periodic u=0 seam, on a small-radius cylinder
// whose base curvature already satisfies the tolerance at depth 2, so only
// the bump drives localized refinement across the seam.
static void fn_seam_bump(float const* uv, float* p, void* user)
{
    (void) user;
    float du = fminf(uv[0], 1.0f - uv[0]);
    float dv = uv[1] - 0.5f;
    float r = hypotf(du, dv) / 0.18f;
    float h = r < 1.0f ? 0.05f * powf(1.0f - r * r, 3.0f) : 0.0f;
    float theta = uv[0] * 2.0f * PAR_PI;
    float radius = 0.005f;
    p[0] = radius * cosf(theta);
    p[1] = uv[1];
    p[2] = radius * sinf(theta) + h;
}

// Two parallel sheets, open (non-periodic), whose distinct parameter points
// almost coincide in space but carry opposite normals.
static void fn_fold(float const* uv, float* p, void* user)
{
    (void) user;
    int left = uv[0] < 0.5f;
    float g = left ? (0.5f - uv[0]) : (uv[0] - 0.5f);
    float z = (left ? -1.0f : 1.0f) * 1.0e-4f;
    p[0] = g;
    p[1] = uv[1];
    p[2] = z;
}

// Non-aliasing oscillation that cannot satisfy a tight tolerance by depth 4.
static void fn_wave3(float const* uv, float* p, void* user)
{
    (void) user;
    p[0] = uv[0];
    p[1] = uv[1];
    p[2] = 0.5f * sinf(2.0f * PAR_PI * 3.0f * uv[0]);
}

// Pure quadratic; its bilinear error shrinks by four at every level, so a
// tolerance between the depth-7 and depth-8 errors drives a uniform depth-8
// grid (used to exercise index capacity).
static void fn_quad(float const* uv, float* p, void* user)
{
    (void) user;
    p[0] = uv[0];
    p[1] = uv[1];
    p[2] = uv[0] * uv[0];
}

static void fn_bad(float const* uv, float* p, void* user)
{
    (void) user;
    if (uv[0] > 0.5f && uv[1] > 0.5f) {
        p[0] = NAN;
        p[1] = 0.0f;
        p[2] = 0.0f;
        return;
    }
    p[0] = uv[0];
    p[1] = uv[1];
    p[2] = 0.0f;
}

// A collapsed surface: every parameter maps to one point, so every emitted
// triangle is geometrically degenerate and generation must fail.
static void fn_constant(float const* uv, float* p, void* user)
{
    (void) uv;
    (void) user;
    p[0] = 1.0f;
    p[1] = 2.0f;
    p[2] = 3.0f;
}

// ---- independent helpers, built only from the callback and leaf ranges ---

static float vec_dist(float const* a, float const* b)
{
    float x = a[0] - b[0];
    float y = a[1] - b[1];
    float z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

// Recompute the prescribed nine-sample bilinear error for one leaf directly
// from the callback, without consulting anything the generator produced.
static float independent_eta(par_shapes_fn fn, void* user, int d, int ix,
    int iy)
{
    int n = 1 << d;
    float u0 = (float) ix / n;
    float u1 = (float) (ix + 1) / n;
    float v0 = (float) iy / n;
    float v1 = (float) (iy + 1) / n;
    float corner[4][3];
    float uv[2];
    float cu[4] = {u0, u1, u1, u0};
    float cv[4] = {v0, v0, v1, v1};
    for (int k = 0; k < 4; k++) {
        uv[0] = cu[k];
        uv[1] = cv[k];
        fn(uv, corner[k], user);
    }
    // Five interior samples with their bilinear weights (s, t).
    float su[5] = {0.5f, 1.0f, 0.5f, 0.0f, 0.5f};
    float sv[5] = {0.0f, 0.5f, 1.0f, 0.5f, 0.5f};
    float worst = 0.0f;
    for (int k = 0; k < 5; k++) {
        float s = su[k];
        float t = sv[k];
        float w[4] = {
            (1 - s) * (1 - t), s * (1 - t), s * t, (1 - s) * t
        };
        float bil[3] = {0, 0, 0};
        for (int c = 0; c < 4; c++) {
            for (int j = 0; j < 3; j++) {
                bil[j] += w[c] * corner[c][j];
            }
        }
        float truep[3];
        uv[0] = u0 + s * (u1 - u0);
        uv[1] = v0 + t * (v1 - v0);
        fn(uv, truep, user);
        float dist = vec_dist(bil, truep);
        if (dist > worst) {
            worst = dist;
        }
    }
    return worst;
}

static par_shapes_adaptive_leaf const* find_leaf(
    par_shapes_adaptive const* a, int d, int ix, int iy)
{
    for (int i = 0; i < a->nleaves; i++) {
        par_shapes_adaptive_leaf const* L = a->leaves + i;
        if (L->depth == d && L->ix == ix && L->iy == iy) {
            return L;
        }
    }
    return 0;
}

static int find_vertex(par_shapes_mesh const* m, float u, float v)
{
    for (int i = 0; i < m->npoints; i++) {
        if (fabsf(m->tcoords[2 * i] - u) < 1.0e-6f &&
            fabsf(m->tcoords[2 * i + 1] - v) < 1.0e-6f) {
            return i;
        }
    }
    return -1;
}

static int count_depth(par_shapes_adaptive const* a, int d)
{
    int n = 0;
    for (int i = 0; i < a->nleaves; i++) {
        if (a->leaves[i].depth == d) {
            n++;
        }
    }
    return n;
}

// Depth of the leaf region across one edge of L, probing the finer (d+1),
// same (d) and coarser (d-1) levels.  Periodic axes wrap.  Returns -1 if no
// leaf occupies the neighbouring region.
static int neighbor_depth(par_shapes_adaptive const* a,
    par_shapes_adaptive_leaf const* L, int edge, int wrap_u, int wrap_v)
{
    static int const step[4][2] = {
        {0, -1}, {1, 0}, {0, 1}, {-1, 0}
    };
    int d = L->depth;
    int q = 1 << d;
    int nix = L->ix + step[edge][0];
    int niy = L->iy + step[edge][1];
    if (wrap_u) {
        nix = (nix + q) % q;
    }
    if (wrap_v) {
        niy = (niy + q) % q;
    }
    if ((!wrap_u && (nix < 0 || nix >= q)) ||
        (!wrap_v && (niy < 0 || niy >= q))) {
        return -1;
    }
    // Finer children of the neighbouring cell that touch the shared edge.
    int c0u = 2 * nix;
    int c0v = 2 * niy;
    if (edge == 1) {
        c0u = 2 * nix;
    } else if (edge == 3) {
        c0u = 2 * nix + 1;
    }
    if (edge == 0) {
        c0v = 2 * niy + 1;
    } else if (edge == 2) {
        c0v = 2 * niy;
    }
    int fine = 0;
    if (edge == 0 || edge == 2) {
        fine = find_leaf(a, d + 1, c0u, c0v) != 0 ||
            find_leaf(a, d + 1, c0u + 1, c0v) != 0;
    } else {
        fine = find_leaf(a, d + 1, c0u, c0v) != 0 ||
            find_leaf(a, d + 1, c0u, c0v + 1) != 0;
    }
    if (fine) {
        return d + 1;
    }
    if (find_leaf(a, d, nix, niy)) {
        return d;
    }
    if (d > 2 && find_leaf(a, d - 1, nix / 2, niy / 2)) {
        return d - 1;
    }
    return -1;
}

// Triangles that belong to a leaf's fan are exactly those referencing its
// center vertex.
static int fan_uses_vertex(par_shapes_adaptive const* a,
    par_shapes_adaptive_leaf const* leaf, int vertex)
{
    par_shapes_mesh const* m = a->mesh;
    for (int f = 0; f < m->ntriangles; f++) {
        if (m->triangles[3 * f] == leaf->center) {
            if (m->triangles[3 * f + 1] == vertex ||
                m->triangles[3 * f + 2] == vertex) {
                return 1;
            }
        }
    }
    return 0;
}

// ---- refine ---------------------------------------------------------------

static void group_refine(void)
{
    // A flat plane never needs refinement: exactly the sixteen depth-2
    // leaves, and every independently measured error is zero.
    par_shapes_adaptive* plane =
        par_shapes_create_adaptive(fn_plane, 0, 0.002f, 7, false, false);
    TASK_CHECK(plane);
    TASK_CHECK(plane->nleaves == 16);
    TASK_CHECK(count_depth(plane, 2) == 16);
    int zero_error = 1;
    for (int i = 0; i < plane->nleaves; i++) {
        par_shapes_adaptive_leaf const* L = plane->leaves + i;
        if (independent_eta(fn_plane, 0, L->depth, L->ix, L->iy) > 0.0f) {
            zero_error = 0;
        }
    }
    TASK_CHECK(zero_error);
    par_shapes_free_adaptive(plane);

    // Localized bump, tolerance 0.002, max depth 7.
    par_shapes_adaptive* bump =
        par_shapes_create_adaptive(fn_bump, 0, 0.002f, 7, false, false);
    TASK_CHECK(bump);
    // At least four coarse depth-2 leaves survive away from the bump.
    TASK_CHECK(count_depth(bump, 2) >= 4);
    // Refinement really happened, but stayed local: far short of a uniform
    // deepest grid, which would hold 4^7 = 16384 leaves.
    TASK_CHECK(count_depth(bump, 7) > 0);
    TASK_CHECK(bump->nleaves < 4096);
    // Every emitted leaf independently meets the measured tolerance.
    int all_within = 1;
    for (int i = 0; i < bump->nleaves; i++) {
        par_shapes_adaptive_leaf const* L = bump->leaves + i;
        if (independent_eta(fn_bump, 0, L->depth, L->ix, L->iy) > 0.002f) {
            all_within = 0;
        }
    }
    TASK_CHECK(all_within);
    // The depth-2 cell under the bump genuinely exceeded the tolerance, so
    // refinement was driven by the error rather than performed blindly.
    TASK_CHECK(independent_eta(fn_bump, 0, 2, 1, 1) > 0.002f);
    par_shapes_free_adaptive(bump);
}

// ---- transition ------------------------------------------------------------

typedef struct {
    int lo, hi, dir; // dir +1 if a triangle traverses lo->hi, -1 if hi->lo
} task_edge;

static int edge_cmp(void const* x, void const* y)
{
    task_edge const* a = x;
    task_edge const* b = y;
    if (a->lo != b->lo) {
        return a->lo < b->lo ? -1 : 1;
    }
    if (a->hi != b->hi) {
        return a->hi < b->hi ? -1 : 1;
    }
    return 0;
}

static void group_transition(void)
{
    par_shapes_adaptive* a =
        par_shapes_create_adaptive(fn_bump, 0, 0.002f, 7, false, false);
    TASK_CHECK(a);
    par_shapes_mesh* m = a->mesh;

    // Leaves partition the unit square without overlap.
    double coverage = 0.0;
    int unique = 1;
    for (int i = 0; i < a->nleaves; i++) {
        double side = 1.0 / (1 << a->leaves[i].depth);
        coverage += side * side;
        for (int j = i + 1; j < a->nleaves; j++) {
            par_shapes_adaptive_leaf const* x = a->leaves + i;
            par_shapes_adaptive_leaf const* y = a->leaves + j;
            if (x->depth == y->depth && x->ix == y->ix &&
                x->iy == y->iy) {
                unique = 0;
            }
        }
    }
    TASK_CHECK(unique);
    TASK_CHECK(fabs(coverage - 1.0) < 1.0e-12);

    // All tcoords are true parameter coordinates and every stored point is
    // exactly the callback evaluation of its tcoord (no projection, no
    // spatial re-welding).
    int tcoord_fidelity = 1;
    for (int i = 0; i < m->npoints; i++) {
        if (m->tcoords[2 * i] < -1.0e-7f || m->tcoords[2 * i] > 1.0f + 1.0e-7f
            || m->tcoords[2 * i + 1] < -1.0e-7f ||
            m->tcoords[2 * i + 1] > 1.0f + 1.0e-7f) {
            tcoord_fidelity = 0;
        }
        float q[3];
        fn_bump(m->tcoords + 2 * i, q, 0);
        for (int k = 0; k < 3; k++) {
            if (fabsf(q[k] - m->points[3 * i + k]) > 1.0e-5f) {
                tcoord_fidelity = 0;
            }
        }
    }
    TASK_CHECK(tcoord_fidelity);

    // 2:1 balance across every shared edge, open borders ignored.
    int balanced = 1;
    int transition_pair = 0;
    for (int i = 0; i < a->nleaves; i++) {
        par_shapes_adaptive_leaf const* L = a->leaves + i;
        for (int e = 0; e < 4; e++) {
            int nd = neighbor_depth(a, L, e, 0, 0);
            if (nd < 0) {
                continue;
            }
            if (abs(nd - L->depth) > 1) {
                balanced = 0;
            }
            if (nd == L->depth + 1) {
                transition_pair++;
            }
        }
    }
    TASK_CHECK(balanced);
    TASK_CHECK(transition_pair > 0);

    // A real fine/coarse boundary shares one edge-midpoint vertex, used by
    // both the coarse fan and the finer neighbour's fan (no T-junction).
    int shared_midpoint = 0;
    for (int i = 0; i < a->nleaves && !shared_midpoint; i++) {
        par_shapes_adaptive_leaf const* L = a->leaves + i;
        for (int e = 0; e < 4 && !shared_midpoint; e++) {
            if (neighbor_depth(a, L, e, 0, 0) != L->depth + 1) {
                continue;
            }
            int nq = 1 << (L->depth + 1);
            float mu, mv;
            if (e == 0 || e == 2) {
                mu = (float) (2 * L->ix + 1) / nq;
                mv = (float) (e == 0 ? 2 * L->iy : 2 * L->iy + 2) / nq;
            } else {
                mu = (float) (e == 3 ? 2 * L->ix : 2 * L->ix + 2) / nq;
                mv = (float) (2 * L->iy + 1) / nq;
            }
            int mid = find_vertex(m, mu, mv);
            if (mid < 0) {
                continue;
            }
            if (fan_uses_vertex(a, L, mid)) {
                // A finer leaf touching the shared edge references it too.
                int fine_uses = 0;
                for (int j = 0; j < a->nleaves; j++) {
                    par_shapes_adaptive_leaf const* C = a->leaves + j;
                    if (C->depth == L->depth + 1 &&
                        fan_uses_vertex(a, C, mid)) {
                        fine_uses = 1;
                    }
                }
                if (fine_uses) {
                    shared_midpoint = 1;
                }
            }
        }
    }
    TASK_CHECK(shared_midpoint);

    // Every fan triangle has positive signed area in UV space, i.e. winding
    // follows the outward dP/du cross dP/dv orientation.
    int positive_uv = 1;
    for (int f = 0; f < m->ntriangles; f++) {
        int t[3] = {
            m->triangles[3 * f], m->triangles[3 * f + 1],
            m->triangles[3 * f + 2]
        };
        float uv[3][2];
        for (int k = 0; k < 3; k++) {
            uv[k][0] = m->tcoords[2 * t[k]];
            uv[k][1] = m->tcoords[2 * t[k] + 1];
        }
        float area2 = (uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1]) -
            (uv[1][1] - uv[0][1]) * (uv[2][0] - uv[0][0]);
        if (!(area2 > 0.0f)) {
            positive_uv = 0;
        }
    }
    TASK_CHECK(positive_uv);

    // Manifold connectivity: interior undirected edges occur exactly twice
    // with reversed winding; a single-use edge must lie on the open domain
    // boundary, which rules out interior T-junctions and duplicate faces.
    int nedges = 3 * m->ntriangles;
    task_edge* edges = malloc(sizeof(task_edge) * (size_t) nedges);
    TASK_CHECK(edges);
    for (int f = 0; f < m->ntriangles; f++) {
        int t[3] = {
            m->triangles[3 * f], m->triangles[3 * f + 1],
            m->triangles[3 * f + 2]
        };
        for (int k = 0; k < 3; k++) {
            int x = t[k];
            int y = t[(k + 1) % 3];
            task_edge* e = edges + 3 * f + k;
            e->lo = x < y ? x : y;
            e->hi = x < y ? y : x;
            e->dir = x < y ? 1 : -1;
        }
    }
    qsort(edges, (size_t) nedges, sizeof(task_edge), edge_cmp);
    int manifold = 1;
    for (int i = 0; i < nedges;) {
        int j = i + 1;
        while (j < nedges && edges[j].lo == edges[i].lo &&
            edges[j].hi == edges[i].hi) {
            j++;
        }
        int count = j - i;
        if (count == 2) {
            if (edges[i].dir + edges[i + 1].dir != 0) {
                manifold = 0;
            }
        } else if (count == 1) {
            int x = edges[i].lo;
            int y = edges[i].hi;
            float on_boundary = 0.0f;
            for (int k = 0; k < 2; k++) {
                int vidx = k == 0 ? x : y;
                float u = m->tcoords[2 * vidx];
                float v = m->tcoords[2 * vidx + 1];
                if (u <= 1.0e-9f || u >= 1.0f - 1.0e-9f ||
                    v <= 1.0e-9f || v >= 1.0f - 1.0e-9f) {
                    on_boundary += 0.5f;
                }
            }
            if (on_boundary < 1.0f) {
                manifold = 0; // interior edge used once: a T-junction
            }
        } else {
            manifold = 0;
        }
        i = j;
    }
    TASK_CHECK(manifold);
    free(edges);
    par_shapes_free_adaptive(a);
}

// ---- seams -----------------------------------------------------------------

static void group_seams(void)
{
    // Double-periodic torus.
    par_shapes_adaptive* t =
        par_shapes_create_adaptive(fn_torus, 0, 0.02f, 6, true, true);
    TASK_CHECK(t);
    par_shapes_mesh* m = t->mesh;

    // Refinement on the two periodic sides is synchronized: every leaf
    // touching a wrapped edge has a neighbour across it within one level.
    int synced = 1;
    for (int i = 0; i < t->nleaves; i++) {
        par_shapes_adaptive_leaf const* L = t->leaves + i;
        int q = 1 << L->depth;
        if (L->ix == 0 && neighbor_depth(t, L, 3, 1, 1) < 0) {
            synced = 0;
        }
        if (L->ix == q - 1 && neighbor_depth(t, L, 1, 1, 1) < 0) {
            synced = 0;
        }
        if (L->iy == 0 && neighbor_depth(t, L, 0, 1, 1) < 0) {
            synced = 0;
        }
        if (L->iy == q - 1 && neighbor_depth(t, L, 2, 1, 1) < 0) {
            synced = 0;
        }
    }
    TASK_CHECK(synced);

    // UV-0 and UV-1 seam vertices stay distinct render vertices, but share
    // one geometric position and one smooth normal.
    int seam_pairs = 0;
    for (int i = 0; i < m->npoints; i++) {
        if (fabsf(m->tcoords[2 * i]) > 1.0e-9f) {
            continue;
        }
        float v = m->tcoords[2 * i + 1];
        int j = find_vertex(m, 1.0f, v);
        if (j < 0) {
            continue;
        }
        TASK_CHECK(i != j);
        int match = 1;
        for (int k = 0; k < 3; k++) {
            if (fabsf(m->points[3 * i + k] - m->points[3 * j + k]) > 1.0e-5f
                || fabsf(m->normals[3 * i + k] -
                    m->normals[3 * j + k]) > 1.0e-5f) {
                match = 0;
            }
        }
        TASK_CHECK(match);
        seam_pairs++;
    }
    TASK_CHECK(seam_pairs > 0);

    // Four corner copies of a doubly periodic domain: distinct tcoords,
    // identical positions and normals.
    int corner_idx[4];
    corner_idx[0] = find_vertex(m, 0.0f, 0.0f);
    corner_idx[1] = find_vertex(m, 1.0f, 0.0f);
    corner_idx[2] = find_vertex(m, 0.0f, 1.0f);
    corner_idx[3] = find_vertex(m, 1.0f, 1.0f);
    int corners_ok = 1;
    for (int k = 0; k < 4; k++) {
        if (corner_idx[k] < 0) {
            corners_ok = 0;
        }
    }
    for (int k = 1; k < 4 && corners_ok; k++) {
        int a = corner_idx[0];
        int b = corner_idx[k];
        if (a == b) {
            corners_ok = 0;
        }
        for (int j = 0; j < 3; j++) {
            if (fabsf(m->points[3 * a + j] - m->points[3 * b + j]) > 1.0e-5f
                || fabsf(m->normals[3 * a + j] -
                    m->normals[3 * b + j]) > 1.0e-5f) {
                corners_ok = 0;
            }
        }
    }
    TASK_CHECK(corners_ok);
    par_shapes_free_adaptive(t);

    // Bump crossing the periodic u seam refines both sides in lockstep and
    // keeps matching seam geometry.
    par_shapes_adaptive* s =
        par_shapes_create_adaptive(fn_seam_bump, 0, 0.002f, 7, true, false);
    TASK_CHECK(s);
    int fine_left = 0;
    int fine_right = 0;
    int seam_sync = 1;
    for (int i = 0; i < s->nleaves; i++) {
        par_shapes_adaptive_leaf const* L = s->leaves + i;
        int q = 1 << L->depth;
        if (L->ix == 0) {
            if (L->depth > 2) {
                fine_left++;
            }
            if (neighbor_depth(s, L, 3, 1, 0) < 0) {
                seam_sync = 0;
            }
        }
        if (L->ix == q - 1) {
            if (L->depth > 2) {
                fine_right++;
            }
            if (neighbor_depth(s, L, 1, 1, 0) < 0) {
                seam_sync = 0;
            }
        }
    }
    TASK_CHECK(seam_sync);
    TASK_CHECK(fine_left > 0 && fine_right > 0);
    par_shapes_mesh* sm = s->mesh;
    int seam_match = 1;
    int matched = 0;
    for (int i = 0; i < sm->npoints; i++) {
        if (fabsf(sm->tcoords[2 * i]) > 1.0e-9f) {
            continue;
        }
        int j = find_vertex(sm, 1.0f, sm->tcoords[2 * i + 1]);
        if (j < 0) {
            continue;
        }
        matched++;
        for (int k = 0; k < 3; k++) {
            if (fabsf(sm->points[3 * i + k] - sm->points[3 * j + k]) >
                1.0e-5f ||
                fabsf(sm->normals[3 * i + k] - sm->normals[3 * j + k]) >
                1.0e-5f) {
                seam_match = 0;
            }
        }
    }
    TASK_CHECK(matched > 0 && seam_match);
    par_shapes_free_adaptive(s);

    // Non-periodic folded surface: nearby but parameter-distinct points stay
    // separate and retain opposite normals (no spatial welding/bleeding).
    par_shapes_adaptive* f =
        par_shapes_create_adaptive(fn_fold, 0, 0.01f, 5, false, false);
    TASK_CHECK(f);
    par_shapes_mesh* fm = f->mesh;
    int close = 0;
    int opposite = 0;
    for (int i = 0; i < fm->npoints; i++) {
        for (int j = i + 1; j < fm->npoints; j++) {
            float d[3] = {
                fm->points[3 * i] - fm->points[3 * j],
                fm->points[3 * i + 1] - fm->points[3 * j + 1],
                fm->points[3 * i + 2] - fm->points[3 * j + 2]
            };
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1.0e-6f) {
                close++;
                float nd = 0.0f;
                for (int k = 0; k < 3; k++) {
                    nd += powf(fm->normals[3 * i + k] -
                        fm->normals[3 * j + k], 2.0f);
                }
                if (nd > 1.0f) {
                    opposite++;
                }
            }
        }
    }
    TASK_CHECK(close > 0 && opposite == close);
    par_shapes_free_adaptive(f);
}

// ---- failure ---------------------------------------------------------------

static void group_failure(void)
{
    // Illegal arguments.
    TASK_CHECK(!par_shapes_create_adaptive(0, 0, 0.002f, 7, false, false));
    TASK_CHECK(!par_shapes_create_adaptive(fn_plane, 0, 0.0f, 7, false,
        false));
    TASK_CHECK(!par_shapes_create_adaptive(fn_plane, 0, -1.0f, 7, false,
        false));
    TASK_CHECK(!par_shapes_create_adaptive(fn_plane, 0, INFINITY, 7, false,
        false));
    TASK_CHECK(!par_shapes_create_adaptive(fn_plane, 0, NAN, 7, false,
        false));
    TASK_CHECK(!par_shapes_create_adaptive(fn_plane, 0, 0.002f, 1, false,
        false));
    TASK_CHECK(!par_shapes_create_adaptive(fn_plane, 0, 0.002f, 9, false,
        false));

    // Depth exhaustion returns null rather than silently deepening.
    TASK_CHECK(!par_shapes_create_adaptive(fn_wave3, 0, 1.0e-6f, 3, false,
        false));
    TASK_CHECK(!par_shapes_create_adaptive(fn_wave3, 0, 1.0e-6f, 4, false,
        false));

    // Non-finite callback output.
    TASK_CHECK(!par_shapes_create_adaptive(fn_bad, 0, 0.01f, 6, false,
        false));

    // A collapsed callback yields zero-area triangles and must fail rather
    // than hand back a broken mesh.
    TASK_CHECK(!par_shapes_create_adaptive(fn_constant, 0, 0.01f, 4, false,
        false));

    // Forced allocation failures always yield null and never corrupt state;
    // a normal allocation immediately afterwards still succeeds.
    int recovered = 1;
    int injected = 0;
    for (int fail_at = 1; fail_at <= 12; fail_at++) {
        task_alloc_fail_at = fail_at;
        task_alloc_calls = 0;
        par_shapes_adaptive* a =
            par_shapes_create_adaptive(fn_bump, 0, 0.002f, 7, false, false);
        if (task_alloc_calls >= fail_at) {
            injected++;
            if (a) {
                recovered = 0;
                par_shapes_free_adaptive(a);
            }
        } else if (a) {
            par_shapes_free_adaptive(a);
        }
    }
    task_alloc_fail_at = 0;
    par_shapes_adaptive* after =
        par_shapes_create_adaptive(fn_plane, 0, 0.5f, 2, false, false);
    TASK_CHECK(injected > 0);
    TASK_CHECK(recovered);
    TASK_CHECK(after);
    par_shapes_free_adaptive(after);

    // Index capacity: a uniform depth-8 grid needs 131585 vertices.  The
    // 16-bit configuration must reject it, the 32-bit configuration build it.
    float tol = 1.0f / 131072.0f;
    par_shapes_adaptive* big =
        par_shapes_create_adaptive(fn_quad, 0, tol, 8, false, false);
    if ((long long) ((PAR_SHAPES_T) -1) < 100000) {
        TASK_CHECK(!big);
    } else {
        TASK_CHECK(big);
        if (big) {
            TASK_CHECK(big->nleaves == 65536);
            TASK_CHECK(count_depth(big, 8) == 65536);
            par_shapes_free_adaptive(big);
        }
    }

    // A small valid input succeeds under whichever index width is active,
    // and the free function tolerates null.
    par_shapes_adaptive* small =
        par_shapes_create_adaptive(fn_plane, 0, 0.5f, 2, false, false);
    TASK_CHECK(small && small->mesh && small->mesh->normals &&
        small->mesh->tcoords);
    par_shapes_free_adaptive(small);
    par_shapes_free_adaptive(0);
}

void task_cases(const char* group)
{
    if (!strcmp(group, "refine")) {
        group_refine();
    } else if (!strcmp(group, "transition")) {
        group_transition();
    } else if (!strcmp(group, "seams")) {
        group_seams();
    } else if (!strcmp(group, "failure")) {
        group_failure();
    } else {
        fprintf(stderr, "unknown group %s\n", group);
        exit(2);
    }
}
