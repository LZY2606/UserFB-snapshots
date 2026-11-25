/*
 * deblock_ref.c - independent coordinate/peixel deblocking reference.
 *
 * The threshold constants below are the normative tables of ITU-T H.264
 * (08/2021 edition), clause 8.7.2 "Deblocking filter process":
 *   - QpC mapping table for chroma QP (Table 8-15 family),
 *   - alpha and beta threshold tables indexed by A/B (Tables 8-16/8-17),
 *   - t'_C0 table indexed by A and bS-1 (Table 8-18).
 * They were transcribed from the written standard for this reference; the
 * numeric values necessarily equal the specification and therefore the
 * library's private static tables, but no production array/macro is used.
 *
 * All arithmetic is written from the specification equations.  Signed
 * right shifts in the production compiler behave as arithmetic floor
 * shifts; asr() makes that behaviour explicit in standard C.
 */
#include "deblock_ref.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Normative tables                                                    */
/* ------------------------------------------------------------------ */

static const uint8_t REF_ALPHA[52] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     4,  4,  5,  6,  7,  8,  9, 10, 12, 13, 15, 17, 20, 22, 25, 28,
    32, 36, 40, 45, 50, 56, 63, 71, 80, 90,101,113,127,144,162,182,
   203,226,255,255
};

static const uint8_t REF_BETA[52] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     2,  2,  2,  3,  3,  3,  3,  4,  4,  4,  6,  6,  7,  7,  8,  8,
     9,  9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18
};

/* tC0[indexA][bS-1], bS = 1,2,3 */
static const uint8_t REF_TC0[52][3] = {
    { 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},
    { 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},
    { 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},{ 0, 0, 0},{ 0, 0, 1},
    { 0, 0, 1},{ 0, 0, 1},{ 0, 0, 1},{ 0, 1, 1},{ 0, 1, 1},{ 1, 1, 1},
    { 1, 1, 1},{ 1, 1, 1},{ 1, 1, 1},{ 1, 1, 2},{ 1, 1, 2},{ 1, 1, 2},
    { 1, 1, 2},{ 1, 2, 3},{ 1, 2, 3},{ 2, 2, 3},{ 2, 2, 4},{ 2, 3, 4},
    { 2, 3, 4},{ 3, 3, 5},{ 3, 4, 6},{ 3, 4, 6},{ 4, 5, 7},{ 4, 5, 8},
    { 4, 6, 9},{ 5, 7,10},{ 6, 8,11},{ 6, 8,13},{ 7,10,14},{ 8,11,16},
    { 9,12,18},{10,13,20},{11,15,23},{13,17,25}
};

/* QpC = Clip3(0,51, ... ) mapping for chroma qp index, 4:2:0. */
static const uint8_t REF_QPC[52] = {
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,
    20,21,22,23,24,25,26,27,28,29,29,30,31,32,32,33,34,34,35,35,
    36,36,37,37,37,38,38,38,39,39,39,39
};

unsigned dref_alpha(unsigned i) { return REF_ALPHA[i < 52 ? i : 51]; }
unsigned dref_beta(unsigned i)  { return REF_BETA[i  < 52 ? i : 51]; }
unsigned dref_tc0(unsigned ia, unsigned k) {
    return REF_TC0[ia < 52 ? ia : 51][k < 3 ? k : 2];
}
unsigned dref_qpc(unsigned q) { return REF_QPC[q < 52 ? q : 51]; }

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int iclip(int lo, int hi, int v) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* arithmetic (floor) right shift by b bits, b in 1..3 */
static int asr(int v, int b) {
    if (v >= 0) return v >> b;
    {
        int d = 1 << b;
        return -((-v + d - 1) / d);
    }
}

static int iabs(int v) { return v < 0 ? -v : v; }

/* Emulates an assignment/conversion to u8 in the production code. */
static uint8_t tou8(int v) { return (uint8_t)(v & 255); }

