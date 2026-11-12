/*
 * Regression contract for the reversible 5/3 wavelet path.
 *
 * The production forward transform is checked against the independent
 * 64-bit reference in tests/dwt_reference.c (written from the JPEG2000
 * lifting definitions, not from the OpenJPEG implementation). The inverse
 * is checked by feeding reference-generated coefficients back into the
 * production inverse and requiring the exact original samples. A final
 * group exercises the public encode/decode API, including cp_reduce and
 * opj_set_decode_area() windows across tile boundaries.
 *
 * Every failure reports the direction, decomposition level, subband,
 * global coordinate and the first differing integer. There are no skip
 * branches: a group returns 1 only after every assertion passes.
 */

#define OPJ_SKIP_POISON
#include "opj_includes.h"
#include "tests/dwt_reference.h"
#include "verification/driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Failure reporting and bookkeeping                                  */
/* ------------------------------------------------------------------ */

static const char *band_name(int band)
{
    switch (band) {
    case REF_BAND_LL:
        return "LL";
    case REF_BAND_HL:
        return "HL";
    case REF_BAND_LH:
        return "LH";
    case REF_BAND_HH:
        return "HH";
    default:
        return "??";
    }
}

static int report_fail(const char *direction, int level, const char *band,
                       int64_t gx, int64_t gy, int64_t got, int64_t want)
{
    fprintf(stderr,
            "DWT_FAIL direction=%s level=%d band=%s global=(%lld,%lld) "
            "got=%lld want=%lld\n",
            direction, level, band, (long long)gx, (long long)gy,
            (long long)got, (long long)want);
    return 0;
}

static int ceildivpow2_i32(OPJ_INT32 a, int b)
{
    return (unsigned)((a + ((OPJ_INT64)1 << b) - 1) >> b);
}

/* Independent expectation for a subband rectangle, straight from
 * equation B-15 of the standard. */
static ref_rect64 expected_band_rect(ref_rect64 comp, unsigned numres,
                                     int level, int band)
{
    unsigned nb = numres - (unsigned)level;
    int64_t x0b = band & 1;
    int64_t y0b = band >> 1;
    int64_t ox = x0b * ((int64_t)1 << (nb - 1));
    int64_t oy = y0b * ((int64_t)1 << (nb - 1));
    ref_rect64 out;
    out.x0 = ref_ceil_div_pow2(comp.x0 - ox, nb);
    out.x1 = ref_ceil_div_pow2(comp.x1 - ox, nb);
    out.y0 = ref_ceil_div_pow2(comp.y0 - oy, nb);
    out.y1 = ref_ceil_div_pow2(comp.y1 - oy, nb);
    return out;
}

/* ------------------------------------------------------------------ */
/* Deterministic input patterns, bounded so int32 intermediates of    */
/* four 5/3 levels cannot overflow.                                   */
/* ------------------------------------------------------------------ */

enum { PAT_NEG = 0, PAT_RAMP = 1, PAT_PULSE = 2, PAT_APERIODIC = 3 };

static OPJ_INT32 pattern_sample(int pat, int x, int y, int w, int h)
{
    switch (pat) {
    case PAT_NEG:
        return -(OPJ_INT32)(1 + ((x * 37 + y * 19) % 400));
    case PAT_RAMP:
        return (OPJ_INT32)(x * 11 - y * 13 + (x * y) % 7);
    case PAT_PULSE:
        return (OPJ_INT32)((x == 0 ? 900 : 0) + (x == w - 1 ? -700 : 0) +
                           (y == 0 ? 500 : 0) + (y == h - 1 ? -600 : 0));
    default:
        return (OPJ_INT32)(((int64_t)x * 1103515245 + (int64_t)y * 12345 +
                            (int64_t)x * y * 7919) % 997) - 498;
    }
}

static const int k_pattern_count = 4;

/* ------------------------------------------------------------------ */
/* Production DWT harness over a single tile component                */
/* ------------------------------------------------------------------ */

typedef struct {
    opj_tcd_t tcd;
    opj_tcd_tilecomp_t tile;
    opj_tcd_resolution_t *res;
    OPJ_INT32 *buf;
    OPJ_INT32 *original;
    int w;
    int h;
    unsigned nr;
} prod_dwt_t;

static int prod_open(prod_dwt_t *p, int x0, int y0, int w, int h,
                     unsigned nr, int num_threads)
{
    unsigned r;
    memset(p, 0, sizeof(*p));
    p->w = w;
    p->h = h;
    p->nr = nr;
    p->res = (opj_tcd_resolution_t*)calloc(nr, sizeof(*p->res));
    p->buf = (OPJ_INT32*)malloc((size_t)w * h * sizeof(OPJ_INT32));
    p->original = (OPJ_INT32*)malloc((size_t)w * h * sizeof(OPJ_INT32));
    if (!p->res || !p->buf || !p->original) {
        return 0;
    }
    p->tcd.thread_pool = opj_thread_pool_create(num_threads);
    p->tcd.whole_tile_decoding = OPJ_TRUE;
    if (!p->tcd.thread_pool) {
        return 0;
    }
    p->tile.x0 = x0;
    p->tile.y0 = y0;
    p->tile.x1 = x0 + w;
    p->tile.y1 = y0 + h;
    p->tile.numresolutions = nr;
    p->tile.minimum_num_resolutions = nr;
    p->tile.resolutions = p->res;
    p->tile.data = p->buf;
    p->tile.data_size = (size_t)w * h * sizeof(OPJ_INT32);
    for (r = 0; r < nr; ++r) {
        int shift = (int)(nr - 1 - r);
        p->res[r].x0 = (OPJ_INT32)ceildivpow2_i32(p->tile.x0, shift);
        p->res[r].y0 = (OPJ_INT32)ceildivpow2_i32(p->tile.y0, shift);
        p->res[r].x1 = (OPJ_INT32)ceildivpow2_i32(p->tile.x1, shift);
        p->res[r].y1 = (OPJ_INT32)ceildivpow2_i32(p->tile.y1, shift);
    }
    return 1;
}

