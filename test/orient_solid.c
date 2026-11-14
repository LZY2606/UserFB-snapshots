// Tests for par_shapes_orient_solid.  All reference geometry is constructed
// by hand in this file and verified with independent probes (ray casting,
// signed volumes, face-normal dot products); the new API is never used to
// generate expectations.

#include "task_support/check.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

// ---------------------------------------------------------------- helpers

static par_shapes_mesh* tmesh(int np, int nt)
{
    par_shapes_mesh* m = (par_shapes_mesh*) calloc(1, sizeof(par_shapes_mesh));
    m->npoints = np;
    m->ntriangles = nt;
    m->points = (float*) calloc((size_t) np * 3 + 1, sizeof(float));
    m->triangles = (PAR_SHAPES_T*) calloc((size_t) nt * 3 + 1,
        sizeof(PAR_SHAPES_T));
    return m;
}

static void tmerge(par_shapes_mesh* dst, par_shapes_mesh* src)
{
    par_shapes_merge(dst, src);
    par_shapes_free_mesh(src);
}

static void flip_face(par_shapes_mesh* m, int f)
{
    PAR_SHAPES_T t = m->triangles[3 * f + 1];
    m->triangles[3 * f + 1] = m->triangles[3 * f + 2];
    m->triangles[3 * f + 2] = t;
}

// Signed volume contribution of one face, in double precision.
static double tvol_face(par_shapes_mesh const* m, int f)
{
    const float* a = m->points + 3 * m->triangles[3 * f + 0];
    const float* b = m->points + 3 * m->triangles[3 * f + 1];
    const float* c = m->points + 3 * m->triangles[3 * f + 2];
    return ((double) a[0] * ((double) b[1] * c[2] - (double) b[2] * c[1]) +
        (double) a[1] * ((double) b[2] * c[0] - (double) b[0] * c[2]) +
        (double) a[2] * ((double) b[0] * c[1] - (double) b[1] * c[0])) / 6.0;
}

static double tvol_range(par_shapes_mesh const* m, int f0, int f1)
{
    double v = 0;
    for (int f = f0; f < f1; f++) {
        v += tvol_face(m, f);
    }
    return v;
}

// Dot product of a face's geometric normal with (centroid - p).
static double tfacedot(par_shapes_mesh const* m, int f, double px, double py,
    double pz)
{
    const float* va = m->points + 3 * m->triangles[3 * f + 0];
    const float* vb = m->points + 3 * m->triangles[3 * f + 1];
    const float* vc = m->points + 3 * m->triangles[3 * f + 2];
    double e1[3], e2[3], n[3], g[3];
    double p[3] = {px, py, pz};
    for (int k = 0; k < 3; k++) {
        e1[k] = (double) vb[k] - va[k];
        e2[k] = (double) vc[k] - va[k];
        g[k] = ((double) va[k] + vb[k] + vc[k]) / 3.0 - p[k];
    }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    return n[0] * g[0] + n[1] * g[1] + n[2] * g[2];
}

// Independent point-in-solid test: cast a ray and count triangle crossings.
// Returns 0/1, or -1 if the ray grazes an edge (unlucky probe point).
static int tinside(par_shapes_mesh const* m, double px, double py, double pz)
{
    static const double dir[3] = {0.8860772638, 0.3350378756, 0.3184713376};
    int hits = 0;
    for (int f = 0; f < m->ntriangles; f++) {
        const float* va = m->points + 3 * m->triangles[3 * f + 0];
        const float* vb = m->points + 3 * m->triangles[3 * f + 1];
        const float* vc = m->points + 3 * m->triangles[3 * f + 2];
        double e1[3], e2[3];
        for (int k = 0; k < 3; k++) {
            e1[k] = (double) vb[k] - va[k];
            e2[k] = (double) vc[k] - va[k];
        }
        double pv[3];
        pv[0] = dir[1] * e2[2] - dir[2] * e2[1];
        pv[1] = dir[2] * e2[0] - dir[0] * e2[2];
        pv[2] = dir[0] * e2[1] - dir[1] * e2[0];
        double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
        double sc = fabs(e1[0]) + fabs(e1[1]) + fabs(e1[2]);
        if (fabs(det) < 1e-12 * sc * sc * sc) {
            return -1;
        }
        double tv[3] = {px - va[0], py - va[1], pz - va[2]};
        double inv = 1.0 / det;
        double u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
        double qv[3];
        qv[0] = tv[1] * e1[2] - tv[2] * e1[1];
        qv[1] = tv[2] * e1[0] - tv[0] * e1[2];
        qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
        double v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * inv;
        double t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
        int inside = (u > 0 && v > 0 && u + v < 1.0);
        int near = (u > -1e-9 && v > -1e-9 && u + v < 1.0 + 1e-9);
        if (t > 0 && !inside && near) {
            return -1;
        }
        if (t > 0 && inside) {
            hits++;
        }
    }
    return hits & 1;
}

