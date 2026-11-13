/***
 * libccd
 * ---------------------------------
 * Copyright (c)2012 Daniel Fiser <danfis@danfis.cz>
 *
 *
 *  This file is part of libccd.
 *
 *  Distributed under the OSI-approved BSD License (the "License");
 *  see accompanying file BDS-LICENSE for details or see
 *  <http://www.opensource.org/licenses/bsd-license.php>.
 *
 *  This software is distributed WITHOUT ANY WARRANTY; without even the
 *  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *  See the License for more information.
 */

#include <math.h>
#include <ccd/ccd.h>
#include <ccd/vec3.h>
#include "support.h"
#include "dbg.h"


/**
 * Context of a translation sweep. The original objects and the original
 * ccd_t structure are only read, never modified: the swept support points
 * are obtained by calling the user supplied support mapping with the
 * original object pointer and adding the time dependent translation to the
 * returned point.
 */
struct _ccd_cast_ctx_t {
    const void *obj1;
    const void *obj2;
    const ccd_t *ccd;
    const ccd_vec3_t *delta1;
    const ccd_vec3_t *delta2;
    ccd_real_t t;
};
typedef struct _ccd_cast_ctx_t ccd_cast_ctx_t;


/** Support mapping of obj1 swept to time ctx->t. */
static void castSupport1(const void *obj, const ccd_vec3_t *dir,
                         ccd_vec3_t *out)
{
    const ccd_cast_ctx_t *ctx = (const ccd_cast_ctx_t *)obj;
    ccd_real_t t = ctx->t;

    ctx->ccd->support1(ctx->obj1, dir, out);
    out->v[0] += t * ctx->delta1->v[0];
    out->v[1] += t * ctx->delta1->v[1];
    out->v[2] += t * ctx->delta1->v[2];
}

/** Support mapping of obj2 swept to time ctx->t. */
static void castSupport2(const void *obj, const ccd_vec3_t *dir,
                         ccd_vec3_t *out)
{
    const ccd_cast_ctx_t *ctx = (const ccd_cast_ctx_t *)obj;
    ccd_real_t t = ctx->t;

    ctx->ccd->support2(ctx->obj2, dir, out);
    out->v[0] += t * ctx->delta2->v[0];
    out->v[1] += t * ctx->delta2->v[1];
    out->v[2] += t * ctx->delta2->v[2];
}

/** Minkowski difference support (obj1 swept minus obj2 swept) at ctx->t. */
static void castSupport(const ccd_cast_ctx_t *ctx, const ccd_vec3_t *dir,
                        ccd_support_t *supp)
{
    ccd_vec3_t d;

    ccdVec3Copy(&d, dir);
    castSupport1(ctx, &d, &supp->v1);

    ccdVec3Scale(&d, -CCD_ONE);
    castSupport2(ctx, &d, &supp->v2);

    ccdVec3Sub2(&supp->v, &supp->v1, &supp->v2);
}

static int ccdIsFinite(ccd_real_t x)
{
    return x == x && x < CCD_REAL_MAX && x > -CCD_REAL_MAX;
}

/** Relative pivot tolerance for the small Gram-system solver. */
#define CAST_PIVOT_TOL CCD_REAL(1e-10)

/** Absolute slack for barycentric weights to count as non-negative. */
#ifdef CCD_SINGLE
# define CAST_FEAS_TOL CCD_REAL(1e-6)
#else
# define CAST_FEAS_TOL CCD_REAL(1e-9)
#endif

/**
 * Solves a small linear system (n <= 5) using Gauss-Jordan elimination with
 * partial pivoting. Returns 0 on success, -1 if the matrix is (numerically)
 * singular.
 */
