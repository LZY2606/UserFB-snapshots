/*
 * test_limit.c -- checks for the direct Catmull-Clark limit-surface query.
 *
 * References are built independently here (a uniform bicubic B-spline
 * evaluator written straight from the raw control points, an eigen limit
 * mask, and position finite differences) rather than by cross-checking the
 * new interface against itself.
 *
 * This translation unit owns the cc_Limit.h implementation and provides no
 * main(); task_support/driver.c runs the registered cases.
 */
#define CC_LIMIT_IMPLEMENTATION
#include "cc_Limit.h"

#include "task_support/check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Cage construction helpers                                          */
/* ------------------------------------------------------------------ */

/* Build a closed, consistently oriented quad cage from corner tables. */
static cc_Mesh *cage_build(int nv, const int (*F)[4], int nf,
                          const double (*pos)[3])
{
    cc_Mesh *m = ccm_Create(nv, 0, 4 * nf, 0, nf);
    if (!m) return NULL;
    m->edgeCount = 2 * nf;
    free(m->uvs); m->uvs = NULL; m->uvCount = 0;
    m->edgeToHalfedgeIDs =
        (int32_t *)realloc(m->edgeToHalfedgeIDs, sizeof(int32_t) * 2 * nf);
    m->creases =
        (cc_Crease *)realloc(m->creases, sizeof(cc_Crease) * 2 * nf);
    if (!m->edgeToHalfedgeIDs || !m->creases) { ccm_Release(m); return NULL; }

    for (int v = 0; v < nv; ++v) {
        m->vertexPoints[v].x = (float)pos[v][0];
        m->vertexPoints[v].y = (float)pos[v][1];
        m->vertexPoints[v].z = (float)pos[v][2];
        m->vertexToHalfedgeIDs[v] = -1;
    }
    for (int f = 0; f < nf; ++f) {
        m->faceToHalfedgeIDs[f] = 4 * f;
        for (int j = 0; j < 4; ++j) {
            int h = 4 * f + j;
            cc_Halfedge *e = &m->halfedges[h];
            e->vertexID = F[f][j];
            e->faceID = f;
            e->nextID = 4 * f + (j + 1) % 4;
            e->prevID = 4 * f + (j + 3) % 4;
            e->twinID = -1;
            e->edgeID = -1;
            e->uvID = -1;
            m->vertexToHalfedgeIDs[e->vertexID] = h;
        }
    }
    int edge = 0;
    for (int h = 0; h < 4 * nf; ++h) {
        if (m->halfedges[h].edgeID >= 0) continue;
        int a = m->halfedges[h].vertexID;
        int b = m->halfedges[m->halfedges[h].nextID].vertexID;
        for (int t = h + 1; t < 4 * nf; ++t) {
            int c = m->halfedges[t].vertexID;
            int d = m->halfedges[m->halfedges[t].nextID].vertexID;
            if (c == b && d == a) {
                m->halfedges[h].twinID = t;
                m->halfedges[t].twinID = h;
                m->halfedges[h].edgeID = m->halfedges[t].edgeID = edge;
                break;
            }
        }
        if (m->halfedges[h].edgeID < 0) { ccm_Release(m); return NULL; }
        m->edgeToHalfedgeIDs[edge] = m->halfedges[h].twinID;
        m->creases[edge].nextID = m->creases[edge].prevID = edge;
        m->creases[edge].sharpness = 0.0f;
        ++edge;
    }
    if (edge != 2 * nf) { ccm_Release(m); return NULL; }
    return m;
}

/* Asymmetric toroidal grid: every vertex valence 4, no boundary. */
static cc_Mesh *toroidal_grid(int N, double (*P_out)[3], int *nv_out)
{
    static double P[400][3];
    static int F[400][4];
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            int v = y * N + x;
            P[v][0] = (double)x;
            P[v][1] = (double)y;
            P[v][2] = 0.7 * sin(0.9 * x + 0.3) * cos(0.8 * y - 0.2)
                    + 0.31 * x - 0.23 * y + 0.17 * sin(0.5 * (x + y));
        }
    int f = 0;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            int xn = (x + 1) % N, yn = (y + 1) % N;
            F[f][0] = y * N + x;
            F[f][1] = y * N + xn;
            F[f][2] = yn * N + xn;
            F[f][3] = yn * N + x;
            ++f;
        }
    if (P_out) memcpy(P_out, P, sizeof(double[3]) * N * N);
    if (nv_out) *nv_out = N * N;
    return cage_build(N * N, F, N * N, P);
}

