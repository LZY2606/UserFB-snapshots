/*
 * Task test cases for the Bluestein fast path and the workspace APIs.
 * Compiled by support/target.sh together with the library sources.
 * Provides task_cases(); driver.c supplies main() and task_check().
 */

#include "checks.h"
#include "kiss_fft.h"
#include "kiss_fftr.h"
#include "kiss_fftnd.h"
#include "alloc.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_L 3.14159265358979323846264338327950288L
#define GUARD 16
#define GUARD_BYTE 0xA5

static unsigned rng_state = 20261112u;

/* deterministic pseudo-random value in [-1,1) */
static double frand(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (double)(rng_state >> 8) / 8388608.0 - 1.0;
}

static double tol_base(void)
{
    return sizeof(kiss_fft_scalar) == 8 ? 2e-12 : 3e-5;
}

static double cabs_sum(const kiss_fft_cpx *x, int n, int stride)
{
    double s = 0;
    int j;
    for (j = 0; j < n; ++j)
        s += hypot((double)x[(size_t)j * stride].r, (double)x[(size_t)j * stride].i);
    return s;
}

/* independent reference: complex DFT by direct long double summation */
static void ref_cdft(const kiss_fft_cpx *in, int stride, int n, int inverse,
                     double *re, double *im)
{
    int k, j;
    for (k = 0; k < n; ++k) {
        long double sr = 0.0L, si = 0.0L;
        for (j = 0; j < n; ++j) {
            long double a = (long double)(inverse ? 2 : -2) * PI_L
                * ((long double)j * (long double)k) / (long double)n;
            long double c = cosl(a), s = sinl(a);
            long double xr = in[(size_t)j * stride].r;
            long double xi = in[(size_t)j * stride].i;
            sr += xr * c - xi * s;
            si += xr * s + xi * c;
        }
        re[k] = (double)sr;
        im[k] = (double)si;
    }
}

static void check_cpx_spectrum(const char *msg, const kiss_fft_cpx *out,
                               const double *re, const double *im, int n, double insum)
{
    double maxerr = 0;
    int k;
    for (k = 0; k < n; ++k) {
        double e = hypot((double)out[k].r - re[k], (double)out[k].i - im[k]);
        if (e > maxerr)
            maxerr = e;
    }
    if (maxerr > tol_base() * (1.0 + insum))
        fprintf(stderr, "spectrum mismatch %s: maxerr=%.9g tol=%.9g\n",
                msg, maxerr, tol_base() * (1.0 + insum));
    task_check(maxerr <= tol_base() * (1.0 + insum), msg);
}

static void fill_guard(unsigned char *p, int nbytes)
{
    memset(p, GUARD_BYTE, (size_t)nbytes);
}