static int solveLinear(ccd_real_t *a, ccd_real_t *b, int n)
{
    ccd_real_t pivot, maxv, scale, factor, tmp;
    int i, j, k, row;

    for (i = 0; i < n; i++){
        // partial pivoting
        row = i;
        maxv = CCD_FABS(a[i * n + i]);
        for (k = i + 1; k < n; k++){
            if (CCD_FABS(a[k * n + i]) > maxv){
                maxv = CCD_FABS(a[k * n + i]);
                row = k;
            }
        }

        // relative pivot tolerance, robust to a zero-sized matrix
        scale = CCD_ZERO;
        for (k = 0; k < n; k++){
            for (j = 0; j < n; j++){
                if (CCD_FABS(a[k * n + j]) > scale)
                    scale = CCD_FABS(a[k * n + j]);
            }
        }
        if (maxv <= CAST_PIVOT_TOL * (scale + CCD_ONE))
            return -1;

        if (row != i){
            for (j = 0; j < n; j++){
                tmp = a[i * n + j];
                a[i * n + j] = a[row * n + j];
                a[row * n + j] = tmp;
            }
            tmp = b[i];
            b[i] = b[row];
            b[row] = tmp;
        }

        pivot = a[i * n + i];
        for (k = 0; k < n; k++){
            if (k == i)
                continue;
            factor = a[k * n + i] / pivot;
            if (factor == CCD_ZERO)
                continue;
            for (j = i; j < n; j++)
                a[k * n + j] -= factor * a[i * n + j];
            b[k] -= factor * b[i];
        }
    }

    for (i = 0; i < n; i++)
        b[i] /= a[i * n + i];

    return 0;
}

/**
 * Finds the closest point of the convex hull of a simplex (at most four
 * Minkowski vertices) to the origin. All non-empty subsets of the simplex
 * are tested: for each subset the barycentric coordinates of the closest
 * point of its affine hull are solved from the Gram system, and the subset
 * is feasible only if every coordinate is non-negative. Enumerating the
 * subsets makes duplicate support points and degenerate (segment/triangle)
 * simplices harmless - an affinely dependent subset is singular and simply
 * skipped, its hull being covered by a smaller subset.
 *
 * Returns the number of vertices of the winning subset; idx[] receives
 * their positions in the input simplex, lambda[] their barycentric weights
 * and closest the closest point (= sum lambda_i * v_i).
 */
static int closestOnSimplex(const ccd_support_t *simplex, int n,
                            int *idx, ccd_real_t *lambda, ccd_vec3_t *closest)
{
    ccd_real_t best_dist2;
    int best_mask, best_k;
    int mask, i, j, k;

    best_dist2 = CCD_REAL_MAX;
    best_mask = 1;
    best_k = 1;

    for (mask = 1; mask < (1 << n); mask++){
        ccd_real_t m[5 * 5];
        ccd_real_t rhs[5];
        ccd_real_t sol[5];
        ccd_vec3_t pts[4];
        ccd_real_t dist2, neg;
        int ids[4];

        k = 0;
        for (i = 0; i < n; i++){
            if (mask & (1 << i)){
                ids[k] = i;
                ccdVec3Copy(&pts[k], &simplex[i].v);
                k++;
            }
        }

        // System: [G 1] [lambda] = [0]
        //         [1 0] [ mu  ]   [1]
        // with G the Gram matrix of the subset vertices.
        for (i = 0; i < k; i++){
            for (j = 0; j < k; j++)
                m[i * (k + 1) + j] = ccdVec3Dot(&pts[i], &pts[j]);
            m[i * (k + 1) + k] = CCD_ONE;
        }
        for (j = 0; j < k; j++)
            m[k * (k + 1) + j] = CCD_ONE;
        m[k * (k + 1) + k] = CCD_ZERO;

        for (i = 0; i < k; i++)
            rhs[i] = CCD_ZERO;
        rhs[k] = CCD_ONE;

        if (solveLinear(m, rhs, k + 1) != 0)
            continue;

        for (i = 0; i < k; i++)
            sol[i] = rhs[i];

        // feasibility: barycentric weights must be non-negative
        neg = CCD_ZERO;
        for (i = 0; i < k; i++){
            if (sol[i] < CCD_ZERO)
                neg -= sol[i];
        }
        if (neg > CAST_FEAS_TOL)
            continue;

        dist2 = CCD_ZERO;
        for (i = 0; i < k; i++)
            for (j = 0; j < k; j++)
                dist2 += sol[i] * sol[j] * ccdVec3Dot(&pts[i], &pts[j]);
        if (dist2 < CCD_ZERO)
            dist2 = CCD_ZERO;

        if (dist2 < best_dist2){
            best_dist2 = dist2;
            best_mask = mask;
            best_k = k;
            for (i = 0; i < k; i++)
                lambda[i] = sol[i];
        }
    }

    k = 0;
    ccdVec3Set(closest, CCD_ZERO, CCD_ZERO, CCD_ZERO);
    for (i = 0; i < n; i++){
        if (best_mask & (1 << i)){
            ccd_vec3_t w;
            idx[k] = i;
            ccdVec3Copy(&w, &simplex[i].v);
            ccdVec3Scale(&w, lambda[k]);
            ccdVec3Add(closest, &w);
            k++;
        }
    }

    return best_k;
}

