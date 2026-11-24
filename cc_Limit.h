/*
 * cc_Limit.h -- direct limit-surface queries for Catmull-Clark control cages.
 *
 * Single-header library. In exactly one translation unit, define
 * CC_LIMIT_IMPLEMENTATION before including this file.
 *
 * The evaluator never builds a high-resolution mesh of the whole model:
 * every query subdivides only the local neighbourhood of the queried face
 * until the carrying sub-patch has four regular corners, then evaluates the
 * exact uniform bicubic B-spline patch of the infinite Catmull-Clark limit.
 * Analytic parametric derivatives are carried through the chain of child
 * charts, so results do not depend on any preview subdivision depth.
 *
 * Accepted cages: finite coordinates, boundary-free, consistently oriented
 * combinatorial 2-manifold quad meshes, vertex valences 3..12, all crease
 * sharpnesses zero. Multiple connected components are allowed; adjacent
 * extraordinary vertices are allowed. Self-intersections and degenerate
 * limit surfaces are not handled.
 */
#ifndef CC_LIMIT_INCLUDE_H
#define CC_LIMIT_INCLUDE_H

#include <stdint.h>
#include "CatmullClark.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque limit surface. Owns its own copy of the cage geometry/topology. */
typedef struct cc_LimitSurface cc_LimitSurface;

typedef struct {
    double position[3];
    double du[3];
    double dv[3];
    double normal[3];
    int derivatives_defined; /* 0 only at an extraordinary corner sample */
} cc_LimitSample;

/* Returns NULL (reclaiming partial allocations) if the cage is rejected. */
cc_LimitSurface *cc_limit_create(const cc_Mesh *mesh);
void cc_limit_release(cc_LimitSurface *surface);

/*
 * Face parameterisation: for face f, starting at faceToHalfedgeIDs[f], the
 * four corners map to (u,v) = (0,0), (1,0), (1,1), (0,1) in that order.
 * u and v are clamped to the closed domain [0,1] by the caller contract;
 * non-finite or out-of-range values, bad face ids and null outputs fail.
 * Returns 0 on success, non-zero on failure (output left untouched).
 */
int cc_limit_sample(const cc_LimitSurface *surface,
                    int32_t face, double u, double v,
                    cc_LimitSample *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CC_LIMIT_INCLUDE_H */

#ifdef CC_LIMIT_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef CCL_ASSERT
#  include <assert.h>
#  define CCL_ASSERT(x) assert(x)
#endif

/* ------------------------------------------------------------------ */
/* Owned cage copy                                                    */
/* ------------------------------------------------------------------ */

struct cc_LimitSurface {
    int32_t nv;
    int32_t nf;
    double (*p)[3];          /* nv vertex positions                   */
    int32_t *fv;             /* nf*4 corner vertex ids                */
    int32_t *nbr;            /* nf*4 neighbour face across edge j     */
    int32_t *vface_off;      /* nv+1 offsets into vface               */
    int32_t *vface;          /* incident faces per vertex             */
    int32_t *deg;            /* nv cage vertex valences               */
    uint8_t *extra;          /* per-vertex extraordinary flag         */
    double scale;            /* bounding-box diagonal length L        */
};

/* A packed local quad mesh: halfedge id = 4*face + corner. */
typedef struct {
    int32_t nf;
    int32_t nv;
    double (*p)[3];          /* nv positions                          */
    int32_t *fv;             /* nf*4 corner vertex ids (local)        */
    int32_t *tw;             /* nf*4 twin halfedge ids, -1 at border  */
    int32_t *val;            /* nv topological valences (global)      */
} ccl_Patch;

static void ccl_free_patch(ccl_Patch *q)
{
    if (!q) return;
    free(q->p);
    free(q->fv);
    free(q->tw);
    free(q->val);
    q->p = NULL;
    q->fv = NULL;
    q->tw = NULL;
    q->val = NULL;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int ccl_finite3(const double p[3])
{
    return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]);
}

static void ccl_sub3(double o[3], const double a[3], const double b[3])
{
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}