static void prod_load_pattern(prod_dwt_t *p, int pat)
{
    int x, y;
    for (y = 0; y < p->h; ++y) {
        for (x = 0; x < p->w; ++x) {
            OPJ_INT32 v = pattern_sample(pat, x, y, p->w, p->h);
            p->buf[y * p->w + x] = v;
            p->original[y * p->w + x] = v;
        }
    }
}

static void prod_close(prod_dwt_t *p)
{
    if (p->tcd.thread_pool) {
        opj_thread_pool_destroy(p->tcd.thread_pool);
    }
    free(p->res);
    free(p->buf);
    free(p->original);
}

/* Compare every coefficient of a production forward transform with the
 * independent reference object. */
static int compare_forward_coefficients(const prod_dwt_t *p,
                                        const ref_dwt64 *rf,
                                        ref_rect64 comp)
{
    int lw0 = p->res[0].x1 - p->res[0].x0;
    int lh0 = p->res[0].y1 - p->res[0].y0;
    int x, y, level;

    if (rf->ll_rect.x0 != p->res[0].x0 || rf->ll_rect.y0 != p->res[0].y0 ||
            rf->ll_rect.x1 != p->res[0].x1 ||
            rf->ll_rect.y1 != p->res[0].y1) {
        return report_fail("forward", 0, "LL", rf->ll_rect.x0,
                           rf->ll_rect.y0, (int64_t)p->res[0].x0,
                           rf->ll_rect.x0);
    }
    for (y = 0; y < lh0; ++y) {
        for (x = 0; x < lw0; ++x) {
            int64_t gx = p->res[0].x0 + x;
            int64_t gy = p->res[0].y0 + y;
            int64_t rv = rf->ll[y * lw0 + x];
            if ((int64_t)p->buf[y * p->w + x] != rv) {
                return report_fail("forward", 0, "LL", gx, gy,
                                   (int64_t)p->buf[y * p->w + x], rv);
            }
        }
    }

    for (level = 1; level < (int)p->nr; ++level) {
        int lw = p->res[level - 1].x1 - p->res[level - 1].x0;
        int lh = p->res[level - 1].y1 - p->res[level - 1].y0;
        const ref_band64 *rb = &rf->bands[(level - 1) * 3];
        int bi;
        for (bi = 0; bi < 3; ++bi) {
            ref_rect64 want = expected_band_rect(comp, p->nr, level,
                                                 bi + 1);
            const ref_band64 *b = &rb[bi];
            int bw = (int)(b->rect.x1 - b->rect.x0);
            int bh = (int)(b->rect.y1 - b->rect.y0);
            int px, py;
            if (want.x0 != b->rect.x0 || want.y0 != b->rect.y0 ||
                    want.x1 != b->rect.x1 || want.y1 != b->rect.y1) {
                return report_fail("forward", level, band_name(bi + 1),
                                   b->rect.x0, b->rect.y0, want.x0,
                                   b->rect.x0);
            }
            for (py = 0; py < bh; ++py) {
                for (px = 0; px < bw; ++px) {
                    int64_t gx = b->rect.x0 + px;
                    int64_t gy = b->rect.y0 + py;
                    int64_t rv = ref_band64_get(b, gx, gy);
                    int local_x, local_y;
                    if (bi == 0) {        /* HL */
                        local_x = lw + px;
                        local_y = py;
                    } else if (bi == 1) { /* LH */
                        local_x = px;
                        local_y = lh + py;
                    } else {              /* HH */
                        local_x = lw + px;
                        local_y = lh + py;
                    }
                    if ((int64_t)p->buf[local_y * p->w + local_x] != rv) {
                        return report_fail("forward", level,
                                           band_name(bi + 1), gx, gy,
                                           (int64_t)p->buf[local_y * p->w +
                                                       local_x], rv);
                    }
                }
            }
        }
    }
    return 1;
}

/* Scatter reference coefficients into the production tile buffer. */
static void scatter_reference_into_tile(prod_dwt_t *p, const ref_dwt64 *rf)
{
    int lw0 = p->res[0].x1 - p->res[0].x0;
    int lh0 = p->res[0].y1 - p->res[0].y0;
    int x, y, level, bi;
    for (y = 0; y < lh0; ++y) {
        for (x = 0; x < lw0; ++x) {
            p->buf[y * p->w + x] = (OPJ_INT32)rf->ll[y * lw0 + x];
        }
    }
    for (level = 1; level < (int)p->nr; ++level) {
        int lw = p->res[level - 1].x1 - p->res[level - 1].x0;
        int lh = p->res[level - 1].y1 - p->res[level - 1].y0;
        const ref_band64 *rb = &rf->bands[(level - 1) * 3];
        for (bi = 0; bi < 3; ++bi) {
            const ref_band64 *b = &rb[bi];
            int bw = (int)(b->rect.x1 - b->rect.x0);
            int bh = (int)(b->rect.y1 - b->rect.y0);
            int px, py;
            for (py = 0; py < bh; ++py) {
                for (px = 0; px < bw; ++px) {
                    int local_x, local_y;
                    if (bi == 0) {
                        local_x = lw + px;
                        local_y = py;
                    } else if (bi == 1) {
                        local_x = px;
                        local_y = lh + py;
                    } else {
                        local_x = lw + px;
                        local_y = lh + py;
                    }
                    p->buf[local_y * p->w + local_x] =
                        (OPJ_INT32)ref_band64_get(b, b->rect.x0 + px,
                                                  b->rect.y0 + py);
                }
            }
        }
    }
}