/**
 * GJK closest-point query at a fixed time. On success (return 0) dist is
 * the Euclidean distance between the two swept objects and p1, p2 are the
 * corresponding witness points on object 1 and object 2 in world
 * coordinates. Returns -3 if the iteration limit is reached.
 */
static int gjkDistance(const ccd_cast_ctx_t *ctx,
                       ccd_real_t *dist, ccd_vec3_t *p1, ccd_vec3_t *p2)
{
    ccd_support_t simplex[5];
    ccd_support_t supp;
    ccd_support_t compact[4];
    ccd_vec3_t dir, closest;
    ccd_real_t lambda[4];
    int idx[4];
    int n, k, i, iter;
    ccd_real_t tol, progress, prev_dist2;

    tol = ctx->ccd->dist_tolerance;
    n = 0;

    // initial direction: center difference when available, otherwise the
    // configured first direction
    if (ctx->ccd->center1 && ctx->ccd->center2){
        ccd_vec3_t c1, c2;
        ctx->ccd->center1(ctx->obj1, &c1);
        ctx->ccd->center2(ctx->obj2, &c2);
        ccdVec3Sub2(&dir, &c2, &c1);
        if (ccdIsZero(ccdVec3Len2(&dir)))
            ccdVec3Set(&dir, CCD_ONE, CCD_ZERO, CCD_ZERO);
    }else{
        if (ctx->ccd->first_dir)
            ctx->ccd->first_dir(ctx->obj1, ctx->obj2, &dir);
        else
            ccdVec3Set(&dir, CCD_ONE, CCD_ZERO, CCD_ZERO);
        if (ccdIsZero(ccdVec3Len2(&dir)))
            ccdVec3Set(&dir, CCD_ONE, CCD_ZERO, CCD_ZERO);
    }
    ccdVec3Normalize(&dir);

    castSupport(ctx, &dir, &supp);
    ccdSupportCopy(&simplex[0], &supp);
    n = 1;

    // closest-point distance of the previous iteration; GJK distance is
    // monotonically non-increasing and the iteration stops once it no
    // longer improves by more than the world-space distance tolerance
    prev_dist2 = CCD_REAL_MAX;

    for (iter = 0; iter < 64; iter++){
        ccd_real_t cur_dist2;

        k = closestOnSimplex(simplex, n, idx, lambda, &closest);

        // compact the simplex to the winning subset
        for (i = 0; i < k; i++)
            ccdSupportCopy(&compact[i], &simplex[idx[i]]);
        for (i = 0; i < k; i++)
            ccdSupportCopy(&simplex[i], &compact[i]);
        n = k;

        cur_dist2 = ccdVec3Len2(&closest);

        if (ccdIsZero(cur_dist2)){
            // origin lies in the Minkowski difference: touching/overlap
            ccdVec3Set(p1, CCD_ZERO, CCD_ZERO, CCD_ZERO);
            ccdVec3Set(p2, CCD_ZERO, CCD_ZERO, CCD_ZERO);
            for (i = 0; i < n; i++){
                ccd_vec3_t w1, w2;
                ccdVec3Copy(&w1, &simplex[i].v1);
                ccdVec3Scale(&w1, lambda[i]);
                ccdVec3Add(p1, &w1);
                ccdVec3Copy(&w2, &simplex[i].v2);
                ccdVec3Scale(&w2, lambda[i]);
                ccdVec3Add(p2, &w2);
            }
            *dist = CCD_ZERO;
            return 0;
        }

        // primary stopping condition: the closest point itself stopped
        // improving. Comparing squared distances with a threshold scaled by
        // the current magnitude keeps the test in world-length units.
        if (CCD_SQRT(prev_dist2) - CCD_SQRT(cur_dist2) <= tol){
            break;
        }
        prev_dist2 = cur_dist2;

        // search direction from the closest point towards the origin
        ccdVec3Copy(&dir, &closest);
        ccdVec3Scale(&dir, -CCD_ONE);
        ccdVec3Normalize(&dir);

        castSupport(ctx, &dir, &supp);

        // secondary condition: the support plane is within tol of the
        // closest point, so no further expansion can improve the distance
        progress = ccdVec3Dot(&supp.v, &dir) - ccdVec3Dot(&closest, &dir);
        if (progress <= tol)
            break;

        // a support point already in the simplex cannot expand it further
        for (i = 0; i < n; i++){
            if (ccdVec3Eq(&supp.v, &simplex[i].v)){
                progress = CCD_ZERO;
                break;
            }
        }
        if (progress <= tol)
            break;

        if (n >= 4){
            // a non-enclosing 4-simplex always reduces to a face/edge/
            // vertex subset in closestOnSimplex, so this is unreachable for
            // a converging query; treat it as exhaustion
            return -3;
        }
        ccdSupportCopy(&simplex[n], &supp);
        n++;
    }

    if (iter >= 64)
        return -3;

    // final closest point and witnesses from the compacted simplex
    k = closestOnSimplex(simplex, n, idx, lambda, &closest);
    ccdVec3Set(p1, CCD_ZERO, CCD_ZERO, CCD_ZERO);
    ccdVec3Set(p2, CCD_ZERO, CCD_ZERO, CCD_ZERO);
    for (i = 0; i < k; i++){
        ccd_vec3_t w1, w2;
        ccdVec3Copy(&w1, &simplex[idx[i]].v1);
        ccdVec3Scale(&w1, lambda[i]);
        ccdVec3Add(p1, &w1);
        ccdVec3Copy(&w2, &simplex[idx[i]].v2);
        ccdVec3Scale(&w2, lambda[i]);
        ccdVec3Add(p2, &w2);
    }

    *dist = CCD_SQRT(ccdVec3Len2(&closest));
    return 0;
}

