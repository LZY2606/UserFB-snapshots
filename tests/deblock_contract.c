/*
 * deblock_contract.c - whole-picture numerical regression for the
 * h264bsd portable C deblocking path.
 *
 * Every case builds a REAL image_t + mbStorage_t grid, calls the production
 * h264bsdFilterPicture() (or the public h264bsdDecode entry in one case),
 * and byte-compares all Y/Cb/Cr planes with an independent coordinate
 * reference (deblock_ref.c/.h) adapted through deblock_adapter.h.
 *
 * This file intentionally defines no main(); task_support/driver.c owns the
 * runner and reports the strength/threshold/order groups.
 */
#include "check.h"

#include "h264bsd_decoder.h"
#include "h264bsd_util.h"

#include "deblock_adapter.h"

/* The fixed "target" build line compiles only this translation unit from
 * tests/, so pull the independently authored reference implementation in
 * textually.  It remains a separate module (deblock_ref.{h,c}). */
#include "deblock_ref.c"

#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Small scenario builder                                              */
/* ------------------------------------------------------------------ */

static int ref_token[8];

typedef struct {
    int W, H, n;
    dref_mb *mb;
    uint8_t *y, *cb, *cr;
} fx_t;

static fx_t *fx_new(int W, int H) {
    int n = W * H;
    fx_t *f = (fx_t *)calloc(1, sizeof *f);
    int i, q;
    f->W = W; f->H = H; f->n = n;
    f->mb = (dref_mb *)calloc((size_t)n, sizeof(dref_mb));
    f->y = (uint8_t *)malloc((size_t)n * 256);
    f->cb = (uint8_t *)malloc((size_t)n * 64);
    f->cr = (uint8_t *)malloc((size_t)n * 64);
    if (!f || !f->mb || !f->y || !f->cb || !f->cr) abort();
    for (i = 0; i < n; ++i) {
        dref_mb *m = &f->mb[i];
        m->intra = 0;
        m->part = DREF_PART_16X16;
        m->qpY = 28;
        m->chromaQpIndexOffset = 0;
        m->offA = 0; m->offB = 0;
        m->sliceId = (unsigned)(i / W) + 1;
        m->disableIdc = 0;
        for (q = 0; q < 4; ++q) m->ref[q] = &ref_token[0];
    }
    memset(f->y, 128, (size_t)n * 256);
    memset(f->cb, 128, (size_t)n * 64);
    memset(f->cr, 128, (size_t)n * 64);
    return f;
}

static void fx_free(fx_t *f) {
    if (!f) return;
    free(f->mb); free(f->y); free(f->cb); free(f->cr); free(f);
}

static ad_scenario fx_scn(fx_t *f) {
    ad_scenario s;
    s.wMb = f->W; s.hMb = f->H; s.mbs = f->mb;
    s.y = f->y; s.cb = f->cb; s.cr = f->cr;
    return s;
}

/* Run production vs reference, print first coordinate diff, and assert. */
#define FX_CHECK(f) do {                                                 \
        ad_scenario scn_ = fx_scn(f);                                    \
        ad_result rr_;                                                   \
        int ok_ = ad_run_and_compare(&scn_, &rr_);                       \
        if (!ok_) printf("  %s\n", rr_.first);                           \
        TASK_CHECK(ok_);                                                 \
        TASK_CHECK(!rr_.frontGuardBad && !rr_.rearGuardBad);            \
    } while (0)

/* Filter the scenario planes with the reference so a case can make concrete
 * numeric assertions on the expected output.  Call after FX_CHECK. */
static void fx_commit_ref(fx_t *f) {
    dref_pic p;
    p.wMb = f->W; p.hMb = f->H; p.mb = f->mb;
    p.y = f->y; p.cb = f->cb; p.cr = f->cr;
    dref_filter(&p);
}

static long count_neq(const uint8_t *a, const uint8_t *b, int n) {
    long c = 0; int i;
    for (i = 0; i < n; ++i) if (a[i] != b[i]) ++c;
    return c;
}