/* Two-pole spindle: the two poles have valence n, ring vertices valence 3. */
static cc_Mesh *polar_cage(int n)
{
    int nv = 2 + 2 * n, nf = 2 * n;
    static int F[64][4];
    static double P[64][3];
    P[0][0] = 0.0; P[0][1] = 0.0; P[0][2] = 1.0;
    P[1][0] = 0.0; P[1][1] = 0.0; P[1][2] = -1.0;
    for (int i = 0; i < n; ++i) {
        double ang = 2.0 * 3.14159265358979323846 * i / n;
        double jr = 1.0 + 0.13 * sin(3.0 * ang + 0.7);
        double jz = 0.09 * cos(2.0 * ang - 0.4);
        P[2 + i][0] = 0.9 * jr * cos(ang);
        P[2 + i][1] = 0.9 * jr * sin(ang);
        P[2 + i][2] = 0.35 + jz;
        P[2 + n + i][0] = 0.7 * (1.0 - 0.11 * cos(4.0 * ang)) * cos(ang + 0.05);
        P[2 + n + i][1] = 0.7 * (1.0 - 0.11 * cos(4.0 * ang)) * sin(ang + 0.05);
        P[2 + n + i][2] = -0.4 + 0.08 * sin(5.0 * ang + 1.1);
    }
    for (int i = 0; i < n; ++i) {
        int im = (i + n - 1) % n;
        F[i][0] = 0; F[i][1] = 2 + i; F[i][2] = 2 + n + i; F[i][3] = 2 + im;
        F[n + i][0] = 1; F[n + i][1] = 2 + n + im;
        F[n + i][2] = 2 + im; F[n + i][3] = 2 + n + i;
    }
    return cage_build(nv, F, nf, P);
}

/* ------------------------------------------------------------------ */
/* Independent uniform bicubic B-spline reference                     */
/* ------------------------------------------------------------------ */

