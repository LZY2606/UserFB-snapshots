/*
 * Independent 64-bit reference implementation of the JPEG2000 reversible
 * 5/3 wavelet transform. See dwt_reference.h for the design constraints.
 *
 * The lifting steps follow the annex F definitions with symmetric boundary
 * extension. Negative numerators are divided with an explicit floor
 * towards negative infinity; no C shift operator is used on signed values.
 */

#include "dwt_reference.h"

#include <stdlib.h>
#include <string.h>

int64_t ref_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && (r < 0) != (b < 0)) {
        --q;
    }
    return q;
}

int64_t ref_ceil_div(int64_t a, int64_t b)
{
    return -ref_floor_div(-a, b);
}

int64_t ref_floor_div_pow2(int64_t a, unsigned shift)
{
    if (shift == 0) {
        return a;
    }
    /* Arithmetic right shift is not used: stay explicit about rounding. */
    if (a >= 0) {
        return a >> shift;
    }
    return -((-a + (((int64_t)1 << shift) - 1)) >> shift);
}

int64_t ref_ceil_div_pow2(int64_t a, unsigned shift)
{
    if (shift == 0) {
        return a;
    }
    if (a >= 0) {
        return (a + (((int64_t)1 << shift) - 1)) >> shift;
    }
    return -((-a) >> shift);
}

ref_rect64 ref_resolution_rect(ref_rect64 comp, unsigned numres, unsigned r)
{
    unsigned shift = numres - 1 - r;
    ref_rect64 out;
    out.x0 = ref_ceil_div_pow2(comp.x0, shift);
    out.y0 = ref_ceil_div_pow2(comp.y0, shift);
    out.x1 = ref_ceil_div_pow2(comp.x1, shift);
    out.y1 = ref_ceil_div_pow2(comp.y1, shift);
    return out;
}