static int guard_ok(const unsigned char *p, int nbytes)
{
    int i;
    for (i = 0; i < nbytes; ++i)
        if (p[i] != GUARD_BYTE)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* numeric: Bluestein lengths against an independent long double DFT  */
/* ------------------------------------------------------------------ */
static int cases_numeric(void)
{
    static const int lens[] = { 17, 257, 509, 323 /* 17*19 */, 4093 };
    size_t li;

    for (li = 0; li < sizeof(lens) / sizeof(lens[0]); ++li) {
        const int n = lens[li];
        kiss_fft_cpx *x = malloc((size_t)n * sizeof(*x));
        kiss_fft_cpx *xs = malloc((size_t)n * 3 * sizeof(*xs));
        kiss_fft_cpx *out = malloc((size_t)n * sizeof(*out));
        kiss_fft_cpx *back = malloc((size_t)n * sizeof(*back));
        double *rre = malloc((size_t)n * sizeof(*rre));
        double *rim = malloc((size_t)n * sizeof(*rim));
        kiss_fft_cfg fwd = kiss_fft_alloc(n, 0, NULL, NULL);
        kiss_fft_cfg inv = kiss_fft_alloc(n, 1, NULL, NULL);
        size_t wsz, w1, w2;
        double insum;
        int j;

        task_check(x && xs && out && back && rre && rim, "numeric: host buffers");
        task_check(fwd && inv, "numeric: plans created");
        w1 = kiss_fft_work_size(fwd);
        w2 = kiss_fft_work_size(inv);
        wsz = w1 > w2 ? w1 : w2;
        task_check(wsz > 0, "numeric: nonzero work size for prime length");
        {
            void *work = malloc(wsz);
            task_check(work != NULL, "numeric: work buffer");

            insum = 0;
            for (j = 0; j < n; ++j) {
                x[j].r = (kiss_fft_scalar)frand();
                x[j].i = (kiss_fft_scalar)frand();
                xs[(size_t)j * 3] = x[j];
            }
            insum = cabs_sum(x, n, 1);

            /* forward, unit stride, out of place */
            ref_cdft(x, 1, n, 0, rre, rim);
            task_check(kiss_fft_stride_work(fwd, x, out, 1, work, wsz) == 0,
                       "numeric: fwd work call");
            check_cpx_spectrum("numeric: fwd stride1", out, rre, rim, n, insum);

            /* forward, stride 3, same logical input */
            task_check(kiss_fft_stride_work(fwd, xs, out, 3, work, wsz) == 0,
                       "numeric: fwd stride3 call");
            check_cpx_spectrum("numeric: fwd stride3", out, rre, rim, n, insum);

            /* forward, in place */
            memcpy(back, x, (size_t)n * sizeof(*x));
            task_check(kiss_fft_stride_work(fwd, back, back, 1, work, wsz) == 0,
                       "numeric: fwd inplace call");
            check_cpx_spectrum("numeric: fwd inplace", back, rre, rim, n, insum);

            /* old entry point must use the same fast kernel */
            kiss_fft(fwd, x, out);
            check_cpx_spectrum("numeric: fwd legacy entry", out, rre, rim, n, insum);

            /* inverse direction */
            ref_cdft(x, 1, n, 1, rre, rim);
            task_check(kiss_fft_stride_work(inv, x, out, 1, work, wsz) == 0,
                       "numeric: inv work call");
            check_cpx_spectrum("numeric: inv stride1", out, rre, rim, n, insum);

            /* round trip: inverse(forward(x)) == n * x */
            task_check(kiss_fft_stride_work(fwd, x, out, 1, work, wsz) == 0,
                       "numeric: roundtrip fwd");
            task_check(kiss_fft_stride_work(inv, out, back, 1, work, wsz) == 0,
                       "numeric: roundtrip inv");
            {
                double maxerr = 0, scale = 0;
                for (j = 0; j < n; ++j) {
                    double e = hypot((double)back[j].r - n * (double)x[j].r,
                                     (double)back[j].i - n * (double)x[j].i);
                    if (e > maxerr)
                        maxerr = e;
                }
                scale = tol_base() * (1.0 + (double)n * insum);
                if (maxerr > scale)
                    fprintf(stderr, "roundtrip mismatch n=%d maxerr=%.9g tol=%.9g\n",
                            n, maxerr, scale);
                task_check(maxerr <= scale, "numeric: roundtrip is n*x");
            }
            free(work);
        }
        free(x); free(xs); free(out); free(back); free(rre); free(rim);
        kiss_fft_free(fwd);
        kiss_fft_free(inv);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* planning: size queries, caller memory, nested plans, sentinels     */
/* ------------------------------------------------------------------ */
static int cases_planning(void)
{
    size_t sz = 0, z;
    unsigned char *block;
    kiss_fft_cfg cfg, cfg2;
    kiss_fft_cpx *in, *out;
    int i;
    const int n = 257;

    /* pure query: no allocation, reports size */
    task_allocs = task_frees = 0;
    cfg = kiss_fft_alloc(n, 0, NULL, &sz);
    task_check(cfg == NULL && sz > 0, "planning: query reports size");
    task_check(task_allocs == 0, "planning: query does not allocate");

    /* the Bluestein plan must account for the convolution machinery:
       chirp table + kernel spectrum + two nested smooth plans */
    {
        size_t sz_smooth = 0;
        int conv = kiss_fft_next_fast_size(2 * n - 1);
        kiss_fft_alloc(256, 0, NULL, &sz_smooth);
        task_check(conv >= 2 * n - 1, "planning: convolution length covers 2n-1");
        task_check(sz >= sz_smooth + (size_t)(2 * conv) * sizeof(kiss_fft_cpx),
                   "planning: nested convolution plans counted in size");
    }

    /* caller supplied memory: whole initialization stays in the block */
    block = malloc(sz + 2 * GUARD);
    task_check(block != NULL, "planning: host block");
    fill_guard(block, GUARD);
    fill_guard(block + GUARD + sz, GUARD);
    z = sz;
    task_allocs = task_frees = 0;
    cfg = kiss_fft_alloc(n, 0, block + GUARD, &z);
    task_check(cfg == (kiss_fft_cfg)(block + GUARD), "planning: returns caller mem");
    task_check(z == sz, "planning: caller mem size matches query");
    task_check(task_allocs == 0, "planning: caller mem init does not allocate");
    task_check(guard_ok(block, GUARD) && guard_ok(block + GUARD + sz, GUARD),
               "planning: sentinels intact after init");

    /* the plan in caller memory computes correctly */
    in = malloc((size_t)n * sizeof(*in));
    out = malloc((size_t)n * sizeof(*out));
    {
        double *rre = malloc((size_t)n * sizeof(*rre));
        double *rim = malloc((size_t)n * sizeof(*rim));
        double insum;
        for (i = 0; i < n; ++i) {
            in[i].r = (kiss_fft_scalar)frand();
            in[i].i = (kiss_fft_scalar)frand();
        }
        insum = cabs_sum(in, n, 1);
        ref_cdft(in, 1, n, 0, rre, rim);
        kiss_fft(cfg, in, out);
        check_cpx_spectrum("planning: caller-mem plan computes", out, rre, rim, n, insum);
        free(rre); free(rim);
    }
    task_check(guard_ok(block, GUARD) && guard_ok(block + GUARD + sz, GUARD),
               "planning: sentinels intact after execution");

    /* too-small caller buffer: NULL and the required size is reported */
    z = sz - 1;
    cfg2 = kiss_fft_alloc(n, 0, block, &z);
    task_check(cfg2 == NULL && z == sz, "planning: small buffer reports needed size");

    /* invalid lengths: NULL and zero size, no wrap-around */
    z = 777;
    task_check(kiss_fft_alloc(0, 0, NULL, &z) == NULL && z == 0, "planning: nfft 0 rejected");
    z = 777;
    task_check(kiss_fft_alloc(-257, 0, NULL, &z) == NULL && z == 0, "planning: negative nfft rejected");
    z = 777;
    task_check(kiss_fft_alloc(INT_MAX, 0, NULL, &z) == NULL && z == 0,
               "planning: 2n-1 overflow rejected");

    /* malloc'd plan is one block, freed with plain free/kiss_fft_free */
    cfg2 = kiss_fft_alloc(n, 0, NULL, NULL);
    task_check(cfg2 != NULL, "planning: malloc plan");
    kiss_fft_free(cfg2);

    /* tiny sizes still work */
    {
        kiss_fft_cfg c1 = kiss_fft_alloc(1, 0, NULL, NULL);
        kiss_fft_cpx one = { (kiss_fft_scalar)3, (kiss_fft_scalar)-2 }, o1;
        task_check(c1 != NULL, "planning: nfft 1");
        kiss_fft(c1, &one, &o1);
        task_check(o1.r == one.r && o1.i == one.i, "planning: nfft 1 identity");
        kiss_fft_free(c1);
    }

    /* real planning: query, caller mem, invalid */
    {
        size_t szr = 0;
        kiss_fftr_cfg fcfg;
        task_check(kiss_fftr_alloc(514, 0, NULL, &szr) == NULL && szr > 0,
                   "planning: fftr query");
        z = 777;
        task_check(kiss_fftr_alloc(513, 0, NULL, &z) == NULL && z == 0,
                   "planning: odd real length rejected");
        z = 777;
        task_check(kiss_fftr_alloc(0, 0, NULL, &z) == NULL && z == 0,
                   "planning: zero real length rejected");
        z = 777;
        task_check(kiss_fftr_alloc(-4, 0, NULL, &z) == NULL && z == 0,
                   "planning: negative real length rejected");
        {
            unsigned char *rblock = malloc(szr + 2 * GUARD);
            kiss_fft_scalar *tin = malloc(514 * sizeof(*tin));
            kiss_fft_cpx *fout = malloc(258 * sizeof(*fout));
            size_t rsz;
            fill_guard(rblock, GUARD);
            fill_guard(rblock + GUARD + szr, GUARD);
            z = szr;
            task_allocs = task_frees = 0;
            fcfg = kiss_fftr_alloc(514, 0, rblock + GUARD, &z);
            task_check(fcfg == (kiss_fftr_cfg)(rblock + GUARD) && z == szr,
                       "planning: fftr caller mem");
            task_check(task_allocs == 0, "planning: fftr caller mem no alloc");
            for (i = 0; i < 514; ++i)
                tin[i] = (kiss_fft_scalar)frand();
            rsz = kiss_fftr_work_size(fcfg);
            {
                void *rwork = malloc(rsz);
                task_check(kiss_fftr_work(fcfg, tin, fout, rwork, rsz) == 0,
                           "planning: fftr caller-mem plan runs");
                free(rwork);
            }
            task_check(guard_ok(rblock, GUARD) && guard_ok(rblock + GUARD + szr, GUARD),
                       "planning: fftr sentinels intact");
            z = szr - 1;
            task_check(kiss_fftr_alloc(514, 0, rblock, &z) == NULL && z == szr,
                       "planning: fftr small buffer reports size");
            free(rblock); free(tin); free(fout);
        }
    }

    /* multi-dimensional planning: query, caller mem, invalid shapes */
    {
        int dims[2] = { 3, 257 };
        int dims_bad[2] = { 3, 0 };
        int dims_neg[2] = { 3, -257 };
        int dims_big[3] = { 65536, 65536, 65536 };
        size_t sznd = 0;
        kiss_fftnd_cfg ncfg;
        task_check(kiss_fftnd_alloc(dims, 2, 0, NULL, &sznd) == NULL && sznd > 0,
                   "planning: fftnd query");
        z = 777;
        task_check(kiss_fftnd_alloc(NULL, 2, 0, NULL, &z) == NULL && z == 0,
                   "planning: NULL dims rejected");
        z = 777;
        task_check(kiss_fftnd_alloc(dims, 0, 0, NULL, &z) == NULL && z == 0,
                   "planning: zero ndims rejected");
        z = 777;
        task_check(kiss_fftnd_alloc(dims, -2, 0, NULL, &z) == NULL && z == 0,
                   "planning: negative ndims rejected");
        z = 777;
        task_check(kiss_fftnd_alloc(dims_bad, 2, 0, NULL, &z) == NULL && z == 0,
                   "planning: zero dim rejected");
        z = 777;
        task_check(kiss_fftnd_alloc(dims_neg, 2, 0, NULL, &z) == NULL && z == 0,
                   "planning: negative dim rejected");
        z = 777;
        task_check(kiss_fftnd_alloc(dims_big, 3, 0, NULL, &z) == NULL && z == 0,
                   "planning: dim product overflow rejected");
        {
            unsigned char *nblock = malloc(sznd + 2 * GUARD);
            kiss_fft_cpx *nin = malloc(771 * sizeof(*nin));
            kiss_fft_cpx *nout = malloc(771 * sizeof(*nout));
            size_t nsz;
            fill_guard(nblock, GUARD);
            fill_guard(nblock + GUARD + sznd, GUARD);
            z = sznd;
            task_allocs = task_frees = 0;
            ncfg = kiss_fftnd_alloc(dims, 2, 0, nblock + GUARD, &z);
            task_check(ncfg == (kiss_fftnd_cfg)(nblock + GUARD) && z == sznd,
                       "planning: fftnd caller mem");
            task_check(task_allocs == 0, "planning: fftnd caller mem no alloc");
            for (i = 0; i < 771; ++i) {
                nin[i].r = (kiss_fft_scalar)frand();
                nin[i].i = (kiss_fft_scalar)frand();
            }
            nsz = kiss_fftnd_work_size(ncfg);
            {
                void *nwork = malloc(nsz);
                task_check(kiss_fftnd_work(ncfg, nin, nout, nwork, nsz) == 0,
                           "planning: fftnd caller-mem plan runs");
                free(nwork);
            }
            task_check(guard_ok(nblock, GUARD) && guard_ok(nblock + GUARD + sznd, GUARD),
                       "planning: fftnd sentinels intact");
            z = sznd - 1;
            task_check(kiss_fftnd_alloc(dims, 2, 0, nblock, &z) == NULL && z == sznd,
                       "planning: fftnd small buffer reports size");
            free(nblock); free(nin); free(nout);
        }
    }

    free(in); free(out);
    return 1;
}

/* ------------------------------------------------------------------ */
/* workspace: exact sizing, rejection, reuse, in-place, no allocation */
/* ------------------------------------------------------------------ */
static int cases_workspace(void)
{
    const int n = 257;
    kiss_fft_cfg fwd = kiss_fft_alloc(n, 0, NULL, NULL);
    kiss_fft_cfg inv = kiss_fft_alloc(n, 1, NULL, NULL);
    kiss_fft_cfg other = kiss_fft_alloc(323, 0, NULL, NULL);
    kiss_fft_cfg smooth = kiss_fft_alloc(120, 0, NULL, NULL);
    kiss_fft_cfg gen7 = kiss_fft_alloc(49, 0, NULL, NULL);
    size_t sz = kiss_fft_work_size(fwd);
    size_t sz_other = kiss_fft_work_size(other);
    size_t szmax;
    void *work, *work2;
    kiss_fft_cpx *in, *in2, *out, *out2, *out3, *ref;
    int i;

    task_check(fwd && inv && other && smooth && gen7, "workspace: plans");
    task_check(kiss_fft_work_size(NULL) == 0, "workspace: NULL cfg size is 0");
    task_check(sz > 0 && sz % sizeof(kiss_fft_cpx) == 0, "workspace: size sane");
    task_check(sz >= (size_t)(2 * n - 1) * sizeof(kiss_fft_cpx),
               "workspace: size covers convolution length");
    task_check(kiss_fft_work_size(inv) == sz, "workspace: inverse same size");
    task_check(kiss_fft_work_size(smooth) >= (size_t)120 * sizeof(kiss_fft_cpx),
               "workspace: smooth plan in-place size");
    szmax = sz > sz_other ? sz : sz_other;

    in = malloc((size_t)n * sizeof(*in));
    in2 = malloc((size_t)n * sizeof(*in2));
    out = malloc((size_t)n * sizeof(*out));
    out2 = malloc((size_t)n * sizeof(*out2));
    out3 = malloc((size_t)n * sizeof(*out3));
    ref = malloc((size_t)n * sizeof(*ref));
    work = malloc(szmax);
    work2 = malloc(szmax);
    task_check(in && in2 && out && out2 && out3 && ref && work && work2,
               "workspace: host buffers");

    for (i = 0; i < n; ++i) {
        in[i].r = (kiss_fft_scalar)frand();
        in[i].i = (kiss_fft_scalar)frand();
        in2[i].r = (kiss_fft_scalar)frand();
        in2[i].i = (kiss_fft_scalar)frand();
    }

    /* reference results from the legacy entry points */
    kiss_fft(fwd, in, ref);

    /* exact-size workspace reproduces them */
    task_check(kiss_fft_stride_work(fwd, in, out, 1, work, sz) == 0,
               "workspace: exact size call");
    task_check(memcmp(out, ref, (size_t)n * sizeof(*out)) == 0,
               "workspace: exact size matches legacy");

    /* one byte short: refused, output untouched */
    memset(out, 0x5C, (size_t)n * sizeof(*out));
    task_check(kiss_fft_stride_work(fwd, in, out, 1, work, sz - 1) == -1,
               "workspace: short workspace refused");
    {
        unsigned char expect[sizeof(kiss_fft_cpx)];
        memset(expect, 0x5C, sizeof(expect));
        task_check(memcmp(out, expect, sizeof(expect)) == 0,
                   "workspace: output untouched after refusal");
    }

    /* missing workspace / bad arguments: refused, plan stays valid */
    task_check(kiss_fft_stride_work(fwd, in, out, 1, NULL, sz) == -1,
               "workspace: NULL work refused");
    task_check(kiss_fft_stride_work(NULL, in, out, 1, work, sz) == -1,
               "workspace: NULL cfg refused");
    task_check(kiss_fft_stride_work(fwd, NULL, out, 1, work, sz) == -1,
               "workspace: NULL fin refused");
    task_check(kiss_fft_stride_work(fwd, in, NULL, 1, work, sz) == -1,
               "workspace: NULL fout refused");
    task_check(kiss_fft_stride_work(fwd, in, out, 0, work, sz) == -1,
               "workspace: zero stride refused");
    task_check(kiss_fft_stride_work(fwd, in, out, -2, work, sz) == -1,
               "workspace: negative stride refused");
    task_check(kiss_fft_stride_work(fwd, in, out, 1, work, sz) == 0,
               "workspace: plan still valid after refusals");
    task_check(memcmp(out, ref, (size_t)n * sizeof(*out)) == 0,
               "workspace: result valid after refusals");

    /* execution performs no heap allocation at all */
    task_allocs = task_frees = 0;
    task_check(kiss_fft_stride_work(fwd, in, out, 1, work, sz) == 0, "workspace: noalloc fwd");
    task_check(kiss_fft_stride_work(inv, out, out2, 1, work, sz) == 0, "workspace: noalloc inv");
    task_check(kiss_fft_stride_work(gen7, in, out3, 1, work, kiss_fft_work_size(gen7)) == 0,
               "workspace: noalloc generic-radix");
    task_check(task_allocs == 0, "workspace: execution does not allocate");

    /* one plan, two workspaces, interleaved */
    task_check(kiss_fft_stride_work(fwd, in, out, 1, work, sz) == 0, "workspace: interleave a1");
    task_check(kiss_fft_stride_work(fwd, in2, out2, 1, work2, sz) == 0, "workspace: interleave b1");
    task_check(kiss_fft_stride_work(fwd, in, out3, 1, work2, sz) == 0, "workspace: interleave a2");
    task_check(memcmp(out, out3, (size_t)n * sizeof(*out)) == 0,
               "workspace: same plan any workspace");
    task_check(kiss_fft_stride_work(fwd, in2, out3, 1, work, sz) == 0, "workspace: interleave b2");
    task_check(memcmp(out2, out3, (size_t)n * sizeof(*out)) == 0,
               "workspace: workspaces interchangeable");

    /* one workspace, different plans, one after another: no stale leakage */
    {
        kiss_fft_cpx *oin = malloc(323 * sizeof(*oin));
        kiss_fft_cpx *oout = malloc(323 * sizeof(*oout));
        kiss_fft_cpx *oout2 = malloc(323 * sizeof(*oout2));
        kiss_fft_cpx *oref = malloc(323 * sizeof(*oref));
        for (i = 0; i < 323; ++i) {
            oin[i].r = (kiss_fft_scalar)frand();
            oin[i].i = (kiss_fft_scalar)frand();
        }
        kiss_fft(other, oin, oref);
        task_check(kiss_fft_stride_work(other, oin, oout, 1, work, sz_other) == 0,
                   "workspace: 323 in shared work");
        task_check(kiss_fft_stride_work(fwd, in, out, 1, work, sz) == 0,
                   "workspace: 257 after 323");
        task_check(memcmp(out, ref, (size_t)n * sizeof(*out)) == 0,
                   "workspace: no stale data from longer plan");
        task_check(kiss_fft_stride_work(other, oin, oout2, 1, work, sz_other) == 0,
                   "workspace: 323 again");
        task_check(memcmp(oout, oout2, 323 * sizeof(*oout)) == 0 &&
                   memcmp(oout, oref, 323 * sizeof(*oout)) == 0,
                   "workspace: repeatable across reuse");
        free(oin); free(oout); free(oout2); free(oref);
    }

    /* in-place and strided forms */
    memcpy(out2, in, (size_t)n * sizeof(*in));
    task_check(kiss_fft_stride_work(fwd, out2, out2, 1, work, sz) == 0,
               "workspace: in-place call");
    task_check(memcmp(out2, ref, (size_t)n * sizeof(*out)) == 0,
               "workspace: in-place matches");
    {
        kiss_fft_cpx *xs = malloc((size_t)n * 3 * sizeof(*xs));
        kiss_fft_cpx *sref = malloc((size_t)n * sizeof(*sref));
        for (i = 0; i < n; ++i)
            xs[(size_t)i * 3] = in[i];
        kiss_fft_stride(fwd, xs, sref, 3);
        task_check(kiss_fft_stride_work(fwd, xs, out3, 3, work, sz) == 0,
                   "workspace: stride 3 call");
        task_check(memcmp(out3, sref, (size_t)n * sizeof(*out)) == 0,
                   "workspace: stride 3 matches legacy");
        free(xs); free(sref);
    }

    /* smooth and generic-radix plans through the work entry, incl. in-place */
    {
        kiss_fft_cpx *sin = malloc(120 * sizeof(*sin));
        kiss_fft_cpx *sout = malloc(120 * sizeof(*sout));
        kiss_fft_cpx *sref = malloc(120 * sizeof(*sref));
        size_t ssz = kiss_fft_work_size(smooth);
        void *swork = malloc(ssz);
        for (i = 0; i < 120; ++i) {
            sin[i].r = (kiss_fft_scalar)frand();
            sin[i].i = (kiss_fft_scalar)frand();
        }
        kiss_fft(smooth, sin, sref);
        task_check(kiss_fft_stride_work(smooth, sin, sout, 1, swork, ssz) == 0,
                   "workspace: smooth work call");
        task_check(memcmp(sout, sref, 120 * sizeof(*sout)) == 0,
                   "workspace: smooth matches legacy");
        memcpy(sout, sin, 120 * sizeof(*sin));
        task_check(kiss_fft_stride_work(smooth, sout, sout, 1, swork, ssz) == 0,
                   "workspace: smooth in-place");
        task_check(memcmp(sout, sref, 120 * sizeof(*sout)) == 0,
                   "workspace: smooth in-place matches");
        task_check(kiss_fft_stride_work(smooth, sin, sout, 1, swork, ssz - 1) == -1,
                   "workspace: smooth short workspace refused");
        free(sin); free(sout); free(sref); free(swork);
    }

    /* real work entries */
    {
        kiss_fftr_cfg f514 = kiss_fftr_alloc(514, 0, NULL, NULL);
        kiss_fftr_cfg i514 = kiss_fftr_alloc(514, 1, NULL, NULL);
        size_t rszf = kiss_fftr_work_size(f514);
        size_t rszi = kiss_fftr_work_size(i514);
        size_t rsz = rszf > rszi ? rszf : rszi;
        void *rwork = malloc(rsz);
        kiss_fft_scalar *tin = malloc(514 * sizeof(*tin));
        kiss_fft_scalar *back = malloc(514 * sizeof(*back));
        kiss_fft_scalar *inplace = malloc((514 + 2) * sizeof(*inplace));
        kiss_fft_cpx *fout = malloc(258 * sizeof(*fout));
        kiss_fft_cpx *fref = malloc(258 * sizeof(*fref));
        task_check(f514 && i514, "workspace: real plans");
        task_check(kiss_fftr_work_size(NULL) == 0, "workspace: NULL fftr cfg size 0");
        task_check(rszf > 0 && rszi > 0, "workspace: real work sizes");
        for (i = 0; i < 514; ++i)
            tin[i] = (kiss_fft_scalar)frand();
        kiss_fftr(f514, tin, fref);
        task_check(kiss_fftr_work(f514, tin, fout, rwork, rsz) == 0,
                   "workspace: fftr work call");
        task_check(memcmp(fout, fref, 258 * sizeof(*fout)) == 0,
                   "workspace: fftr matches legacy");
        task_check(kiss_fftr_work(f514, tin, fout, rwork, rszf - 1) == -1,
                   "workspace: fftr short workspace refused");
        task_check(kiss_fftr_work(f514, tin, fout, NULL, rszf) == -1,
                   "workspace: fftr NULL work refused");
        task_check(kiss_fftr_work(i514, tin, fout, rwork, rsz) == -1,
                   "workspace: fftr wrong direction refused");
        task_check(kiss_fftri_work(f514, fref, back, rwork, rsz) == -1,
                   "workspace: fftri wrong direction refused");
        task_allocs = task_frees = 0;
        task_check(kiss_fftr_work(f514, tin, fout, rwork, rsz) == 0, "workspace: fftr noalloc");
        task_check(kiss_fftri_work(i514, fref, back, rwork, rsz) == 0, "workspace: fftri noalloc");
        task_check(task_allocs == 0, "workspace: real execution does not allocate");
        {
            double maxerr = 0;
            for (i = 0; i < 514; ++i) {
                double e = fabs((double)back[i] - 514.0 * (double)tin[i]);
                if (e > maxerr)
                    maxerr = e;
            }
            task_check(maxerr <= tol_base() * 514.0 * 2.0,
                       "workspace: real roundtrip is n*x");
        }
        /* full in-place real form: one buffer of nfft+2 scalars */
        memcpy(inplace, tin, 514 * sizeof(*tin));
        task_check(kiss_fftr_work(f514, inplace, (kiss_fft_cpx *)inplace, rwork, rsz) == 0,
                   "workspace: fftr in-place");
        task_check(memcmp(inplace, fref, 258 * sizeof(kiss_fft_cpx)) == 0,
                   "workspace: fftr in-place matches");
        task_check(kiss_fftri_work(i514, (kiss_fft_cpx *)inplace, inplace, rwork, rsz) == 0,
                   "workspace: fftri in-place");
        {
            double maxerr = 0;
            for (i = 0; i < 514; ++i) {
                double e = fabs((double)inplace[i] - 514.0 * (double)tin[i]);
                if (e > maxerr)
                    maxerr = e;
            }
            task_check(maxerr <= tol_base() * 514.0 * 2.0,
                       "workspace: real in-place roundtrip");
        }
        kiss_fftr_free(f514); kiss_fftr_free(i514);
        free(rwork); free(tin); free(back); free(inplace); free(fout); free(fref);
    }

    /* multi-dimensional work entry */
    {
        int dims[2] = { 3, 257 };
        kiss_fftnd_cfg ncfg = kiss_fftnd_alloc(dims, 2, 0, NULL, NULL);
        size_t nsz = kiss_fftnd_work_size(ncfg);
        void *nwork = malloc(nsz);
        kiss_fft_cpx *nin = malloc(771 * sizeof(*nin));
        kiss_fft_cpx *nout = malloc(771 * sizeof(*nout));
        kiss_fft_cpx *nref = malloc(771 * sizeof(*nref));
        task_check(ncfg != NULL, "workspace: nd plan");
        task_check(kiss_fftnd_work_size(NULL) == 0, "workspace: NULL nd cfg size 0");
        task_check(nsz >= 771 * sizeof(kiss_fft_cpx), "workspace: nd size covers tmpbuf");
        for (i = 0; i < 771; ++i) {
            nin[i].r = (kiss_fft_scalar)frand();
            nin[i].i = (kiss_fft_scalar)frand();
        }
        kiss_fftnd(ncfg, nin, nref);
        task_check(kiss_fftnd_work(ncfg, nin, nout, nwork, nsz) == 0,
                   "workspace: nd work call");
        task_check(memcmp(nout, nref, 771 * sizeof(*nout)) == 0,
                   "workspace: nd matches legacy");
        task_check(kiss_fftnd_work(ncfg, nin, nout, nwork, nsz - 1) == -1,
                   "workspace: nd short workspace refused");
        task_check(kiss_fftnd_work(ncfg, nin, nout, NULL, nsz) == -1,
                   "workspace: nd NULL work refused");
        task_check(kiss_fftnd_work(NULL, nin, nout, nwork, nsz) == -1,
                   "workspace: nd NULL cfg refused");
        task_allocs = task_frees = 0;
        task_check(kiss_fftnd_work(ncfg, nin, nout, nwork, nsz) == 0, "workspace: nd noalloc");
        task_check(task_allocs == 0, "workspace: nd execution does not allocate");
        memcpy(nout, nin, 771 * sizeof(*nin));
        task_check(kiss_fftnd_work(ncfg, nout, nout, nwork, nsz) == 0,
                   "workspace: nd in-place");
        task_check(memcmp(nout, nref, 771 * sizeof(*nout)) == 0,
                   "workspace: nd in-place matches");
        kiss_fft_free(ncfg);
        free(nwork); free(nin); free(nout); free(nref);
    }

    kiss_fft_free(fwd); kiss_fft_free(inv); kiss_fft_free(other);
    kiss_fft_free(smooth); kiss_fft_free(gen7);
    free(in); free(in2); free(out); free(out2); free(out3); free(ref);
    free(work); free(work2);
    return 1;
}

/* ------------------------------------------------------------------ */
/* consumers: real 514 and 3x257 chains vs independent references,    */
/* legacy entries alongside, and the generic-butterfly counting hook  */
/* ------------------------------------------------------------------ */
static int cases_consumers(void)
{
    /* --- 514-point real chain vs independent long double DFT --- */
    {
        const int n = 514;
        const int nb = n / 2 + 1;
        kiss_fft_scalar *tin = malloc((size_t)n * sizeof(*tin));
        kiss_fft_scalar *back = malloc((size_t)n * sizeof(*back));
        kiss_fft_cpx *fout = malloc((size_t)nb * sizeof(*fout));
        kiss_fft_cpx *fout_legacy = malloc((size_t)nb * sizeof(*fout_legacy));
        double *rre = malloc((size_t)nb * sizeof(*rre));
        double *rim = malloc((size_t)nb * sizeof(*rim));
        kiss_fftr_cfg fcfg = kiss_fftr_alloc(n, 0, NULL, NULL);
        kiss_fftr_cfg icfg = kiss_fftr_alloc(n, 1, NULL, NULL);
        size_t wszf = kiss_fftr_work_size(fcfg);
        size_t wszi = kiss_fftr_work_size(icfg);
        size_t wsz = wszf > wszi ? wszf : wszi;
        void *work = malloc(wsz);
        double insum = 0, maxerr;
        int k, j;

        task_check(fcfg && icfg, "consumers: real plans");
        for (j = 0; j < n; ++j) {
            tin[j] = (kiss_fft_scalar)(0.6 * frand() + 0.2 * cos(2.0 * 3.141592653589793 * 5 * j / n));
            insum += fabs((double)tin[j]);
        }
        /* independent reference for the packed real spectrum */
        for (k = 0; k < nb; ++k) {
            long double sr = 0.0L, si = 0.0L;
            for (j = 0; j < n; ++j) {
                long double a = -2.0L * PI_L * ((long double)j * (long double)k) / (long double)n;
                sr += (long double)tin[j] * cosl(a);
                si += (long double)tin[j] * sinl(a);
            }
            rre[k] = (double)sr;
            rim[k] = (double)si;
        }
        task_check(kiss_fftr_work(fcfg, tin, fout, work, wsz) == 0, "consumers: fftr work");
        maxerr = 0;
        for (k = 0; k < nb; ++k) {
            double e = hypot((double)fout[k].r - rre[k], (double)fout[k].i - rim[k]);
            if (e > maxerr)
                maxerr = e;
        }
        task_check(maxerr <= tol_base() * (1.0 + insum),
                   "consumers: real 514 spectrum vs reference");
        task_check(fout[0].i == 0 && fout[nb - 1].i == 0,
                   "consumers: DC and Nyquist are real");
        /* legacy entry alongside: identical results */
        kiss_fftr(fcfg, tin, fout_legacy);
        task_check(memcmp(fout, fout_legacy, (size_t)nb * sizeof(*fout)) == 0,
                   "consumers: fftr legacy matches work");
        /* inverse chain: n times the input */
        task_check(kiss_fftri_work(icfg, fout, back, work, wsz) == 0, "consumers: fftri work");
        maxerr = 0;
        for (j = 0; j < n; ++j) {
            double e = fabs((double)back[j] - (double)n * (double)tin[j]);
            if (e > maxerr)
                maxerr = e;
        }
        task_check(maxerr <= tol_base() * (1.0 + (double)n * insum),
                   "consumers: real 514 inverse is n*x");
        free(tin); free(back); free(fout); free(fout_legacy); free(rre); free(rim); free(work);
        kiss_fftr_free(fcfg); kiss_fftr_free(icfg);
    }

    /* --- 3x257 multi-dimensional chain vs independent 2D DFT --- */
    {
        int dims[2] = { 3, 257 };
        const int nn = 771;
        kiss_fft_cpx *in = malloc((size_t)nn * sizeof(*in));
        kiss_fft_cpx *out = malloc((size_t)nn * sizeof(*out));
        kiss_fft_cpx *out_legacy = malloc((size_t)nn * sizeof(*out_legacy));
        kiss_fft_cpx *back = malloc((size_t)nn * sizeof(*back));
        double *rre = malloc((size_t)nn * sizeof(*rre));
        double *rim = malloc((size_t)nn * sizeof(*rim));
        kiss_fftnd_cfg ncfg = kiss_fftnd_alloc(dims, 2, 0, NULL, NULL);
        kiss_fftnd_cfg nicfg = kiss_fftnd_alloc(dims, 2, 1, NULL, NULL);
        size_t wsz = kiss_fftnd_work_size(ncfg);
        size_t wszi = kiss_fftnd_work_size(nicfg);
        void *work;
        double insum, maxerr;
        int u, v, i, j;

        if (wszi > wsz)
            wsz = wszi;
        work = malloc(wsz);
        task_check(ncfg && nicfg, "consumers: nd plans");
        insum = 0;
        for (i = 0; i < nn; ++i) {
            in[i].r = (kiss_fft_scalar)frand();
            in[i].i = (kiss_fft_scalar)frand();
        }
        insum = cabs_sum(in, nn, 1);
        /* independent 2D reference, original dimension order */
        for (u = 0; u < 3; ++u) {
            for (v = 0; v < 257; ++v) {
                long double sr = 0.0L, si = 0.0L;
                for (i = 0; i < 3; ++i) {
                    for (j = 0; j < 257; ++j) {
                        long double a = -2.0L * PI_L *
                            ((long double)u * i / 3.0L + (long double)v * j / 257.0L);
                        long double c = cosl(a), s = sinl(a);
                        long double xr = in[i * 257 + j].r;
                        long double xi = in[i * 257 + j].i;
                        sr += xr * c - xi * s;
                        si += xr * s + xi * c;
                    }
                }
                rre[u * 257 + v] = (double)sr;
                rim[u * 257 + v] = (double)si;
            }
        }
        task_check(kiss_fftnd_work(ncfg, in, out, work, wsz) == 0, "consumers: nd work");
        check_cpx_spectrum("consumers: 3x257 spectrum vs reference", out, rre, rim, nn, insum);
        kiss_fftnd(ncfg, in, out_legacy);
        task_check(memcmp(out, out_legacy, (size_t)nn * sizeof(*out)) == 0,
                   "consumers: nd legacy matches work");
        /* inverse chain: dimprod times the input */
        task_check(kiss_fftnd_work(nicfg, out, back, work, wsz) == 0, "consumers: nd inverse");
        maxerr = 0;
        for (i = 0; i < nn; ++i) {
            double e = hypot((double)back[i].r - (double)nn * (double)in[i].r,
                             (double)back[i].i - (double)nn * (double)in[i].i);
            if (e > maxerr)
                maxerr = e;
        }
        task_check(maxerr <= tol_base() * (1.0 + (double)nn * insum),
                   "consumers: 3x257 inverse is n*x");
        free(in); free(out); free(out_legacy); free(back); free(rre); free(rim); free(work);
        kiss_fft_free(ncfg); kiss_fft_free(nicfg);
    }

    /* --- counting hook: big prime factors never reach the generic butterfly --- */
    {
        kiss_fft_op_count oc;
        kiss_fft_cfg c257 = kiss_fft_alloc(257, 0, NULL, NULL);
        kiss_fft_cfg c323 = kiss_fft_alloc(323, 0, NULL, NULL);
        kiss_fft_cfg c49 = kiss_fft_alloc(49, 0, NULL, NULL);
        kiss_fft_cfg c11 = kiss_fft_alloc(11, 0, NULL, NULL);
        kiss_fft_cpx *buf = malloc(771 * sizeof(*buf));
        kiss_fft_cpx *bout = malloc(771 * sizeof(*bout));
        int dims[2] = { 3, 257 };
        kiss_fftnd_cfg nd = kiss_fftnd_alloc(dims, 2, 0, NULL, NULL);
        kiss_fftnd_cfg ndi = kiss_fftnd_alloc(dims, 2, 1, NULL, NULL);
        kiss_fftr_cfg fr = kiss_fftr_alloc(514, 0, NULL, NULL);
        kiss_fftr_cfg ir = kiss_fftr_alloc(514, 1, NULL, NULL);
        kiss_fft_scalar *rt = malloc(514 * sizeof(*rt));
        kiss_fft_cpx *rf = malloc(258 * sizeof(*rf));
        int i;

        for (i = 0; i < 771; ++i) {
            buf[i].r = (kiss_fft_scalar)frand();
            buf[i].i = (kiss_fft_scalar)frand();
        }
        for (i = 0; i < 514; ++i)
            rt[i] = (kiss_fft_scalar)frand();

        kiss_fft_op_count_reset();
        kiss_fft(c257, buf, bout);
        kiss_fft(c323, buf, bout);
        kiss_fftnd(nd, buf, buf);       /* in-place legacy nd on 3x257 subset */
        kiss_fftnd(ndi, buf, bout);
        kiss_fftr(fr, rt, rf);
        kiss_fftri(ir, rf, rt);
        kiss_fft_op_count_get(&oc);
        task_check(oc.generic_calls == 0 && oc.generic_radix_sum == 0 && oc.generic_madds == 0,
                   "consumers: large prime factors left the generic butterfly");

        /* control: radix 7 still exercises the generic path and is counted */
        kiss_fft_op_count_reset();
        kiss_fft(c49, buf, bout);
        kiss_fft_op_count_get(&oc);
        task_check(oc.generic_calls == 8 && oc.generic_radix_sum == 56 &&
                   oc.generic_madds == 588,
                   "consumers: radix-7 generic work counted");

        /* control: radix 11 stays on the classic generic path too */
        kiss_fft_op_count_reset();
        kiss_fft(c11, buf, bout);
        kiss_fft_op_count_get(&oc);
        task_check(oc.generic_calls == 1 && oc.generic_radix_sum == 11 &&
                   oc.generic_madds == 110,
                   "consumers: radix-11 generic work counted");

        kiss_fft_free(c257); kiss_fft_free(c323); kiss_fft_free(c49); kiss_fft_free(c11);
        kiss_fft_free(nd); kiss_fft_free(ndi);
        kiss_fftr_free(fr); kiss_fftr_free(ir);
        free(buf); free(bout); free(rt); free(rf);
    }
    return 1;
}

int task_cases(const char *group)
{
    if (strcmp(group, "numeric") == 0)
        return cases_numeric();
    if (strcmp(group, "planning") == 0)
        return cases_planning();
    if (strcmp(group, "workspace") == 0)
        return cases_workspace();
    if (strcmp(group, "consumers") == 0)
        return cases_consumers();
    return 0;
}
