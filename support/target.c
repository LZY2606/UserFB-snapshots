#include "check.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Allocation hooks with a failure budget, used to verify that the
// adaptive generator releases everything when an allocation fails.
static long t_live = 0;
static long t_budget = -1;
static long t_made = 0;
static void* t_malloc(size_t sz)
{
    if (t_budget >= 0 && t_made >= t_budget) {
        return 0;
    }
    t_made++;
    void* p = malloc(sz);
    if (p) {
        t_live++;
    }
    return p;
}
static void* t_calloc(size_t nm, size_t sz)
{
    if (t_budget >= 0 && t_made >= t_budget) {
        return 0;
    }
    t_made++;
    void* p = calloc(nm, sz);
    if (p) {
        t_live++;
    }
    return p;
}
static void* t_realloc(void* p, size_t sz)
{
    if (t_budget >= 0 && t_made >= t_budget) {
        return 0;
    }
    if (!p) {
        t_made++;
    }
    void* q = realloc(p, sz);
    if (q && !p) {
        t_live++;
    }
    return q;
}
static void t_free(void* p)
{
    if (p) {
        t_live--;
    }
    free(p);
}
#define PAR_MALLOC(T, N) ((T*) t_malloc(sizeof(T) * (size_t)(N)))
#define PAR_CALLOC(T, N) ((T*) t_calloc((size_t)(N), sizeof(T)))
#define PAR_REALLOC(T, BUF, N) ((T*) t_realloc((BUF), sizeof(T) * (size_t)(N)))
#define PAR_FREE(BUF) t_free(BUF)
#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

// -- callbacks ---------------------------------------------------------------

static void plane_fn(float const* uv, float* p, void* ud)
{
    (void) ud;
    p[0] = uv[0];
    p[1] = uv[1];
    p[2] = 0.0f;
}

// Graph surface with a single localized smooth bump at (0.25, 0.25).
static void bump_fn(float const* uv, float* p, void* ud)
{
    (void) ud;
    float du = (uv[0] - 0.25f) / 0.18f, dv = (uv[1] - 0.25f) / 0.18f;
    float r2 = du * du + dv * dv, h = 0.0f;
    if (r2 < 1.0f) {
        float t = 1.0f - r2;
        h = 0.2f * t * t * t;
    }
    p[0] = uv[0];
    p[1] = uv[1];
    p[2] = h;
}

static void torus_fn(float const* uv, float* p, void* ud)
{
    (void) ud;
    float theta = uv[0] * 2.0f * (float) PAR_PI;
    float phi = uv[1] * 2.0f * (float) PAR_PI;
    float beta = 1.0f + 0.3f * cosf(phi);
    p[0] = cosf(theta) * beta;
    p[1] = sinf(theta) * beta;
    p[2] = 0.3f * sinf(phi);
}

// Torus with a localized bump straddling the u = 0 / u = 1 seam.
static void torus_bump_fn(float const* uv, float* p, void* ud)
{
    (void) ud;
    float du = fabsf(uv[0]);
    du = fminf(du, 1.0f - du);
    float dv = uv[1] - 0.3f;
    float rr = du * du + dv * dv;
    float minor = 0.3f;
    float R = 0.15f;
    if (rr < R * R) {
        float t = 1.0f - rr / (R * R);
        minor += 0.1f * t * t * t;
    }
    float theta = uv[0] * 2.0f * (float) PAR_PI;
    float phi = uv[1] * 2.0f * (float) PAR_PI;
    float beta = 1.0f + minor * cosf(phi);
    p[0] = cosf(theta) * beta;
    p[1] = sinf(theta) * beta;
    p[2] = minor * sinf(phi);
}

// Thin fold-back surface: parameters u and 1-u map to almost the same
// world position (closer than the weld epsilon) with opposite normals.
static void fold_fn(float const* uv, float* p, void* ud)
{
    (void) ud;
    p[0] = sinf((float) PAR_PI * uv[0]);
    p[1] = uv[1];
    p[2] = 0.0004f * sinf(2.0f * (float) PAR_PI * uv[0]);
}