static int check_inverse_reconstruction(const prod_dwt_t *p)
{
    int x, y;
    for (y = 0; y < p->h; ++y) {
        for (x = 0; x < p->w; ++x) {
            if (p->buf[y * p->w + x] != p->original[y * p->w + x]) {
                return report_fail("inverse", (int)p->nr - 1, "SAMPLE",
                                   (int64_t)(p->tile.x0 + x),
                                   (int64_t)(p->tile.y0 + y),
                                   (int64_t)p->buf[y * p->w + x],
                                   (int64_t)p->original[y * p->w + x]);
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Coverage matrix                                                    */
/* ------------------------------------------------------------------ */

static const int k_origins[4] = {0, 1, 2, 3};

static const int k_shapes[][2] = {
    {1, 1}, {1, 2}, {2, 1}, {1, 3}, {3, 1}, {2, 2},
    {2, 3}, {3, 2}, {3, 3}, {3, 5}, {5, 3}, {5, 5},
    {5, 1}, {1, 5}, {17, 19}, {19, 17}
};
static const int k_shape_count = (int)(sizeof(k_shapes) / sizeof(k_shapes[0]));

/* ------------------------------------------------------------------ */
/* Group 1: production forward coefficients vs the reference         */
/* ------------------------------------------------------------------ */

static int case_coefficient_grid(void)
{
    int si, oi, oj, pat, level;
    unsigned long local_combos = 0;

    for (si = 0; si < k_shape_count; ++si) {
        int w = k_shapes[si][0];
        int h = k_shapes[si][1];
        for (oi = 0; oi < 4; ++oi) {
            for (oj = 0; oj < 4; ++oj) {
                int x0 = k_origins[oi];
                int y0 = k_origins[oj];
                ref_rect64 comp;
                comp.x0 = x0;
                comp.y0 = y0;
                comp.x1 = x0 + w;
                comp.y1 = y0 + h;
                for (level = 1; level <= 5; ++level) {
                    unsigned nr = (unsigned)level;
                    if (nr > ref_max_numres(comp)) {
                        continue;
                    }
                    for (pat = 0; pat < k_pattern_count; ++pat) {
                        prod_dwt_t p;
                        ref_dwt64 *rf;
                        int64_t *img64;
                        int i;
                        ++local_combos;
                        if (!prod_open(&p, x0, y0, w, h, nr, 0)) {
                            return 0;
                        }
                        prod_load_pattern(&p, pat);
                        if (!opj_dwt_encode(&p.tcd, &p.tile)) {
                            prod_close(&p);
                            return 0;
                        }
                        img64 = (int64_t*)malloc((size_t)w * h *
                                                 sizeof(int64_t));
                        if (!img64) {
                            prod_close(&p);
                            return 0;
                        }
                        for (i = 0; i < w * h; ++i) {
                            img64[i] = p.original[i];
                        }
                        rf = ref_dwt64_forward(img64, comp, nr);
                        if (!rf ||
                                !compare_forward_coefficients(&p, rf, comp)) {
                            free(img64);
                            if (rf) {
                                ref_dwt64_destroy(rf);
                            }
                            prod_close(&p);
                            return 0;
                        }
                        ref_dwt64_destroy(rf);
                        free(img64);
                        prod_close(&p);
                    }
                }
            }
        }
    }

    /* Independent expectation for the odd-origin singleton: the lone
     * sample sits at an odd horizontal coordinate, so the forward
     * transform must scale it by two. It must not be treated as an
     * ordinary even-origin passthrough. */
    {
        static const OPJ_INT32 singleton_vals[3] = {17, -31, 1234};
        int vi;
        for (vi = 0; vi < 3; ++vi) {
            prod_dwt_t p;
            ref_dwt64 *rf;
            int64_t img[1] = {singleton_vals[vi]};
            ref_rect64 comp = {1, 0, 2, 1};
            ++local_combos;
            if (!prod_open(&p, 1, 0, 1, 1, 2, 0)) {
                return 0;
            }
            p.buf[0] = singleton_vals[vi];
            p.original[0] = singleton_vals[vi];
            if (!opj_dwt_encode(&p.tcd, &p.tile)) {
                prod_close(&p);
                return 0;
            }
            rf = ref_dwt64_forward(img, comp, 2);
            if (!rf) {
                prod_close(&p);
                return 0;
            }
            if (p.buf[0] != 2 * singleton_vals[vi]) {
                ref_dwt64_destroy(rf);
                prod_close(&p);
                return report_fail("forward", 1, "HL", 0, 0,
                                   (int64_t)p.buf[0],
                                   (int64_t)(2 * singleton_vals[vi]));
            }
            if (ref_band64_get(&rf->bands[0], 0, 0) !=
                    2 * (int64_t)singleton_vals[vi]) {
                ref_dwt64_destroy(rf);
                prod_close(&p);
                return report_fail("forward", 1, "HL", 0, 0,
                                   ref_band64_get(&rf->bands[0], 0, 0),
                                   (int64_t)(2 * singleton_vals[vi]));
            }
            ref_dwt64_destroy(rf);
            prod_close(&p);
        }
    }

    printf("COMBINATIONS coefficient-grid %lu\n", local_combos);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Group 2: reference coefficients through the production inverse     */
/* ------------------------------------------------------------------ */

static int case_inverse_grid(void)
{
    int si, oi, oj, pat, level;
    unsigned long local_combos = 0;

    for (si = 0; si < k_shape_count; ++si) {
        int w = k_shapes[si][0];
        int h = k_shapes[si][1];
        for (oi = 0; oi < 4; ++oi) {
            for (oj = 0; oj < 4; ++oj) {
                int x0 = k_origins[oi];
                int y0 = k_origins[oj];
                ref_rect64 comp;
                comp.x0 = x0;
                comp.y0 = y0;
                comp.x1 = x0 + w;
                comp.y1 = y0 + h;
                for (level = 1; level <= 5; ++level) {
                    unsigned nr = (unsigned)level;
                    if (nr > ref_max_numres(comp)) {
                        continue;
                    }
                    for (pat = 0; pat < k_pattern_count; ++pat) {
                        prod_dwt_t p;
                        ref_dwt64 *rf;
                        int64_t *img64;
                        int i;
                        ++local_combos;
                        if (!prod_open(&p, x0, y0, w, h, nr, 0)) {
                            return 0;
                        }
                        prod_load_pattern(&p, pat);
                        img64 = (int64_t*)malloc((size_t)w * h *
                                                 sizeof(int64_t));
                        if (!img64) {
                            prod_close(&p);
                            return 0;
                        }
                        for (i = 0; i < w * h; ++i) {
                            img64[i] = p.original[i];
                        }
                        rf = ref_dwt64_forward(img64, comp, nr);
                        if (!rf) {
                            free(img64);
                            prod_close(&p);
                            return 0;
                        }
                        /* Reference coefficients, never production ones,
                         * drive the inverse under test. */
                        scatter_reference_into_tile(&p, rf);
                        if (!opj_dwt_decode(&p.tcd, &p.tile, nr) ||
                                !check_inverse_reconstruction(&p)) {
                            ref_dwt64_destroy(rf);
                            free(img64);
                            prod_close(&p);
                            return 0;
                        }
                        ref_dwt64_destroy(rf);
                        free(img64);
                        prod_close(&p);
                    }
                }
            }
        }
    }

    printf("COMBINATIONS inverse-grid %lu\n", local_combos);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Group 3: multilevel geometry on real component coordinates         */
/* ------------------------------------------------------------------ */

static int case_multilevel(void)
{
    static const struct {
        int w, h, x0, y0, nr;
    } cases[] = {
        {5, 5, 1, 2, 3}, {5, 5, 3, 1, 4}, {3, 2, 3, 3, 2},
        {17, 19, 3, 5, 4}, {19, 17, 2, 3, 4}, {2, 3, 1, 1, 2},
        {17, 19, 3, 5, 5}, {19, 17, 2, 3, 5}
    };
    int ci, pat;
    unsigned long local_combos = 0;

    for (ci = 0; ci < (int)(sizeof(cases) / sizeof(cases[0])); ++ci) {
        int w = cases[ci].w;
        int h = cases[ci].h;
        int x0 = cases[ci].x0;
        int y0 = cases[ci].y0;
        unsigned nr = (unsigned)cases[ci].nr;
        ref_rect64 comp;
        comp.x0 = x0;
        comp.y0 = y0;
        comp.x1 = x0 + w;
        comp.y1 = y0 + h;

        for (pat = 0; pat < k_pattern_count; ++pat) {
            prod_dwt_t p;
            ref_dwt64 *rf;
            int64_t *img64, *roundtrip;
            int i, r;
            ++local_combos;
            if (!prod_open(&p, x0, y0, w, h, nr, 0)) {
                return 0;
            }
            prod_load_pattern(&p, pat);
            img64 = (int64_t*)malloc((size_t)w * h * sizeof(int64_t));
            roundtrip = (int64_t*)malloc((size_t)w * h * sizeof(int64_t));
            if (!img64 || !roundtrip) {
                free(img64);
                free(roundtrip);
                prod_close(&p);
                return 0;
            }
            for (i = 0; i < w * h; ++i) {
                img64[i] = p.original[i];
            }
            rf = ref_dwt64_forward(img64, comp, nr);
            if (!rf) {
                free(img64);
                free(roundtrip);
                prod_close(&p);
                return 0;
            }
            if (!opj_dwt_encode(&p.tcd, &p.tile) ||
                    !compare_forward_coefficients(&p, rf, comp)) {
                ref_dwt64_destroy(rf);
                free(img64);
                free(roundtrip);
                prod_close(&p);
                return 0;
            }

            /* Every level keeps the real global component origin: the
             * coarsest rectangle must not silently restart at zero, and
             * each split continues on the previous level's coordinates. */
            for (r = 0; r < (int)nr; ++r) {
                ref_rect64 want = ref_resolution_rect(comp, nr, (unsigned)r);
                if (want.x0 != p.res[r].x0 || want.y0 != p.res[r].y0 ||
                        want.x1 != p.res[r].x1 ||
                        want.y1 != p.res[r].y1) {
                    ref_dwt64_destroy(rf);
                    free(img64);
                    free(roundtrip);
                    prod_close(&p);
                    return report_fail("forward", r, "RES", want.x0, want.y0,
                                       (int64_t)p.res[r].x0, want.x0);
                }
            }
            if (rf->ll_rect.x0 == 0 && (x0 & 1)) {
                ref_dwt64_destroy(rf);
                free(img64);
                free(roundtrip);
                prod_close(&p);
                return report_fail("forward", 0, "LL", rf->ll_rect.x0,
                                   rf->ll_rect.y0, 0, 1);
            }

            /* The reference must also reconstruct itself exactly. */
            if (!ref_dwt64_inverse(rf, roundtrip)) {
                ref_dwt64_destroy(rf);
                free(img64);
                free(roundtrip);
                prod_close(&p);
                return 0;
            }
            for (i = 0; i < w * h; ++i) {
                if (roundtrip[i] != img64[i]) {
                    int gx = x0 + (i % w);
                    int gy = y0 + (i / w);
                    ref_dwt64_destroy(rf);
                    free(img64);
                    free(roundtrip);
                    prod_close(&p);
                    return report_fail("inverse", (int)nr - 1, "SAMPLE",
                                       (int64_t)gx, (int64_t)gy,
                                       roundtrip[i], img64[i]);
                }
            }
            ref_dwt64_destroy(rf);
            free(img64);
            free(roundtrip);
            prod_close(&p);
        }
    }

    printf("COMBINATIONS multilevel %lu\n", local_combos);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public codec integration: in-memory J2K streams                    */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *data;
    size_t cap;
    size_t len;
    size_t pos;
} mem_stream_t;

static OPJ_SIZE_T mem_write(void *buf, OPJ_SIZE_T n, void *user)
{
    mem_stream_t *m = (mem_stream_t*)user;
    if (m->pos + n > m->cap) {
        size_t cap = m->cap ? m->cap : 4096;
        unsigned char *q;
        while (cap < m->pos + n) {
            cap *= 2;
        }
        q = (unsigned char*)realloc(m->data, cap);
        if (!q) {
            return (OPJ_SIZE_T)-1;
        }
        m->data = q;
        m->cap = cap;
    }
    memcpy(m->data + m->pos, buf, n);
    m->pos += n;
    if (m->pos > m->len) {
        m->len = m->pos;
    }
    return n;
}

static OPJ_SIZE_T mem_read(void *buf, OPJ_SIZE_T n, void *user)
{
    mem_stream_t *m = (mem_stream_t*)user;
    size_t avail = m->len - m->pos;
    if (n > avail) {
        n = avail;
    }
    if (n == 0) {
        return (OPJ_SIZE_T)-1;
    }
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}

static OPJ_OFF_T mem_skip(OPJ_OFF_T n, void *user)
{
    mem_stream_t *m = (mem_stream_t*)user;
    m->pos = (size_t)((OPJ_OFF_T)m->pos + n);
    if (m->pos > m->len) {
        m->pos = m->len;
    }
    return n;
}

static OPJ_BOOL mem_seek(OPJ_OFF_T n, void *user)
{
    mem_stream_t *m = (mem_stream_t*)user;
    if ((size_t)n > m->len) {
        return OPJ_FALSE;
    }
    m->pos = (size_t)n;
    return OPJ_TRUE;
}

static void quiet_handler(const char *msg, void *user)
{
    (void)msg;
    (void)user;
}

#define CODEC_NC 2
#define CODEC_NR 3u

static const OPJ_UINT32 k_img_x0 = 5;
static const OPJ_UINT32 k_img_y0 = 3;
static const OPJ_UINT32 k_img_x1 = 21;
static const OPJ_UINT32 k_img_y1 = 15;
static const OPJ_UINT32 k_tile_dx = 8;
static const OPJ_UINT32 k_tile_dy = 6;
static const unsigned k_comp_dx[CODEC_NC] = {1, 2};
static const unsigned k_comp_dy[CODEC_NC] = {1, 2};

typedef struct {
    unsigned cw[CODEC_NC];
    unsigned ch[CODEC_NC];
    unsigned cx0[CODEC_NC];
    unsigned cy0[CODEC_NC];
    OPJ_INT32 *keep[CODEC_NC];
    mem_stream_t stream;
} codec_fixture_t;

static unsigned ceildiv_u(unsigned a, unsigned b)
{
    return (a + b - 1) / b;
}

static int build_codestream(codec_fixture_t *fx)
{
    opj_image_cmptparm_t cmpt[CODEC_NC];
    opj_image_t *image;
    opj_cparameters_t param;
    opj_codec_t *codec;
    opj_stream_t *stream;
    unsigned c;

    memset(fx, 0, sizeof(*fx));
    memset(cmpt, 0, sizeof(cmpt));
    image = opj_image_tile_create(CODEC_NC, cmpt, OPJ_CLRSPC_UNSPECIFIED);
    if (!image) {
        return 0;
    }
    image->x0 = k_img_x0;
    image->y0 = k_img_y0;
    image->x1 = k_img_x1;
    image->y1 = k_img_y1;
    image->color_space = OPJ_CLRSPC_UNSPECIFIED;

    for (c = 0; c < CODEC_NC; ++c) {
        size_t n, i;
        image->comps[c].dx = k_comp_dx[c];
        image->comps[c].dy = k_comp_dy[c];
        image->comps[c].prec = 16;
        image->comps[c].sgnd = 1;
        fx->cx0[c] = ceildiv_u(k_img_x0, k_comp_dx[c]);
        fx->cy0[c] = ceildiv_u(k_img_y0, k_comp_dy[c]);
        fx->cw[c] = ceildiv_u(k_img_x1, k_comp_dx[c]) - fx->cx0[c];
        fx->ch[c] = ceildiv_u(k_img_y1, k_comp_dy[c]) - fx->cy0[c];
        image->comps[c].x0 = fx->cx0[c];
        image->comps[c].y0 = fx->cy0[c];
        image->comps[c].w = fx->cw[c];
        image->comps[c].h = fx->ch[c];
        n = (size_t)fx->cw[c] * fx->ch[c];
        image->comps[c].data = (OPJ_INT32*)opj_image_data_alloc(
                                    n * sizeof(OPJ_INT32));
        fx->keep[c] = (OPJ_INT32*)malloc(n * sizeof(OPJ_INT32));
        if (!image->comps[c].data || !fx->keep[c]) {
            opj_image_destroy(image);
            return 0;
        }
        for (i = 0; i < n; ++i) {
            OPJ_INT32 v = (OPJ_INT32)(((int64_t)i * 1103 +
                                       (int64_t)c * 7919) % 2001) - 1000;
            image->comps[c].data[i] = v;
            fx->keep[c][i] = v;
        }
    }

    opj_set_default_encoder_parameters(&param);
    param.tcp_numlayers = 1;
    param.cp_fixed_quality = 1;
    param.tcp_distoratio[0] = 0;
    param.irreversible = 0;               /* reversible 5/3 */
    param.tcp_mct = 0;                    /* MCT explicitly disabled */
    param.numresolution = CODEC_NR;
    param.tile_size_on = OPJ_TRUE;
    param.cp_tx0 = 0;
    param.cp_ty0 = 0;
    param.cp_tdx = (int)k_tile_dx;
    param.cp_tdy = (int)k_tile_dy;
    param.cblockw_init = 32;
    param.cblockh_init = 32;

    codec = opj_create_compress(OPJ_CODEC_J2K);
    opj_set_info_handler(codec, quiet_handler, NULL);
    opj_set_warning_handler(codec, quiet_handler, NULL);
    opj_set_error_handler(codec, quiet_handler, NULL);
    if (!opj_setup_encoder(codec, &param, image)) {
        opj_destroy_codec(codec);
        opj_image_destroy(image);
        return 0;
    }
    stream = opj_stream_create(1024, OPJ_FALSE);
    opj_stream_set_user_data(stream, &fx->stream, NULL);
    opj_stream_set_write_function(stream, mem_write);
    if (!opj_start_compress(codec, image, stream) ||
            !opj_encode(codec, stream) ||
            !opj_end_compress(codec, stream)) {
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        opj_image_destroy(image);
        return 0;
    }
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    opj_image_destroy(image);
    return fx->stream.len >= 2 &&
           fx->stream.data[0] == 0xFF && fx->stream.data[1] == 0x4F;
}

static void fixture_destroy(codec_fixture_t *fx)
{
    unsigned c;
    for (c = 0; c < CODEC_NC; ++c) {
        free(fx->keep[c]);
    }
    free(fx->stream.data);
}

/* Every call builds a brand new decoder, as required by the contract. */
static opj_image_t *decode_image(const codec_fixture_t *fx,
                                 OPJ_UINT32 cp_reduce, int use_area,
                                 OPJ_INT32 ax0, OPJ_INT32 ay0,
                                 OPJ_INT32 ax1, OPJ_INT32 ay1)
{
    opj_dparameters_t dparam;
    opj_codec_t *codec;
    opj_stream_t *stream;
    opj_image_t *image = NULL;
    mem_stream_t backend = fx->stream;
    backend.pos = 0;   /* each decode starts at the beginning of the stream */

    opj_set_default_decoder_parameters(&dparam);
    dparam.cp_reduce = cp_reduce;
    codec = opj_create_decompress(OPJ_CODEC_J2K);
    opj_set_info_handler(codec, quiet_handler, NULL);
    opj_set_warning_handler(codec, quiet_handler, NULL);
    opj_set_error_handler(codec, quiet_handler, NULL);
    if (!opj_setup_decoder(codec, &dparam)) {
        opj_destroy_codec(codec);
        return NULL;
    }
    stream = opj_stream_create(1024, OPJ_TRUE);
    opj_stream_set_user_data(stream, &backend, NULL);
    opj_stream_set_user_data_length(stream, backend.len);
    opj_stream_set_read_function(stream, mem_read);
    opj_stream_set_skip_function(stream, mem_skip);
    opj_stream_set_seek_function(stream, mem_seek);
    if (!opj_read_header(stream, codec, &image)) {
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return NULL;
    }
    if (use_area && !opj_set_decode_area(codec, image, ax0, ay0, ax1, ay1)) {
        opj_image_destroy(image);
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return NULL;
    }
    if (!opj_decode(codec, stream, image) ||
            !opj_end_decompress(codec, stream)) {
        opj_image_destroy(image);
        image = NULL;
    }
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    return image;
}

/* ------------------------------------------------------------------ */
/* Group 6 (defined first in the runner order via the table at end):  */
/* full reversible lossless roundtrip through the public API          */
/* ------------------------------------------------------------------ */

static int case_codec_roundtrip(void)
{
    codec_fixture_t fx;
    opj_image_t *image;
    unsigned c;
    unsigned long local_combos = 0;

    if (!build_codestream(&fx)) {
        return 0;
    }
    image = decode_image(&fx, 0, 0, 0, 0, 0, 0);
    if (!image) {
        fixture_destroy(&fx);
        return 0;
    }
    ++local_combos;
    for (c = 0; c < CODEC_NC; ++c) {
        opj_image_comp_t *oc = &image->comps[c];
        size_t i;
        if (oc->w != fx.cw[c] || oc->h != fx.ch[c] ||
                oc->x0 != fx.cx0[c] || oc->y0 != fx.cy0[c]) {
            opj_image_destroy(image);
            fixture_destroy(&fx);
            return report_fail("inverse", (int)CODEC_NR - 1, "SAMPLE",
                               (int64_t)oc->x0, (int64_t)oc->y0,
                               (int64_t)oc->w, (int64_t)fx.cw[c]);
        }
        for (i = 0; i < (size_t)fx.cw[c] * fx.ch[c]; ++i) {
            if (oc->data[i] != fx.keep[c][i]) {
                opj_image_destroy(image);
                fixture_destroy(&fx);
                return report_fail("inverse", (int)CODEC_NR - 1, "SAMPLE",
                                   (int64_t)(fx.cx0[c] + i % fx.cw[c]),
                                   (int64_t)(fx.cy0[c] + i / fx.cw[c]),
                                   (int64_t)oc->data[i],
                                   (int64_t)fx.keep[c][i]);
            }
        }
    }
    opj_image_destroy(image);
    fixture_destroy(&fx);
    printf("COMBINATIONS codec-roundtrip %lu\n", local_combos);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Group 4: cp_reduce windows across tile edges vs full decodes, and  */
/* low-resolution LL against the independent reference               */
/* ------------------------------------------------------------------ */

static int case_roi_reduce(void)
{
    static const OPJ_INT32 windows[][4] = {
        {7, 5, 17, 13},    /* crosses tile edges x=8,16 and y=6,12 */
        {8, 6, 16, 12}     /* aligned exactly on tile boundaries */
    };
    codec_fixture_t fx;
    opj_image_t *full[CODEC_NR];
    OPJ_UINT32 reduce;
    unsigned wi, c;
    unsigned long local_combos = 0;

    if (!build_codestream(&fx)) {
        return 0;
    }
    for (reduce = 0; reduce < CODEC_NR; ++reduce) {
        full[reduce] = decode_image(&fx, reduce, 0, 0, 0, 0, 0);
        if (!full[reduce]) {
            fixture_destroy(&fx);
            return 0;
        }
    }
    ++local_combos;

    for (wi = 0; wi < (unsigned)(sizeof(windows) / sizeof(windows[0]));
            ++wi) {
        OPJ_INT32 ax0 = windows[wi][0];
        OPJ_INT32 ay0 = windows[wi][1];
        OPJ_INT32 ax1 = windows[wi][2];
        OPJ_INT32 ay1 = windows[wi][3];
        for (reduce = 0; reduce < CODEC_NR; ++reduce) {
            opj_image_t *win = decode_image(&fx, reduce, 1, ax0, ay0, ax1,
                                            ay1);
            if (!win) {
                fixture_destroy(&fx);
                return 0;
            }
            ++local_combos;
            for (c = 0; c < CODEC_NC; ++c) {
                unsigned shift = reduce;
                unsigned base_x0 = ceildiv_u((unsigned)ax0, k_comp_dx[c]);
                unsigned base_y0 = ceildiv_u((unsigned)ay0, k_comp_dy[c]);
                unsigned wx0 = ceildiv_u(base_x0, 1u << shift);
                unsigned wy0 = ceildiv_u(base_y0, 1u << shift);
                unsigned wx1 = ceildiv_u(
                                   ceildiv_u((unsigned)ax1, k_comp_dx[c]),
                                   1u << shift);
                unsigned wy1 = ceildiv_u(
                                   ceildiv_u((unsigned)ay1, k_comp_dy[c]),
                                   1u << shift);
                unsigned full_x0 = ceildiv_u(fx.cx0[c], 1u << shift);
                unsigned full_y0 = ceildiv_u(fx.cy0[c], 1u << shift);
                opj_image_comp_t *wc = &win->comps[c];
                opj_image_comp_t *fc = &full[reduce]->comps[c];
                unsigned x, y;
                /* Explicit window-to-component coordinate conversion,
                 * including the non-zero, odd image origin. */
                if (wc->x0 != (OPJ_UINT32)base_x0 ||
                        wc->y0 != (OPJ_UINT32)base_y0) {
                    opj_image_destroy(win);
                    fixture_destroy(&fx);
                    return report_fail("inverse", (int)reduce, "WINDOW",
                                       (int64_t)base_x0, (int64_t)base_y0,
                                       (int64_t)wc->x0, (int64_t)base_x0);
                }
                if (wc->w != wx1 - wx0 || wc->h != wy1 - wy0) {
                    opj_image_destroy(win);
                    fixture_destroy(&fx);
                    return report_fail("inverse", (int)reduce, "WINDOW",
                                       (int64_t)wx0, (int64_t)wy0,
                                       (int64_t)wc->w,
                                       (int64_t)(wx1 - wx0));
                }
                if (wx0 < full_x0 || wy0 < full_y0) {
                    opj_image_destroy(win);
                    fixture_destroy(&fx);
                    return 0;
                }
                for (y = 0; y < wc->h; ++y) {
                    for (x = 0; x < wc->w; ++x) {
                        OPJ_INT32 got = wc->data[y * wc->w + x];
                        OPJ_INT32 want =
                            fc->data[(size_t)(wy0 - full_y0 + y) * fc->w +
                                     (wx0 - full_x0 + x)];
                        if (got != want) {
                            opj_image_destroy(win);
                            fixture_destroy(&fx);
                            return report_fail("inverse", (int)reduce,
                                               "WINDOW",
                                               (int64_t)(wx0 + x),
                                               (int64_t)(wy0 + y),
                                               (int64_t)got, (int64_t)want);
                        }
                    }
                }
            }
            opj_image_destroy(win);
        }
    }

    /* Low-resolution full decodes must equal the independent LL pyramid of
     * every tile; naively subsampling the original would be wrong. */
    for (reduce = 1; reduce < CODEC_NR; ++reduce) {
        unsigned ntx = ceildiv_u(k_img_x1, k_tile_dx);
        unsigned nty = ceildiv_u(k_img_y1, k_tile_dy);
        for (c = 0; c < CODEC_NC; ++c) {
            unsigned ty, tx;
            for (ty = 0; ty < nty; ++ty) {
                for (tx = 0; tx < ntx; ++tx) {
                    unsigned tx0 = ceildiv_u(
                                       tx * k_tile_dx > k_img_x0 ?
                                       tx * k_tile_dx : k_img_x0,
                                       k_comp_dx[c]);
                    unsigned tx1 = ceildiv_u(
                                       (tx + 1) * k_tile_dx < k_img_x1 ?
                                       (tx + 1) * k_tile_dx : k_img_x1,
                                       k_comp_dx[c]);
                    unsigned ty0 = ceildiv_u(
                                       ty * k_tile_dy > k_img_y0 ?
                                       ty * k_tile_dy : k_img_y0,
                                       k_comp_dy[c]);
                    unsigned ty1 = ceildiv_u(
                                       (ty + 1) * k_tile_dy < k_img_y1 ?
                                       (ty + 1) * k_tile_dy : k_img_y1,
                                       k_comp_dy[c]);
                    int64_t llx0 = ref_ceil_div_pow2((int64_t)tx0, reduce);
                    int64_t lly0 = ref_ceil_div_pow2((int64_t)ty0, reduce);
                    int64_t llx1 = ref_ceil_div_pow2((int64_t)tx1, reduce);
                    int64_t lly1 = ref_ceil_div_pow2((int64_t)ty1, reduce);
                    unsigned full_x0 = ceildiv_u(fx.cx0[c], 1u << reduce);
                    unsigned full_y0 = ceildiv_u(fx.cy0[c], 1u << reduce);
                    int64_t *tile_img, yy, xx;
                    ref_dwt64 *rf;
                    opj_image_comp_t *fc;
                    if (llx1 == llx0 || lly1 == lly0) {
                        continue;   /* tile collapses at this resolution */
                    }
                    tile_img = (int64_t*)malloc(
                                   (size_t)(tx1 - tx0) * (ty1 - ty0) *
                                   sizeof(int64_t));
                    if (!tile_img) {
                        fixture_destroy(&fx);
                        return 0;
                    }
                    for (yy = (int64_t)ty0; yy < (int64_t)ty1; ++yy) {
                        for (xx = (int64_t)tx0; xx < (int64_t)tx1; ++xx) {
                            tile_img[(yy - (int64_t)ty0) * (tx1 - tx0) +
                                     (xx - (int64_t)tx0)] =
                                fx.keep[c][(size_t)(yy - fx.cy0[c]) *
                                           fx.cw[c] +
                                           (xx - fx.cx0[c])];
                        }
                    }
                    {
                        ref_rect64 tr = {(int64_t)tx0, (int64_t)ty0,
                                         (int64_t)tx1, (int64_t)ty1
                        };
                        rf = ref_dwt64_forward(tile_img, tr, reduce + 1);
                    }
                    if (!rf) {
                        free(tile_img);
                        fixture_destroy(&fx);
                        return 0;
                    }
                    fc = &full[reduce]->comps[c];
                    for (yy = lly0; yy < lly1; ++yy) {
                        for (xx = llx0; xx < llx1; ++xx) {
                            int64_t want =
                                rf->ll[(yy - rf->ll_rect.y0) *
                                       (rf->ll_rect.x1 -
                                        rf->ll_rect.x0) +
                                       (xx - rf->ll_rect.x0)];
                            OPJ_INT32 got =
                                fc->data[(size_t)(yy - (int64_t)full_y0) *
                                         fc->w +
                                         (xx - (int64_t)full_x0)];
                            if (want != (int64_t)got) {
                                free(tile_img);
                                ref_dwt64_destroy(rf);
                                fixture_destroy(&fx);
                                return report_fail("forward", (int)reduce,
                                                   "LL", xx, yy,
                                                   (int64_t)got, want);
                            }
                        }
                    }
                    ref_dwt64_destroy(rf);
                    free(tile_img);
                }
            }
        }
    }

    for (reduce = 0; reduce < CODEC_NR; ++reduce) {
        opj_image_destroy(full[reduce]);
    }
    fixture_destroy(&fx);
    printf("COMBINATIONS roi-reduce %lu\n", local_combos);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Group 5: thread counts 0 and 2 must give identical results         */
/* ------------------------------------------------------------------ */

static int case_threads(void)
{
    static const struct {
        int w, h, x0, y0;
    } shapes[] = {
        {17, 19, 3, 5}, {19, 17, 2, 4}, {17, 19, 1, 1}, {5, 5, 3, 2}
    };
    int si, pat;
    unsigned long local_combos = 0;

    for (si = 0; si < (int)(sizeof(shapes) / sizeof(shapes[0])); ++si) {
        int w = shapes[si].w;
        int h = shapes[si].h;
        int x0 = shapes[si].x0;
        int y0 = shapes[si].y0;
        for (pat = 0; pat < k_pattern_count; ++pat) {
            prod_dwt_t p0, p2;
            int i;
            ++local_combos;
            if (!prod_open(&p0, x0, y0, w, h, 4, 0) ||
                    !prod_open(&p2, x0, y0, w, h, 4, 2)) {
                prod_close(&p0);
                prod_close(&p2);
                return 0;
            }
            prod_load_pattern(&p0, pat);
            memcpy(p2.buf, p0.buf, (size_t)w * h * sizeof(OPJ_INT32));
            memcpy(p2.original, p0.original,
                   (size_t)w * h * sizeof(OPJ_INT32));
            if (!opj_dwt_encode(&p0.tcd, &p0.tile) ||
                    !opj_dwt_encode(&p2.tcd, &p2.tile)) {
                prod_close(&p0);
                prod_close(&p2);
                return 0;
            }
            for (i = 0; i < w * h; ++i) {
                if (p0.buf[i] != p2.buf[i]) {
                    int gx = x0 + (i % w);
                    int gy = y0 + (i / w);
                    report_fail("forward", 4, "COEF", (int64_t)gx,
                                (int64_t)gy, (int64_t)p2.buf[i],
                                (int64_t)p0.buf[i]);
                    prod_close(&p0);
                    prod_close(&p2);
                    return 0;
                }
            }
            if (!opj_dwt_decode(&p0.tcd, &p0.tile, 4) ||
                    !opj_dwt_decode(&p2.tcd, &p2.tile, 4)) {
                prod_close(&p0);
                prod_close(&p2);
                return 0;
            }
            for (i = 0; i < w * h; ++i) {
                if (p0.buf[i] != p2.buf[i] ||
                        p0.buf[i] != p0.original[i]) {
                    int gx = x0 + (i % w);
                    int gy = y0 + (i / w);
                    report_fail("inverse", 3, "SAMPLE", (int64_t)gx,
                                (int64_t)gy, (int64_t)p2.buf[i],
                                (int64_t)p0.original[i]);
                    prod_close(&p0);
                    prod_close(&p2);
                    return 0;
                }
            }
            prod_close(&p0);
            prod_close(&p2);
        }
    }

    printf("COMBINATIONS threads %lu\n", local_combos);
    return 1;
}

test_case cases[] = {
    {"coefficient-grid", case_coefficient_grid},
    {"inverse-grid", case_inverse_grid},
    {"multilevel", case_multilevel},
    {"roi-reduce", case_roi_reduce},
    {"threads", case_threads},
    {"codec-roundtrip", case_codec_roundtrip}
};
size_t case_count = sizeof(cases) / sizeof(cases[0]);