/* The production clipping table is a saturation to [0,255]. */
static uint8_t sat255(int v) {
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

typedef struct {
    uint8_t *p;
    int stride;
} plane_t;

static int pg(const plane_t *pl, int x, int y) {
    return pl->p[(long)y * pl->stride + x];
}
static void ps(const plane_t *pl, int x, int y, int v) {
    pl->p[(long)y * pl->stride + x] = tou8(v);
}
/* saturated write for the p0/q0 taps that go through h264bsdClip */
static void pss(const plane_t *pl, int x, int y, int v) {
    pl->p[(long)y * pl->stride + x] = sat255(v);
}

/* ------------------------------------------------------------------ */
/* Thresholds                                                          */
/* ------------------------------------------------------------------ */

enum { THR_TOP = 0, THR_LEFT = 1, THR_INNER = 2 };

typedef struct {
    int alpha, beta;
    const uint8_t *tc0;
} thr_t;

static void luma_inner_thr(const dref_mb *m, thr_t *t) {
    int ia = iclip(0, 51, (int)m->qpY + m->offA);
    int ib = iclip(0, 51, (int)m->qpY + m->offB);
    t->alpha = REF_ALPHA[ia];
    t->beta  = REF_BETA[ib];
    t->tc0   = REF_TC0[ia];
}

static void chroma_inner_thr(const dref_mb *m, thr_t *t) {
    int qpC = REF_QPC[iclip(0, 51, (int)m->qpY + m->chromaQpIndexOffset)];
    int ia = iclip(0, 51, qpC + m->offA);
    int ib = iclip(0, 51, qpC + m->offB);
    t->alpha = REF_ALPHA[ia];
    t->beta  = REF_BETA[ib];
    t->tc0   = REF_TC0[ia];
}

/* Outer edge threshold.  sideQpRaw is the neighbouring MB qpY.  For chroma
 * the two sides are clipped+mapped FIRST and only then averaged with
 * rounding, which is the ordering the contract pins. */
static void luma_outer_thr(const dref_mb *m, unsigned sideQpRaw, thr_t *t) {
    if (sideQpRaw == m->qpY) { luma_inner_thr(m, t); return; }
    {
        int av = ((int)m->qpY + (int)sideQpRaw + 1) >> 1;
        int ia = iclip(0, 51, av + m->offA);
        int ib = iclip(0, 51, av + m->offB);
        t->alpha = REF_ALPHA[ia];
        t->beta  = REF_BETA[ib];
        t->tc0   = REF_TC0[ia];
    }
}

static void chroma_outer_thr(const dref_mb *m, unsigned sideQpRaw,
                             thr_t *t) {
    if (sideQpRaw == m->qpY) { chroma_inner_thr(m, t); return; }
    {
        int qpC0 = REF_QPC[iclip(0, 51,
                        (int)m->qpY + m->chromaQpIndexOffset)];
        int qpC1 = REF_QPC[iclip(0, 51,
                        (int)sideQpRaw + m->chromaQpIndexOffset)];
        int av = (qpC0 + qpC1 + 1) >> 1;   /* rounded average after map */
        int ia = iclip(0, 51, av + m->offA);
        int ib = iclip(0, 51, av + m->offB);
        t->alpha = REF_ALPHA[ia];
        t->beta  = REF_BETA[ib];
        t->tc0   = REF_TC0[ia];
    }
}

/* ------------------------------------------------------------------ */
/* Boundary strength, natural 4x4 coordinates                          */
/* ------------------------------------------------------------------ */

static int quadrant(int n) {
    int bx = n & 3, by = n >> 2;
    return (by >> 1) * 2 + (bx >> 1);
}

/* Full inter bS rule for two natural blocks (possibly across MBs). */
static int edge_bs_full(const dref_mb *a, int ba,
                        const dref_mb *b, int bb) {
    if (a->coeff[ba] || b->coeff[bb]) return 2;
    if (a->ref[quadrant(ba)] != b->ref[quadrant(bb)]) return 1;
    if (iabs(a->mvx[ba] - b->mvx[bb]) >= 4 ||
        iabs(a->mvy[ba] - b->mvy[bb]) >= 4)
        return 1;
    return 0;
}

/* Coefficient-only shortcut used on edges where a partition shape makes
 * motion/reference differences impossible by construction. */
static int edge_bs_coeff(const dref_mb *a, int ba,
                         const dref_mb *b, int bb) {
    return (a->coeff[ba] || b->coeff[bb]) ? 2 : 0;
}

typedef struct {
    unsigned char left[16];   /* index band*4 + local vertical edge 0..3 */
    unsigned char top[16];    /* index band*4 + column segment 0..3      */
    int hasLeft, hasTop;
} bs_grid_t;

static const dref_mb *mb_at(const dref_pic *p, int c, int r) {
    if (c < 0 || r < 0 || c >= p->wMb || r >= p->hMb) return NULL;
    return &p->mb[r * p->wMb + c];
}

static void build_bs(const dref_pic *pic, int mc, int mr, bs_grid_t *bs) {
    const dref_mb *m = mb_at(pic, mc, mr);
    const dref_mb *A = mb_at(pic, mc - 1, mr);
    const dref_mb *B = mb_at(pic, mc, mr - 1);
    int idc = (int)m->disableIdc;
    int innerOn = (idc != 1);
    int leftOn = innerOn && A && (idc != 2 || m->sliceId == A->sliceId);
    int topOn  = innerOn && B && (idc != 2 || m->sliceId == B->sliceId);
    int j, i;

    for (i = 0; i < 16; ++i) { bs->left[i] = 0; bs->top[i] = 0; }
    bs->hasLeft = leftOn;
    bs->hasTop = topOn;

    /* Outer top edge: four 4-pixel column segments. */
    if (topOn) {
        for (i = 0; i < 4; ++i) {
            if (m->intra || B->intra)
                bs->top[i] = 4;
            else
                bs->top[i] = (unsigned char)edge_bs_full(m, i, B, 12 + i);
        }
    }

    /* Outer left edge: four 4-pixel row bands. */
    if (leftOn) {
        for (j = 0; j < 4; ++j) {
            int k = j * 4;
            if (m->intra || A->intra)
                bs->left[k] = 4;
            else
                bs->left[k] =
                    (unsigned char)edge_bs_full(m, j * 4, A, j * 4 + 3);
        }
    }

    if (!innerOn) return;

    if (m->intra) {
        /* Inner luma edges of an intra MB all get bS=3. */
        for (j = 1; j < 4; ++j)
            for (i = 0; i < 4; ++i)
                bs->top[j * 4 + i] = 3;
        for (j = 0; j < 4; ++j)
            for (i = 1; i < 4; ++i)
                bs->left[j * 4 + i] = 3;
        return;
    }

    /* Natural-coordinate adjacent block pairs for every inner segment. */
    for (j = 1; j < 4; ++j) {
        for (i = 0; i < 4; ++i) {
            int above = (j - 1) * 4 + i;
            int below = j * 4 + i;
            int full = 0;
            if (m->part == DREF_PART_8X8) full = 1;
            else if (m->part == DREF_PART_16X8 && j == 2) full = 1;
            bs->top[j * 4 + i] = (unsigned char)(full
                ? edge_bs_full(m, above, m, below)
                : edge_bs_coeff(m, above, m, below));
        }
    }
    for (j = 0; j < 4; ++j) {
        for (i = 1; i < 4; ++i) {
            int lft = j * 4 + (i - 1);
            int rgt = j * 4 + i;
            int full = 0;
            if (m->part == DREF_PART_8X8) full = 1;
            else if (m->part == DREF_PART_8X16 && i == 2) full = 1;
            bs->left[j * 4 + i] = (unsigned char)(full
                ? edge_bs_full(m, lft, m, rgt)
                : edge_bs_coeff(m, lft, m, rgt));
        }
    }
    /* DREF_PART_16X16: every inner segment is coefficient-only, which the
     * two loops above already produce because no "full" condition fires. */
}

/* ------------------------------------------------------------------ */
/* Luma sample filters                                                 */
/* ------------------------------------------------------------------ */

static void filter_ver_luma(plane_t *pl, int ex, int y0,
                            int bS, const thr_t *t) {
    int alpha = t->alpha, beta = t->beta;
    int y;
    if (bS == 0) return;

    if (bS < 4) {
        int tc = t->tc0[bS - 1];
        int tmp = tc;
        for (y = y0; y < y0 + 4; ++y) {
            int p1 = pg(pl, ex - 2, y), p0 = pg(pl, ex - 1, y);
            int q0 = pg(pl, ex, y),     q1 = pg(pl, ex + 1, y);
            if ((unsigned)iabs(p0 - q0) < (unsigned)alpha &&
                (unsigned)iabs(p1 - p0) < (unsigned)beta &&
                (unsigned)iabs(q1 - q0) < (unsigned)beta) {
                int p2 = pg(pl, ex - 3, y);
                int q2 = pg(pl, ex + 2, y);
                if ((unsigned)iabs(p2 - p0) < (unsigned)beta) {
                    int val = asr(p2 + ((p0 + q0 + 1) >> 1) - (2 * p1), 1);
                    ps(pl, ex - 2, y, p1 + iclip(-tc, tc, val));
                    ++tmp;
                }
                if ((unsigned)iabs(q2 - q0) < (unsigned)beta) {
                    int val = asr(q2 + ((p0 + q0 + 1) >> 1) - (2 * q1), 1);
                    ps(pl, ex + 1, y, q1 + iclip(-tc, tc, val));
                    ++tmp;
                }
                {
                    int val = asr(4 * (q0 - p0) + (p1 - q1) + 4, 3);
                    int delta = iclip(-tmp, tmp, val);
                    pss(pl, ex - 1, y, p0 + delta);
                    pss(pl, ex, y, q0 - delta);
                }
                tmp = tc;
            }
        }
    } else {
        for (y = y0; y < y0 + 4; ++y) {
            int p1 = pg(pl, ex - 2, y), p0 = pg(pl, ex - 1, y);
            int q0 = pg(pl, ex, y),     q1 = pg(pl, ex + 1, y);
            if ((unsigned)iabs(p0 - q0) < (unsigned)alpha &&
                (unsigned)iabs(p1 - p0) < (unsigned)beta &&
                (unsigned)iabs(q1 - q0) < (unsigned)beta) {
                int small = (unsigned)iabs(p0 - q0) <
                            (unsigned)((alpha >> 2) + 2);
                int p2 = pg(pl, ex - 3, y);
                int q2 = pg(pl, ex + 2, y);
                if (small && (unsigned)iabs(p2 - p0) < (unsigned)beta) {
                    int tmp = p1 + p0 + q0;
                    ps(pl, ex - 1, y, (p2 + 2 * tmp + q1 + 4) >> 3);
                    ps(pl, ex - 2, y, (p2 + tmp + 2) >> 2);
                    /* ex-4 is read LIVE: an earlier vertical edge may
                     * have just updated it. */
                    ps(pl, ex - 3, y,
                       (2 * pg(pl, ex - 4, y) + 3 * p2 + tmp + 4) >> 3);
                } else {
                    ps(pl, ex - 1, y, (2 * p1 + p0 + q1 + 2) >> 2);
                }
                if (small && (unsigned)iabs(q2 - q0) < (unsigned)beta) {
                    int tmp = p0 + q0 + q1;
                    ps(pl, ex, y, (p1 + 2 * tmp + q2 + 4) >> 3);
                    ps(pl, ex + 1, y, (tmp + q2 + 2) >> 2);
                    ps(pl, ex + 2, y,
                       (2 * pg(pl, ex + 3, y) + 3 * q2 + tmp + 4) >> 3);
                } else {
                    ps(pl, ex, y, (2 * q1 + q0 + p1 + 2) >> 2);
                }
            }
        }
    }
}

/* Horizontal luma segment: edge row ey (p0 = ey-1, q0 = ey), four columns
 * x0..x0+3.  Same equations with axes swapped. */
static void filter_hor_luma(plane_t *pl, int ey, int x0,
                            int bS, const thr_t *t) {
    int alpha = t->alpha, beta = t->beta;
    int x;
    if (bS == 0) return;

    if (bS < 4) {
        int tc = t->tc0[bS - 1];
        int tmp = tc;
        for (x = x0; x < x0 + 4; ++x) {
            int p1 = pg(pl, x, ey - 2), p0 = pg(pl, x, ey - 1);
            int q0 = pg(pl, x, ey),     q1 = pg(pl, x, ey + 1);
            if ((unsigned)iabs(p0 - q0) < (unsigned)alpha &&
                (unsigned)iabs(p1 - p0) < (unsigned)beta &&
                (unsigned)iabs(q1 - q0) < (unsigned)beta) {
                int p2 = pg(pl, x, ey - 3);
                int q2 = pg(pl, x, ey + 2);
                if ((unsigned)iabs(p2 - p0) < (unsigned)beta) {
                    int val = asr(p2 + ((p0 + q0 + 1) >> 1) - (2 * p1), 1);
                    ps(pl, x, ey - 2, p1 + iclip(-tc, tc, val));
                    ++tmp;
                }
                if ((unsigned)iabs(q2 - q0) < (unsigned)beta) {
                    int val = asr(q2 + ((p0 + q0 + 1) >> 1) - (2 * q1), 1);
                    ps(pl, x, ey + 1, q1 + iclip(-tc, tc, val));
                    ++tmp;
                }
                {
                    int val = asr(4 * (q0 - p0) + (p1 - q1) + 4, 3);
                    int delta = iclip(-tmp, tmp, val);
                    pss(pl, x, ey - 1, p0 + delta);
                    pss(pl, x, ey, q0 - delta);
                }
                tmp = tc;
            }
        }
    } else {
        for (x = x0; x < x0 + 4; ++x) {
            int p1 = pg(pl, x, ey - 2), p0 = pg(pl, x, ey - 1);
            int q0 = pg(pl, x, ey),     q1 = pg(pl, x, ey + 1);
            if ((unsigned)iabs(p0 - q0) < (unsigned)alpha &&
                (unsigned)iabs(p1 - p0) < (unsigned)beta &&
                (unsigned)iabs(q1 - q0) < (unsigned)beta) {
                int small = (unsigned)iabs(p0 - q0) <
                            (unsigned)((alpha >> 2) + 2);
                int p2 = pg(pl, x, ey - 3);
                int q2 = pg(pl, x, ey + 2);
                if (small && (unsigned)iabs(p2 - p0) < (unsigned)beta) {
                    int tmp = p1 + p0 + q0;
                    ps(pl, x, ey - 1, (p2 + 2 * tmp + q1 + 4) >> 3);
                    ps(pl, x, ey - 2, (p2 + tmp + 2) >> 2);
                    ps(pl, x, ey - 3,
                       (2 * pg(pl, x, ey - 4) + 3 * p2 + tmp + 4) >> 3);
                } else {
                    ps(pl, x, ey - 1, (2 * p1 + p0 + q1 + 2) >> 2);
                }
                if (small && (unsigned)iabs(q2 - q0) < (unsigned)beta) {
                    int tmp = p0 + q0 + q1;
                    ps(pl, x, ey, (p1 + 2 * tmp + q2 + 4) >> 3);
                    ps(pl, x, ey + 1, (tmp + q2 + 2) >> 2);
                    ps(pl, x, ey + 2,
                       (2 * pg(pl, x, ey + 3) + 3 * q2 + tmp + 4) >> 3);
                } else {
                    ps(pl, x, ey, (2 * q1 + q0 + p1 + 2) >> 2);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Chroma sample filters (two samples per segment)                     */
/* ------------------------------------------------------------------ */

static void chroma_ver_pair(plane_t *pl, int ex, int y,
                            int bS, const thr_t *t) {
    int p1 = pg(pl, ex - 2, y), p0 = pg(pl, ex - 1, y);
    int q0 = pg(pl, ex, y),     q1 = pg(pl, ex + 1, y);
    if ((unsigned)iabs(p0 - q0) < (unsigned)t->alpha &&
        (unsigned)iabs(p1 - p0) < (unsigned)t->beta &&
        (unsigned)iabs(q1 - q0) < (unsigned)t->beta) {
        if (bS < 4) {
            int tc = t->tc0[bS - 1] + 1;
            int delta = iclip(-tc, tc,
                asr(4 * (q0 - p0) + (p1 - q1) + 4, 3));
            pss(pl, ex - 1, y, p0 + delta);
            pss(pl, ex, y, q0 - delta);
        } else {
            ps(pl, ex - 1, y, (2 * p1 + p0 + q1 + 2) >> 2);
            ps(pl, ex, y, (2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

static void filter_ver_chroma(plane_t *pl, int ex, int y0,
                              int bS, const thr_t *t) {
    if (bS == 0) return;
    chroma_ver_pair(pl, ex, y0, bS, t);
    chroma_ver_pair(pl, ex, y0 + 1, bS, t);
}

static void chroma_hor_pair(plane_t *pl, int ey, int x,
                            int bS, const thr_t *t) {
    int p1 = pg(pl, x, ey - 2), p0 = pg(pl, x, ey - 1);
    int q0 = pg(pl, x, ey),     q1 = pg(pl, x, ey + 1);
    if ((unsigned)iabs(p0 - q0) < (unsigned)t->alpha &&
        (unsigned)iabs(p1 - p0) < (unsigned)t->beta &&
        (unsigned)iabs(q1 - q0) < (unsigned)t->beta) {
        if (bS < 4) {
            int tc = t->tc0[bS - 1] + 1;
            int delta = iclip(-tc, tc,
                asr(4 * (q0 - p0) + (p1 - q1) + 4, 3));
            pss(pl, x, ey - 1, p0 + delta);
            pss(pl, x, ey, q0 - delta);
        } else {
            ps(pl, x, ey - 1, (2 * p1 + p0 + q1 + 2) >> 2);
            ps(pl, x, ey, (2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

static void filter_hor_chroma(plane_t *pl, int ey, int x0,
                              int bS, const thr_t *t) {
    if (bS == 0) return;
    chroma_hor_pair(pl, ey, x0, bS, t);
    chroma_hor_pair(pl, ey, x0 + 1, bS, t);
}

/* ------------------------------------------------------------------ */
/* Whole picture                                                       */
/* ------------------------------------------------------------------ */

static void filter_chroma_plane(plane_t *pl, const dref_pic *pic,
                                int mc, int mr, const bs_grid_t *bs,
                                const thr_t thr[3]) {
    int cx = mc * 8, cy = mr * 8;
    int v, il;

    /* Two 4-luma-row halves = four chroma rows per iteration.  The call
     * order matches the library: both outer pieces then the inner edge. */
    for (v = 0; v < 2; ++v) {
        int j0 = 2 * v;
        const thr_t *tl = &thr[THR_LEFT];
        const thr_t *th = v == 0 ? &thr[THR_TOP] : &thr[THR_INNER];

        filter_ver_chroma(pl, cx,     cy + 2 * j0,
                          bs->left[(j0 + 0) * 4 + 0], tl);
        filter_ver_chroma(pl, cx,     cy + 2 * (j0 + 1),
                          bs->left[(j0 + 1) * 4 + 0], tl);
        filter_ver_chroma(pl, cx + 4, cy + 2 * j0,
                          bs->left[(j0 + 0) * 4 + 2], &thr[THR_INNER]);
        filter_ver_chroma(pl, cx + 4, cy + 2 * (j0 + 1),
                          bs->left[(j0 + 1) * 4 + 2], &thr[THR_INNER]);

        /* Horizontal edge: top outer on v=0 (y=cy), the single inner
         * chroma edge on v=1 (y=cy+4, sourced from luma band 2). */
        if (v == 0) {
            for (il = 0; il < 4; ++il)
                filter_hor_chroma(pl, cy, cx + 2 * il, bs->top[il], th);
        } else {
            for (il = 0; il < 4; ++il)
                filter_hor_chroma(pl, cy + 4, cx + 2 * il,
                                  bs->top[8 + il], th);
        }
    }
    (void)pic;
}

void dref_filter(dref_pic *pic) {
    int W = pic->wMb, H = pic->hMb;
    plane_t ypl = { pic->y, W * 16 };
    plane_t cbpl = { pic->cb, W * 8 };
    plane_t crpl = { pic->cr, W * 8 };
    int mr, mc, j, i;

    for (mr = 0; mr < H; ++mr) {
        for (mc = 0; mc < W; ++mc) {
            const dref_mb *m = &pic->mb[mr * W + mc];
            const dref_mb *A = mb_at(pic, mc - 1, mr);
            const dref_mb *B = mb_at(pic, mc, mr - 1);
            bs_grid_t bs;
            thr_t thrY[3], thrC[3];
            int X = mc * 16, Y = mr * 16;

            build_bs(pic, mc, mr, &bs);

            luma_inner_thr(m, &thrY[THR_INNER]);
            if (bs.hasTop)
                luma_outer_thr(m, B->qpY, &thrY[THR_TOP]);
            if (bs.hasLeft)
                luma_outer_thr(m, A->qpY, &thrY[THR_LEFT]);

            chroma_inner_thr(m, &thrC[THR_INNER]);
            if (bs.hasTop)
                chroma_outer_thr(m, B->qpY, &thrC[THR_TOP]);
            if (bs.hasLeft)
                chroma_outer_thr(m, A->qpY, &thrC[THR_LEFT]);

            /* Luma: per 4-row band, all vertical edge segments first,
             * then the horizontal edge segments.  Horizontal reads the
             * pixels just written by vertical filtering in this band, and
             * earlier MBs/bands have already committed. */
            for (j = 0; j < 4; ++j) {
                int y0 = Y + 4 * j;
                for (i = 0; i < 4; ++i) {
                    int b = bs.left[j * 4 + i];
                    if (b)
                        filter_ver_luma(&ypl, X + 4 * i, y0, b,
                                        i == 0 ? &thrY[THR_LEFT]
                                               : &thrY[THR_INNER]);
                }
                {
                    const thr_t *th = (j == 0)
                        ? &thrY[THR_TOP] : &thrY[THR_INNER];
                    for (i = 0; i < 4; ++i) {
                        int b = bs.top[j * 4 + i];
                        if (b)
                            filter_hor_luma(&ypl, Y + 4 * j, X + 4 * i,
                                            b, th);
                    }
                }
            }

            filter_chroma_plane(&cbpl, pic, mc, mr, &bs, thrC);
            filter_chroma_plane(&crpl, pic, mc, mr, &bs, thrC);
        }
    }
}