static void nan_fn(float const* uv, float* p, void* ud)
{
    (void) ud;
    p[0] = uv[0];
    p[1] = uv[1];
    p[2] = uv[0] > 0.5f ? (float) NAN : 0.0f;
}

// Paraboloid: constant nonzero bilinear error everywhere, so refinement
// is perfectly uniform for a given tolerance.
static void parabola_fn(float const* uv, float* p, void* ud)
{
    (void) ud;
    p[0] = uv[0];
    p[1] = uv[1];
    p[2] = uv[0] * uv[0] + uv[1] * uv[1];
}

// -- helpers -----------------------------------------------------------------

static float dist3(float const* a, float const* b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Independently recomputed leaf error estimate: maximum distance between
// the five non-corner samples and the bilinear corner interpolation.
static float leaf_error(par_shapes_fn fn, void* ud, int depth, int ix,
    int iy)
{
    float s = 1.0f / (1 << depth);
    float u0 = ix * s, v0 = iy * s;
    static const float cu[4] = {0, 1, 1, 0};
    static const float cv[4] = {0, 0, 1, 1};
    static const float st[5][2] = {
        {0.5f, 0.0f}, {1.0f, 0.5f}, {0.5f, 1.0f}, {0.0f, 0.5f},
        {0.5f, 0.5f}};
    float c[4][3];
    for (int k = 0; k < 4; k++) {
        float uv[2] = {u0 + cu[k] * s, v0 + cv[k] * s};
        fn(uv, c[k], ud);
    }
    float eta = 0.0f;
    for (int k = 0; k < 5; k++) {
        float su = st[k][0], tv = st[k][1];
        float uv[2] = {u0 + su * s, v0 + tv * s};
        float p[3];
        fn(uv, p, ud);
        float w00 = (1.0f - su) * (1.0f - tv), w10 = su * (1.0f - tv);
        float w11 = su * tv, w01 = (1.0f - su) * tv;
        float d2 = 0.0f;
        for (int j = 0; j < 3; j++) {
            float b = w00 * c[0][j] + w10 * c[1][j] + w11 * c[2][j] +
                w01 * c[3][j];
            float dj = p[j] - b;
            d2 += dj * dj;
        }
        if (d2 > eta) {
            eta = d2;
        }
    }
    return sqrtf(eta);
}

// Fill a 2^max_depth grid with leaf ids, asserting that the leaves cover
// the domain exactly once.
static int* coverage_grid(par_shapes_adaptive* a, int max_depth, int* out_n)
{
    int n = 1 << max_depth;
    int* g = malloc(sizeof(int) * (size_t) n * n);
    for (int i = 0; i < n * n; i++) {
        g[i] = -1;
    }
    for (int i = 0; i < a->nleaves; i++) {
        par_shapes_adaptive_leaf* L = a->leaves + i;
        TASK_CHECK(L->depth >= 2 && L->depth <= max_depth);
        TASK_CHECK(L->ix >= 0 && L->ix < (1 << L->depth));
        TASK_CHECK(L->iy >= 0 && L->iy < (1 << L->depth));
        int k = 1 << (max_depth - L->depth);
        int fx = L->ix * k, fy = L->iy * k;
        for (int y = 0; y < k; y++) {
            for (int x = 0; x < k; x++) {
                TASK_CHECK(g[(fy + y) * n + fx + x] == -1);
                g[(fy + y) * n + fx + x] = i;
            }
        }
    }
    for (int i = 0; i < n * n; i++) {
        TASK_CHECK(g[i] >= 0);
    }
    *out_n = n;
    return g;
}

// -- refine ------------------------------------------------------------------

static void test_refine(void)
{
    // A plane has zero error everywhere: the initial 4x4 grid suffices.
    par_shapes_adaptive* a = par_shapes_create_adaptive(plane_fn, 0, 0.01f,
        6, false, false);
    TASK_CHECK(a != 0);
    TASK_CHECK(a->nleaves == 16);
    TASK_CHECK(a->mesh->npoints == 41);
    TASK_CHECK(a->mesh->ntriangles == 64);
    TASK_CHECK(a->mesh->normals != 0 && a->mesh->tcoords != 0);
    for (int i = 0; i < a->nleaves; i++) {
        par_shapes_adaptive_leaf* L = a->leaves + i;
        TASK_CHECK(L->depth == 2);
        TASK_CHECK(L->ix >= 0 && L->ix < 4);
        TASK_CHECK(L->iy >= 0 && L->iy < 4);
        TASK_CHECK(leaf_error(plane_fn, 0, L->depth, L->ix, L->iy) <=
            0.01f);
    }
    // Texture coordinates are true parameter coordinates: evaluating the
    // callback at the tcoords reproduces the position.
    for (int i = 0; i < a->mesh->npoints; i++) {
        float p[3];
        plane_fn(a->mesh->tcoords + 2 * i, p, 0);
        TASK_CHECK(dist3(p, a->mesh->points + 3 * i) < 1e-6f);
    }
    par_shapes_free_adaptive(a);
    TASK_CHECK(t_live == 0);

    // Localized smooth bump: refinement concentrates near the bump while
    // coarse leaves survive far away.
    a = par_shapes_create_adaptive(bump_fn, 0, 0.002f, 7, false, false);
    TASK_CHECK(a != 0);
    int nd2 = 0, maxd = 0;
    for (int i = 0; i < a->nleaves; i++) {
        par_shapes_adaptive_leaf* L = a->leaves + i;
        float eta = leaf_error(bump_fn, 0, L->depth, L->ix, L->iy);
        TASK_CHECK(eta <= 0.002f * 1.001f);
        if (L->depth == 2) {
            nd2++;
        }
        if (L->depth > maxd) {
            maxd = L->depth;
        }
    }
    TASK_CHECK(nd2 >= 4);
    TASK_CHECK(maxd >= 5);
    // Leaf centers are valid, distinct vertex indices.
    char* seen = calloc((size_t) a->mesh->npoints, 1);
    for (int i = 0; i < a->nleaves; i++) {
        int c = a->leaves[i].center;
        TASK_CHECK(c >= 0 && c < a->mesh->npoints);
        TASK_CHECK(!seen[c]);
        seen[c] = 1;
    }
    free(seen);
    par_shapes_free_adaptive(a);
    TASK_CHECK(t_live == 0);
}

// -- transition ----------------------------------------------------------------

typedef struct {
    long long key;
    int dir;
} tedge;

static int tedge_cmp(const void* pa, const void* pb)
{
    tedge const* a = pa;
    tedge const* b = pb;
    return (a->key > b->key) - (a->key < b->key);
}

static void test_transition(void)
{
    par_shapes_adaptive* a = par_shapes_create_adaptive(bump_fn, 0, 0.002f,
        7, false, false);
    TASK_CHECK(a != 0);
    par_shapes_mesh* m = a->mesh;
    int n;
    int* g = coverage_grid(a, 7, &n);

    // 2:1 balance, genuine fine-coarse adjacency, and midpoint vertices
    // in the coarse leaf's fan wherever the neighbor is finer.
    int found_transition = 0;
    for (int i = 0; i < a->nleaves; i++) {
        par_shapes_adaptive_leaf* L = a->leaves + i;
        int k = 1 << (7 - L->depth);
        int fx = L->ix * k, fy = L->iy * k, h = k >> 1;
        float s = 1.0f / (1 << L->depth);
        float u0 = L->ix * s, v0 = L->iy * s;
        // Per edge: sample offset in the grid and midpoint parameters.
        const int sx[4] = {h, k, h, -1};
        const int sy[4] = {-1, h, k, h};
        const float mu[4] = {u0 + 0.5f * s, u0 + s, u0 + 0.5f * s, u0};
        const float mv[4] = {v0, v0 + 0.5f * s, v0 + s, v0 + 0.5f * s};
        int nfiner = 0;
        int need_mid[4] = {0, 0, 0, 0};
        for (int e = 0; e < 4; e++) {
            int nx = fx + sx[e], ny = fy + sy[e];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) {
                continue;
            }
            int nd = a->leaves[g[ny * n + nx]].depth;
            TASK_CHECK(nd >= L->depth - 1 && nd <= L->depth + 1);
            if (nd == L->depth + 1) {
                need_mid[e] = 1;
                nfiner++;
            }
        }
        // Scan this leaf's fan: triangle count and midpoint presence.
        int fancount = 0;
        int got_mid[4] = {0, 0, 0, 0};
        for (int t = 0; t < m->ntriangles; t++) {
            int ids[3] = {m->triangles[3 * t], m->triangles[3 * t + 1],
                m->triangles[3 * t + 2]};
            if (ids[0] != L->center && ids[1] != L->center &&
                ids[2] != L->center) {
                continue;
            }
            fancount++;
            for (int q = 0; q < 3; q++) {
                float u = m->tcoords[2 * ids[q]];
                float v = m->tcoords[2 * ids[q] + 1];
                for (int e = 0; e < 4; e++) {
                    if (need_mid[e] && fabsf(u - mu[e]) < 1e-6f &&
                        fabsf(v - mv[e]) < 1e-6f) {
                        got_mid[e] = 1;
                    }
                }
            }
        }
        TASK_CHECK(fancount == 4 + nfiner);
        for (int e = 0; e < 4; e++) {
            if (need_mid[e]) {
                TASK_CHECK(got_mid[e]);
            }
        }
        if (nfiner > 0) {
            found_transition = 1;
        }
    }
    TASK_CHECK(found_transition);
    free(g);

    // Every interior edge is shared by exactly two triangles traversed in
    // opposite directions; boundary edges appear once and lie on the
    // parameter domain border.  This rules out T-junctions, duplicate
    // faces, and inconsistent orientation.
    int np = m->npoints, nt = m->ntriangles;
    tedge* edges = malloc(sizeof(tedge) * 3 * (size_t) nt);
    int ne = 0;
    for (int t = 0; t < nt; t++) {
        int ids[3] = {m->triangles[3 * t], m->triangles[3 * t + 1],
            m->triangles[3 * t + 2]};
        for (int e = 0; e < 3; e++) {
            int x = ids[e], y = ids[(e + 1) % 3];
            edges[ne].key = x < y ? (long long) x * np + y :
                (long long) y * np + x;
            edges[ne].dir = x < y ? 1 : -1;
            ne++;
        }
    }
    qsort(edges, (size_t) ne, sizeof(tedge), tedge_cmp);
    int interior = 0, border = 0;
    for (int i = 0; i < ne;) {
        int j = i;
        int dirsum = 0;
        while (j < ne && edges[j].key == edges[i].key) {
            dirsum += edges[j].dir;
            j++;
        }
        int count = j - i;
        if (count == 2) {
            TASK_CHECK(dirsum == 0);
            interior++;
        } else if (count == 1) {
            int x = (int)(edges[i].key / np);
            int y = (int)(edges[i].key % np);
            float ux = m->tcoords[2 * x], vx = m->tcoords[2 * x + 1];
            float uy = m->tcoords[2 * y], vy = m->tcoords[2 * y + 1];
            int onu = ux == uy && (ux == 0.0f || ux == 1.0f);
            int onv = vx == vy && (vx == 0.0f || vx == 1.0f);
            TASK_CHECK(onu || onv);
            border++;
        } else {
            TASK_CHECK(0);
        }
        i = j;
    }
    TASK_CHECK(interior > 0);
    TASK_CHECK(border > 0);
    free(edges);

    // Every fan triangle has positive UV area and contains exactly one
    // leaf center.
    char* iscenter = calloc((size_t) np, 1);
    for (int i = 0; i < a->nleaves; i++) {
        TASK_CHECK(a->leaves[i].center >= 0 && a->leaves[i].center < np);
        iscenter[a->leaves[i].center] = 1;
    }
    for (int t = 0; t < nt; t++) {
        int i0 = m->triangles[3 * t], i1 = m->triangles[3 * t + 1];
        int i2 = m->triangles[3 * t + 2];
        TASK_CHECK(iscenter[i0] + iscenter[i1] + iscenter[i2] == 1);
        float u0 = m->tcoords[2 * i0], v0 = m->tcoords[2 * i0 + 1];
        float u1 = m->tcoords[2 * i1], v1 = m->tcoords[2 * i1 + 1];
        float u2 = m->tcoords[2 * i2], v2 = m->tcoords[2 * i2 + 1];
        float area2 = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0);
        TASK_CHECK(area2 > 0.0f);
    }
    free(iscenter);
    par_shapes_free_adaptive(a);
    TASK_CHECK(t_live == 0);
}