static uint32_t case_rng = 0x1234abcdU;
static unsigned crand(void) {
    case_rng = case_rng * 1664525U + 1013904223U;
    return case_rng >> 16;
}

/* Smooth 4x4-step texture with mild deterministic jitter.  Step amplitudes
 * are kept in the range the QP28 alpha/beta gates usually pass. */
static void tex_smooth(fx_t *f, unsigned seed, int amp) {
    int x, y, W = f->W * 16, H = f->H * 16;
    case_rng = seed ? seed : 1;
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x) {
            int v = 128 + amp * (((x >> 2) + 2 * (y >> 2)) % 7 - 3)
                        + (int)(crand() % 5) - 2;
            if (v < 0) v = 0; if (v > 255) v = 255;
            f->y[y * W + x] = (uint8_t)v;
        }
    for (y = 0; y < H / 2; ++y)
        for (x = 0; x < W / 2; ++x) {
            int v = 128 + (amp / 2) * (((x >> 1) + (y >> 1)) % 5 - 2)
                        + (int)(crand() % 3) - 1;
            f->cb[y * (W / 2) + x] = (uint8_t)(v + 8);
            f->cr[y * (W / 2) + x] = (uint8_t)(v - 8);
        }
}

/* ------------------------------------------------------------------ */
/* STRENGTH group                                                      */
/* ------------------------------------------------------------------ */

/* Intra MBs: cross-MB edges bS=4, inner edges bS=3 must move pixels. */
static int case_strength_intra_edges(void) {
    fx_t *f = fx_new(2, 2);
    uint8_t *before;
    long changed;
    int i;
    for (i = 0; i < f->n; ++i) f->mb[i].intra = 1;
    tex_smooth(f, 11, 9);

    before = malloc((size_t)f->n * 384);
    memcpy(before, f->y, (size_t)f->n * 256);
    FX_CHECK(f);
    fx_commit_ref(f);
    changed = count_neq(before, f->y, f->n * 256);
    /* Strong/normal intra filtering must be visibly active. */
    TASK_CHECK(changed > 40);
    /* A specific inner vertical edge of MB0 must have moved. */
    TASK_CHECK(f->y[0 * (f->W * 16) + 3] != before[3] ||
               f->y[4] != before[4]);
    free(before); fx_free(f);
    return 0;
}

/* Inter coefficient presence selects bS=2 and changes edge pixels; an
 * otherwise identical all-zero grid selects bS=0 and preserves pixels. */
static int case_strength_residual_edges(void) {
    fx_t *a = fx_new(1, 1);
    fx_t *b = fx_new(1, 1);
    uint8_t *beforeA;
    tex_smooth(a, 21, 7);
    tex_smooth(b, 21, 7);
    a->mb[0].part = DREF_PART_16X16;
    b->mb[0].part = DREF_PART_16X16;
    /* Coefficient on natural block 0 makes the edge between natural blocks
     * 0 and 1 a bS=2 edge through the 16x16 shortcut. */
    a->mb[0].coeff[0] = 1;

    FX_CHECK(a);
    FX_CHECK(b);

    beforeA = malloc(256);
    memcpy(beforeA, a->y, 256);
    fx_commit_ref(a);
    TASK_CHECK(count_neq(beforeA + 0, a->y, 256) > 0);
    free(beforeA);

    /* bS=0 twin must be completely untouched by the reference. */
    {
        uint8_t *beforeB = malloc(384);
        memcpy(beforeB, b->y, 256);
        memcpy(beforeB + 256, b->cb, 64);
        memcpy(beforeB + 320, b->cr, 64);
        fx_commit_ref(b);
        TASK_CHECK(memcmp(beforeB, b->y, 256) == 0);
        TASK_CHECK(memcmp(beforeB + 256, b->cb, 64) == 0);
        TASK_CHECK(memcmp(beforeB + 320, b->cr, 64) == 0);
        free(beforeB);
    }
    fx_free(a); fx_free(b);
    return 0;
}