static void ccl_cross3(double o[3], const double a[3], const double b[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static double ccl_dot3(const double a[3], const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Ordered-pair hash: maps directed edge (a,b) -> halfedge id. */
typedef struct {
    int cap;                 /* power of two                          */
    int32_t *key_a;
    int32_t *key_b;
    int32_t *val;
} ccl_EdgeMap;

static int ccl_edgemap_init(ccl_EdgeMap *m, int slots)
{
    int cap = 8;
    while (cap < slots * 2) cap <<= 1;
    m->cap = cap;
    m->key_a = (int32_t *)malloc(sizeof(int32_t) * (size_t)cap);
    m->key_b = (int32_t *)malloc(sizeof(int32_t) * (size_t)cap);
    m->val   = (int32_t *)malloc(sizeof(int32_t) * (size_t)cap);
    if (!m->key_a || !m->key_b || !m->val) {
        free(m->key_a); free(m->key_b); free(m->val);
        m->key_a = m->key_b = m->val = NULL;
        return 0;
    }
    for (int i = 0; i < cap; ++i) { m->key_a[i] = -1; m->val[i] = -1; }
    return 1;
}

static void ccl_edgemap_free(ccl_EdgeMap *m)
{
    free(m->key_a); free(m->key_b); free(m->val);
    m->key_a = m->key_b = NULL; m->val = NULL;
}

static uint32_t ccl_pair_hash(int32_t a, int32_t b)
{
    uint32_t x = (uint32_t)a * 2654435761u ^ (uint32_t)b * 2246822519u;
    x ^= x >> 15; x *= 732441947u; x ^= x >> 13;
    return x;
}

/* Insert; returns 0 on duplicate. */
static int ccl_edgemap_put(ccl_EdgeMap *m, int32_t a, int32_t b, int32_t he)
{
    uint32_t i = ccl_pair_hash(a, b) & (uint32_t)(m->cap - 1);
    while (m->key_a[i] != -1) {
        if (m->key_a[i] == a && m->key_b[i] == b) return 0;
        i = (i + 1u) & (uint32_t)(m->cap - 1);
    }
    m->key_a[i] = a; m->key_b[i] = b; m->val[i] = he;
    return 1;
}

static int32_t ccl_edgemap_get(const ccl_EdgeMap *m, int32_t a, int32_t b)
{
    uint32_t i = ccl_pair_hash(a, b) & (uint32_t)(m->cap - 1);
    while (m->key_a[i] != -1) {
        if (m->key_a[i] == a && m->key_b[i] == b) return m->val[i];
        i = (i + 1u) & (uint32_t)(m->cap - 1);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Cage validation and copy                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t a, b, he;
} ccl_DirEdge;

static int ccl_diredge_cmp(const void *x, const void *y)
{
    const ccl_DirEdge *ex = (const ccl_DirEdge *)x;
    const ccl_DirEdge *ey = (const ccl_DirEdge *)y;
    if (ex->a != ey->a) return ex->a < ey->a ? -1 : 1;
    if (ex->b != ey->b) return ex->b < ey->b ? -1 : 1;
    return 0;
}

static void ccl_release_surface(cc_LimitSurface *s)
{
    if (!s) return;
    free(s->p); free(s->fv); free(s->nbr);
    free(s->vface_off); free(s->vface); free(s->deg); free(s->extra);
    free(s);
}

cc_LimitSurface *cc_limit_create(const cc_Mesh *mesh)
{
    if (!mesh) return NULL;
    const int32_t nv = mesh->vertexCount;
    const int32_t nf = mesh->faceCount;
    const int32_t nh = mesh->halfedgeCount;
    const int32_t ne = mesh->edgeCount;
    if (nv < 4 || nf < 1 || ne < 1 || nh != 4 * nf) return NULL;
    if (!mesh->vertexPoints || !mesh->halfedges || !mesh->creases ||
        !mesh->faceToHalfedgeIDs || !mesh->vertexToHalfedgeIDs ||
        !mesh->edgeToHalfedgeIDs) return NULL;

    cc_LimitSurface *s = (cc_LimitSurface *)calloc(1, sizeof(*s));
    int32_t *fv = NULL, *nbr = NULL, *voff = NULL, *vface = NULL;
    int32_t *deg = NULL, *twin = NULL;
    uint8_t *extra = NULL;
    double (*p)[3] = NULL;
    ccl_DirEdge *dirs = NULL;
    int ok = 0;

    do {
        fv = (int32_t *)malloc(sizeof(int32_t) * (size_t)nh);
        nbr = (int32_t *)malloc(sizeof(int32_t) * (size_t)nh);
        voff = (int32_t *)calloc((size_t)nv + 1, sizeof(int32_t));
        vface = (int32_t *)malloc(sizeof(int32_t) * (size_t)nh);
        deg = (int32_t *)calloc((size_t)nv, sizeof(int32_t));
        extra = (uint8_t *)calloc((size_t)nv, sizeof(uint8_t));
        p = malloc(sizeof(double[3]) * (size_t)nv);
        dirs = (ccl_DirEdge *)malloc(sizeof(ccl_DirEdge) * (size_t)nh);
        twin = (int32_t *)malloc(sizeof(int32_t) * (size_t)nh);
        if (!fv || !nbr || !voff || !vface || !deg || !extra ||
            !p || !dirs || !twin) break;

        /* finite coordinates */
        for (int32_t v = 0; v < nv; ++v) {
            p[v][0] = (double)mesh->vertexPoints[v].x;
            p[v][1] = (double)mesh->vertexPoints[v].y;
            p[v][2] = (double)mesh->vertexPoints[v].z;
            if (!ccl_finite3(p[v])) goto fail;
        }

        /* every face is a quad, walked from its recorded start halfedge */
        uint8_t *seen = (uint8_t *)calloc((size_t)nh, 1);
        if (!seen) break;
        for (int32_t f = 0; f < nf; ++f) {
            int32_t h0 = mesh->faceToHalfedgeIDs[f];
            if (h0 < 0 || h0 >= nh) { free(seen); goto fail; }
            int32_t h = h0;
            for (int j = 0; j < 4; ++j) {
                if (h < 0 || h >= nh || seen[h]) { free(seen); goto fail; }
                seen[h] = 1;
                int32_t vid = mesh->halfedges[h].vertexID;
                if (vid < 0 || vid >= nv) { free(seen); goto fail; }
                fv[4 * f + j] = vid;
                h = mesh->halfedges[h].nextID;
            }
            if (h != h0) { free(seen); goto fail; }
            for (int j = 0; j < 4; ++j)
                for (int k = j + 1; k < 4; ++k)
                    if (fv[4 * f + j] == fv[4 * f + k]) {
                        free(seen); goto fail;
                    }
        }
        for (int32_t h = 0; h < nh; ++h)
            if (!seen[h]) { free(seen); goto fail; }
        free(seen);

        /* all creases smooth */
        for (int32_t h = 0; h < nh; ++h) {
            int32_t e = mesh->halfedges[h].edgeID;
            if (e < 0 || e >= ne) goto fail;
            float sharp = mesh->creases[e].sharpness;
            if (sharp != sharp || sharp != 0.0f) goto fail;
        }

        /* unordered endpoint pairs: exactly two directed halfedges each */
        for (int32_t h = 0; h < nh; ++h) {
            int32_t a = mesh->halfedges[h].vertexID;
            int32_t b = mesh->halfedges[mesh->halfedges[h].nextID].vertexID;
            if (a < 0 || a >= nv || b < 0 || b >= nv || a == b) goto fail;
            if (a > b) { int32_t t = a; a = b; b = t; }
            dirs[h].a = a; dirs[h].b = b; dirs[h].he = h;
        }
        qsort(dirs, (size_t)nh, sizeof(ccl_DirEdge), ccl_diredge_cmp);
        for (int32_t k = 0; k < nh;) {
            int32_t a = dirs[k].a, b = dirs[k].b;
            int32_t k1 = k + 1;
            while (k1 < nh && dirs[k1].a == a && dirs[k1].b == b) ++k1;
            if (k1 - k != 2) goto fail; /* boundary / non-manifold edge */
            k = k1;
        }

        /* twin table on our packed layout, cross-checked with the mesh */
        ccl_EdgeMap em;
        if (!ccl_edgemap_init(&em, nh)) goto fail;
        for (int32_t h = 0; h < nh; ++h) {
            int32_t a = mesh->halfedges[h].vertexID;
            int32_t b = mesh->halfedges[mesh->halfedges[h].nextID].vertexID;
            if (!ccl_edgemap_put(&em, a, b, h)) {
                ccl_edgemap_free(&em); goto fail;
            }
        }
        for (int32_t h = 0; h < nh; ++h) {
            int32_t a = mesh->halfedges[h].vertexID;
            int32_t b = mesh->halfedges[mesh->halfedges[h].nextID].vertexID;
            int32_t t = ccl_edgemap_get(&em, b, a);
            if (t < 0) { ccl_edgemap_free(&em); goto fail; }
            twin[h] = t;
            int32_t stated = mesh->halfedges[h].twinID;
            if (stated >= 0 && stated != t) {
                ccl_edgemap_free(&em); goto fail;
            }
        }
        ccl_edgemap_free(&em);

        /* neighbour face across each packed corner edge */
        for (int32_t f = 0; f < nf; ++f) {
            for (int j = 0; j < 4; ++j) {
                int32_t h = mesh->faceToHalfedgeIDs[f];
                for (int k = 0; k < j; ++k)
                    h = mesh->halfedges[h].nextID;
                int32_t t = twin[h];
                int32_t nfid = mesh->halfedges[t].faceID;
                if (nfid < 0 || nfid >= nf) goto fail;
                nbr[4 * f + j] = nfid;
            }
        }

        /* vertex degrees */
        for (int32_t f = 0; f < nf; ++f)
            for (int j = 0; j < 4; ++j)
                ++deg[fv[4 * f + j]];
        for (int32_t v = 0; v < nv; ++v) {
            if (deg[v] < 3 || deg[v] > 12) goto fail;
            extra[v] = (deg[v] != 4) ? 1 : 0;
        }

        /* each vertex link must be one closed fan of deg[v] faces */
        for (int32_t v = 0; v < nv; ++v) {
            int32_t start = mesh->vertexToHalfedgeIDs[v];
            if (start < 0 || start >= nh ||
                mesh->halfedges[start].vertexID != v) goto fail;
            int32_t h = start;
            int32_t last_face = -2;
            for (int k = 0; k < deg[v]; ++k) {
                if (mesh->halfedges[h].vertexID != v) goto fail;
                int32_t f = mesh->halfedges[h].faceID;
                if (f < 0 || f >= nf || f == last_face) goto fail;
                int32_t t = mesh->halfedges[h].twinID;
                if (t < 0) goto fail;
                h = mesh->halfedges[t].nextID;
                last_face = f;
            }
            if (h != start) goto fail;
        }

        /* incident face lists per vertex */
        for (int32_t f = 0; f < nf; ++f)
            for (int j = 0; j < 4; ++j)
                ++voff[fv[4 * f + j] + 1];
        for (int32_t v = 0; v < nv; ++v)
            voff[v + 1] += voff[v];
        int32_t *wpos = (int32_t *)malloc(sizeof(int32_t) * (size_t)nv);
        if (!wpos) goto fail;
        memcpy(wpos, voff, sizeof(int32_t) * (size_t)nv);
        for (int32_t f = 0; f < nf; ++f)
            for (int j = 0; j < 4; ++j)
                vface[wpos[fv[4 * f + j]]++] = f;
        free(wpos);

        /* bounding-box diagonal */
        double lo[3], hi[3];
        for (int c = 0; c < 3; ++c) { lo[c] = p[0][c]; hi[c] = p[0][c]; }
        for (int32_t v = 1; v < nv; ++v)
            for (int c = 0; c < 3; ++c) {
                if (p[v][c] < lo[c]) lo[c] = p[v][c];
                if (p[v][c] > hi[c]) hi[c] = p[v][c];
            }
        double d[3]; ccl_sub3(d, hi, lo);
        double L = sqrt(ccl_dot3(d, d));
        if (!(L > 0.0)) goto fail; /* degenerate cage */

        s->nv = nv; s->nf = nf;
        s->p = p; s->fv = fv; s->nbr = nbr;
        s->vface_off = voff; s->vface = vface;
        s->deg = deg; s->extra = extra; s->scale = L;
        ok = 1;
    } while (0);

fail:
    free(twin); free(dirs);
    if (!ok) {
        free(p); free(fv); free(nbr); free(voff);
        free(vface); free(deg); free(extra);
        ccl_release_surface(s);
        return NULL;
    }
    return s;
}

void cc_limit_release(cc_LimitSurface *surface)
{
    ccl_release_surface(surface);
}

/* ------------------------------------------------------------------ */
/* Local patch construction                                           */
/* ------------------------------------------------------------------ */

static void ccl_edgemap_fill_twins(ccl_Patch *q)
{
    ccl_EdgeMap em;
    if (!ccl_edgemap_init(&em, q->nf * 4)) return;
    for (int f = 0; f < q->nf; ++f)
        for (int j = 0; j < 4; ++j)
            ccl_edgemap_put(&em, q->fv[4 * f + j],
                            q->fv[4 * f + (j + 1) % 4], 4 * f + j);
    for (int f = 0; f < q->nf; ++f)
        for (int j = 0; j < 4; ++j) {
            int32_t a = q->fv[4 * f + j];
            int32_t b = q->fv[4 * f + (j + 1) % 4];
            q->tw[4 * f + j] = ccl_edgemap_get(&em, b, a);
        }
    ccl_edgemap_free(&em);
}

/* Crop the owned cage to f0 plus `rings` expansions of corner-adjacent
 * faces (rings=2 covers the full 4x4 B-spline grid of a regular face).
 * Expansion saturates harmlessly once a whole connected component is
 * included, in which case the patch has no border at all.
 * The local index assigned to f0 is returned in *seed. */
static int ccl_crop_face(const cc_LimitSurface *s, int32_t f0,
                         ccl_Patch *q, int *seed, int rings)
{
    uint8_t *pick = (uint8_t *)calloc((size_t)s->nf, 1);
    int32_t *map = (int32_t *)malloc(sizeof(int32_t) * (size_t)s->nv);
    if (!pick || !map) { free(pick); free(map); return 0; }
    for (int32_t v = 0; v < s->nv; ++v) map[v] = -1;

    pick[f0] = 1;
    for (int r = 0; r < rings; ++r) {
        uint8_t *grow = (uint8_t *)calloc((size_t)s->nf, 1);
        if (!grow) { free(pick); free(map); return 0; }
        for (int32_t f = 0; f < s->nf; ++f) {
            if (!pick[f]) continue;
            for (int j = 0; j < 4; ++j) {
                int32_t v = s->fv[4 * f + j];
                for (int32_t k = s->vface_off[v]; k < s->vface_off[v + 1]; ++k)
                    grow[s->vface[k]] = 1;
            }
        }
        int changed = 0;
        for (int32_t f = 0; f < s->nf; ++f)
            if (grow[f] && !pick[f]) { pick[f] = 1; changed = 1; }
        free(grow);
        if (!changed) break;
    }
    int nf = 0;
    for (int32_t f = 0; f < s->nf; ++f) if (pick[f]) ++nf;

    int32_t *lfv = (int32_t *)malloc(sizeof(int32_t) * (size_t)nf * 4u);
    if (!lfv) { free(pick); free(map); return 0; }
    int nv = 0, fi = 0, seed_local = -1;
    for (int32_t f = 0; f < s->nf; ++f) {
        if (!pick[f]) continue;
        if (f == f0) seed_local = fi;
        for (int j = 0; j < 4; ++j) {
            int32_t v = s->fv[4 * f + j];
            if (map[v] < 0) map[v] = nv++;
            lfv[4 * fi + j] = map[v];
        }
        ++fi;
    }
    double (*lp)[3] = malloc(sizeof(double[3]) * (size_t)nv);
    int32_t *ltw = (int32_t *)malloc(sizeof(int32_t) * (size_t)nf * 4u);
    int32_t *lval = (int32_t *)malloc(sizeof(int32_t) * (size_t)nv);
    if (!lp || !ltw || !lval) {
        free(lfv); free(lp); free(ltw); free(lval); free(pick); free(map);
        return 0;
    }
    for (int32_t v = 0; v < s->nv; ++v)
        if (map[v] >= 0) {
            memcpy(lp[map[v]], s->p[v], sizeof(double[3]));
            lval[map[v]] = s->deg[v];
        }

    free(pick); free(map);
    q->nf = nf; q->nv = nv; q->p = lp; q->fv = lfv; q->tw = ltw; q->val = lval;
    ccl_edgemap_fill_twins(q);
    *seed = seed_local;
    return seed_local >= 0;
}

/* Crop an existing patch around face f0, expanding `rings` times through
 * corner face fans. The local index of f0 is returned in *seed. */
static int ccl_crop_patch(const ccl_Patch *src, int f0,
                          ccl_Patch *q, int *seed, int rings)
{
    uint8_t *pick = (uint8_t *)calloc((size_t)src->nf, 1);
    int32_t *map = (int32_t *)malloc(sizeof(int32_t) * (size_t)src->nv);
    if (!pick || !map) { free(pick); free(map); return 0; }
    for (int32_t v = 0; v < src->nv; ++v) map[v] = -1;

    /* mark each marked corner's full face fan, repeated `rings` times */
    pick[f0] = 1;
    for (int pass = 0; pass < rings; ++pass) {
        uint8_t *grow = (uint8_t *)calloc((size_t)src->nf, 1);
        if (!grow) { free(pick); free(map); return 0; }
        for (int pf = 0; pf < src->nf; ++pf) {
            if (!pick[pf]) continue;
            for (int j = 0; j < 4; ++j) {
                int32_t v = src->fv[4 * pf + j];
                int32_t start = -1;
                for (int f = 0; f < src->nf && start < 0; ++f)
                    for (int k = 0; k < 4; ++k)
                        if (src->fv[4 * f + k] == v) { start = 4 * f + k; break; }
                if (start < 0) { free(grow); free(pick); free(map); return 0; }
                int32_t h = start;
                do {
                    grow[h >> 2] = 1;
                    int32_t t = src->tw[h];
                    if (t < 0) break;
                    h = (t & ~3) | ((t + 1) & 3); /* next(twin(h)) */
                } while (h != start);
            }
        }
        int changed = 0;
        for (int f = 0; f < src->nf; ++f)
            if (grow[f] && !pick[f]) { pick[f] = 1; changed = 1; }
        free(grow);
        if (!changed) break;
    }

    int nf = 0;
    for (int f = 0; f < src->nf; ++f) if (pick[f]) ++nf;
    int32_t *lfv = (int32_t *)malloc(sizeof(int32_t) * (size_t)nf * 4u);
    if (!lfv) { free(pick); free(map); return 0; }
    int nv = 0, fi = 0, seed_local = -1;
    for (int f = 0; f < src->nf; ++f) {
        if (!pick[f]) continue;
        if (f == f0) seed_local = fi;
        for (int j = 0; j < 4; ++j) {
            int32_t v = src->fv[4 * f + j];
            if (map[v] < 0) map[v] = nv++;
            lfv[4 * fi + j] = map[v];
        }
        ++fi;
    }
    double (*lp)[3] = malloc(sizeof(double[3]) * (size_t)nv);
    int32_t *ltw = (int32_t *)malloc(sizeof(int32_t) * (size_t)nf * 4u);
    int32_t *lval = (int32_t *)malloc(sizeof(int32_t) * (size_t)nv);
    if (!lp || !ltw || !lval) {
        free(lfv); free(lp); free(ltw); free(lval); free(pick); free(map);
        return 0;
    }
    for (int32_t v = 0; v < src->nv; ++v)
        if (map[v] >= 0) {
            memcpy(lp[map[v]], src->p[v], sizeof(double[3]));
            lval[map[v]] = src->val[v];
        }

    free(pick); free(map);
    q->nf = nf; q->nv = nv; q->p = lp; q->fv = lfv; q->tw = ltw; q->val = lval;
    ccl_edgemap_fill_twins(q);
    *seed = seed_local;
    return seed_local >= 0;
}

/* One Catmull-Clark refinement of a packed patch. Vertices at the crop
 * border use the same rules with missing neighbours replaced by available
 * data; only faces whose rings stay interior are consumed afterwards. */
static int ccl_refine_patch(const ccl_Patch *q, ccl_Patch *qn)
{
    int nf = q->nf, nv = q->nv;
    double (*fp)[3] = malloc(sizeof(double[3]) * (size_t)nf);
    double (*ep)[3] = malloc(sizeof(double[3]) * (size_t)nf * 4u);
    double (*np)[3] = malloc(sizeof(double[3]) * (size_t)nv);
    int32_t *nfv = (int32_t *)malloc(sizeof(int32_t) * (size_t)nf * 16u);
    if (!fp || !ep || !np || !nfv) {
        free(fp); free(ep); free(np); free(nfv); return 0;
    }

    /* face points: quad centroids */
    for (int f = 0; f < nf; ++f) {
        for (int c = 0; c < 3; ++c) fp[f][c] = 0.0;
        for (int j = 0; j < 4; ++j)
            for (int c = 0; c < 3; ++c)
                fp[f][c] += q->p[q->fv[4 * f + j]][c];
        for (int c = 0; c < 3; ++c) fp[f][c] *= 0.25;
    }

    /* edge points: (va + vb + f1 + f2) / 4 */
    for (int f = 0; f < nf; ++f) {
        for (int j = 0; j < 4; ++j) {
            int32_t he = 4 * f + j;
            int32_t a = q->fv[he], b = q->fv[4 * f + (j + 1) % 4];
            int32_t t = q->tw[he];
            for (int c = 0; c < 3; ++c) {
                ep[he][c] = q->p[a][c] + q->p[b][c] + fp[f][c];
                ep[he][c] += (t >= 0) ? fp[t >> 2][c] : fp[f][c];
                ep[he][c] *= 0.25;
            }
        }
    }

    /* vertex points: (1-3/n) V + (1/n^2) sum_j (4 E_j - F_j). Edge-point
     * slots owned by non-canonical halfedges are never referenced by a face;
     * their stored value is irrelevant. */
    for (int32_t v = 0; v < nv; ++v) {
        int32_t start = -1;
        for (int f = 0; f < nf && start < 0; ++f)
            for (int j = 0; j < 4; ++j)
                if (q->fv[4 * f + j] == v) { start = 4 * f + j; break; }
        if (start < 0) {
            np[v][0] = np[v][1] = np[v][2] = 0.0;
            continue;
        }
        double acc[3] = {0, 0, 0};
        int n = 0;
        int32_t h = start;
        do {
            for (int c = 0; c < 3; ++c)
                acc[c] += 4.0 * ep[h][c] - fp[h >> 2][c];
            ++n;
            int32_t t = q->tw[h];
            if (t < 0) break;
            h = (t & ~3) | ((t + 1) & 3);
        } while (h != start);
        double w = 1.0 / (double)n;
        for (int c = 0; c < 3; ++c)
            np[v][c] = (1.0 - 3.0 * w) * q->p[v][c] + w * w * acc[c];
    }

    /* child face j of f: [V'_j, E'_j, F', E'_{j-1}]. Edge points are
     * shared across an edge: the canonical halfedge min(h,twin(h)) owns
     * the edge-point id. */
    for (int f = 0; f < nf; ++f) {
        int32_t fid = nv + f;
        for (int j = 0; j < 4; ++j) {
            int32_t he_j = 4 * f + j;
            int32_t he_p = 4 * f + (j + 3) % 4;
            int32_t can_j = he_j;
            int32_t can_p = he_p;
            if (q->tw[he_j] >= 0 && q->tw[he_j] < can_j) can_j = q->tw[he_j];
            if (q->tw[he_p] >= 0 && q->tw[he_p] < can_p) can_p = q->tw[he_p];
            int32_t eid_j = nv + nf + can_j;
            int32_t eid_p = nv + nf + can_p;
            int32_t *cf = &nfv[16 * f + 4 * j];
            cf[0] = q->fv[4 * f + j]; /* slot reused: new V keeps its id */
            cf[1] = eid_j;
            cf[2] = fid;
            cf[3] = eid_p;
        }
    }
    int nnv = nv + nf + 4 * nf;
    double (*npp)[3] = malloc(sizeof(double[3]) * (size_t)nnv);
    int32_t *nval = (int32_t *)malloc(sizeof(int32_t) * (size_t)nnv);
    if (!npp || !nval) { free(npp); free(nval); free(fp); free(ep); free(np); free(nfv); return 0; }
    memcpy(npp, np, sizeof(double[3]) * (size_t)nv);
    memcpy(&npp[nv], fp, sizeof(double[3]) * (size_t)nf);
    memcpy(&npp[nv + nf], ep, sizeof(double[3]) * (size_t)nf * 4u);
    for (int32_t v = 0; v < nv; ++v) nval[v] = q->val[v]; /* image keeps valence */
    for (int32_t v = nv; v < nnv; ++v) nval[v] = 4;      /* edge/face pts regular */
    free(np); free(fp); free(ep);

    int32_t *ntw = (int32_t *)malloc(sizeof(int32_t) * (size_t)nf * 16u);
    if (!ntw) { free(npp); free(nval); free(nfv); return 0; }
    qn->nf = 4 * nf; qn->nv = nnv;
    qn->p = npp; qn->fv = nfv; qn->tw = ntw; qn->val = nval;
    ccl_edgemap_fill_twins(qn);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Bicubic uniform B-spline patch evaluation                          */
/* ------------------------------------------------------------------ */

static double ccl_bspline_b(int i, double t)
{
    double t2 = t * t, t3 = t2 * t;
    switch (i) {
        case 0: { double z = 1.0 - t; return z * z * z / 6.0; }
        case 1: return (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0;
        case 2: return (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0;
        default:return t3 / 6.0;
    }
}

static double ccl_bspline_db(int i, double t)
{
    double t2 = t * t;
    switch (i) {
        case 0: return -(1.0 - t) * (1.0 - t) * 0.5;
        case 1: return (3.0 * t2 - 4.0 * t) * 0.5;
        case 2: return (-3.0 * t2 + 2.0 * t + 1.0) * 0.5;
        default:return t2 * 0.5;
    }
}

/*
 * Build the 4x4 B-spline control grid of face f whose four corners are
 * regular vertices. Grid indices g[it][is] with is,it in 0..3 mapping to
 * parameter (s,t) = (is-1, it-1); g[1][1],g[1][2],g[2][2],g[2][1] are the
 * face corners c0,c1,c2,c3.
 */
static int ccl_build_grid(const ccl_Patch *q, int f, int32_t g[4][4])
{
    const int32_t c0 = q->fv[4 * f + 0];
    const int32_t c1 = q->fv[4 * f + 1];
    const int32_t c2 = q->fv[4 * f + 2];
    const int32_t c3 = q->fv[4 * f + 3];
    g[1][1] = c0; g[1][2] = c1; g[2][2] = c2; g[2][1] = c3;

    int32_t t, tj, oj, oj1;

    /* Across edge c_j -> c_{j+1}, the twin face's directed edge t runs
     * reversed. Its two vertices not on the shared edge are:
     *   o_j  = slot tj+2 (adjacent to c_j),
     *   o_j1 = slot tj+3 (adjacent to c_{j+1}). */
#define CCL_OUT()                                                          \
    do {                                                                   \
        t = q->tw[4 * f + edge];                                           \
        if (t < 0) return 0;                                               \
        tj = t & 3;                                                        \
        oj  = q->fv[(t & ~3) | ((tj + 2) & 3)];                           \
        oj1 = q->fv[(t & ~3) | ((tj + 3) & 3)];                           \
    } while (0)

    int edge = 0;
    CCL_OUT(); g[0][1] = oj;  g[0][2] = oj1;   /* across c0-c1 */
    edge = 1;
    CCL_OUT(); g[1][3] = oj;  g[2][3] = oj1;   /* across c1-c2 */
    edge = 2;
    CCL_OUT(); g[3][2] = oj;  g[3][1] = oj1;   /* across c2-c3 */
    edge = 3;
    CCL_OUT(); g[2][0] = oj;  g[1][0] = oj1;   /* across c3-c0 */
#undef CCL_OUT

    /* Diagonal 2-ring points. Around corner c_j the four incident faces are:
     * seed, an edge-neighbour, the diagonal face, the other edge-neighbour.
     * Two (twin,next) rotations from the seed halfedge leaving c_j reach the
     * diagonal face; its vertex opposite c_j is the diagonal grid point. */
    static const int diag_slot[4] = {
        (0 * 4 + 0), (0 * 4 + 3), (3 * 4 + 3), (3 * 4 + 0)
    };
    for (int j = 0; j < 4; ++j) {
        int32_t h = 4 * f + j;            /* seed halfedge leaving c_j */
        for (int hop = 0; hop < 2; ++hop) {
            int32_t tw = q->tw[h];
            if (tw < 0) return 0;
            h = (tw & ~3) | ((tw + 1) & 3);
        }
        g[diag_slot[j] >> 2][diag_slot[j] & 3] =
            q->fv[(h & ~3) | (((h & 3) + 2) & 3)];
    }
    return 1;
}

static void ccl_eval_bspline(const ccl_Patch *q, const int32_t g[4][4],
                             double u, double v,
                             double pos[3], double du[3], double dv[3])
{
    double bu[4], bv[4], dbu[4], dbv[4];
    for (int i = 0; i < 4; ++i) {
        bu[i] = ccl_bspline_b(i, u);
        bv[i] = ccl_bspline_b(i, v);
        dbu[i] = ccl_bspline_db(i, u);
        dbv[i] = ccl_bspline_db(i, v);
    }
    for (int c = 0; c < 3; ++c) {
        pos[c] = 0.0; du[c] = 0.0; dv[c] = 0.0;
    }
    for (int is = 0; is < 4; ++is) {
        for (int it = 0; it < 4; ++it) {
            const double *x = q->p[g[it][is]];
            double w  = bu[is] * bv[it];
            double wu = dbu[is] * bv[it];
            double wv = bu[is] * dbv[it];
            for (int c = 0; c < 3; ++c) {
                pos[c] += w * x[c];
                du[c] += wu * x[c];
                dv[c] += wv * x[c];
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Extraordinary corner limit position and tangent plane              */
/* ------------------------------------------------------------------ */

static int ccl_corner_limit(const cc_LimitSurface *s, int32_t f, int corner,
                            double pos[3], double normal[3])
{
    int32_t v = s->fv[4 * f + corner];
    int n = s->vface_off[v + 1] - s->vface_off[v];
    if (n < 3) return 0;

    /* Ordered ring of directed halfedges leaving v, starting inside f. */
    int32_t *ring_he = (int32_t *)malloc(sizeof(int32_t) * (size_t)n);
    int32_t *ring_nv = (int32_t *)malloc(sizeof(int32_t) * (size_t)n);
    if (!ring_he || !ring_nv) { free(ring_he); free(ring_nv); return 0; }

    int32_t h0 = -1;
    for (int j = 0; j < 4; ++j)
        if (s->fv[4 * f + j] == v) h0 = 4 * f + j;
    if (h0 < 0) { free(ring_he); free(ring_nv); return 0; }

    int32_t h = h0;
    for (int k = 0; k < n; ++k) {
        ring_he[k] = h;
        int32_t gf = h >> 2, gj = h & 3;
        ring_nv[k] = s->fv[4 * gf + ((gj + 1) & 3)];
        int32_t nfid = s->nbr[h];
        if (nfid < 0) { free(ring_he); free(ring_nv); return 0; }
        int32_t tj = -1;
        for (int j = 0; j < 4; ++j) {
            int32_t a = s->fv[4 * nfid + j];
            int32_t b = s->fv[4 * nfid + (j + 1) % 4];
            if (b == s->fv[4 * gf + gj] &&
                a == s->fv[4 * gf + (gj + 1) % 4]) { tj = j; break; }
        }
        if (tj < 0) { free(ring_he); free(ring_nv); return 0; }
        h = 4 * nfid + ((tj + 1) % 4);
    }
    if (h != h0) { free(ring_he); free(ring_nv); return 0; }

    /*
     * Catmull-Clark eigen-limit mask (left eigenvector of the local
     * subdivision matrix, verified numerically for valences 3..12):
     *   V_inf = (n-1)/(n+5)            V
     *         + 2 / (n (n+5))           sum_j R_j
     *         + 4 / (n (n+5))           sum_j F_j
     * where R_j are the edge-neighbour vertices and F_j the centroids of
     * the n wedge faces around v. (The plain vertex rule only gives the
     * depth-1 image, not the limit.)
     */
    double sr[3] = {0, 0, 0}, sf[3] = {0, 0, 0};
    for (int k = 0; k < n; ++k) {
        int32_t fh = ring_he[k];
        int32_t ff = fh >> 2;
        double fc[3];
        for (int c = 0; c < 3; ++c) fc[c] = 0.0;
        for (int j = 0; j < 4; ++j)
            for (int c = 0; c < 3; ++c)
                fc[c] += s->p[s->fv[4 * ff + j]][c];
        for (int c = 0; c < 3; ++c) {
            fc[c] *= 0.25;
            sr[c] += s->p[ring_nv[k]][c];
            sf[c] += fc[c];
        }
    }
    double wv = (double)(n - 1) / (double)(n + 5);
    double wr = 2.0 / ((double)n * (double)(n + 5));
    double wf = 4.0 / ((double)n * (double)(n + 5));
    for (int c = 0; c < 3; ++c)
        pos[c] = wv * s->p[v][c] + wr * sr[c] + wf * sf[c];

    /* Tangent eigenvectors of the ring deltas. */
    double t1[3] = {0, 0, 0}, t2[3] = {0, 0, 0};
    for (int k = 0; k < n; ++k) {
        double d[3]; ccl_sub3(d, s->p[ring_nv[k]], s->p[v]);
        double ang = 2.0 * 3.14159265358979323846 * (double)k / (double)n;
        double ca = cos(ang), sa = sin(ang);
        for (int c = 0; c < 3; ++c) {
            t1[c] += ca * d[c];
            t2[c] += sa * d[c];
        }
    }
    double nrm[3]; ccl_cross3(nrm, t1, t2);
    double len = sqrt(ccl_dot3(nrm, nrm));
    if (!(len > 0.0)) {
        double e1[3], e2[3], fn[3];
        ccl_sub3(e1, s->p[s->fv[4 * f + 1]], s->p[s->fv[4 * f + 0]]);
        ccl_sub3(e2, s->p[s->fv[4 * f + 3]], s->p[s->fv[4 * f + 0]]);
        ccl_cross3(fn, e1, e2);
        len = sqrt(ccl_dot3(fn, fn));
        if (!(len > 0.0)) { free(ring_he); free(ring_nv); return 0; }
        memcpy(nrm, fn, sizeof(nrm));
    }
    for (int c = 0; c < 3; ++c) nrm[c] /= len;

    /* Orient against the carrying face (c0->c1) x (c0->c3). */
    double e1[3], e2[3], fn[3];
    ccl_sub3(e1, s->p[s->fv[4 * f + 1]], s->p[s->fv[4 * f + 0]]);
    ccl_sub3(e2, s->p[s->fv[4 * f + 3]], s->p[s->fv[4 * f + 0]]);
    ccl_cross3(fn, e1, e2);
    if (ccl_dot3(fn, nrm) < 0.0)
        for (int c = 0; c < 3; ++c) nrm[c] = -nrm[c];
    memcpy(normal, nrm, sizeof(double[3]));

    free(ring_he); free(ring_nv);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public query                                                       */
/* ------------------------------------------------------------------ */

/* Choose the child quadrant containing (lu,lv); report its forward
 * Jacobian J = d(child)/d(parent) (det +4) and the child parameters. */
static int ccl_pick_child(double lu, double lv, double J[2][2],
                          double *nu, double *nv)
{
    if (lu < 0.5 && lv < 0.5) {
        *nu = 2.0 * lu; *nv = 2.0 * lv;
        J[0][0] = 2.0; J[0][1] = 0.0; J[1][0] = 0.0; J[1][1] = 2.0;
        return 0;
    } else if (lu >= 0.5 && lv < 0.5) {
        *nu = 2.0 * lv; *nv = 2.0 * (1.0 - lu);
        J[0][0] = 0.0; J[0][1] = 2.0; J[1][0] = -2.0; J[1][1] = 0.0;
        return 1;
    } else if (lu >= 0.5 && lv >= 0.5) {
        /* child 2 origin is parent corner c2=(1,1): (s,t)=(2(1-u),2(1-v)) */
        *nu = 2.0 * (1.0 - lu); *nv = 2.0 * (1.0 - lv);
        J[0][0] = -2.0; J[0][1] = 0.0; J[1][0] = 0.0; J[1][1] = -2.0;
        return 2;
    } else {
        *nu = 2.0 * (1.0 - lv); *nv = 2.0 * lu;
        J[0][0] = 0.0; J[0][1] = -2.0; J[1][0] = 2.0; J[1][1] = 0.0;
        return 3;
    }
}

/*
 * A face is a true uniform bicubic B-spline patch only when its entire 4x4
 * control stencil is present and every stencil vertex is regular (valence
 * 4). Checking just the four face corners is insufficient: after the first
 * subdivision extraordinary vertices are isolated but can still lie in the
 * face's two-ring. Returns the assembled grid in *grid when regular.
 */
static int ccl_regular_stencil(const ccl_Patch *q, int f, int32_t grid[4][4])
{
    if (!ccl_build_grid(q, f, grid)) return 0;
    for (int it = 0; it < 4; ++it)
        for (int is = 0; is < 4; ++is)
            if (q->val[grid[it][is]] != 4) return 0;
    return 1;
}

int cc_limit_sample(const cc_LimitSurface *s,
                    int32_t face, double u, double v,
                    cc_LimitSample *out)
{
    if (!s || !out) return 1;
    if (face < 0 || face >= s->nf) return 1;
    if (!isfinite(u) || !isfinite(v) || u < 0.0 || u > 1.0 ||
        v < 0.0 || v > 1.0) return 1;

    /* Exact extraordinary corner: limit position and tangent plane only. */
    for (int j = 0; j < 4; ++j) {
        double cu = (j == 1 || j == 2) ? 1.0 : 0.0;
        double cv = (j >= 2) ? 1.0 : 0.0;
        if (u == cu && v == cv && s->extra[s->fv[4 * face + j]]) {
            cc_LimitSample tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (!ccl_corner_limit(s, face, j, tmp.position, tmp.normal))
                return 1;
            tmp.derivatives_defined = 0;
            *out = tmp;
            return 0;
        }
    }

    ccl_Patch cur, nxt, crop;
    memset(&cur, 0, sizeof(cur));
    memset(&nxt, 0, sizeof(nxt));
    memset(&crop, 0, sizeof(crop));

    /*
     * Local subdivision pass. Crops keep a fixed three-ring margin: one
     * subdivision contracts face neighbourhoods by a factor of two, so after
     * each refine + re-crop every Catmull-Clark input read by the carrying
     * face's stencil stays exact; crop-border fallback values only ever sit
     * on faces that are discarded. The loop terminates as soon as the
     * carrying face's full 4x4 stencil is regular.
     */
    int local_face;
    if (!ccl_crop_face(s, face, &cur, &local_face, 3)) {
        ccl_free_patch(&cur);
        return 1;
    }

    /*
     * M maps derivatives of the original (u,v) chart into the current
     * child chart: d(child) = M d(original), accumulated as M <- J*M where
     * J = d(new child)/d(current). Hence global derivatives are recovered
     * by du_original = M . du_local. Every J has determinant +4, so the
     * orientation is preserved through the whole chain.
     */
    double M[2][2] = {{1.0, 0.0}, {0.0, 1.0}};
    double lu = u, lv = v;
    int status = 1;

    for (int iter = 0; iter < 60; ++iter) {
        int32_t grid[4][4];
        if (ccl_regular_stencil(&cur, local_face, grid)) {
            double pos[3], du_l[3], dv_l[3];
            ccl_eval_bspline(&cur, grid, lu, lv, pos, du_l, dv_l);

            cc_LimitSample tmp;
            memset(&tmp, 0, sizeof(tmp));
            memcpy(tmp.position, pos, sizeof(pos));
            for (int c = 0; c < 3; ++c) {
                tmp.du[c] = M[0][0] * du_l[c] + M[0][1] * dv_l[c];
                tmp.dv[c] = M[1][0] * du_l[c] + M[1][1] * dv_l[c];
            }
            double nrm[3];
            ccl_cross3(nrm, tmp.du, tmp.dv);
            double len = sqrt(ccl_dot3(nrm, nrm));
            if (!(len > 0.0)) goto cleanup;
            for (int c = 0; c < 3; ++c) tmp.normal[c] = nrm[c] / len;
            tmp.derivatives_defined = 1;
            *out = tmp;
            status = 0;
            goto cleanup;
        }

        if (!ccl_refine_patch(&cur, &nxt)) goto cleanup;

        double J[2][2], nu, nv;
        int child = ccl_pick_child(lu, lv, J, &nu, &nv);
        double nM[2][2];
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 2; ++c)
                nM[r][c] = J[r][0] * M[0][c] + J[r][1] * M[1][c];
        memcpy(M, nM, sizeof(M));

        /* locate the chosen child face in the refined patch */
        int32_t child_verts[4];
        memcpy(child_verts, &nxt.fv[16 * local_face + 4 * child],
               sizeof(child_verts));
        int found = -1;
        for (int f = 0; f < nxt.nf && found < 0; ++f) {
            int eq = 1;
            for (int j = 0; j < 4; ++j)
                if (nxt.fv[4 * f + j] != child_verts[j]) { eq = 0; break; }
            if (eq) found = f;
        }
        if (found < 0) goto cleanup;

        if (!ccl_crop_patch(&nxt, found, &crop, &local_face, 3))
            goto cleanup;
        ccl_free_patch(&nxt);
        memset(&nxt, 0, sizeof(nxt));
        ccl_free_patch(&cur);
        cur = crop;
        memset(&crop, 0, sizeof(crop));
        lu = nu; lv = nv;
    }
    /* not reached for any valid non-corner parameter */

cleanup:
    ccl_free_patch(&cur);
    ccl_free_patch(&nxt);
    ccl_free_patch(&crop);
    return status;
}

#endif /* CC_LIMIT_IMPLEMENTATION */