// -- seams -------------------------------------------------------------------

// Shared checks for doubly periodic meshes: mirrored subdivision across
// the seams, consistent 0/1 render copies, and smooth normals rebuilt
// independently by parametric identity.
static void check_periodic(par_shapes_adaptive* a, par_shapes_fn fn,
    int max_depth)
{
    par_shapes_mesh* m = a->mesh;
    int n;
    int* g = coverage_grid(a, max_depth, &n);
    for (int fy = 0; fy < n; fy++) {
        TASK_CHECK(a->leaves[g[fy * n]].depth ==
            a->leaves[g[fy * n + n - 1]].depth);
    }
    for (int fx = 0; fx < n; fx++) {
        TASK_CHECK(a->leaves[g[fx]].depth ==
            a->leaves[g[(n - 1) * n + fx]].depth);
    }
    free(g);
    int np = m->npoints;
    int cu0 = 0, cu1 = 0, cv0 = 0, cv1 = 0;
    for (int i = 0; i < np; i++) {
        float u = m->tcoords[2 * i], v = m->tcoords[2 * i + 1];
        if (u == 0.0f) cu0++;
        if (u == 1.0f) cu1++;
        if (v == 0.0f) cv0++;
        if (v == 1.0f) cv1++;
    }
    TASK_CHECK(cu0 > 0 && cu0 == cu1);
    TASK_CHECK(cv0 > 0 && cv0 == cv1);

    // Canonical parameter identity per vertex (wrapped axes map 1 to 0).
    float* cu = malloc(sizeof(float) * (size_t) np);
    float* cv = malloc(sizeof(float) * (size_t) np);
    int* rep = malloc(sizeof(int) * (size_t) np);
    for (int i = 0; i < np; i++) {
        float u = m->tcoords[2 * i], v = m->tcoords[2 * i + 1];
        cu[i] = u == 1.0f ? 0.0f : u;
        cv[i] = v == 1.0f ? 0.0f : v;
        rep[i] = -1;
        for (int j = 0; j < i; j++) {
            if (cu[j] == cu[i] && cv[j] == cv[i]) {
                rep[i] = rep[j];
                break;
            }
        }
        if (rep[i] < 0) {
            rep[i] = i;
        }
    }
    // Copies share position and normal; positions come from the canonical
    // parameter copy.
    int n2 = 0, n4 = 0;
    for (int i = 0; i < np; i++) {
        if (rep[i] != i) {
            TASK_CHECK(dist3(m->points + 3 * i, m->points + 3 * rep[i]) <
                1e-6f);
            TASK_CHECK(dist3(m->normals + 3 * i, m->normals + 3 * rep[i]) <
                1e-6f);
        }
        float uv[2] = {cu[i], cv[i]};
        float p[3];
        fn(uv, p, 0);
        TASK_CHECK(dist3(p, m->points + 3 * i) < 1e-5f);
    }
    for (int i = 0; i < np; i++) {
        if (rep[i] != i) {
            continue;
        }
        int s = 0;
        for (int j = 0; j < np; j++) {
            if (rep[j] == i) {
                s++;
            }
        }
        TASK_CHECK(s == 1 || s == 2 || s == 4);
        if (s == 2) {
            n2++;
        }
        if (s == 4) {
            n4++;
        }
    }
    TASK_CHECK(n2 > 0);
    TASK_CHECK(n4 == 1);

    // Rebuild area-weighted smooth normals by parametric identity and
    // compare against the mesh.
    float* acc = calloc(3 * (size_t) np, sizeof(float));
    for (int t = 0; t < m->ntriangles; t++) {
        int ids[3] = {m->triangles[3 * t], m->triangles[3 * t + 1],
            m->triangles[3 * t + 2]};
        float const* p0 = m->points + 3 * ids[0];
        float const* p1 = m->points + 3 * ids[1];
        float const* p2 = m->points + 3 * ids[2];
        float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float cp[3] = {e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0]};
        for (int e = 0; e < 3; e++) {
            float* an = acc + 3 * rep[ids[e]];
            an[0] += cp[0];
            an[1] += cp[1];
            an[2] += cp[2];
        }
    }
    for (int i = 0; i < np; i++) {
        float* an = acc + 3 * rep[i];
        float len = sqrtf(an[0] * an[0] + an[1] * an[1] + an[2] * an[2]);
        TASK_CHECK(len > 0.0f);
        for (int q = 0; q < 3; q++) {
            TASK_CHECK(fabsf(an[q] / len - m->normals[3 * i + q]) < 1e-3f);
        }
    }
    free(acc);
    free(cu);
    free(cv);
    free(rep);
}