/* refAddr pointer identity matters even when refPic index is identical. */
static int case_strength_ref_identity(void) {
    fx_t *same = fx_new(1, 1);
    fx_t *diff = fx_new(1, 1);
    uint8_t before[256];
    tex_smooth(same, 31, 8);
    tex_smooth(diff, 31, 8);
    same->mb[0].part = DREF_PART_8X8;
    diff->mb[0].part = DREF_PART_8X8;
    /* Edge between TL quadrant (blocks 0/1 row) and TR: force different
     * reference identities on the two sides for the "diff" grid. */
    diff->mb[0].ref[0] = &ref_token[0];
    diff->mb[0].ref[1] = &ref_token[1];
    same->mb[0].ref[0] = &ref_token[0];
    same->mb[0].ref[1] = &ref_token[0];

    FX_CHECK(same);
    FX_CHECK(diff);

    memcpy(before, diff->y, 256);
    fx_commit_ref(diff);
    /* Different real reference storage -> bS=1 -> pixels must move. */
    TASK_CHECK(count_neq(before, diff->y, 256) > 0);

    memcpy(before, same->y, 256);
    fx_commit_ref(same);
    /* Same identity, zero MVs, zero coeff -> every edge bS=0. */
    TASK_CHECK(count_neq(before, same->y, 256) == 0);
    fx_free(same); fx_free(diff);
    return 0;
}

/* MV component difference threshold is exactly 4 quarter-luma units. */
static int case_strength_mv_threshold(void) {
    int d;
    for (d = 0; d <= 5; ++d) {
        fx_t *f = fx_new(1, 1);
        uint8_t before[256];
        long ch;
        tex_smooth(f, 41, 8);
        f->mb[0].part = DREF_PART_8X8;
        /* Natural blocks 0 and 1 share a vertical edge at x=4. */
        f->mb[0].mvx[0] = 0;
        f->mb[0].mvx[1] = (d == 3 || d == 4) ? d : (d == 5 ? -4 : d);
        /* Use explicit boundary values on two dedicated iterations. */
        if (d == 0) { f->mb[0].mvx[1] = 3; }
        if (d == 1) { f->mb[0].mvx[1] = 4; }
        if (d == 2) { f->mb[0].mvy[4] = -3; f->mb[0].mvy[0] = 0; }
        if (d == 3) { f->mb[0].mvy[4] = -4; }
        FX_CHECK(f);
        memcpy(before, f->y, 256);
        fx_commit_ref(f);
        ch = count_neq(before, f->y, 256);
        if (d == 0 || d == 2)
            TASK_CHECK(ch == 0);    /* |MV diff| 3 is below threshold */
        else
            TASK_CHECK(ch > 0);     /* |MV diff| 4 selects bS=1 */
        fx_free(f);
    }
    return 0;
}

/* All four partition shapes must make the production shortcut branches
 * agree with coordinate-derived strengths. */
static int case_strength_partitions(void) {
    int part;
    for (part = 0; part < 4; ++part) {
        unsigned seed;
        for (seed = 1; seed <= 6; ++seed) {
            fx_t *f = fx_new(2, 2);
            int i;
            for (i = 0; i < f->n; ++i) {
                f->mb[i].part = part;
                /* deterministic scattered coefficients */
                int k;
                for (k = 0; k < 16; ++k)
                    f->mb[i].coeff[k] = (crand() >> 4) & 1;
            }
            tex_smooth(f, 100u + seed * 17u + part * 7u, 10);
            FX_CHECK(f);
            fx_free(f);
        }
    }
    return 0;
}