ref_rect64 ref_band_rect(ref_rect64 comp, unsigned numres, int level, int band)
{
    /* Annex B equation B-15: with nb = numres - level, the band grid
     * coordinate is nb(k) = ceil((k - kb * 2^(nb-1)) / 2^nb). */
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

unsigned ref_max_numres(ref_rect64 comp)
{
    unsigned nr;
    /* A level count is legal when every resolution level that gets split is
     * non-empty in both dimensions. The coarsest level may collapse to zero
     * width or height, which happens for odd-origin singleton dimensions. */
    for (nr = 1; nr < 33; ++nr) {
        unsigned r;
        for (r = 1; r <= nr; ++r) {
            ref_rect64 q = ref_resolution_rect(comp, nr + 1, r);
            if (q.x1 <= q.x0 || q.y1 <= q.y0) {
                return nr;
            }
        }
    }
    return 33;
}

static int64_t clamp_index(int64_t i, int64_t n)
{
    if (i < 0) {
        return 0;
    }
    if (i >= n) {
        return n - 1;
    }
    return i;
}

/*
 * One dimensional forward 5/3 lifting on n samples with global origin g0.
 * The parity of g0 selects the cas convention. Outputs are the low pass
 * (sn = ceil(n/2)) and high pass (dn = n - sn) coefficients in band order.
 */
static void fwd_1d(const int64_t *src, int64_t n, int64_t g0,
                   int64_t *low, int64_t *high)
{
    int cas = (int)(g0 & 1);
    /* Even origin keeps the extra sample in the low pass; odd origin in the
     * high pass, matching sn = (n + (even ? 1 : 0)) >> 1 in the library. */
    int64_t sn = cas ? (n / 2) : ((n + 1) / 2);
    int64_t dn = n - sn;
    int64_t k;

    if (n == 1) {
        if (cas == 0) {
            low[0] = src[0];
        } else {
            /* Degenerate odd-origin singleton: the lone sample is classified
             * as high pass and scaled by two by the standard. */
            high[0] = src[0] * 2;
        }
        return;
    }

    if (cas == 0) {
        /* Predict: high[k] at local index 2k+1. */
        for (k = 0; k < dn; ++k) {
            int64_t left = src[2 * k];
            int64_t right = (2 * k + 2 < n) ? src[2 * k + 2] : src[2 * k];
            high[k] = src[2 * k + 1] - ref_floor_div(left + right, 2);
        }
        /* Update: low[k] at local index 2k. */
        for (k = 0; k < sn; ++k) {
            int64_t prev = high[clamp_index(k - 1, dn)];
            int64_t next = high[clamp_index(k, dn)];
            low[k] = src[2 * k] + ref_floor_div(prev + next + 2, 4);
        }
    } else {
        /* High pass samples sit at local even indices 2k. */
        for (k = 0; k < dn; ++k) {
            int64_t left = src[(k > 0) ? (2 * k - 1) : 1];
            int64_t right = (2 * k + 1 < n) ? src[2 * k + 1] :
                            src[(2 * k > 0) ? (2 * k - 1) : 1];
            high[k] = src[2 * k] - ref_floor_div(left + right, 2);
        }
        /* Low pass samples sit at local odd indices 2k+1. */
        for (k = 0; k < sn; ++k) {
            int64_t prev = high[clamp_index(k, dn)];
            int64_t next = high[clamp_index(k + 1, dn)];
            low[k] = src[2 * k + 1] + ref_floor_div(prev + next + 2, 4);
        }
    }
}

/*
 * One dimensional inverse lifting. low/high are the band arrays produced by
 * fwd_1d and out receives the n reconstructed samples in local order.
 */
static int inv_1d(const int64_t *low, const int64_t *high, int64_t n,
                  int64_t g0, int64_t *out)
{
    int cas = (int)(g0 & 1);
    int64_t sn = cas ? (n / 2) : ((n + 1) / 2);
    int64_t dn = n - sn;
    int64_t k;
    int64_t *l = (int64_t*)malloc((size_t)(sn > 0 ? sn : 1) * sizeof(int64_t));
    int64_t *h = (int64_t*)malloc((size_t)(dn > 0 ? dn : 1) * sizeof(int64_t));
    if (!l || !h) {
        free(l);
        free(h);
        return 0;
    }
    for (k = 0; k < sn; ++k) {
        l[k] = low[k];
    }
    for (k = 0; k < dn; ++k) {
        h[k] = high[k];
    }

    if (n == 1) {
        if (cas == 0) {
            out[0] = l[0];
        } else {
            /* Inverse of the x2 singleton scaling; the value stays even. */
            out[0] = h[0] / 2;
        }
        free(l);
        free(h);
        return 1;
    }

    if (cas == 0) {
        for (k = 0; k < sn; ++k) {
            int64_t prev = h[clamp_index(k - 1, dn)];
            int64_t next = h[clamp_index(k, dn)];
            l[k] -= ref_floor_div(prev + next + 2, 4);
        }
        for (k = 0; k < dn; ++k) {
            int64_t left = l[clamp_index(k, sn)];
            int64_t right = l[clamp_index(k + 1, sn)];
            h[k] += ref_floor_div(left + right, 2);
        }
        for (k = 0; k < dn; ++k) {
            out[2 * k] = l[k];
            out[2 * k + 1] = h[k];
        }
        if (sn > dn) {
            out[2 * (sn - 1)] = l[sn - 1];
        }
    } else {
        /* Undo the update: reconstruct samples at local odd positions, which
         * hold the low pass band. */
        for (k = 0; k < sn; ++k) {
            int64_t prev = h[clamp_index(k, dn)];
            int64_t next = h[clamp_index(k + 1, dn)];
            l[k] -= ref_floor_div(prev + next + 2, 4);
        }
        /* Undo the predict: reconstruct the high pass positions from their
         * low pass neighbours. */
        for (k = 0; k < dn; ++k) {
            int64_t left = l[clamp_index(k - 1, sn)];
            int64_t right = l[clamp_index(k, sn)];
            h[k] += ref_floor_div(left + right, 2);
        }
        /* Local even positions are high pass, odd positions low pass. */
        for (k = 0; k < dn; ++k) {
            out[2 * k] = h[k];
        }
        for (k = 0; k < sn; ++k) {
            out[2 * k + 1] = l[k];
        }
    }
    free(l);
    free(h);
    return 1;
}
static int64_t rect_pixels(ref_rect64 r)
{
    return (r.x1 - r.x0) * (r.y1 - r.y0);
}

static int64_t *alloc_i64(int64_t count)
{
    if (count < 0) {
        return NULL;
    }
    /* Always hand back a valid pointer so zero-pixel degenerate bands do
     * not need NULL handling at every call site. */
    if (count == 0) {
        count = 1;
    }
    return (int64_t*)malloc((size_t)count * sizeof(int64_t));
}

/* One forward decomposition level of a tile-local image. The pass order
 * matches the library: vertical lifting first, then horizontal lifting. The
 * order is observable because integer rounding and the odd-origin singleton
 * scaling do not commute across the two passes. */
static int fwd_level(const int64_t *img, ref_rect64 cur, ref_rect64 low,
                     int64_t *ll_img, ref_band64 *hl, ref_band64 *lh,
                     ref_band64 *hh)
{
    int64_t w = cur.x1 - cur.x0;
    int64_t h = cur.y1 - cur.y0;
    int64_t lw = low.x1 - low.x0;
    int64_t lhgt = low.y1 - low.y0;
    int64_t dn_x = w - lw;
    int64_t dn_y = h - lhgt;
    int64_t x, y, k;
    int64_t *vbuf = alloc_i64(w * h);
    int64_t *fbuf = alloc_i64(w * h);
    int64_t *row_l = alloc_i64(w);
    int64_t *row_h = alloc_i64(w);
    int64_t *col = alloc_i64(h);
    int64_t *col_l = alloc_i64(h);
    int64_t *col_h = alloc_i64(h);

    if (!vbuf || !fbuf || !row_l || !row_h || !col || !col_l || !col_h) {
        free(vbuf);
        free(fbuf);
        free(row_l);
        free(row_h);
        free(col);
        free(col_l);
        free(col_h);
        return 0;
    }

    /* Vertical pass: low rows first, then high rows, in every column. */
    for (x = 0; x < w; ++x) {
        for (y = 0; y < h; ++y) {
            col[y] = img[y * w + x];
        }
        fwd_1d(col, h, cur.y0, col_l, col_h);
        for (y = 0; y < lhgt; ++y) {
            vbuf[y * w + x] = col_l[y];
        }
        for (y = 0; y < dn_y; ++y) {
            vbuf[(lhgt + y) * w + x] = col_h[y];
        }
    }

    /* Horizontal pass on every row of the vertically split buffer. */
    for (y = 0; y < h; ++y) {
        fwd_1d(vbuf + y * w, w, cur.x0, row_l, row_h);
        for (x = 0; x < lw; ++x) {
            fbuf[y * w + x] = row_l[x];
        }
        for (x = 0; x < dn_x; ++x) {
            fbuf[y * w + lw + x] = row_h[x];
        }
    }

    /* Scatter into the global band grids. */
    for (y = 0; y < lhgt; ++y) {
        for (x = 0; x < lw; ++x) {
            ll_img[y * lw + x] = fbuf[y * w + x];
        }
    }
    for (y = 0; y < lhgt; ++y) {
        for (k = 0; k < dn_x; ++k) {
            hl->coef[y * dn_x + k] = fbuf[y * w + lw + k];
        }
    }
    for (k = 0; k < dn_y; ++k) {
        for (x = 0; x < lw; ++x) {
            lh->coef[k * lw + x] = fbuf[(lhgt + k) * w + x];
        }
        for (x = 0; x < dn_x; ++x) {
            hh->coef[k * dn_x + x] = fbuf[(lhgt + k) * w + lw + x];
        }
    }

    free(vbuf);
    free(fbuf);
    free(row_l);
    free(row_h);
    free(col);
    free(col_l);
    free(col_h);
    return 1;
}

/* Inverse of fwd_level: rebuild the cur-rectangle image from its four bands. */
/* Inverse of fwd_level: rebuild the cur-rectangle image from its four bands.
 * The library inverts horizontal lifting first and vertical lifting second,
 * which is the exact reverse of the forward pass order. */
static int inv_level(const int64_t *ll_img, const ref_band64 *hl,
                     const ref_band64 *lh, const ref_band64 *hh,
                     ref_rect64 cur, ref_rect64 low, int64_t *img)
{
    int64_t w = cur.x1 - cur.x0;
    int64_t h = cur.y1 - cur.y0;
    int64_t lw = low.x1 - low.x0;
    int64_t lhgt = low.y1 - low.y0;
    int64_t dn_x = w - lw;
    int64_t dn_y = h - lhgt;
    int64_t x, y, k;
    int64_t *fbuf = alloc_i64(w * h);
    int64_t *hbuf = alloc_i64(w * h);
    int64_t *col = alloc_i64(h);
    int64_t *col_l = alloc_i64(h);
    int64_t *col_h = alloc_i64(h);
    int64_t *row = alloc_i64(w);
    int64_t *row_l = alloc_i64(w);
    int64_t *row_h = alloc_i64(w);

    if (!fbuf || !hbuf || !col || !col_l || !col_h || !row || !row_l ||
            !row_h) {
        free(fbuf);
        free(hbuf);
        free(col);
        free(col_l);
        free(col_h);
        free(row);
        free(row_l);
        free(row_h);
        return 0;
    }

    /* Reassemble the band-interleaved buffer. */
    for (y = 0; y < lhgt; ++y) {
        for (x = 0; x < lw; ++x) {
            fbuf[y * w + x] = ll_img[y * lw + x];
        }
        for (k = 0; k < dn_x; ++k) {
            fbuf[y * w + lw + k] = hl->coef[y * dn_x + k];
        }
    }
    for (k = 0; k < dn_y; ++k) {
        for (x = 0; x < lw; ++x) {
            fbuf[(lhgt + k) * w + x] = lh->coef[k * lw + x];
        }
        for (x = 0; x < dn_x; ++x) {
            fbuf[(lhgt + k) * w + lw + x] = hh->coef[k * dn_x + x];
        }
    }

    /* Horizontal inverse per row. */
    for (y = 0; y < h; ++y) {
        for (x = 0; x < lw; ++x) {
            row_l[x] = fbuf[y * w + x];
        }
        for (x = 0; x < dn_x; ++x) {
            row_h[x] = fbuf[y * w + lw + x];
        }
        if (!inv_1d(row_l, row_h, w, cur.x0, row)) {
            goto fail;
        }
        for (x = 0; x < w; ++x) {
            hbuf[y * w + x] = row[x];
        }
    }

    /* Vertical inverse per column. */
    for (x = 0; x < w; ++x) {
        for (y = 0; y < lhgt; ++y) {
            col_l[y] = hbuf[y * w + x];
        }
        for (y = 0; y < dn_y; ++y) {
            col_h[y] = hbuf[(lhgt + y) * w + x];
        }
        if (!inv_1d(col_l, col_h, h, cur.y0, col)) {
            goto fail;
        }
        for (y = 0; y < h; ++y) {
            img[y * w + x] = col[y];
        }
    }

    free(fbuf);
    free(hbuf);
    free(col);
    free(col_l);
    free(col_h);
    free(row);
    free(row_l);
    free(row_h);
    return 1;

fail:
    free(fbuf);
    free(hbuf);
    free(col);
    free(col_l);
    free(col_h);
    free(row);
    free(row_l);
    free(row_h);
    return 0;
}

ref_dwt64 *ref_dwt64_forward(const int64_t *img, ref_rect64 comp,
                             unsigned numres)
{
    ref_dwt64 *d;
    int64_t **levels;
    ref_rect64 *rects;
    unsigned r;
    unsigned bi;

    if (numres < 1 || numres > 33 || comp.x1 <= comp.x0 ||
            comp.y1 <= comp.y0) {
        return NULL;
    }
    d = (ref_dwt64*)calloc(1, sizeof(ref_dwt64));
    levels = (int64_t**)calloc(numres, sizeof(int64_t*));
    rects = (ref_rect64*)calloc(numres, sizeof(ref_rect64));
    if (!d || !levels || !rects) {
        free(d);
        free(levels);
        free(rects);
        return NULL;
    }
    d->numres = numres;
    d->comp = comp;
    d->nb_bands = 3U * (numres - 1);
    d->bands = d->nb_bands ?
               (ref_band64*)calloc(d->nb_bands, sizeof(ref_band64)) : NULL;
    if (d->nb_bands && !d->bands) {
        free(d);
        free(levels);
        free(rects);
        return NULL;
    }

    for (r = 0; r < numres; ++r) {
        rects[r] = ref_resolution_rect(comp, numres, r);
        /* Coarsest levels may have a zero extent (odd-origin singleton), but
         * a reversed rectangle would mean an illegal level count. */
        if (rects[r].x1 < rects[r].x0 || rects[r].y1 < rects[r].y0) {
            goto fail;
        }
        if (r > 0 && (rects[r].x1 <= rects[r].x0 ||
                      rects[r].y1 <= rects[r].y0)) {
            goto fail;
        }
    }
    d->ll_rect = rects[0];

    levels[numres - 1] = alloc_i64(rect_pixels(rects[numres - 1]));
    if (!levels[numres - 1]) {
        goto fail;
    }
    memcpy(levels[numres - 1], img,
           (size_t)rect_pixels(rects[numres - 1]) * sizeof(int64_t));

    /* Decompose finest to coarsest; bands are stored coarsest-first so that
     * level l occupies slots (l-1)*3 .. (l-1)*3+2. */
    for (r = numres - 1; r >= 1; --r) {
        bi = (r - 1) * 3;
        ref_band64 *hl = &d->bands[bi];
        ref_band64 *lh = &d->bands[bi + 1];
        ref_band64 *hh = &d->bands[bi + 2];
        hl->level = (int)r;
        lh->level = (int)r;
        hh->level = (int)r;
        hl->band = REF_BAND_HL;
        lh->band = REF_BAND_LH;
        hh->band = REF_BAND_HH;
        hl->rect = ref_band_rect(comp, numres, (int)r, REF_BAND_HL);
        lh->rect = ref_band_rect(comp, numres, (int)r, REF_BAND_LH);
        hh->rect = ref_band_rect(comp, numres, (int)r, REF_BAND_HH);
        hl->coef = alloc_i64(rect_pixels(hl->rect));
        lh->coef = alloc_i64(rect_pixels(lh->rect));
        hh->coef = alloc_i64(rect_pixels(hh->rect));
        levels[r - 1] = alloc_i64(rect_pixels(rects[r - 1]));
        if ((rect_pixels(hl->rect) > 0 && !hl->coef) ||
                (rect_pixels(lh->rect) > 0 && !lh->coef) ||
                (rect_pixels(hh->rect) > 0 && !hh->coef) ||
                !levels[r - 1]) {
            goto fail;
        }
        if (!fwd_level(levels[r], rects[r], rects[r - 1], levels[r - 1],
                       hl, lh, hh)) {
            goto fail;
        }
    }

    d->ll = levels[0];
    free(levels);
    free(rects);
    return d;

fail:
    for (r = 0; r < numres; ++r) {
        free(levels[r]);
    }
    free(levels);
    free(rects);
    ref_dwt64_destroy(d);
    return NULL;
}

int ref_dwt64_inverse(const ref_dwt64 *d, int64_t *out)
{
    int64_t **levels;
    ref_rect64 *rects;
    unsigned r;

    if (!d || !out) {
        return 0;
    }
    levels = (int64_t**)calloc(d->numres, sizeof(int64_t*));
    rects = (ref_rect64*)calloc(d->numres, sizeof(ref_rect64));
    if (!levels || !rects) {
        free(levels);
        free(rects);
        return 0;
    }
    for (r = 0; r < d->numres; ++r) {
        rects[r] = ref_resolution_rect(d->comp, d->numres, r);
    }
    levels[0] = alloc_i64(rect_pixels(rects[0]));
    if (!levels[0]) {
        free(levels);
        free(rects);
        return 0;
    }
    memcpy(levels[0], d->ll,
           (size_t)rect_pixels(rects[0]) * sizeof(int64_t));

    for (r = 1; r < d->numres; ++r) {
        unsigned bi = (r - 1) * 3;
        const ref_band64 *hl = &d->bands[bi];
        const ref_band64 *lh = &d->bands[bi + 1];
        const ref_band64 *hh = &d->bands[bi + 2];
        levels[r] = alloc_i64(rect_pixels(rects[r]));
        if (!levels[r] ||
                !inv_level(levels[r - 1], hl, lh, hh, rects[r], rects[r - 1],
                           levels[r])) {
            unsigned k;
            for (k = 0; k <= r; ++k) {
                free(levels[k]);
            }
            free(levels);
            free(rects);
            return 0;
        }
    }

    memcpy(out, levels[d->numres - 1],
           (size_t)rect_pixels(rects[d->numres - 1]) * sizeof(int64_t));
    for (r = 0; r < d->numres; ++r) {
        free(levels[r]);
    }
    free(levels);
    free(rects);
    return 1;
}

int64_t ref_band64_get(const ref_band64 *b, int64_t gx, int64_t gy)
{
    return b->coef[(gy - b->rect.y0) * (b->rect.x1 - b->rect.x0) +
                   (gx - b->rect.x0)];
}

void ref_dwt64_destroy(ref_dwt64 *d)
{
    unsigned i;
    if (!d) {
        return;
    }
    if (d->bands) {
        for (i = 0; i < d->nb_bands; ++i) {
            free(d->bands[i].coef);
        }
        free(d->bands);
    }
    if (d->ll) {
        free(d->ll);
    }
    free(d);
}