/**
 * One evaluation of the swept distance: the objects are positioned at time
 * t by offsetting the support points of the original callbacks. Besides the
 * distance, the unit separation normal (object 1 towards object 2) and the
 * two witness points in world coordinates are recorded.
 * Returns 0 on success and -3 if the inner GJK did not converge.
 */
static int distanceAt(ccd_cast_ctx_t *ctx, ccd_real_t t,
                      ccd_real_t *d, ccd_vec3_t *n,
                      ccd_vec3_t *p1, ccd_vec3_t *p2)
{
    int ret;

    ctx->t = t;
    ret = gjkDistance(ctx, d, p1, p2);
    if (ret != 0)
        return ret;
    if (!ccdIsFinite(*d))
        return -3;

    ccdVec3Sub2(n, p2, p1);
    if (ccdVec3Len2(n) > CCD_ZERO)
        ccdVec3Normalize(n);
    return 0;
}

/** Golden section ratios and refinement budgets. */
#define CAST_GR CCD_REAL(0.6180339887498948482)   /* (sqrt(5)-1)/2 */
#define CAST_GC CCD_REAL(0.3819660112501051518)   /* 1 - GR */
#define CAST_MIN_ITERS 70
#define CAST_ROOT_ITERS 60

int ccdGJKCast(const void *obj1, const void *obj2, const ccd_t *ccd,
               const ccd_vec3_t *delta1, const ccd_vec3_t *delta2,
               ccd_cast_result_t *result)
{
    ccd_cast_ctx_t ctx;
    ccd_vec3_t rel;
    ccd_real_t rel_speed, tol;
    unsigned long max_evals, evals;
    ccd_real_t lo, hi, x1, x2, d1, d2, dm, tm;
    ccd_vec3_t n1, n2, nm, q1_1, q1_2, q2_1, q2_2, qm1, qm2;
    ccd_vec3_t fallback_normal;
    ccd_real_t d0, t_hit;
    ccd_vec3_t n0, w01, w02;
    int have_fallback_normal, i, ret;

    if (result != NULL){
        result->toi = CCD_ZERO;
        ccdVec3Set(&result->normal, CCD_ZERO, CCD_ZERO, CCD_ZERO);
        ccdVec3Set(&result->point1, CCD_ZERO, CCD_ZERO, CCD_ZERO);
        ccdVec3Set(&result->point2, CCD_ZERO, CCD_ZERO, CCD_ZERO);
    }

    if (obj1 == NULL || obj2 == NULL || ccd == NULL
            || delta1 == NULL || delta2 == NULL || result == NULL
            || ccd->support1 == NULL || ccd->support2 == NULL)
        return -1;

    if (!ccdIsFinite(delta1->v[0]) || !ccdIsFinite(delta1->v[1])
            || !ccdIsFinite(delta1->v[2])
            || !ccdIsFinite(delta2->v[0]) || !ccdIsFinite(delta2->v[1])
            || !ccdIsFinite(delta2->v[2]))
        return -1;

    if (!(ccd->dist_tolerance > CCD_ZERO)
            || !ccdIsFinite(ccd->dist_tolerance))
        return -1;

    tol = ccd->dist_tolerance;

    ccdVec3Sub2(&rel, delta2, delta1);
    rel_speed = CCD_SQRT(ccdVec3Len2(&rel));

    ctx.obj1 = obj1;
    ctx.obj2 = obj2;
    ctx.ccd = ccd;
    ctx.delta1 = delta1;
    ctx.delta2 = delta2;
    ctx.t = CCD_ZERO;

    // budget of distance evaluations; a default is used when
    // ccd->max_iterations is left at its (unsigned long)-1 default. The
    // minimization gets at least two thirds of the budget, the root
    // refinement spends the remainder.
    if (ccd->max_iterations == (unsigned long)-1
            || ccd->max_iterations == 0UL)
        max_evals = 256UL;
    else
        max_evals = ccd->max_iterations;
    evals = 0;

    have_fallback_normal = 0;

    // distance at the beginning of the interval
    ret = distanceAt(&ctx, CCD_ZERO, &d0, &n0, &w01, &w02);
    if (ret != 0)
        return ret;
    evals++;
    if (ccdVec3Len2(&n0) > CCD_ZERO){
        ccdVec3Copy(&fallback_normal, &n0);
        have_fallback_normal = 1;
    }

    // initial intersection or touching contact: dedicated zeroed report,
    // the caller resolves penetration with the static queries
    if (d0 <= tol){
        result->toi = CCD_ZERO;
        ccdVec3Set(&result->normal, CCD_ZERO, CCD_ZERO, CCD_ZERO);
        ccdVec3Set(&result->point1, CCD_ZERO, CCD_ZERO, CCD_ZERO);
        ccdVec3Set(&result->point2, CCD_ZERO, CCD_ZERO, CCD_ZERO);
        return 0;
    }

    // no relative motion: the distance is constant over the whole interval
    if (rel_speed == CCD_ZERO)
        return 1;

    /*
     * The distance d(t) between two convex sets in relative linear motion
     * is a convex function of t, hence unimodal on [0, 1]. Golden section
     * search locates its global minimum deterministically - a short contact
     * window cannot be missed because no time sampling is involved.
     */
    lo = CCD_ZERO;
    hi = CCD_ONE;
    x1 = lo + CAST_GC * (hi - lo);
    x2 = lo + CAST_GR * (hi - lo);

    ret = distanceAt(&ctx, x1, &d1, &n1, &q1_1, &q1_2);
    if (ret != 0)
        return ret;
    evals++;
    ret = distanceAt(&ctx, x2, &d2, &n2, &q2_1, &q2_2);
    if (ret != 0)
        return ret;
    evals++;

    // best evaluation seen so far
    dm = d0;
    tm = CCD_ZERO;
    ccdVec3Copy(&nm, &n0);
    ccdVec3Copy(&qm1, &w01);
    ccdVec3Copy(&qm2, &w02);
    if (d1 < dm){
        dm = d1; tm = x1;
        ccdVec3Copy(&nm, &n1);
        ccdVec3Copy(&qm1, &q1_1);
        ccdVec3Copy(&qm2, &q1_2);
    }
    if (d2 < dm){
        dm = d2; tm = x2;
        ccdVec3Copy(&nm, &n2);
        ccdVec3Copy(&qm1, &q2_1);
        ccdVec3Copy(&qm2, &q2_2);
    }

    for (i = 0; i < CAST_MIN_ITERS; i++){
        if (evals >= max_evals){
            if (dm <= tol)
                break; // a contact was already bracketed
            return -3;
        }

        if (d1 < d2){
            hi = x2;
            x2 = x1; d2 = d1;
            ccdVec3Copy(&n2, &n1);
            ccdVec3Copy(&q2_1, &q1_1);
            ccdVec3Copy(&q2_2, &q1_2);
            x1 = lo + CAST_GC * (hi - lo);
            ret = distanceAt(&ctx, x1, &d1, &n1, &q1_1, &q1_2);
        }else{
            lo = x1;
            x1 = x2; d1 = d2;
            ccdVec3Copy(&n1, &n2);
            ccdVec3Copy(&q1_1, &q2_1);
            ccdVec3Copy(&q1_2, &q2_2);
            x2 = lo + CAST_GR * (hi - lo);
            ret = distanceAt(&ctx, x2, &d2, &n2, &q2_1, &q2_2);
        }
        if (ret != 0)
            return ret;
        evals++;

        if (d1 < dm){
            dm = d1; tm = x1;
            ccdVec3Copy(&nm, &n1);
            ccdVec3Copy(&qm1, &q1_1);
            ccdVec3Copy(&qm2, &q1_2);
        }
        if (d2 < dm){
            dm = d2; tm = x2;
            ccdVec3Copy(&nm, &n2);
            ccdVec3Copy(&qm1, &q2_1);
            ccdVec3Copy(&qm2, &q2_2);
        }

        // below the contact gap further refinement only chases noise
        if (dm <= tol && (hi - lo) * rel_speed <= tol)
            break;
    }

    // the global minimum stays separated: no contact on [0, 1]
    if (dm > tol)
        return 1;

    /*
     * A contact exists. Bisection on [0, tm] finds the first time at which
     * the gap drops to the contact tolerance; d(0) is above the touching
     * tolerance and d(tm) is below the gap tolerance.
     */
    t_hit = tm;
    if (tm > CCD_ZERO){
        ccd_real_t blo = CCD_ZERO, bhi = tm;
        ccd_real_t bd, bt;
        ccd_vec3_t bn, bp1, bp2;

        for (i = 0; i < CAST_ROOT_ITERS; i++){
            if (evals >= max_evals)
                return -3;
            bt = CCD_REAL(0.5) * (blo + bhi);
            ret = distanceAt(&ctx, bt, &bd, &bn, &bp1, &bp2);
            if (ret != 0)
                return ret;
            evals++;
            if (ccdVec3Len2(&bn) > CCD_ZERO){
                ccdVec3Copy(&fallback_normal, &bn);
                have_fallback_normal = 1;
            }
            if (bd <= tol){
                bhi = bt;
                t_hit = bt;
                dm = bd;
                ccdVec3Copy(&nm, &bn);
                ccdVec3Copy(&qm1, &bp1);
                ccdVec3Copy(&qm2, &bp2);
            }else{
                blo = bt;
            }
        }
    }

    // fill the report; witness points already carry each object's own
    // t * delta displacement because the swept support mappings offset
    // v1 and v2 independently
    result->toi = t_hit;
    ccdVec3Copy(&result->point1, &qm1);
    ccdVec3Copy(&result->point2, &qm2);

    if (ccdVec3Len2(&nm) > CCD_ZERO){
        ccdVec3Copy(&result->normal, &nm);
    }else if (have_fallback_normal){
        ccdVec3Copy(&result->normal, &fallback_normal);
    }else{
        // last resort: relative motion direction oriented from object 1
        // towards object 2
        ccdVec3Copy(&result->normal, &rel);
        ccdVec3Scale(&result->normal, -CCD_ONE);
        ccdVec3Normalize(&result->normal);
    }
    return 0;
}