/* sliceId + disable_idc gate outer edges only; inner edges stay active. */
static int case_strength_slice_gating(void) {
    int idc;
    for (idc = 0; idc <= 2; ++idc) {
        fx_t *f = fx_new(2, 1);
        uint8_t before[32 * 16];
        long outerChange, innerChange;
        int x, y;
        /* Vertically invariant 4-column ramp: horizontal edges are no-ops
         * (adjacent rows identical), so changes in columns 15/16 can only
         * come from the cross-MB vertical edge at x=16.  Every 4px step is
         * small enough for the QP28 alpha/beta gates. */
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 32; ++x)
                f->y[y * 32 + x] = (uint8_t)(104 + 5 * (x >> 2));
        f->mb[0].intra = 1;
        f->mb[1].intra = 1;
        f->mb[0].sliceId = 1;
        f->mb[1].sliceId = 2;   /* vertical slice boundary between MBs */
        f->mb[0].disableIdc = (unsigned)idc;
        f->mb[1].disableIdc = (unsigned)idc;
        FX_CHECK(f);

        memcpy(before, f->y, sizeof before);
        fx_commit_ref(f);
        /* Outer cross-MB vertical edge is at x=16.  idc==1 disables
         * everything; idc==2 + different slice disables the outer edge;
         * idc==0 filters it.  Compare the two p0/p1 columns. */
        outerChange = 0;
        {
            int y;
            for (y = 0; y < 16; ++y) {
                int w = f->W * 16;
                if (f->y[y * w + 15] != before[y * w + 15] ||
                    f->y[y * w + 16] != before[y * w + 16])
                    ++outerChange;
            }
        }
        /* An inner edge of MB0 (x=4) must remain active unless idc==1. */
        innerChange = count_neq(before + 0, f->y, 256);
        if (idc == 0) TASK_CHECK(outerChange > 0);
        if (idc == 2) TASK_CHECK(outerChange == 0);
        if (idc == 1) TASK_CHECK(innerChange == 0);
        if (idc != 1) TASK_CHECK(innerChange > 0);
        fx_free(f);
    }
    return 0;
}

/* Cross-macroblock edges: coefficient on either side, distinct refAddr
 * storage and the MV threshold all drive EdgeBoundaryStrength. */