typedef struct {
    float u, v;
} tuv;

static int tuv_cmp(const void* pa, const void* pb)
{
    tuv const* a = pa;
    tuv const* b = pb;
    if (a->u != b->u) {
        return a->u < b->u ? -1 : 1;
    }
    return (a->v > b->v) - (a->v < b->v);
}

static void test_seams(void)
{
    // Doubly periodic torus.
    par_shapes_adaptive* a = par_shapes_create_adaptive(torus_fn, 0, 0.01f,
        6, true, true);
    TASK_CHECK(a != 0);
    check_periodic(a, torus_fn, 6);
    par_shapes_free_adaptive(a);
    TASK_CHECK(t_live == 0);

    // Torus with a bump straddling the u seam: the refined region crosses
    // the seam and the subdivision stays synchronized.
    a = par_shapes_create_adaptive(torus_bump_fn, 0, 0.005f, 6, true,
        true);
    TASK_CHECK(a != 0);
    check_periodic(a, torus_bump_fn, 6);
    int maxd = 0, deep_lo = 0, deep_hi = 0;
    for (int i = 0; i < a->nleaves; i++) {
        par_shapes_adaptive_leaf* L = a->leaves + i;
        if (L->depth > maxd) {
            maxd = L->depth;
        }
        if (L->depth >= 4 && L->ix == 0) {
            deep_lo = 1;
        }
        if (L->depth >= 4 && L->ix + 1 == (1 << L->depth)) {
            deep_hi = 1;
        }
    }
    TASK_CHECK(maxd >= 4);
    TASK_CHECK(deep_lo);
    TASK_CHECK(deep_hi);
    par_shapes_free_adaptive(a);
    TASK_CHECK(t_live == 0);

    // Thin fold-back surface without periodic declaration: parametrically
    // distinct points that are closer than the weld epsilon must not be
    // merged, and their normals must not bleed into each other.
    a = par_shapes_create_adaptive(fold_fn, 0, 0.01f, 5, false, false);
    TASK_CHECK(a != 0);
    par_shapes_mesh* m = a->mesh;
    int i1 = -1, i2 = -1;
    for (int i = 0; i < m->npoints; i++) {
        float u = m->tcoords[2 * i], v = m->tcoords[2 * i + 1];
        if (u == 0.25f && v == 0.5f) {
            i1 = i;
        }
        if (u == 0.75f && v == 0.5f) {
            i2 = i;
        }
    }
    TASK_CHECK(i1 >= 0 && i2 >= 0);
    TASK_CHECK(i1 != i2);
    float d = dist3(m->points + 3 * i1, m->points + 3 * i2);
    TASK_CHECK(d > 0.0f && d < 0.001f);
    float dot = 0.0f;
    for (int q = 0; q < 3; q++) {
        dot += m->normals[3 * i1 + q] * m->normals[3 * i2 + q];
    }
    TASK_CHECK(dot < -0.999f);
    float p1[3], p2[3];
    fold_fn(m->tcoords + 2 * i1, p1, 0);
    fold_fn(m->tcoords + 2 * i2, p2, 0);
    TASK_CHECK(dist3(p1, m->points + 3 * i1) < 1e-6f);
    TASK_CHECK(dist3(p2, m->points + 3 * i2) < 1e-6f);
    // No two vertices share a parameter position.
    int np = m->npoints;
    tuv* uvs = malloc(sizeof(tuv) * (size_t) np);
    for (int i = 0; i < np; i++) {
        uvs[i].u = m->tcoords[2 * i];
        uvs[i].v = m->tcoords[2 * i + 1];
    }
    qsort(uvs, (size_t) np, sizeof(tuv), tuv_cmp);
    for (int i = 1; i < np; i++) {
        TASK_CHECK(uvs[i].u != uvs[i - 1].u || uvs[i].v != uvs[i - 1].v);
    }
    free(uvs);
    par_shapes_free_adaptive(a);
    TASK_CHECK(t_live == 0);
}