// ------------------------------------------------------------ shell builders

static par_shapes_mesh* make_cube(double cx, double cy, double cz, double half,
    int inward)
{
    static const double corners[8][3] = {
        {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
        {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    static const int faces[12][3] = {
        {0, 3, 2}, {0, 2, 1}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
        {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    par_shapes_mesh* m = tmesh(8, 12);
    for (int i = 0; i < 8; i++) {
        m->points[3 * i + 0] = (float) (cx + half * corners[i][0]);
        m->points[3 * i + 1] = (float) (cy + half * corners[i][1]);
        m->points[3 * i + 2] = (float) (cz + half * corners[i][2]);
    }
    for (int f = 0; f < 12; f++) {
        m->triangles[3 * f + 0] = (PAR_SHAPES_T) faces[f][0];
        m->triangles[3 * f + 1] = (PAR_SHAPES_T) faces[f][inward ? 2 : 1];
        m->triangles[3 * f + 2] = (PAR_SHAPES_T) faces[f][inward ? 1 : 2];
    }
    return m;
}

static par_shapes_mesh* make_tetra(double tx, double ty, double tz, int inward)
{
    static const double verts[4][3] = {
        {1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {1, -1, -1}};
    static const int faces[4][3] = {
        {0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
    par_shapes_mesh* m = tmesh(4, 4);
    for (int i = 0; i < 4; i++) {
        m->points[3 * i + 0] = (float) (tx + verts[i][0]);
        m->points[3 * i + 1] = (float) (ty + verts[i][1]);
        m->points[3 * i + 2] = (float) (tz + verts[i][2]);
    }
    for (int f = 0; f < 4; f++) {
        m->triangles[3 * f + 0] = (PAR_SHAPES_T) faces[f][0];
        m->triangles[3 * f + 1] = (PAR_SHAPES_T) faces[f][inward ? 2 : 1];
        m->triangles[3 * f + 2] = (PAR_SHAPES_T) faces[f][inward ? 1 : 2];
    }
    return m;
}

// Concave closed shell: an octahedron whose apex is pushed below the center,
// forming a funnel-shaped dent.  Star-shaped with respect to (0, 0, -0.7).
static par_shapes_mesh* make_dent(double tx, double ty, double tz, double s,
    int inward)
{
    static const double verts[6][3] = {
        {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -0.4},
        {0, 0, -1}};
    static const int faces[8][3] = {
        {4, 0, 1}, {4, 1, 2}, {4, 2, 3}, {4, 3, 0},
        {5, 1, 0}, {5, 2, 1}, {5, 3, 2}, {5, 0, 3}};
    par_shapes_mesh* m = tmesh(6, 8);
    for (int i = 0; i < 6; i++) {
        m->points[3 * i + 0] = (float) (tx + s * verts[i][0]);
        m->points[3 * i + 1] = (float) (ty + s * verts[i][1]);
        m->points[3 * i + 2] = (float) (tz + s * verts[i][2]);
    }
    for (int f = 0; f < 8; f++) {
        m->triangles[3 * f + 0] = (PAR_SHAPES_T) faces[f][0];
        m->triangles[3 * f + 1] = (PAR_SHAPES_T) faces[f][inward ? 2 : 1];
        m->triangles[3 * f + 2] = (PAR_SHAPES_T) faces[f][inward ? 1 : 2];
    }
    return m;
}

// Torus built directly on a wrapped index grid (no duplicated seam verts).
static par_shapes_mesh* make_torus(int slices, int stacks, double R, double r,
    int inward)
{
    par_shapes_mesh* m = tmesh(slices * stacks, 2 * slices * stacks);
    for (int i = 0; i < slices; i++) {
        double theta = 2.0 * PAR_PI * i / slices;
        for (int j = 0; j < stacks; j++) {
            double phi = 2.0 * PAR_PI * j / stacks;
            double beta = R + r * cos(phi);
            float* p = m->points + 3 * (i * stacks + j);
            p[0] = (float) (cos(theta) * beta);
            p[1] = (float) (sin(theta) * beta);
            p[2] = (float) (r * sin(phi));
        }
    }
    int f = 0;
    for (int i = 0; i < slices; i++) {
        for (int j = 0; j < stacks; j++) {
            int a = i * stacks + j;
            int b = ((i + 1) % slices) * stacks + j;
            int c = ((i + 1) % slices) * stacks + (j + 1) % stacks;
            int d = i * stacks + (j + 1) % stacks;
            PAR_SHAPES_T* t = m->triangles + 3 * f;
            t[0] = (PAR_SHAPES_T) a;
            t[1] = (PAR_SHAPES_T) (inward ? c : b);
            t[2] = (PAR_SHAPES_T) (inward ? b : c);
            t[3] = (PAR_SHAPES_T) a;
            t[4] = (PAR_SHAPES_T) (inward ? d : c);
            t[5] = (PAR_SHAPES_T) (inward ? c : d);
            f += 2;
        }
    }
    return m;
}

// Closed non-orientable manifold (Klein bottle) built by stitching a grid
// with a twisted wrap.  Coordinates embed it as a torus; only the index
// topology matters here.
static par_shapes_mesh* make_klein(int n, int k)
{
    par_shapes_mesh* m = tmesh(n * k, 2 * n * k);
    for (int i = 0; i < n; i++) {
        double theta = 2.0 * PAR_PI * i / n;
        for (int j = 0; j < k; j++) {
            double phi = 2.0 * PAR_PI * j / k;
            double beta = 1.0 + 0.35 * cos(phi);
            float* p = m->points + 3 * (i * k + j);
            p[0] = (float) (cos(theta) * beta);
            p[1] = (float) (sin(theta) * beta);
            p[2] = (float) (0.35 * sin(phi));
        }
    }
    int f = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < k; j++) {
            int a = i * k + j;
            int d = i * k + (j + 1) % k;
            int b, c;
            if (i + 1 < n) {
                b = (i + 1) * k + j;
                c = (i + 1) * k + (j + 1) % k;
            } else {
                b = (k - j % k) % k;
                c = (k - (j + 1) % k) % k;
            }
            PAR_SHAPES_T* t = m->triangles + 3 * f;
            t[0] = (PAR_SHAPES_T) a;
            t[1] = (PAR_SHAPES_T) b;
            t[2] = (PAR_SHAPES_T) c;
            t[3] = (PAR_SHAPES_T) a;
            t[4] = (PAR_SHAPES_T) c;
            t[5] = (PAR_SHAPES_T) d;
            f += 2;
        }
    }
    return m;
}

// Calls par_shapes_orient_solid on a mesh that must be rejected, and verifies
// that neither the mesh nor the face_depth array was modified.
static int expect_false(par_shapes_mesh* m)
{
    int np = m->npoints;
    int nt = m->ntriangles;
    size_t tsz = sizeof(PAR_SHAPES_T) * 3 * (size_t) nt;
    size_t psz = sizeof(float) * 3 * (size_t) np;
    PAR_SHAPES_T* tc = (PAR_SHAPES_T*) malloc(tsz);
    float* pc = (float*) malloc(psz);
    int* fd = (int*) malloc(sizeof(int) * (size_t) nt);
    memcpy(tc, m->triangles, tsz);
    memcpy(pc, m->points, psz);
    for (int f = 0; f < nt; f++) {
        fd[f] = -777;
    }
    TASK_CHECK(!par_shapes_orient_solid(m, fd));
    TASK_CHECK(m->npoints == np);
    TASK_CHECK(m->ntriangles == nt);
    TASK_CHECK(!memcmp(m->triangles, tc, tsz));
    TASK_CHECK(!memcmp(m->points, pc, psz));
    for (int f = 0; f < nt; f++) {
        TASK_CHECK(fd[f] == -777);
    }
    free(tc);
    free(pc);
    free(fd);
    par_shapes_free_mesh(m);
    return 0;
}

// ------------------------------------------------------------------- cases

static int case_scrambled_shell(void)
{
    par_shapes_mesh* m = make_cube(0.2, -0.3, 0.5, 1.0, 0);
    int nt = m->ntriangles;
    TASK_CHECK(nt == 12);
    TASK_CHECK(tvol_range(m, 0, nt) > 7.9);
    TASK_CHECK(tinside(m, 0.2, -0.3, 0.5) == 1);
    TASK_CHECK(tinside(m, 5, 5, 5) == 0);
    flip_face(m, 1);
    flip_face(m, 4);
    flip_face(m, 7);
    flip_face(m, 9);
    PAR_SHAPES_T first[12];
    for (int f = 0; f < nt; f++) {
        first[f] = m->triangles[3 * f];
    }
    int depth[12];
    for (int f = 0; f < nt; f++) {
        depth[f] = -1;
    }
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    for (int f = 0; f < nt; f++) {
        TASK_CHECK(depth[f] == 0);
        TASK_CHECK(m->triangles[3 * f] == first[f]);
        TASK_CHECK(tfacedot(m, f, 0.2, -0.3, 0.5) > 0);
    }
    TASK_CHECK(tvol_range(m, 0, nt) > 7.9);
    PAR_SHAPES_T snapshot[36];
    memcpy(snapshot, m->triangles, sizeof(snapshot));
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    TASK_CHECK(!memcmp(snapshot, m->triangles, sizeof(snapshot)));
    par_shapes_free_mesh(m);
    return 0;
}

static int case_invalid_meshes(void)
{
    par_shapes_mesh* m;

    // Open boundary: tetrahedron with one face removed.
    m = make_tetra(0, 0, 0, 0);
    m->ntriangles = 3;
    if (expect_false(m)) return 1;

    // Edge shared by three faces.
    m = make_tetra(0, 0, 0, 0);
    m->npoints = 5;
    m->ntriangles = 5;
    m->points = (float*) realloc(m->points, 3 * 5 * sizeof(float));
    m->triangles = (PAR_SHAPES_T*) realloc(m->triangles,
        3 * 5 * sizeof(PAR_SHAPES_T));
    m->points[12] = 0;
    m->points[13] = -2;
    m->points[14] = 3;
    m->triangles[12] = 0;
    m->triangles[13] = 1;
    m->triangles[14] = 4;
    if (expect_false(m)) return 1;

    // Vertex fan with two rings: two tetrahedra sharing only vertex 0.
    m = tmesh(7, 8);
    {
        static const double tv[4][3] = {
            {1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {1, -1, -1}};
        static const int tf[8][3] = {
            {0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3},
            {0, 5, 4}, {0, 4, 6}, {0, 6, 5}, {4, 5, 6}};
        for (int i = 0; i < 4; i++) {
            for (int a = 0; a < 3; a++) {
                m->points[3 * i + a] = (float) tv[i][a];
            }
        }
        for (int i = 0; i < 3; i++) {
            m->points[3 * (4 + i) + 0] = (float) (tv[i + 1][0] + 4.0);
            m->points[3 * (4 + i) + 1] = (float) tv[i + 1][1];
            m->points[3 * (4 + i) + 2] = (float) tv[i + 1][2];
        }
        for (int f = 0; f < 8; f++) {
            for (int c = 0; c < 3; c++) {
                m->triangles[3 * f + c] = (PAR_SHAPES_T) tf[f][c];
            }
        }
    }
    if (expect_false(m)) return 1;

    // Closed but non-orientable connectivity.
    if (expect_false(make_klein(6, 6))) return 1;

    // Duplicate face (reversed copy of the only other face).
    m = tmesh(3, 2);
    m->points[3] = 1;
    m->points[7] = 1;
    m->triangles[0] = 0;
    m->triangles[1] = 1;
    m->triangles[2] = 2;
    m->triangles[3] = 0;
    m->triangles[4] = 2;
    m->triangles[5] = 1;
    if (expect_false(m)) return 1;

    // Zero-area face: vertex 2 moved onto the line of vertices 0 and 1.
    m = make_tetra(0, 0, 0, 0);
    m->points[6] = 0;
    m->points[7] = 0;
    m->points[8] = 1;
    if (expect_false(m)) return 1;

    // Out-of-range index.
    m = make_tetra(0, 0, 0, 0);
    m->triangles[1] = 4;
    if (expect_false(m)) return 1;

    // Non-finite coordinates.
    m = make_tetra(0, 0, 0, 0);
    m->points[0] = (float) NAN;
    if (expect_false(m)) return 1;
    m = make_tetra(0, 0, 0, 0);
    m->points[5] = (float) INFINITY;
    if (expect_false(m)) return 1;

    return 0;
}

static int case_torus_genus(void)
{
    par_shapes_mesh* m = make_torus(24, 12, 1.0, 0.35, 0);
    int nt = m->ntriangles;
    TASK_CHECK(nt == 576);
    TASK_CHECK(tvol_range(m, 0, nt) > 2.0);
    TASK_CHECK(tinside(m, 0, 0, 0) == 0);
    TASK_CHECK(tinside(m, 1, 0, 0) == 1);
    flip_face(m, 0);
    flip_face(m, 5);
    flip_face(m, 100);
    flip_face(m, 250);
    flip_face(m, 333);
    flip_face(m, 401);
    flip_face(m, 575);
    int* depth = (int*) malloc(sizeof(int) * (size_t) nt);
    for (int f = 0; f < nt; f++) {
        depth[f] = -1;
    }
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    for (int f = 0; f < nt; f++) {
        TASK_CHECK(depth[f] == 0);
    }
    TASK_CHECK(tvol_range(m, 0, nt) > 2.0);
    TASK_CHECK(tinside(m, 0, 0, 0) == 0);
    TASK_CHECK(tinside(m, 1, 0, 0) == 1);
    PAR_SHAPES_T* snapshot = (PAR_SHAPES_T*) malloc(
        sizeof(PAR_SHAPES_T) * 3 * (size_t) nt);
    memcpy(snapshot, m->triangles, sizeof(PAR_SHAPES_T) * 3 * (size_t) nt);
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    TASK_CHECK(!memcmp(snapshot, m->triangles,
        sizeof(PAR_SHAPES_T) * 3 * (size_t) nt));
    free(snapshot);
    free(depth);
    par_shapes_free_mesh(m);
    return 0;
}

static int case_three_layer_nesting(void)
{
    par_shapes_mesh* m = make_cube(0, 0, 0, 1.0, 0);
    tmerge(m, make_cube(0, 0, 0, 0.55, 0));
    tmerge(m, make_cube(0, 0, 0, 0.25, 0));
    TASK_CHECK(m->npoints == 24);
    TASK_CHECK(m->ntriangles == 36);
    // Invert the middle shell entirely; scramble two other faces.
    for (int f = 12; f < 24; f++) {
        flip_face(m, f);
    }
    flip_face(m, 2);
    flip_face(m, 30);
    int depth[36];
    for (int f = 0; f < 36; f++) {
        depth[f] = -1;
    }
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    for (int f = 0; f < 36; f++) {
        TASK_CHECK(depth[f] == f / 12);
    }
    TASK_CHECK(tvol_range(m, 0, 12) > 7.9);
    TASK_CHECK(tvol_range(m, 12, 24) > -1.34 && tvol_range(m, 12, 24) < -1.32);
    TASK_CHECK(tvol_range(m, 24, 36) > 0.124 && tvol_range(m, 24, 36) < 0.126);
    for (int f = 0; f < 36; f++) {
        double d = tfacedot(m, f, 0, 0, 0);
        if (f >= 12 && f < 24) {
            TASK_CHECK(d < 0);
        } else {
            TASK_CHECK(d > 0);
        }
    }
    par_shapes_free_mesh(m);
    return 0;
}

static int case_separate_concave(void)
{
    par_shapes_mesh* m = make_dent(0, 0, 0, 1.0, 0);
    TASK_CHECK(tinside(m, 0, 0, -0.7) == 1);
    TASK_CHECK(tinside(m, 0, 0, 0) == 0);
    par_shapes_mesh* d2 = make_dent(5, 0, 0, 1.5, 0);
    TASK_CHECK(tinside(d2, 5, 0, -1.05) == 1);
    TASK_CHECK(tinside(d2, 5, 0, 0) == 0);
    tmerge(m, d2);
    TASK_CHECK(m->ntriangles == 16);
    flip_face(m, 0);
    flip_face(m, 3);
    flip_face(m, 9);
    flip_face(m, 13);
    int depth[16];
    for (int f = 0; f < 16; f++) {
        depth[f] = -1;
    }
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    for (int f = 0; f < 16; f++) {
        TASK_CHECK(depth[f] == 0);
    }
    for (int f = 0; f < 8; f++) {
        TASK_CHECK(tfacedot(m, f, 0, 0, -0.7) > 0);
    }
    for (int f = 8; f < 16; f++) {
        TASK_CHECK(tfacedot(m, f, 5, 0, -1.05) > 0);
    }
    TASK_CHECK(tvol_range(m, 0, 8) > 0);
    TASK_CHECK(tvol_range(m, 8, 16) > 0);
    par_shapes_free_mesh(m);
    return 0;
}

static int case_shell_in_torus_hole(void)
{
    par_shapes_mesh* m = make_torus(24, 12, 1.0, 0.35, 0);
    TASK_CHECK(tinside(m, 0, 0, 0) == 0);
    TASK_CHECK(tinside(m, 1, 0, 0) == 1);
    par_shapes_mesh* hole = make_cube(0, 0, 0, 0.15, 0);
    par_shapes_mesh* tube = make_cube(1, 0, 0, 0.1, 0);
    TASK_CHECK(tinside(hole, 0, 0, 0) == 1);
    TASK_CHECK(tinside(tube, 1, 0, 0) == 1);
    int torus_nt = m->ntriangles;
    tmerge(m, hole);
    tmerge(m, tube);
    TASK_CHECK(m->ntriangles == torus_nt + 24);
    // Invert the hole cube entirely; scramble a few other faces.
    for (int f = torus_nt; f < torus_nt + 12; f++) {
        flip_face(m, f);
    }
    flip_face(m, 10);
    flip_face(m, 300);
    flip_face(m, torus_nt + 20);
    int nt = m->ntriangles;
    int* depth = (int*) malloc(sizeof(int) * (size_t) nt);
    for (int f = 0; f < nt; f++) {
        depth[f] = -1;
    }
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    for (int f = 0; f < nt; f++) {
        int want = (f >= torus_nt + 12) ? 1 : 0;
        TASK_CHECK(depth[f] == want);
    }
    for (int f = torus_nt; f < torus_nt + 12; f++) {
        TASK_CHECK(tfacedot(m, f, 0, 0, 0) > 0);
    }
    for (int f = torus_nt + 12; f < nt; f++) {
        TASK_CHECK(tfacedot(m, f, 1, 0, 0) < 0);
    }
    TASK_CHECK(tvol_range(m, 0, torus_nt) > 2.0);
    TASK_CHECK(tvol_range(m, torus_nt, torus_nt + 12) > 0);
    TASK_CHECK(tvol_range(m, torus_nt + 12, nt) < 0);
    free(depth);
    par_shapes_free_mesh(m);
    return 0;
}

static int case_rigid_xform_face_order(void)
{
    par_shapes_mesh* m = make_cube(0, 0, 0, 1.0, 0);
    tmerge(m, make_cube(0, 0, 0, 0.45, 0));
    float axis[3] = {1.0f / 3.7416573867739413f, 2.0f / 3.7416573867739413f,
        3.0f / 3.7416573867739413f};
    par_shapes_rotate(m, 0.7f, axis);
    par_shapes_scale(m, 3, 3, 3);
    par_shapes_translate(m, 30, -20, 10);
    for (int f = 12; f < 24; f++) {
        flip_face(m, f);
    }
    // Deterministic shuffle of the face order.
    unsigned s = 12345;
    for (int i = 23; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int) ((s >> 8) % (unsigned) (i + 1));
        for (int c = 0; c < 3; c++) {
            PAR_SHAPES_T t = m->triangles[3 * i + c];
            m->triangles[3 * i + c] = m->triangles[3 * j + c];
            m->triangles[3 * j + c] = t;
        }
    }
    PAR_SHAPES_T first[24];
    int shell[24];
    for (int f = 0; f < 24; f++) {
        first[f] = m->triangles[3 * f];
        shell[f] = m->triangles[3 * f] >= 8;
    }
    float pts[48];
    memcpy(pts, m->points, sizeof(pts));
    int depth[24];
    for (int f = 0; f < 24; f++) {
        depth[f] = -1;
    }
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    for (int f = 0; f < 24; f++) {
        TASK_CHECK(m->triangles[3 * f] == first[f]);
        TASK_CHECK(depth[f] == shell[f]);
    }
    TASK_CHECK(!memcmp(pts, m->points, sizeof(pts)));
    double v0 = 0, v1 = 0;
    for (int f = 0; f < 24; f++) {
        if (shell[f]) {
            v1 += tvol_face(m, f);
        } else {
            v0 += tvol_face(m, f);
        }
    }
    TASK_CHECK(v0 > 200 && v0 < 235);
    TASK_CHECK(v1 < -18 && v1 > -22);
    PAR_SHAPES_T snapshot[72];
    memcpy(snapshot, m->triangles, sizeof(snapshot));
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    TASK_CHECK(!memcmp(snapshot, m->triangles, sizeof(snapshot)));
    par_shapes_free_mesh(m);
    return 0;
}

static int case_normals(void)
{
    par_shapes_mesh* m = make_cube(0, 0, 0, 1.0, 0);
    // Append an unreferenced vertex and garbage attribute arrays.
    m->npoints = 9;
    m->points = (float*) realloc(m->points, 3 * 9 * sizeof(float));
    m->points[24] = 5;
    m->points[25] = 5;
    m->points[26] = 5;
    m->normals = (float*) malloc(3 * 9 * sizeof(float));
    for (int i = 0; i < 27; i++) {
        m->normals[i] = 7.5f;
    }
    m->tcoords = (float*) malloc(2 * 9 * sizeof(float));
    for (int i = 0; i < 18; i++) {
        m->tcoords[i] = 0.125f * i;
    }
    float tcbak[18];
    memcpy(tcbak, m->tcoords, sizeof(tcbak));
    float ptsbak[27];
    memcpy(ptsbak, m->points, sizeof(ptsbak));
    flip_face(m, 0);
    flip_face(m, 5);
    flip_face(m, 11);
    TASK_CHECK(par_shapes_orient_solid(m, 0));
    TASK_CHECK(m->normals != 0);
    for (int v = 0; v < 8; v++) {
        double nx = m->normals[3 * v];
        double ny = m->normals[3 * v + 1];
        double nz = m->normals[3 * v + 2];
        double len = sqrt(nx * nx + ny * ny + nz * nz);
        TASK_CHECK(fabs(len - 1.0) < 1e-4);
        double dot = nx * m->points[3 * v] + ny * m->points[3 * v + 1] +
            nz * m->points[3 * v + 2];
        TASK_CHECK(dot > 0.5);
    }
    TASK_CHECK(m->normals[24] == 0 && m->normals[25] == 0 &&
        m->normals[26] == 0);
    TASK_CHECK(!memcmp(m->tcoords, tcbak, sizeof(tcbak)));
    TASK_CHECK(!memcmp(m->points, ptsbak, sizeof(ptsbak)));
    par_shapes_free_mesh(m);

    // A mesh without normals keeps a null normals pointer.
    par_shapes_mesh* m2 = make_cube(0, 0, 0, 1.0, 0);
    flip_face(m2, 3);
    TASK_CHECK(par_shapes_orient_solid(m2, 0));
    TASK_CHECK(m2->normals == 0);
    par_shapes_free_mesh(m2);
    return 0;
}

static int case_api_contract(void)
{
    TASK_CHECK(!par_shapes_orient_solid(0, 0));

    par_shapes_mesh* m = make_cube(0, 0, 0, 1.0, 0);
    m->ntriangles = -1;
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    m->ntriangles = 12;
    m->npoints = -2;
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    m->npoints = 8;
    TASK_CHECK(par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);

    // Empty meshes are trivially valid.
    par_shapes_mesh* e = par_shapes_create_empty();
    TASK_CHECK(par_shapes_orient_solid(e, 0));
    par_shapes_free_mesh(e);

    // Empty mesh with points: all vertices are unreferenced, so any
    // existing normals become zero.
    par_shapes_mesh* z = tmesh(2, 0);
    z->points[0] = 1;
    z->points[4] = 2;
    z->normals = (float*) malloc(6 * sizeof(float));
    for (int i = 0; i < 6; i++) {
        z->normals[i] = 1.0f;
    }
    TASK_CHECK(par_shapes_orient_solid(z, 0));
    for (int i = 0; i < 6; i++) {
        TASK_CHECK(z->normals[i] == 0);
    }
    par_shapes_free_mesh(z);

    // Failure leaves everything untouched, including normals and tcoords.
    par_shapes_mesh* b = make_cube(0, 0, 0, 1.0, 0);
    b->normals = (float*) malloc(3 * 8 * sizeof(float));
    b->tcoords = (float*) malloc(2 * 8 * sizeof(float));
    for (int i = 0; i < 24; i++) {
        b->normals[i] = 0.25f * i;
    }
    for (int i = 0; i < 16; i++) {
        b->tcoords[i] = 0.5f * i;
    }
    float nbak[24], uvbak[16], ptsbak[24];
    PAR_SHAPES_T tribak[36];
    b->triangles[0] = 8; // out of range for 8 points
    memcpy(nbak, b->normals, sizeof(nbak));
    memcpy(uvbak, b->tcoords, sizeof(uvbak));
    memcpy(ptsbak, b->points, sizeof(ptsbak));
    memcpy(tribak, b->triangles, sizeof(tribak));
    int fd[12];
    for (int f = 0; f < 12; f++) {
        fd[f] = 999;
    }
    TASK_CHECK(!par_shapes_orient_solid(b, fd));
    TASK_CHECK(b->npoints == 8 && b->ntriangles == 12);
    TASK_CHECK(!memcmp(b->points, ptsbak, sizeof(ptsbak)));
    TASK_CHECK(!memcmp(b->triangles, tribak, sizeof(tribak)));
    TASK_CHECK(!memcmp(b->normals, nbak, sizeof(nbak)));
    TASK_CHECK(!memcmp(b->tcoords, uvbak, sizeof(uvbak)));
    for (int f = 0; f < 12; f++) {
        TASK_CHECK(fd[f] == 999);
    }
    par_shapes_free_mesh(b);
    return 0;
}

const struct task_case task_cases[] = {
    {"topology", "scrambled_shell_is_repaired", case_scrambled_shell},
    {"topology", "invalid_meshes_rejected", case_invalid_meshes},
    {"topology", "torus_genus_shell", case_torus_genus},
    {"nesting", "three_layer_nesting", case_three_layer_nesting},
    {"nesting", "separate_concave_shells", case_separate_concave},
    {"nesting", "shell_in_torus_hole", case_shell_in_torus_hole},
    {"publication", "rigid_xform_face_order", case_rigid_xform_face_order},
    {"publication", "normals_recomputed", case_normals},
    {"publication", "api_contract", case_api_contract},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