static double bspline_b(int i, double t)
{
    double t2 = t * t, t3 = t2 * t;
    switch (i) {
        case 0: { double z = 1.0 - t; return z * z * z / 6.0; }
        case 1: return (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0;
        case 2: return (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0;
        default:return t3 / 6.0;
    }
}
static double bspline_db(int i, double t)
{
    double t2 = t * t;
    switch (i) {
        case 0: return -(1.0 - t) * (1.0 - t) * 0.5;
        case 1: return (3.0 * t2 - 4.0 * t) * 0.5;
        case 2: return (-3.0 * t2 + 2.0 * t + 1.0) * 0.5;
        default:return t2 * 0.5;
    }
}

/* Evaluate a patch straight from its 4x4 raw control-point grid. */
static void ref_bspline(const double (*grid)[3], double u, double v,
                        double pos[3], double du[3], double dv[3])
{
    double bu[4], bv[4], dbu[4], dbv[4];
    for (int i = 0; i < 4; ++i) {
        bu[i] = bspline_b(i, u); bv[i] = bspline_b(i, v);
        dbu[i] = bspline_db(i, u); dbv[i] = bspline_db(i, v);
    }
    for (int c = 0; c < 3; ++c) pos[c] = du[c] = dv[c] = 0.0;
    for (int it = 0; it < 4; ++it)
        for (int is = 0; is < 4; ++is) {
            const double *x = grid[it * 4 + is];
            double w = bu[is] * bv[it], wu = dbu[is] * bv[it], wv = bu[is] * dbv[it];
            for (int c = 0; c < 3; ++c) {
                pos[c] += w * x[c];
                du[c] += wu * x[c];
                dv[c] += wv * x[c];
            }
        }
}

static double vec3_dist(const double a[3], const double b[3])
{
    double s = 0.0;
    for (int c = 0; c < 3; ++c) s += (a[c] - b[c]) * (a[c] - b[c]);
    return sqrt(s);
}

static double bbox_diag(const cc_Mesh *m)
{
    double lo[3], hi[3];
    for (int c = 0; c < 3; ++c) lo[c] = hi[c] = m->vertexPoints[0].array[c];
    for (int v = 1; v < m->vertexCount; ++v)
        for (int c = 0; c < 3; ++c) {
            lo[c] = fmin(lo[c], m->vertexPoints[v].array[c]);
            hi[c] = fmax(hi[c], m->vertexPoints[v].array[c]);
        }
    double d[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
    return sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

/* Independent Catmull-Clark eigen limit mask for an isolated vertex. */
static void ref_limit_point(int n, const double V[3],
                            const double R[][3], const double F[][3],
                            double out[3])
{
    double wv = (double)(n - 1) / (double)(n + 5);
    double wr = 2.0 / ((double)n * (double)(n + 5));
    double wf = 4.0 / ((double)n * (double)(n + 5));
    for (int c = 0; c < 3; ++c) {
        out[c] = wv * V[c];
        for (int k = 0; k < n; ++k) out[c] += wr * R[k][c] + wf * F[k][c];
    }
}

/* ================================================================== */
/* regular group                                                      */
/* ================================================================== */

static int regular_bspline(void)
{
    const int N = 7;
    double raw[400][3];
    int nv;
    cc_Mesh *m = toroidal_grid(N, raw, &nv);
    TASK_CHECK(m != NULL);
    cc_LimitSurface *ls = cc_limit_create(m);
    TASK_CHECK(ls != NULL);
    double L = bbox_diag(m);

    /* interior face (2,2); its 4x4 B-spline grid is raw lattice (1..4)^2 */
    const int fx = 2, fy = 2, face = fy * N + fx;
    double grid[16][3];
    for (int it = 0; it < 4; ++it)
        for (int is = 0; is < 4; ++is) {
            int gx = (fx - 1 + is + N) % N;
            int gy = (fy - 1 + it + N) % N;
            memcpy(grid[it * 4 + is], raw[gy * N + gx], sizeof(double[3]));
        }

    for (int trial = 0; trial < 12; ++trial) {
        double u = 0.11 + 0.73 * ((trial * 37) % 101) / 101.0;
        double v = 0.17 + 0.66 * ((trial * 53) % 101) / 101.0;
        cc_LimitSample sm;
        TASK_CHECK(cc_limit_sample(ls, face, u, v, &sm) == 0);
        TASK_CHECK(sm.derivatives_defined == 1);
        double rp[3], rdu[3], rdv[3];
        ref_bspline(grid, u, v, rp, rdu, rdv);
        TASK_CHECK(vec3_dist(sm.position, rp) < 2e-6 * L);
        double ed = 0.0;
        for (int c = 0; c < 3; ++c) {
            ed = fmax(ed, fabs(sm.du[c] - rdu[c]));
            ed = fmax(ed, fabs(sm.dv[c] - rdv[c]));
        }
        TASK_CHECK(ed < 2e-6 * L);
        /* normal follows du x dv */
        double cr[3];
        cr[0] = rdu[1] * rdv[2] - rdu[2] * rdv[1];
        cr[1] = rdu[2] * rdv[0] - rdu[0] * rdv[2];
        cr[2] = rdu[0] * rdv[1] - rdu[1] * rdv[0];
        double cl = sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
        double nref[3] = { cr[0] / cl, cr[1] / cl, cr[2] / cl };
        double ndot = sm.normal[0] * nref[0] + sm.normal[1] * nref[1]
                    + sm.normal[2] * nref[2];
        TASK_CHECK(acos(fmax(-1.0, fmin(1.0, ndot))) < 2e-4);
        /* normal is exactly the normalized cross product of the derivatives */
        double crs[3];
        crs[0] = sm.du[1] * sm.dv[2] - sm.du[2] * sm.dv[1];
        crs[1] = sm.du[2] * sm.dv[0] - sm.du[0] * sm.dv[2];
        crs[2] = sm.du[0] * sm.dv[1] - sm.du[1] * sm.dv[0];
        double cl2 = sqrt(crs[0] * crs[0] + crs[1] * crs[1] + crs[2] * crs[2]);
        double selfn = 0.0;
        for (int c = 0; c < 3; ++c)
            selfn = fmax(selfn, fabs(sm.normal[c] - crs[c] / cl2));
        TASK_CHECK(selfn < 1e-12);
    }
    cc_limit_release(ls);
    ccm_Release(m);
    return 0;
}

static int regular_finitediff(void)
{
    const int N = 7;
    cc_Mesh *m = toroidal_grid(N, NULL, NULL);
    cc_LimitSurface *ls = cc_limit_create(m);
    TASK_CHECK(ls != NULL);
    double L = bbox_diag(m);
    const int face = 2 * N + 2;
    const double u = 0.40, v = 0.63;
    cc_LimitSample c;
    TASK_CHECK(cc_limit_sample(ls, face, u, v, &c) == 0);

    const double hs[2] = { 1e-3, 1e-4 };
    double prev = 1e30;
    for (int q = 0; q < 2; ++q) {
        double h = hs[q];
        cc_LimitSample a, b;
        TASK_CHECK(cc_limit_sample(ls, face, u + h, v, &a) == 0);
        TASK_CHECK(cc_limit_sample(ls, face, u - h, v, &b) == 0);
        double edu = 0.0;
        for (int k = 0; k < 3; ++k)
            edu = fmax(edu, fabs(c.du[k] - (a.position[k] - b.position[k]) / (2 * h)));
        TASK_CHECK(cc_limit_sample(ls, face, u, v + h, &a) == 0);
        TASK_CHECK(cc_limit_sample(ls, face, u, v - h, &b) == 0);
        double edv = 0.0;
        for (int k = 0; k < 3; ++k)
            edv = fmax(edv, fabs(c.dv[k] - (a.position[k] - b.position[k]) / (2 * h)));
        TASK_CHECK(edu < 1e-6 * L && edv < 1e-6 * L);
        TASK_CHECK(edu <= prev * 1.5); /* central differences converge */
        prev = edu;
    }
    cc_limit_release(ls);
    ccm_Release(m);
    return 0;
}

static int regular_shared_edge(void)
{
    const int N = 7;
    cc_Mesh *m = toroidal_grid(N, NULL, NULL);
    cc_LimitSurface *ls = cc_limit_create(m);
    TASK_CHECK(ls != NULL);
    double L = bbox_diag(m);
    const int fx = 2, fy = 2;
    int fA = fy * N + fx;
    int fB = fy * N + ((fx + 1) % N);          /* shares fA's c1-c2 edge */
    const double w = 0.37;
    cc_LimitSample A, B;
    TASK_CHECK(cc_limit_sample(ls, fA, 1.0, w, &A) == 0);      /* u=1 edge */
    TASK_CHECK(cc_limit_sample(ls, fB, 0.0, w, &B) == 0);      /* u=0 edge, same v */
    TASK_CHECK(vec3_dist(A.position, B.position) < 2e-6 * L);
    double dot = A.normal[0] * B.normal[0] + A.normal[1] * B.normal[1]
               + A.normal[2] * B.normal[2];
    TASK_CHECK(acos(fmax(-1.0, fmin(1.0, dot))) < 2e-4);
    cc_limit_release(ls);
    ccm_Release(m);
    return 0;
}

/* ================================================================== */
/* extraordinary group                                                */
/* ================================================================== */

static int extra_corner_limit(void)
{
    const int valences[3] = { 3, 5, 7 };
    for (int qi = 0; qi < 3; ++qi) {
        int n = valences[qi];
        cc_Mesh *m = polar_cage(n);
        TASK_CHECK(m != NULL);
        cc_LimitSurface *ls = cc_limit_create(m);
        TASK_CHECK(ls != NULL);
        double L = bbox_diag(m);

        /* independent limit mask: pole 0, its n edge neighbours and faces */
        double V[3];
        for (int c = 0; c < 3; ++c) V[c] = m->vertexPoints[0].array[c];
        double R[12][3], F[12][3];
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) R[i][c] = m->vertexPoints[2 + i].array[c];
            double fc[3] = { 0, 0, 0 };
            for (int j = 0; j < 4; ++j) {
                int vid = m->halfedges[4 * i + j].vertexID;
                for (int c = 0; c < 3; ++c) fc[c] += m->vertexPoints[vid].array[c];
            }
            for (int c = 0; c < 3; ++c) F[i][c] = fc[c] / 4.0;
        }
        double ref[3];
        ref_limit_point(n, V, R, F, ref);

        cc_LimitSample corner;
        TASK_CHECK(cc_limit_sample(ls, 0, 0.0, 0.0, &corner) == 0);
        TASK_CHECK(corner.derivatives_defined == 0);
        TASK_CHECK(corner.du[0] == 0.0 && corner.du[1] == 0.0 && corner.du[2] == 0.0);
        TASK_CHECK(corner.dv[0] == 0.0 && corner.dv[1] == 0.0 && corner.dv[2] == 0.0);
        TASK_CHECK(vec3_dist(corner.position, ref) < 2e-6 * L);

        /* near-corner samples at several recursion levels converge
         * monotonically to the corner limit and carry defined derivatives
         * (the rate is the vertex subdominant eigenvalue, so higher
         * valences approach more slowly) */
        double prev = 1e30;
        for (int k = 8; k <= 20; k += 4) {
            double e = ldexp(1.0, -k);
            cc_LimitSample sm;
            TASK_CHECK(cc_limit_sample(ls, 0, e, e, &sm) == 0);
            TASK_CHECK(sm.derivatives_defined == 1);
            double d = vec3_dist(sm.position, corner.position);
            TASK_CHECK(d < prev);
            prev = d;
        }
        cc_LimitSample closest;
        TASK_CHECK(cc_limit_sample(ls, 0, ldexp(1.0, -20), ldexp(1.0, -20),
                                   &closest) == 0);
        TASK_CHECK(vec3_dist(closest.position, corner.position) < 1e-4 * L);
        cc_limit_release(ls);
        ccm_Release(m);
    }
    return 0;
}

static int extra_recursion_levels(void)
{
    const int valences[3] = { 3, 5, 7 };
    for (int qi = 0; qi < 3; ++qi) {
        int n = valences[qi];
        cc_Mesh *m = polar_cage(n);
        cc_LimitSurface *ls = cc_limit_create(m);
        TASK_CHECK(ls != NULL);
        double L = bbox_diag(m);

        /* A fixed parameter yields a single limit answer independent of any
         * preview/recursion depth: repeated calls are identical, and the
         * orientation never flips on a fine sequence of samples. */
        cc_LimitSample fixed;
        TASK_CHECK(cc_limit_sample(ls, 0, ldexp(1.0, -12), ldexp(1.0, -12),
                                   &fixed) == 0);
        for (int rep = 0; rep < 3; ++rep) {
            cc_LimitSample again;
            TASK_CHECK(cc_limit_sample(ls, 0, ldexp(1.0, -12),
                                       ldexp(1.0, -12), &again) == 0);
            TASK_CHECK(vec3_dist(fixed.position, again.position) == 0.0);
            double dot = fixed.normal[0] * again.normal[0]
                       + fixed.normal[1] * again.normal[1]
                       + fixed.normal[2] * again.normal[2];
            TASK_CHECK(dot > 1.0 - 1e-15);
        }
        /* a point as close as 2^-20 is supported and well conditioned */
        double e = ldexp(1.0, -20);
        cc_LimitSample deep;
        TASK_CHECK(cc_limit_sample(ls, 0, e, e, &deep) == 0);
        TASK_CHECK(deep.derivatives_defined == 1);
        double mag = 0.0;
        for (int c = 0; c < 3; ++c) mag += deep.du[c] * deep.du[c];
        TASK_CHECK(sqrt(mag) > 0.0);
        TASK_CHECK(L > 0.0);
        cc_limit_release(ls);
        ccm_Release(m);
    }
    return 0;
}

static int extra_finitediff(void)
{
    const int valences[3] = { 3, 5, 7 };
    for (int qi = 0; qi < 3; ++qi) {
        int n = valences[qi];
        cc_Mesh *m = polar_cage(n);
        cc_LimitSurface *ls = cc_limit_create(m);
        TASK_CHECK(ls != NULL);
        double L = bbox_diag(m);

        /* interior point of an extraordinary face */
        const double u = 0.30, v = 0.35;
        cc_LimitSample c;
        TASK_CHECK(cc_limit_sample(ls, 0, u, v, &c) == 0);
        const double hs[2] = { 1e-4, 1e-5 };
        for (int q = 0; q < 2; ++q) {
            double h = hs[q];
            cc_LimitSample a, b;
            TASK_CHECK(cc_limit_sample(ls, 0, u + h, v, &a) == 0);
            TASK_CHECK(cc_limit_sample(ls, 0, u - h, v, &b) == 0);
            double edu = 0.0;
            for (int k = 0; k < 3; ++k)
                edu = fmax(edu, fabs(c.du[k] - (a.position[k] - b.position[k]) / (2 * h)));
            TASK_CHECK(cc_limit_sample(ls, 0, u, v + h, &a) == 0);
            TASK_CHECK(cc_limit_sample(ls, 0, u, v - h, &b) == 0);
            double edv = 0.0;
            for (int k = 0; k < 3; ++k)
                edv = fmax(edv, fabs(c.dv[k] - (a.position[k] - b.position[k]) / (2 * h)));
            TASK_CHECK(edu < 1e-5 * L && edv < 1e-5 * L);
        }

        /* deep point: finite-difference steps scaled to the local sub-patch
         * (the carrying patch spans ~2^-k in original parameters) */
        double e = ldexp(1.0, -20);
        double h = 1e-3 * e;
        cc_LimitSample dc, da, db;
        TASK_CHECK(cc_limit_sample(ls, 0, e, e, &dc) == 0);
        TASK_CHECK(cc_limit_sample(ls, 0, e + h, e, &da) == 0);
        TASK_CHECK(cc_limit_sample(ls, 0, e - h, e, &db) == 0);
        double edu = 0.0, magdu = 0.0;
        for (int k = 0; k < 3; ++k) {
            edu = fmax(edu, fabs(dc.du[k] - (da.position[k] - db.position[k]) / (2 * h)));
            magdu += dc.du[k] * dc.du[k];
        }
        TASK_CHECK(edu / sqrt(magdu) < 1e-5);
        cc_limit_release(ls);
        ccm_Release(m);
    }
    return 0;
}

static int extra_shared_edge(void)
{
    const int valences[3] = { 3, 5, 7 };
    for (int qi = 0; qi < 3; ++qi) {
        int n = valences[qi];
        cc_Mesh *m = polar_cage(n);
        cc_LimitSurface *ls = cc_limit_create(m);
        TASK_CHECK(ls != NULL);
        double L = bbox_diag(m);
        /* face 0 slot1 (U0->L0); locate the twin face/slot through topology */
        int h = 4 * 0 + 1;
        int t = m->halfedges[h].twinID;
        int tf = m->halfedges[t].faceID, ts = t & 3;
        const double w = 0.31;
        double Ap[2] = { 1.0, w };                 /* slot1 of face 0 */
        double Q[4][2] = { {1.0 - w, 0.0}, {1.0, 1.0 - w},
                           {w, 1.0}, {0.0, w} };
        cc_LimitSample A, B;
        TASK_CHECK(cc_limit_sample(ls, 0, Ap[0], Ap[1], &A) == 0);
        TASK_CHECK(cc_limit_sample(ls, tf, Q[ts][0], Q[ts][1], &B) == 0);
        TASK_CHECK(vec3_dist(A.position, B.position) < 2e-6 * L);
        double dot = A.normal[0] * B.normal[0] + A.normal[1] * B.normal[1]
                   + A.normal[2] * B.normal[2];
        TASK_CHECK(acos(fmax(-1.0, fmin(1.0, dot))) < 2e-4);
        cc_limit_release(ls);
        ccm_Release(m);
    }
    return 0;
}

/* ================================================================== */
/* charts group                                                       */
/* ================================================================== */

/* Rotating a face's start halfedge rotates its (u,v) chart; the surface
 * point and oriented normal must be unchanged and derivatives convert. */
static int charts_start_halfedge_rotation(void)
{
    const int N = 7;
    cc_Mesh *base = toroidal_grid(N, NULL, NULL);
    cc_LimitSurface *l0 = cc_limit_create(base);
    TASK_CHECK(l0 != NULL);
    double L = bbox_diag(base);
    const int face = 2 * N + 2;
    const double u = 0.34, v = 0.58;
    cc_LimitSample s0;
    TASK_CHECK(cc_limit_sample(l0, face, u, v, &s0) == 0);

    for (int r = 1; r < 4; ++r) {
        cc_Mesh *rot = toroidal_grid(N, NULL, NULL);
        /* move each face's recorded start halfedge r corners forward */
        for (int f = 0; f < rot->faceCount; ++f) {
            int h = rot->faceToHalfedgeIDs[f];
            for (int k = 0; k < r; ++k) h = rot->halfedges[h].nextID;
            rot->faceToHalfedgeIDs[f] = h;
        }
        cc_LimitSurface *lr = cc_limit_create(rot);
        TASK_CHECK(lr != NULL);

        double up, vp;
        if (r == 1)      { up = v;       vp = 1.0 - u; }
        else if (r == 2) { up = 1.0 - u; vp = 1.0 - v; }
        else             { up = 1.0 - v; vp = u; }
        cc_LimitSample sr;
        TASK_CHECK(cc_limit_sample(lr, face, up, vp, &sr) == 0);
        TASK_CHECK(vec3_dist(s0.position, sr.position) < 2e-6 * L);
        double dot = s0.normal[0] * sr.normal[0] + s0.normal[1] * sr.normal[1]
                   + s0.normal[2] * sr.normal[2];
        TASK_CHECK(fabs(dot - 1.0) < 1e-12);

        if (r == 1) {
            /* (u',v')=(v,1-u): du_0 = -dv_r, dv_0 = du_r */
            double e1 = 0.0;
            for (int c = 0; c < 3; ++c) {
                e1 = fmax(e1, fabs(s0.du[c] - (-sr.dv[c])));
                e1 = fmax(e1, fabs(s0.dv[c] - sr.du[c]));
            }
            TASK_CHECK(e1 < 2e-6 * L);
        }
        cc_limit_release(lr);
        ccm_Release(rot);
    }
    cc_limit_release(l0);
    ccm_Release(base);
    return 0;
}

/* Shared edge: derivatives expressed in the two face charts convert. */
static int charts_edge_derivative_conversion(void)
{
    const int N = 7;
    cc_Mesh *m = toroidal_grid(N, NULL, NULL);
    cc_LimitSurface *ls = cc_limit_create(m);
    TASK_CHECK(ls != NULL);
    double L = bbox_diag(m);
    const int fx = 2, fy = 2;
    int fA = fy * N + fx, fB = fy * N + ((fx + 1) % N);
    const double w = 0.41;
    cc_LimitSample A, B;
    TASK_CHECK(cc_limit_sample(ls, fA, 1.0, w, &A) == 0);
    TASK_CHECK(cc_limit_sample(ls, fB, 0.0, w, &B) == 0);
    /* along the shared edge v is shared: dv directions agree; the two
     * outward/inward u derivatives both point across the same edge, so the
     * tangential dv vectors must match component-wise */
    double ed = 0.0;
    for (int c = 0; c < 3; ++c) ed = fmax(ed, fabs(A.dv[c] - B.dv[c]));
    TASK_CHECK(ed < 2e-6 * L);
    /* normals already identical => the u derivatives span the same tangent */
    double dot = A.normal[0] * B.normal[0] + A.normal[1] * B.normal[1]
               + A.normal[2] * B.normal[2];
    TASK_CHECK(fabs(dot - 1.0) < 1e-12);
    cc_limit_release(ls);
    ccm_Release(m);
    return 0;
}

static int charts_lifecycle_and_failures(void)
{
    const int N = 7;
    cc_Mesh *m = toroidal_grid(N, NULL, NULL);
    cc_LimitSurface *a = cc_limit_create(m);
    cc_LimitSurface *b = cc_limit_create(m);
    TASK_CHECK(a != NULL && b != NULL);

    /* release the source cage: the surfaces own independent copies */
    cc_LimitSample before;
    TASK_CHECK(cc_limit_sample(a, 3, 0.3, 0.3, &before) == 0);
    ccm_Release(m);
    cc_LimitSample after;
    TASK_CHECK(cc_limit_sample(a, 3, 0.3, 0.3, &after) == 0);
    TASK_CHECK(vec3_dist(before.position, after.position) == 0.0);

    /* two surfaces interleave on the same (now freed) source safely */
    cc_LimitSample sa, sb;
    TASK_CHECK(cc_limit_sample(a, 5, 0.2, 0.7, &sa) == 0);
    TASK_CHECK(cc_limit_sample(b, 5, 0.2, 0.7, &sb) == 0);
    TASK_CHECK(vec3_dist(sa.position, sb.position) == 0.0);
    cc_limit_release(a);
    /* b still usable after a is destroyed */
    TASK_CHECK(cc_limit_sample(b, 5, 0.2, 0.7, &sb) == 0);
    cc_limit_release(b);

    /* invalid cages are rejected: a lone quad has a boundary */
    {
        int F[1][4] = { { 0, 1, 2, 3 } };
        double P[4][3] = { {0,0,0},{1,0,0},{1,1,0},{0,1,0} };
        cc_Mesh *boundary = cage_build(4, F, 1, P);
        TASK_CHECK(boundary == NULL);
    }
    /* an out-of-range valence: a closed spindle whose poles have valence 13 */
    {
        cc_Mesh *high = polar_cage(13);
        TASK_CHECK(high != NULL);              /* cage itself is a valid manifold */
        TASK_CHECK(cc_limit_create(high) == NULL);
        ccm_Release(high);
    }

    /* argument validation leaves output untouched */
    cc_Mesh *good = toroidal_grid(N, NULL, NULL);
    cc_LimitSurface *g = cc_limit_create(good);
    TASK_CHECK(g != NULL);
    cc_LimitSample sentinel;
    memset(&sentinel, 0x7A, sizeof(sentinel));
    cc_LimitSample probe = sentinel;
    TASK_CHECK(cc_limit_sample(g, -1, 0.2, 0.2, &probe) != 0);
    TASK_CHECK(memcmp(&probe, &sentinel, sizeof(probe)) == 0);
    TASK_CHECK(cc_limit_sample(g, 99999, 0.2, 0.2, &probe) != 0);
    TASK_CHECK(memcmp(&probe, &sentinel, sizeof(probe)) == 0);
    TASK_CHECK(cc_limit_sample(g, 1, 1.5, 0.2, &probe) != 0);
    TASK_CHECK(cc_limit_sample(g, 1, 0.2, -0.1, &probe) != 0);
    TASK_CHECK(cc_limit_sample(g, 1, NAN, 0.2, &probe) != 0);
    TASK_CHECK(memcmp(&probe, &sentinel, sizeof(probe)) == 0);
    TASK_CHECK(cc_limit_sample(g, 1, 0.2, 0.2, NULL) != 0);
    TASK_CHECK(cc_limit_sample(NULL, 1, 0.2, 0.2, &probe) != 0);
    /* after failures, valid queries still succeed */
    TASK_CHECK(cc_limit_sample(g, 1, 0.2, 0.2, &probe) == 0);
    TASK_CHECK(probe.derivatives_defined == 1);
    cc_limit_release(g);
    ccm_Release(good);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registry                                                           */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "regular", "bspline_vs_independent", regular_bspline },
    { "regular", "finite_difference",      regular_finitediff },
    { "regular", "shared_edge_continuity", regular_shared_edge },

    { "extraordinary", "corner_limit_mask",     extra_corner_limit },
    { "extraordinary", "recursion_levels",      extra_recursion_levels },
    { "extraordinary", "finite_difference",     extra_finitediff },
    { "extraordinary", "shared_edge_continuity", extra_shared_edge },

    { "charts", "start_halfedge_rotation",      charts_start_halfedge_rotation },
    { "charts", "edge_derivative_conversion",   charts_edge_derivative_conversion },
    { "charts", "lifecycle_and_failures",       charts_lifecycle_and_failures },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