static int case_strength_cross_mb_sources(void) {
    enum { COEFF = 0, REFID = 1, MV3 = 2, MV4 = 3 };
    int mode;
    for (mode = 0; mode < 4; ++mode) {
        fx_t *f = fx_new(2, 1);
        uint8_t before[32 * 16];
        int x, y, outerChange = 0;
        /* vertically invariant active ramp -> horizontal edges are no-ops */
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 32; ++x)
                f->y[y * 32 + x] = (uint8_t)(104 + 5 * (x >> 2));
        f->mb[0].part = DREF_PART_16X16;
        f->mb[1].part = DREF_PART_16X16;
        if (mode == COEFF)
            f->mb[1].coeff[0] = 1;       /* right side has residual */
        else if (mode == REFID)
            f->mb[1].ref[0] = &ref_token[3];
        else {
            int k;
            for (k = 0; k < 16; ++k) f->mb[1].mvx[k] = (mode == MV3) ? 3 : 4;
        }
        FX_CHECK(f);
        memcpy(before, f->y, sizeof before);
        fx_commit_ref(f);
        for (y = 0; y < 16; ++y)
            if (f->y[y * 32 + 15] != before[y * 32 + 15] ||
                f->y[y * 32 + 16] != before[y * 32 + 16])
                ++outerChange;
        if (mode == MV3)
            TASK_CHECK(outerChange == 0);  /* |MV diff| 3 stays bS=0 */
        else
            TASK_CHECK(outerChange > 0);   /* coeff/ref/|mv|>=4 filters */
        fx_free(f);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* THRESHOLD group                                                     */
/* ------------------------------------------------------------------ */

/* Chroma outer edges: clip+map each side QP, then rounded average.  Sweep
 * QP pairs across the nonlinear QpC knee with chroma offsets and
 * independent alpha/beta offsets; byte equality pins the ordering. */
static int case_threshold_chroma_qp_mapping(void) {
    static const int qpPair[][2] = {
        {29, 35}, {30, 37}, {31, 38}, {32, 40}, {27, 36}, {33, 42}
    };
    static const int coffs[] = {-4, -1, 0, 2, 4};
    static const int offAs[] = {-6, -2, 0, 3, 6};
    static const int offBs[] = {-5, -1, 2, 5};
    unsigned pi, ci, ai, bi, orient;
    for (orient = 0; orient < 2; ++orient) {
      for (pi = 0; pi < sizeof qpPair / sizeof qpPair[0]; ++pi) {
       for (ci = 0; ci < sizeof coffs / sizeof coffs[0]; ++ci) {
        for (ai = 0; ai < sizeof offAs / sizeof offAs[0]; ++ai) {
         for (bi = 0; bi < sizeof offBs / sizeof offBs[0]; ++bi) {
            fx_t *f = orient ? fx_new(1, 2) : fx_new(2, 1);
            int i;
            /* inter bS=2 everywhere via coefficients so chroma edges run */
            for (i = 0; i < f->n; ++i) {
                int k;
                f->mb[i].qpY = (unsigned)qpPair[pi][i & 1];
                f->mb[i].chromaQpIndexOffset = coffs[ci];
                f->mb[i].offA = 2 * offAs[ai];
                f->mb[i].offB = 2 * offBs[bi];
                for (k = 0; k < 16; ++k) f->mb[i].coeff[k] = 1;
            }
            /* active chroma ramp + luma kept quiet/flat */
            memset(f->y, 128, (size_t)f->n * 256);
            {
                int Wc = f->W * 8, Hc = f->H * 8, x, y;
                for (y = 0; y < Hc; ++y)
                    for (x = 0; x < Wc; ++x) {
                        int v = 96 + 5 * ((x + 2 * y) % 9);
                        f->cb[y * Wc + x] = (uint8_t)v;
                        f->cr[y * Wc + x] = (uint8_t)(v + 10);
                    }
            }
            {
                ad_scenario scn_ = fx_scn(f);
                ad_result rr_;
                int ok_ = ad_run_and_compare(&scn_, &rr_);
                if (!ok_)
                    printf("  CHROMA_CFG orient=%u qp=(%d,%d) chromaOff=%d "
                           "offA=%d offB=%d\n  %s\n",
                           orient, qpPair[pi][0], qpPair[pi][1],
                           coffs[ci], 2 * offAs[ai], 2 * offBs[bi],
                           rr_.first);
                TASK_CHECK(ok_);
            }
            fx_free(f);
         }
        }
       }
      }
    }
    /* Pin the normative nonlinear mapping values used by the sweep. */
    TASK_CHECK(dref_qpc(30) == 29 && dref_qpc(31) == 30);
    TASK_CHECK(dref_qpc(36) == 34 && dref_qpc(39) == 35);
    return 0;
}

/* alpha/beta let some textures filter and block others. */
static int case_threshold_alpha_beta_gates(void) {
    int strongStep;
    for (strongStep = 0; strongStep < 2; ++strongStep) {
        fx_t *f = fx_new(2, 1);
        int y;
        f->mb[0].intra = 1; f->mb[1].intra = 1;
        f->mb[0].qpY = 30; f->mb[1].qpY = 30;
        /* Flat MB interiors with a controlled step across x=16. */
        for (y = 0; y < 16; ++y) {
            int x;
            for (x = 0; x < 16; ++x) f->y[y * 32 + x] = 110;
            for (x = 16; x < 32; ++x)
                f->y[y * 32 + x] = (uint8_t)(strongStep ? 200 : 116);
        }
        FX_CHECK(f);
        {
            uint8_t before[32 * 16];
            memcpy(before, f->y, sizeof before);
            fx_commit_ref(f);
            if (strongStep)
                TASK_CHECK(count_neq(before, f->y, 32 * 16) == 0);
            else
                TASK_CHECK(count_neq(before, f->y, 32 * 16) > 0);
        }
        fx_free(f);
    }
    return 0;
}

/* Concrete hand-computed numeric vectors for normal (bS<4, tc clipping)
 * and strong (bS=4) luma filtering, including conditional neighbor taps. */
static int case_threshold_tc_strong_numbers(void) {
    /* NORMAL bS=2 vertical inner edge x=4, qpY=28: alpha=20 beta=7 and
     * tc0[28][1]=1.  Row vector p2 p1 p0 | q0 q1 q2 =
     * 101 102 104 | 112 113 114 passes all gates; both side conditions
     * hold.  The side-tap values are +2 and -2 but get clipped to tc=+/-1,
     * and the central delta clips to the widened bound +/-3.  Working the
     * clause-8.7.2 equations by hand gives x2=103 x3=107 x4=109 x5=112,
     * pinned literally below. */
    {
        static const uint8_t pat[16] = {
            100,101,102,104, 112,113,114,115,
            140,140,140,140, 140,140,140,140
        };
        fx_t *f = fx_new(1, 1);
        int y;
        f->mb[0].part = DREF_PART_16X16;
        f->mb[0].qpY = 28;
        f->mb[0].coeff[0] = 1;
        f->mb[0].coeff[1] = 1;   /* natural 0|1 edge -> bS=2 */
        for (y = 0; y < 16; ++y) memcpy(f->y + y * 16, pat, 16);
        FX_CHECK(f);
        fx_commit_ref(f);
        TASK_CHECK(f->y[2] == 103);
        TASK_CHECK(f->y[3] == 107);
        TASK_CHECK(f->y[4] == 109);
        TASK_CHECK(f->y[5] == 112);
        fx_free(f);
    }

    /* STRONG bS=4 cross-MB vertical edge x=16, qpY=32 (alpha=28 beta=8).
     * Flat 120 on the p side and 126 on the q side makes the "small
     * difference" test (6 < (28>>2)+2 = 9) and both p2/q2 conditions true,
     * so all three taps update on each side.  Hand evaluation gives the
     * pinned row-0 columns 13..18 below (p2 p1 p0 | q0 q1 q2). */
    {
        fx_t *g = fx_new(2, 1);
        int x, yy;
        g->mb[0].intra = 1; g->mb[1].intra = 1;
        g->mb[0].qpY = 32; g->mb[1].qpY = 32;
        for (yy = 0; yy < 16; ++yy)
            for (x = 0; x < 32; ++x)
                g->y[yy * 32 + x] = (x < 16) ? 120 : 126;
        FX_CHECK(g);
        fx_commit_ref(g);
        TASK_CHECK(g->y[13] == 121);
        TASK_CHECK(g->y[14] == 122);
        TASK_CHECK(g->y[15] == 122);
        TASK_CHECK(g->y[16] == 124);
        TASK_CHECK(g->y[17] == 125);
        TASK_CHECK(g->y[18] == 125);
        fx_free(g);
    }

    /* Same strong edge but p=112/q=126 (difference 14): the main gate
     * passes (14 < alpha=28) while the "small" test fails
     * (14 >= (28>>2)+2 = 9), so each side takes the single-tap else
     * formula and p1/q1 are NOT rewritten.  Hand values: x15=116 x16=123,
     * with x14=112 and x17=126 preserved. */
    {
        fx_t *h = fx_new(2, 1);
        int x, yy;
        h->mb[0].intra = 1; h->mb[1].intra = 1;
        h->mb[0].qpY = 32; h->mb[1].qpY = 32;
        for (yy = 0; yy < 16; ++yy)
            for (x = 0; x < 32; ++x)
                h->y[yy * 32 + x] = (x < 16) ? 112 : 126;
        FX_CHECK(h);
        fx_commit_ref(h);
        TASK_CHECK(h->y[14] == 112);
        TASK_CHECK(h->y[15] == 116);
        TASK_CHECK(h->y[16] == 123);
        TASK_CHECK(h->y[17] == 126);
        fx_free(h);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ORDER group                                                         */
/* ------------------------------------------------------------------ */

/* Cross-edge textures: horizontal filtering in a band reads pixels that
 * vertical filtering just wrote in the same band. */
static int case_order_cross_edges(void) {
    unsigned seed;
    for (seed = 1; seed <= 12; ++seed) {
        fx_t *f = fx_new(2, 2);
        int i;
        for (i = 0; i < f->n; ++i) f->mb[i].intra = 1;  /* bS 3/4 */
        /* Cross-hatch texture makes both orientations active. */
        {
            int x, y, W = f->W * 16, H = f->H * 16;
            case_rng = 700u + seed;
            for (y = 0; y < H; ++y)
                for (x = 0; x < W; ++x) {
                    int v = 128
                        + 10 * (((x >> 2) & 1) ? 1 : -1)
                        + 8 * (((y >> 2) & 1) ? 1 : -1)
                        + (int)(crand() % 5) - 2;
                    if (v < 0) v = 0; if (v > 255) v = 255;
                    f->y[y * W + x] = (uint8_t)v;
                }
        }
        FX_CHECK(f);
        fx_free(f);
    }
    return 0;
}

/* MB raster ordering: right/bottom MBs read pixels committed by earlier
 * MBs; a multi-MB sweep pins the whole-raster commit order. */
static int case_order_mb_raster(void) {
    int cfg;
    for (cfg = 0; cfg < 8; ++cfg) {
        fx_t *f = fx_new(3, 3);
        int i;
        for (i = 0; i < f->n; ++i) {
            f->mb[i].intra = (cfg & 1);
            f->mb[i].qpY = (unsigned)(26 + (i % 5) * 2);
            f->mb[i].offA = (cfg & 2) ? 2 : 0;
            f->mb[i].offB = (cfg & 4) ? -2 : 0;
            if (!f->mb[i].intra) f->mb[i].coeff[(i * 3) & 15] = 1;
        }
        tex_smooth(f, 900u + cfg * 31u, 12);
        FX_CHECK(f);
        fx_free(f);
    }
    return 0;
}

/* Outer sentinels and pixels around fully disabled edges must survive. */
static int case_order_sentinels_disabled(void) {
    fx_t *f = fx_new(3, 2);
    int i;
    ad_scenario scn;
    ad_result rr;
    for (i = 0; i < f->n; ++i) {
        f->mb[i].intra = 1;
        f->mb[i].disableIdc = 1;   /* switch deblocking off entirely */
    }
    tex_smooth(f, 1234, 12);
    scn = fx_scn(f);
    TASK_CHECK(ad_run_and_compare(&scn, &rr));
    TASK_CHECK(!rr.frontGuardBad && !rr.rearGuardBad);
    fx_free(f);

    /* idc=2 with every MB its own slice id: all outer MB edges off while
     * inner edges stay on; sentinels still intact and planes agree. */
    f = fx_new(3, 2);
    for (i = 0; i < f->n; ++i) {
        f->mb[i].intra = 1;
        f->mb[i].disableIdc = 2;
        f->mb[i].sliceId = (unsigned)(i + 1);
    }
    tex_smooth(f, 4321, 11);
    FX_CHECK(f);
    fx_free(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* PUBLIC ENTRY: h264bsdDecode -> h264bsdNextOutputPicture             */
/* ------------------------------------------------------------------ */

#include "deblock_stream.h"

static int case_public_decode_stream(void) {
    return dstream_run_contract();
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"strength",  "intra_edges",          case_strength_intra_edges},
    {"strength",  "residual_selects_bs2", case_strength_residual_edges},
    {"strength",  "refaddr_identity",     case_strength_ref_identity},
    {"strength",  "mv_quarter_threshold", case_strength_mv_threshold},
    {"strength",  "partition_shapes",     case_strength_partitions},
    {"strength",  "cross_mb_sources",     case_strength_cross_mb_sources},
    {"strength",  "sliceid_disable_idc",  case_strength_slice_gating},

    {"threshold", "chroma_qp_map_order",  case_threshold_chroma_qp_mapping},
    {"threshold", "alpha_beta_gates",     case_threshold_alpha_beta_gates},
    {"threshold", "tc_and_strong_taps",   case_threshold_tc_strong_numbers},
    {"threshold", "public_decode_three_planes",
                                          case_public_decode_stream},

    {"order",     "cross_edges_v_then_h", case_order_cross_edges},
    {"order",     "mb_raster_commit",     case_order_mb_raster},
    {"order",     "sentinels_disabled",   case_order_sentinels_disabled}
};

const size_t task_case_count =
    sizeof task_cases / sizeof task_cases[0];