// -- failure -----------------------------------------------------------------

static void test_failure(void)
{
    // Invalid arguments.
    TASK_CHECK(par_shapes_create_adaptive(0, 0, 0.01f, 4, false, false) ==
        0);
    TASK_CHECK(par_shapes_create_adaptive(plane_fn, 0, 0.0f, 4, false,
        false) == 0);
    TASK_CHECK(par_shapes_create_adaptive(plane_fn, 0, -1.0f, 4, false,
        false) == 0);
    TASK_CHECK(par_shapes_create_adaptive(plane_fn, 0, (float) NAN, 4,
        false, false) == 0);
    TASK_CHECK(par_shapes_create_adaptive(plane_fn, 0, (float) INFINITY, 4,
        false, false) == 0);
    TASK_CHECK(par_shapes_create_adaptive(plane_fn, 0, 0.01f, 1, false,
        false) == 0);
    TASK_CHECK(par_shapes_create_adaptive(plane_fn, 0, 0.01f, 9, false,
        false) == 0);
    TASK_CHECK(t_live == 0);

    // Depth exhausted: the bump cannot be resolved with only depth 2.
    TASK_CHECK(par_shapes_create_adaptive(bump_fn, 0, 1e-6f, 2, false,
        false) == 0);
    TASK_CHECK(t_live == 0);

    // Non-finite callback output.
    TASK_CHECK(par_shapes_create_adaptive(nan_fn, 0, 0.01f, 4, false,
        false) == 0);
    TASK_CHECK(t_live == 0);

    // Allocation failure sweep: every failure must return NULL and leave
    // no allocations behind.
    long budget;
    for (budget = 0; ; budget++) {
        t_budget = budget;
        t_made = 0;
        par_shapes_adaptive* a = par_shapes_create_adaptive(bump_fn, 0,
            0.01f, 5, false, false);
        t_budget = -1;
        if (a) {
            par_shapes_free_adaptive(a);
            break;
        }
        TASK_CHECK(t_live == 0);
        TASK_CHECK(budget < 1000);
    }
    TASK_CHECK(t_live == 0);
    TASK_CHECK(budget > 0);

    // Capacity: a uniformly deep grid exceeds 16-bit indices but fits in
    // 32-bit ones.
    par_shapes_adaptive* a = par_shapes_create_adaptive(parabola_fn, 0,
        1e-5f, 8, false, false);
    if (sizeof(PAR_SHAPES_T) == 2) {
        TASK_CHECK(a == 0);
        TASK_CHECK(t_live == 0);
    } else {
        TASK_CHECK(a != 0);
        TASK_CHECK(a->nleaves == 65536);
        TASK_CHECK(a->mesh->npoints == 131585);
        TASK_CHECK(a->mesh->ntriangles == 262144);
        par_shapes_free_adaptive(a);
        TASK_CHECK(t_live == 0);
    }

    // A small valid input succeeds in either index configuration.
    a = par_shapes_create_adaptive(plane_fn, 0, 0.01f, 4, false, false);
    TASK_CHECK(a != 0);
    TASK_CHECK(a->nleaves == 16);
    TASK_CHECK(a->mesh->npoints == 41);
    par_shapes_free_adaptive(a);
    TASK_CHECK(t_live == 0);
}

void task_cases(const char* group)
{
    if (!strcmp(group, "refine")) {
        test_refine();
    } else if (!strcmp(group, "transition")) {
        test_transition();
    } else if (!strcmp(group, "seams")) {
        test_seams();
    } else if (!strcmp(group, "failure")) {
        test_failure();
    } else {
        fprintf(stderr, "unknown group %s\n", group);
        exit(2);
    }
}
