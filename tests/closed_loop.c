/*
 * tests/closed_loop.c -- independent fixed-point regression tests for the
 * libgsm long term predictor (LTP), the regular pulse excitation (RPE)
 * section and the closed encoder loop.
 *
 * The expected values below are produced by small, independently written
 * reference models.  They use only wide (32 bit) integers and spell out
 * every floor division (arithmetic shift right), rounding shift and
 * saturation explicitly.  They never call the production LTP, RPE,
 * quantization or dequantization routines and the loops are plain for
 * loops rather than copies of the unrolled production loops.
 *
 * The production library is only used as the system under test: every
 * expected scalar/vector comes from the reference model below.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "task_support/check.h"
#include "private.h"
#include "gsm.h"

/* ====================================================================== */
/* Independent fixed point primitives.                                    */
/*                                                                         */
/* All intermediate products live in C "long" (>= 32 bits).  Shifts on    */
/* negative values use arithmetic shift right, i.e. rounding towards      */
/* minus infinity (floor for the integer quotient).                       */
/* ====================================================================== */

static int ref_sasr(long x, int by) { return (int)(x >> by); }

static int ref_abs16(int a)
{
    return a == -32768 ? 32767 : (a < 0 ? -a : a);
}

static int ref_sat16(long x)
{
    return x < -32768 ? -32768 : x > 32767 ? 32767 : (int)x;
}

/* GSM_MULT:   floor(a*b / 32768) */
static int ref_mult(int a, int b)
{
    if (a == -32768 && b == -32768) return 32767;
    return ref_sasr((long)a * b, 15);
}

/* GSM_MULT_R: round(a*b / 32768) */
static int ref_mult_r(int a, int b)
{
    if (a == -32768 && b == -32768) return 32767;
    return (int)(((long)a * b + 16384) >> 15);
}

static int ref_add16(int a, int b) { return ref_sat16((long)a + b); }
static int ref_sub16(int a, int b) { return ref_sat16((long)a - b); }

/* gsm_asl with the negative-shift fallback to arithmetic shift right. */
static int ref_asl16(int a, int n)
{
    if (n >= 16) return 0;
    if (n <= -16) return -(a < 0);
    if (n < 0) return ref_sasr((long)a, -n);
    return a << n;
}

/* gsm_norm: left shifts needed to bring a positive magnitude into the
 * top two bits of a 32 bit word. */
static int ref_norm(long a)
{
    int n = 0;
    if (a < 0) a = ~a;
    if (a == 0) return 0;
    while (a < (long)1 << 30) { a <<= 1; n++; }
    return n;
}

/* Format constants (table 4.3a/b, 4.4, 4.5, 4.6), values copied verbatim
 * from src/table.c so the reference does not depend on the library. */
static const int REF_DLB[4]   = { 6554, 16384, 26214, 32767 };
static const int REF_QLB[4]   = { 3277, 11469, 21299, 32767 };
static const int REF_H[11]    = { -134, -374, 0, 2054, 5741, 8192,
                                  5741, 2054, 0, -374, -134 };
static const int REF_NRFAC[8] = { 29128, 26215, 23832, 21846,
                                  20165, 18725, 17476, 16384 };
static const int REF_FAC[8]   = { 18431, 20479, 22527, 24575,
                                  26623, 28671, 30719, 32767 };

/* ====================================================================== */
/* Assertion infrastructure.                                              */
/*                                                                         */
/* Every comparison goes through task_check so even a fully passing case  */
/* registers real checks.  On the first mismatch we print CONTENT_        */
/* MISMATCH with the stage, frame/subframe and the deviating field or     */
/* sample, then TASK_CHECK makes the fixed runner reject with exit 4.     */
/* ====================================================================== */

static int ref_expect(int cond, const char *stage, const char *fmt, ...)
{
    if (!cond) {
        va_list ap;
        fprintf(stderr, "CONTENT_MISMATCH stage=%s ", stage);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
    return task_check(!!cond, "ref_expect", __FILE__, __LINE__);
}

#define EXPECT(cond, stage, ...) \
    do { if (!ref_expect(!!(cond), stage, __VA_ARGS__)) return 1; } while (0)

/* ====================================================================== */
/* LTP reference model (section 4.2.11/4.2.12).                           */
/*                                                                         */
/* dp is stored as a plain 120 entry history: dp[i] holds the sample at   */
/* the production index i-120, so production dp[k-lambda] is ref dp[k -   */
/* lambda + 120].                                                          */
/* ====================================================================== */

struct ltp_result {
    int scal;                 /* residual scaling actually selected */
    int Nc;                   /* chosen lag */
    int bc;                   /* coded gain level 0..3 */
    long corr[81];            /* full cross correlation for lags 40..120 */
    int tie_count;            /* number of lags sharing the best value */
    int all_non_positive;     /* every correlation <= 0 */
    int dpp[40];              /* prediction */
    int e[40];                /* residual after subtraction */
};

static void ref_ltp(const int *d, const int *dp, struct ltp_result *r)
{
    int k, lambda;
    int dmax = 0, scal, temp;
    long L_max, L_power;

    /* Scaling from the peak magnitude of the current residual. */
    for (k = 0; k < 40; k++) {
        int a = ref_abs16(d[k]);
        if (a > dmax) dmax = a;
    }
    if (dmax == 0) scal = 0;
    else {
        temp = ref_norm((long)dmax << 16);
        scal = temp > 6 ? 0 : 6 - temp;
    }
    r->scal = scal;

    int wt[40];
    for (k = 0; k < 40; k++) wt[k] = ref_sasr(d[k], scal);

    /* Independent sweep of every lag 40..120.  The comparison is strict:
     * an equal positive correlation keeps the first lag encountered, and
     * with no positive correlation L_max stays 0 and Nc stays 40. */
    L_max = 0;
    r->Nc = 40;
    for (lambda = 40; lambda <= 120; lambda++) {
        long acc = 0;
        for (k = 0; k < 40; k++)
            acc += (long)wt[k] * dp[(k - lambda) + 120];
        r->corr[lambda - 40] = acc;
        if (acc > L_max) { L_max = acc; r->Nc = lambda; }
    }

    long best = r->corr[r->Nc - 40];
    r->tie_count = 0;
    r->all_non_positive = 1;
    for (lambda = 40; lambda <= 120; lambda++) {
        if (r->corr[lambda - 40] == best) r->tie_count++;
        if (r->corr[lambda - 40] > 0) r->all_non_positive = 0;
    }

    /* Rescale the correlation and compute the selected history power. */
    long lm = L_max << 1;
    lm = ref_sasr(lm, 6 - scal);

    L_power = 0;
    for (k = 0; k < 40; k++) {
        long t = ref_sasr((long)dp[(k - r->Nc) + 120], 3);
        L_power += t * t;
    }
    L_power <<= 1;

    int bc;
    if (lm <= 0) bc = 0;
    else if (lm >= L_power) bc = 3;
    else {
        temp = ref_norm(L_power);
        long Rl = ref_sasr(lm << temp, 16);
        long Sl = ref_sasr(L_power << temp, 16);
        for (bc = 0; bc <= 2; bc++)
            if (Rl <= (long)ref_mult((int)Sl, REF_DLB[bc])) break;
    }
    r->bc = bc;

    /* Prediction and subtraction use the decoded gain table 4.3b. */
    for (k = 0; k < 40; k++) {
        r->dpp[k] = ref_mult_r(REF_QLB[bc], dp[(k - r->Nc) + 120]);
        r->e[k]   = ref_sub16(d[k], r->dpp[k]);
    }
}

/* Run the production predictor on the same inputs and compare every
 * output against the reference. */
static int check_ltp_production(const char *name, const int *d,
                                const int *dp, const struct ltp_result *r)
{
    struct gsm_state *S = gsm_create();
    word pd[40], pbase[240], *pdp = pbase + 120;
    word pe[50], pdpp[40], pNc, pbc;
    int k, failed = 0;

    for (k = 0; k < 40; k++) pd[k] = (word)d[k];
    for (k = -120; k < 0; k++) pdp[k] = (word)dp[k + 120];
    memset(pe, 0, sizeof pe);

    Gsm_Long_Term_Predictor(S, pd, pdp, pe + 5, pdpp, &pNc, &pbc);

    if (!ref_expect(pNc == r->Nc, "ltp",
           "%s production Nc got=%d want=%d", name, pNc, r->Nc)) failed = 1;
    if (!ref_expect(pbc == r->bc, "ltp",
           "%s production bc got=%d want=%d", name, pbc, r->bc)) failed = 1;
    for (k = 0; k < 40 && !failed; k++) {
        if (!ref_expect(pdpp[k] == r->dpp[k], "ltp",
               "%s production dpp[%d] got=%d want=%d",
               name, k, pdpp[k], r->dpp[k])) { failed = 1; break; }
        if (!ref_expect(pe[5 + k] == r->e[k], "ltp",
               "%s production e[%d] got=%d want=%d",
               name, k, pe[5 + k], r->e[k])) { failed = 1; break; }
    }
    gsm_destroy(S);
    return failed;
}

/* --- Explicit LTP vectors (40 residual / 120 history samples). -------- */

static const int ltp_unique_d[40] = {
   3460, -3228,  2788,   628,  2924,  1340, -1896, -1652,   864,  3976,
  -3624,  -540,  -672,  1256,  1520,  2600,  3484,  -804,  2332, -3200,
  -3120,  -188,  -240, -1888, -2128, -3256,  1496,  2668,  -504, -1392,
  -1636,  3876,  2656,  1092, -1068,  3940,  3860,  3812, -2608,   296,
};
static const int ltp_unique_dp[120] = {
   -755,  -168,   156,  -298,  -799,   823,  -624,  -213,   912,   539,
    287,  -414,    53,  -962,   957,   779,   349,  -693,  -410,  -864,
    692,   558,  -325,   207,   146,   282,  -630,  -204,    -6,  -426,
    276,  -550,   405,  -660,   839,  -853,   357,   291,  -684,  -590,
    241,   154,   139,   213,   446,   532,   923,  -985,   651,   880,
   -329,   291,  -201,  -241,   582,   374,   -49,   631,   399,  -904,
    172,   -47,  -936,   116,   -71,  -563,   226,   865,  -807,   697,
    157,   731,   335,  -474,  -413,   216,   994,  -906,  -135,  -168,
    314,   380,   650,   871,  -201,   583,  -800,  -780,   -47,   -60,
   -472,  -532,  -814,   374,   667,  -126,  -348,  -409,   969,   664,
    273,  -267,   985,   965,   953,  -652,    74,   -15,   252,   263,
    861,  -999,  -810,  -580,   271,   685,  -133,  -524,   766,   573,
};

static const int ltp_tie_d[40] = {
   3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,
   3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,
  -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000,
  -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000,
};
static const int ltp_tie_dp[120] = {
   3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,
   3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,
  -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000,
  -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000,
   3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,
   3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,
  -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000,
  -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000,
   3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,
   3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,  3000,
  -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000,
  -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000, -3000,
};

static const int ltp_nonpos_d[40] = {
   1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,
   1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,
   1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,
   1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,  1000,
};
static const int ltp_nonpos_dp[120] = {
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
  -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000, -1000,
};

static const int ltp_bc1_d[40] = {
    -27,   102,    58,    55,   -22,     4,     7,    14,    52,    92,
      9,     4,   127,    83,    14,   -45,     5,   -22,   -21,   -13,
    -59,     9,    -4,   -39,     9,   -26,   -33,   -76,    13,   -53,
     43,   -34,   -14,    83,    88,   -21,   -11,   -43,     4,   -20,
};
static const int ltp_bc1_dp[120] = {
   -184,   -19,   -97,   -31,   120,   -30,  -126,   141,   152,    70,
    140,    58,   -91,    76,   -39,  -185,  -147,    14,   126,  -116,
    110,   -11,   -78,    -9,  -141,    89,  -192,    36,  -127,  -197,
    -68,   113,    69,    -5,  -154,    59,   -90,  -183,    52,   -34,
     79,  -191,    56,    40,  -182,    31,   -59,  -140,   161,   174,
     26,   -62,   200,   192,  -135,   -91,     5,    89,   118,   -97,
     69,   102,    54,   -32,   145,    24,    53,    17,  -111,  -182,
   -139,  -167,    17,   -99,  -187,     2,    47,   188,   -65,    99,
    -59,   177,    57,   100,   -72,    -1,    77,   -38,   173,   162,
      7,    61,   188,    93,    36,   -54,    52,  -123,   -50,   -41,
   -130,   -22,     0,   -67,    14,  -125,  -100,  -117,   -39,   -51,
     15,   -70,   -17,   156,   104,   -78,  -102,   -33,   -25,   -69,
};

static const int ltp_bc2_d[40] = {
    -31,   104,    24,     3,     5,   -65,  -119,   -51,   -61,    48,
    -81,  -129,   -11,    45,    69,   -66,    63,     8,   114,    -1,
     48,   -16,   -10,    61,   -50,   100,    73,    31,    10,    85,
     74,    27,   -37,    64,   -87,   -56,    15,   -91,     9,    16,
};
/* ltp_bc2 reuses the ltp_bc1 history. */

/* LTP coverage counters, updated by the cases below. */
static int ltp_scal_switch_cases;
static int ltp_tie_cases;
static int ltp_nonpos_cases;
static int ltp_gain_seen[4];

static int ltp_run_case(const char *name, const int *d, const int *dp,
                        int want_scal, int want_Nc, int want_bc,
                        int want_ties, int want_nonpos)
{
    struct ltp_result r;
    memset(&r, 0, sizeof r);
    ref_ltp(d, dp, &r);

    EXPECT(r.scal == want_scal, "ltp", "%s scal got=%d want=%d",
           name, r.scal, want_scal);
    EXPECT(r.Nc == want_Nc, "ltp", "%s Nc got=%d want=%d",
           name, r.Nc, want_Nc);
    EXPECT(r.bc == want_bc, "ltp", "%s bc got=%d want=%d",
           name, r.bc, want_bc);
    EXPECT(r.tie_count == want_ties, "ltp", "%s tie_count got=%d want=%d",
           name, r.tie_count, want_ties);
    EXPECT(r.all_non_positive == want_nonpos, "ltp",
           "%s all_non_positive got=%d want=%d",
           name, r.all_non_positive, want_nonpos);
    EXPECT(r.Nc >= 40 && r.Nc <= 120, "ltp", "%s Nc out of range %d",
           name, r.Nc);
    EXPECT(r.bc >= 0 && r.bc <= 3, "ltp", "%s bc out of range %d",
           name, r.bc);

    if (check_ltp_production(name, d, dp, &r)) return 1;

    if (r.scal > 0) ltp_scal_switch_cases++;
    if (r.tie_count > 1) ltp_tie_cases++;
    if (r.all_non_positive) ltp_nonpos_cases++;
    ltp_gain_seen[r.bc] = 1;
    return 0;
}

static int case_ltp_unique(void)
{
    /* PRBS history, residual is an amplified copy of the lag-53 window:
     * a unique interior optimum and a non-trivial scaling (scal = 3). */
    return ltp_run_case("unique", ltp_unique_d, ltp_unique_dp, 3, 53, 3, 1, 0);
}
static int case_ltp_tie(void)
{
    /* Period 40 square wave: lags 40, 80 and 120 tie; the strict rule
     * keeps the first one, 40. */
    return ltp_run_case("tie", ltp_tie_d, ltp_tie_dp, 3, 40, 3, 3, 0);
}
static int case_ltp_nonpos(void)
{
    /* Opposite signs everywhere: no positive correlation exists, so Nc
     * stays at 40 and the gain collapses to level 0. */
    return ltp_run_case("nonpos", ltp_nonpos_d, ltp_nonpos_dp,
                        1, 40, 0, 81, 1);
}
static int case_ltp_zero(void)
{
    /* All-zero residual: dmax == 0 forces scal = 0; same Nc/bc fallback. */
    static const int zd[40] = {0};
    int dp[120], i;
    for (i = 0; i < 120; i++) dp[i] = (i % 3 ? 250 : -250);
    return ltp_run_case("zero", zd, dp, 0, 40, 0, 81, 1);
}
static int case_ltp_bc1(void)
{
    /* Weak noisy correlation lands on gain level 1. */
    return ltp_run_case("bc1", ltp_bc1_d, ltp_bc1_dp, 0, 40, 1, 1, 0);
}
static int case_ltp_bc2(void)
{
    /* Same noise history, differently shaped residual: gain level 2. */
    return ltp_run_case("bc2", ltp_bc2_d, ltp_bc1_dp, 0, 57, 2, 1, 0);
}
static int case_ltp_coverage(void)
{
    int levels = 0, i;
    for (i = 0; i < 4; i++) levels += ltp_gain_seen[i];
    printf("COVERAGE ltp scal_switch_cases=%d tie_cases=%d nonpos_cases=%d"
           " gain_levels=%d\n",
           ltp_scal_switch_cases, ltp_tie_cases, ltp_nonpos_cases, levels);
    TASK_CHECK(ltp_scal_switch_cases >= 2);
    TASK_CHECK(ltp_tie_cases >= 1);
    TASK_CHECK(ltp_nonpos_cases >= 2);
    TASK_CHECK(levels >= 3);
    return 0;
}

/* ====================================================================== */
/* RPE reference model (sections 4.2.13..4.2.17).                         */
/* ====================================================================== */

struct rpe_result {
    int x[40];                 /* weighting filter output */
    long energy[4];            /* grid energies (the L_MULT scaled values) */
    int Mc;                    /* selected grid */
    int xM[13];                /* downsampled sequence */
    int xmax;
    int xmaxc;
    int exp;
    int mant;
    int xMc[13];               /* three bit pulses */
    int ep[40];                /* independently reconstructed sparse residual */
};

static void ref_rpe(const int *res40, struct rpe_result *r)
{
    /* Five zero samples on each side; finite 11-tap convolution. */
    int padded[50];
    int k, i, m;
    for (k = 0; k < 5; k++) padded[k] = 0;
    for (k = 0; k < 40; k++) padded[5 + k] = res40[k];
    for (k = 45; k < 50; k++) padded[k] = 0;

    for (k = 0; k < 40; k++) {
        long L = 8192 >> 1;           /* explicit rounding term */
        for (i = 0; i <= 10; i++)
            L += (long)padded[k + i] * REF_H[i];
        L = ref_sasr(L, 13);
        r->x[k] = ref_sat16(L);
    }

    /* Four decimated grids.  Strict comparison: equal energy keeps the
     * smaller grid number (grid 0 before grid 3 in particular). */
    long EM = 0;
    r->Mc = 0;
    for (m = 0; m <= 3; m++) {
        long acc = 0;
        for (i = 0; i <= 12; i++) {
            long t = ref_sasr(r->x[m + 3 * i], 2);
            acc += t * t;
        }
        r->energy[m] = acc << 1;      /* factor 2 implied by GSM_L_MULT */
        if (r->energy[m] > EM) { EM = r->energy[m]; r->Mc = m; }
    }
    for (i = 0; i <= 12; i++) r->xM[i] = r->x[r->Mc + 3 * i];

    /* Maximum absolute amplitude and its logarithmic coding. */
    int xmax = 0;
    for (i = 0; i <= 12; i++) {
        int t = ref_abs16(r->xM[i]);
        if (t > xmax) xmax = t;
    }
    r->xmax = xmax;

    int exp = 0, temp = ref_sasr(xmax, 9), itest = 0;
    for (i = 0; i <= 5; i++) {
        itest |= (temp <= 0);
        temp = ref_sasr(temp, 1);
        if (itest == 0) exp++;
    }
    r->xmaxc = ref_add16(ref_sasr(xmax, exp + 5), exp << 3);

    /* Decode xmaxc back into exponent/mantissa with the exact rules. */
    int e2, mant;
    e2 = 0;
    if (r->xmaxc > 15) e2 = ref_sasr(r->xmaxc, 3) - 1;
    mant = r->xmaxc - (e2 << 3);
    if (mant == 0) { e2 = -4; mant = 7; }
    else {
        while (mant <= 7) { mant = (mant << 1) | 1; e2--; }
        mant -= 8;
    }
    r->exp = e2;
    r->mant = mant;

    /* APCM quantization: the 13 three bit pulses. */
    int temp1 = 6 - e2;
    int temp2 = REF_NRFAC[mant];
    for (i = 0; i <= 12; i++) {
        int t = ref_asl16(r->xM[i], temp1);
        t = ref_mult(t, temp2);
        t = ref_sasr(t, 12);
        r->xMc[i] = t + 4;
    }

    /* Independent inverse quantization and grid positioning. */
    int u1 = REF_FAC[mant];
    int u2 = 6 - e2;
    int u3 = ref_asl16(1, u2 - 1);
    for (i = 0; i <= 12; i++) {
        int t = (r->xMc[i] << 1) - 7;   /* restore the signed level */
        t = ref_asl16(t, 12);
        t = ref_mult_r(u1, t);
        t = ref_add16(t, u3);
        r->ep[r->Mc + 3 * i] = ref_sasr(t, u2);  /* other positions stay 0 */
    }
}

static int check_rpe_production(const char *name, const int *res40,
                                const struct rpe_result *r)
{
    struct gsm_state *S = gsm_create();
    word e[50], xmaxc, Mc, xMc[13];
    int i, failed = 0;
    for (i = 0; i < 5; i++) e[i] = 0;
    for (i = 0; i < 40; i++) e[5 + i] = (word)res40[i];
    for (i = 0; i < 5; i++) e[45 + i] = 0;

    Gsm_RPE_Encoding(S, e + 5, &xmaxc, &Mc, xMc);

    if (!ref_expect(Mc == r->Mc, "rpe",
           "%s production Mc got=%d want=%d", name, Mc, r->Mc)) failed = 1;
    if (!ref_expect(xmaxc == r->xmaxc, "rpe",
           "%s production xmaxc got=%d want=%d",
           name, xmaxc, r->xmaxc)) failed = 1;
    for (i = 0; i <= 12; i++) {
        if (!ref_expect(xMc[i] == r->xMc[i], "rpe",
               "%s production xMc[%d] got=%d want=%d",
               name, i, xMc[i], r->xMc[i])) { failed = 1; break; }
        if (!ref_expect(r->xMc[i] >= 0 && r->xMc[i] <= 7, "rpe",
               "%s xMc[%d] outside 3 bits: %d",
               name, i, r->xMc[i])) { failed = 1; break; }
    }
    for (i = 0; i < 40 && !failed; i++)
        if (!ref_expect(e[5 + i] == r->ep[i], "rpe",
               "%s production ep[%d] got=%d want=%d",
               name, i, e[5 + i], r->ep[i])) { failed = 1; break; }
    gsm_destroy(S);
    return failed;
}

static int rpe_mc_seen[4];
static int rpe_exp_seen[11];             /* indexed by exp + 4 */
static int rpe_endpoint_swaps;
static int rpe_zero_cases;
static int rpe_negative_cases;

static int rpe_run_case(const char *name, const int *res40,
                        int want_Mc, int want_xmaxc, int want_exp)
{
    struct rpe_result r;
    memset(&r, 0, sizeof r);
    ref_rpe(res40, &r);

    EXPECT(r.Mc == want_Mc, "rpe", "%s Mc got=%d want=%d",
           name, r.Mc, want_Mc);
    EXPECT(r.xmaxc == want_xmaxc, "rpe", "%s xmaxc got=%d want=%d",
           name, r.xmaxc, want_xmaxc);
    EXPECT(r.exp == want_exp, "rpe", "%s exp got=%d want=%d",
           name, r.exp, want_exp);
    EXPECT(r.mant >= 0 && r.mant <= 7, "rpe",
           "%s mant out of range %d", name, r.mant);

    if (check_rpe_production(name, res40, &r)) return 1;

    rpe_mc_seen[r.Mc] = 1;
    rpe_exp_seen[r.exp + 4] = 1;
    if (want_xmaxc == 0) rpe_zero_cases++;
    return 0;
}

/* Build the residual inputs deterministically. */
static void rpe_impulse(int *res40, int pos, int amp)
{
    int k;
    for (k = 0; k < 40; k++) res40[k] = 0;
    res40[pos] = amp;
}

static int case_rpe_grid3_endpoint(void)
{
    int res[40];
    rpe_impulse(res, 39, 12000);   /* only grid 3 reaches sample 39 */
    struct rpe_result r;
    memset(&r, 0, sizeof r);
    ref_rpe(res, &r);
    EXPECT(r.energy[3] == 18000000L, "rpe", "grid3 endpoint energy[3]");
    EXPECT(r.energy[0] == 0L, "rpe", "grid3 endpoint energy[0]");
    EXPECT(r.xMc[12] == 7, "rpe", "grid3 endpoint last pulse");
    rpe_endpoint_swaps++;
    return rpe_run_case("grid3_endpoint", res, 3, 51, 5);
}
static int case_rpe_grid0_endpoint(void)
{
    int res[40];
    rpe_impulse(res, 0, 12000);    /* only grid 0 reaches sample 0 */
    struct rpe_result r;
    memset(&r, 0, sizeof r);
    ref_rpe(res, &r);
    EXPECT(r.energy[0] == 18000000L, "rpe", "grid0 endpoint energy[0]");
    EXPECT(r.energy[3] == 0L, "rpe", "grid0 endpoint energy[3]");
    EXPECT(r.xMc[0] == 7, "rpe", "grid0 endpoint first pulse");
    rpe_endpoint_swaps++;
    return rpe_run_case("grid0_endpoint", res, 0, 51, 5);
}
static int case_rpe_tie03(void)
{
    /* Equal impulses at both endpoints: grids 0 and 3 share 12 positions
     * and now have identical energy; the tie rule must keep grid 0. */
    int res[40];
    rpe_impulse(res, 0, 12000);
    res[39] = 12000;
    struct rpe_result r;
    memset(&r, 0, sizeof r);
    ref_rpe(res, &r);
    EXPECT(r.energy[0] == r.energy[3], "rpe", "tie03 energies differ");
    EXPECT(r.energy[0] == 18000000L, "rpe", "tie03 energy magnitude");
    EXPECT(r.Mc == 0, "rpe", "tie03 must keep smaller grid 0");
    return rpe_run_case("tie_grid03", res, 0, 51, 5);
}
static int case_rpe_zero(void)
{
    int res[40] = {0};
    struct rpe_result r;
    int i;
    memset(&r, 0, sizeof r);
    ref_rpe(res, &r);
    EXPECT(r.energy[0] == 0 && r.energy[1] == 0 &&
           r.energy[2] == 0 && r.energy[3] == 0, "rpe", "zero energies");
    EXPECT(r.exp == -4 && r.mant == 7, "rpe", "zero exponent/mantissa");
    for (i = 0; i <= 12; i++) EXPECT(r.xMc[i] == 4, "rpe",
                                     "zero xMc[%d] want 4", i);
    return rpe_run_case("zero", res, 0, 0, -4);
}
static int case_rpe_negative_odd(void)
{
    /* Every sample is a negative odd value: exercises sign restoration. */
    int res[40], k;
    for (k = 0; k < 40; k++) res[k] = -(((k + 5) % 7) * 111 + 1);
    rpe_negative_cases++;
    return rpe_run_case("negative_odd", res, 1, 26, 2);
}
static int case_rpe_large(void)
{
    /* Full scale constant input: exponent 6, and the weighting filter
     * saturates all 40 of its outputs to the positive word limit. */
    int res[40], k;
    for (k = 0; k < 40; k++) res[k] = 32767;
    struct rpe_result r;
    memset(&r, 0, sizeof r);
    ref_rpe(res, &r);
    EXPECT(r.exp == 6, "rpe", "large exponent want 6");
    {
        int sat = 1;
        for (k = 0; k < 40; k++) if (r.x[k] != 32767) sat = 0;
        EXPECT(sat, "rpe", "large weighting output must saturate to 32767");
        for (k = 0; k <= 12; k++)
            EXPECT(r.xMc[k] == 7, "rpe", "large xMc[%d] want 7", k);
    }
    return rpe_run_case("large", res, 0, 63, 6);
}
static int case_rpe_tiny(void)
{
    /* +/- 1: xmaxc 0, exponent -4, grid 3 wins by a single energy unit. */
    int res[40], k;
    for (k = 0; k < 40; k++) res[k] = ((k + 5) % 2 ? 1 : -1);
    struct rpe_result r;
    memset(&r, 0, sizeof r);
    ref_rpe(res, &r);
    EXPECT(r.energy[3] == 2, "rpe", "tiny grid3 energy");
    EXPECT(r.xMc[12] == 3, "rpe", "tiny last pulse");
    return rpe_run_case("tiny", res, 3, 0, -4);
}
static int case_rpe_midmix(void)
{
    /* Small mixed signs: exponent -3, another grid-3 selection. */
    int res[40], k;
    for (k = 0; k < 40; k++) res[k] = (((k + 5) * 37) % 101) - 50;
    return rpe_run_case("midmix", res, 3, 1, -3);
}
static int case_rpe_coverage(void)
{
    int mc = 0, exps = 0, i;
    for (i = 0; i < 4; i++) mc += rpe_mc_seen[i];
    for (i = 0; i < 11; i++) exps += rpe_exp_seen[i];
    printf("COVERAGE rpe grids=%d endpoint_swap_pairs=%d zero_cases=%d"
           " negative_odd_cases=%d exponent_levels=%d\n",
           mc, rpe_endpoint_swaps / 2, rpe_zero_cases,
           rpe_negative_cases, exps);
    TASK_CHECK(rpe_endpoint_swaps == 2);
    TASK_CHECK(rpe_zero_cases >= 1);
    TASK_CHECK(rpe_negative_cases >= 1);
    TASK_CHECK(mc >= 3);
    TASK_CHECK(exps >= 3);
    return 0;
}

/* ====================================================================== */
/* Closed loop reference: self-advanced four-subframe encoder.            */
/* ====================================================================== */

struct loop_ref {
    int h[120];                /* reconstructed history, newest last */
};

static void loop_reset(struct loop_ref *r) { memset(r, 0, sizeof *r); }

/* One reference subframe.  The history receives the QUANTIZED reconstructed
 * residual plus the prediction; the original residual d is never fed back.
 * Only the last 120 reconstructed samples survive across subframes. */
static void loop_subframe(struct loop_ref *r, const int *d40,
                          int *Nco, int *bco, int *Mco, int *xmaxco,
                          int *xMc13, int *dpp40, int *ep40)
{
    struct ltp_result lr;
    ref_ltp(d40, r->h, &lr);
    *Nco = lr.Nc;
    *bco = lr.bc;
    memcpy(dpp40, lr.dpp, sizeof lr.dpp);

    int e[40], k;
    for (k = 0; k < 40; k++) e[k] = lr.e[k];

    struct rpe_result rr;
    memset(&rr, 0, sizeof rr);
    ref_rpe(e, &rr);
    *Mco = rr.Mc;
    *xmaxco = rr.xmaxc;
    memcpy(xMc13, rr.xMc, sizeof rr.xMc);
    memcpy(ep40, rr.ep, sizeof rr.ep);

    int grown[160];
    for (k = 0; k < 120; k++) grown[k] = r->h[k];
    for (k = 0; k < 40; k++)
        grown[120 + k] = ref_add16(rr.ep[k], lr.dpp[k]);
    for (k = 0; k < 120; k++) r->h[k] = grown[40 + k];
}

/* Deterministic PCM generators. */
static void loop_pcm_a(int frame, short *s)
{
    int i;
    if (frame <= 2) {
        /* silence, then a periodic square wave */
        for (i = 0; i < 160; i++)
            s[i] = frame == 0 ? 0
                 : ((((i + frame * 160) % 40) < 20) ? 8000 : -8000);
    } else {
        /* abrupt change after the periodic section: uncorrelated noise */
        for (i = 0; i < 160; i++)
            s[i] = (short)(((i * 73 + frame * 191) % 2001) - 1000);
    }
}
static void loop_pcm_b(int frame, short *s)
{
    int i;
    for (i = 0; i < 160; i++) {
        int v = ((i * 37 + frame * 101) % 1000) - 500;
        if (frame & 1) v = -v;
        s[i] = (short)v;
    }
}

struct loop_stream {
    gsm front;                  /* independent context for the front end */
    gsm coder;                  /* Gsm_Coder under test */
    gsm packed;                 /* gsm_encode/gsm_explode under test */
    struct loop_ref ref;
    const char *name;
    void (*pcm)(int, short *);
};

static int loop_check_frame(struct loop_stream *st, int frame)
{
    short s[160];
    st->pcm(frame, s);

    /* Front end on its own independently initialized context. */
    word so[160], LARc[8];
    Gsm_Preprocess(st->front, (word *)s, so);
    Gsm_LPC_Analysis(st->front, so, LARc);
    Gsm_Short_Term_Analysis_Filter(st->front, LARc, so);

    int d[160], k;
    for (k = 0; k < 160; k++) d[k] = so[k];

    int rNc[4], rbc[4], rMc[4], rxmaxc[4], rxMc[52];
    int sf;
    for (sf = 0; sf < 4; sf++) {
        int dpp[40], ep[40];
        loop_subframe(&st->ref, d + 40 * sf,
                      &rNc[sf], &rbc[sf], &rMc[sf], &rxmaxc[sf],
                      rxMc + 13 * sf, dpp, ep);
    }

    /* Gsm_Coder on a second context. */
    word pLARc[8], pNc[4], pbc[4], pMc[4], pxmaxc[4], pxMc[52];
    Gsm_Coder(st->coder, (word *)s, pLARc, pNc, pbc, pMc, pxmaxc, pxMc);

    for (k = 0; k < 8; k++)
        EXPECT(pLARc[k] == LARc[k], "loop",
               "%s frame=%d LARc[%d] got=%d want=%d",
               st->name, frame, k, pLARc[k], LARc[k]);
    for (sf = 0; sf < 4; sf++) {
        EXPECT(pNc[sf] == rNc[sf], "loop",
               "%s frame=%d subframe=%d Nc got=%d want=%d",
               st->name, frame, sf, pNc[sf], rNc[sf]);
        EXPECT(pbc[sf] == rbc[sf], "loop",
               "%s frame=%d subframe=%d bc got=%d want=%d",
               st->name, frame, sf, pbc[sf], rbc[sf]);
        EXPECT(pMc[sf] == rMc[sf], "loop",
               "%s frame=%d subframe=%d Mc got=%d want=%d",
               st->name, frame, sf, pMc[sf], rMc[sf]);
        EXPECT(pxmaxc[sf] == rxmaxc[sf], "loop",
               "%s frame=%d subframe=%d xmaxc got=%d want=%d",
               st->name, frame, sf, pxmaxc[sf], rxmaxc[sf]);
        for (k = 0; k <= 12; k++)
            EXPECT(pxMc[13 * sf + k] == rxMc[13 * sf + k], "loop",
                   "%s frame=%d subframe=%d xMc[%d] got=%d want=%d",
                   st->name, frame, sf, k,
                   pxMc[13 * sf + k], rxMc[13 * sf + k]);
    }

    /* Reconstructed history inside the coder context. */
    for (k = 0; k < 120; k++)
        EXPECT(st->coder->dp0[k] == st->ref.h[k], "loop",
               "%s frame=%d history[%d] got=%d want=%d",
               st->name, frame, k, st->coder->dp0[k], st->ref.h[k]);

    /* Public encode then explode on a third context; exploded subframes
     * occupy 17 shorts each: Nc, bc, Mc, xmaxc, 13 pulses. */
    gsm_byte packed33[33];
    short exploded[76];
    gsm_encode(st->packed, (gsm_signal *)s, packed33);
    EXPECT(gsm_explode(st->packed, packed33, exploded) == 0, "loop",
           "%s frame=%d gsm_explode failed", st->name, frame);
    for (k = 0; k < 8; k++)
        EXPECT(exploded[k] == LARc[k], "loop",
               "%s frame=%d exploded LARc[%d] got=%d want=%d",
               st->name, frame, k, exploded[k], LARc[k]);
    for (sf = 0; sf < 4; sf++) {
        int base = 8 + 17 * sf;
        EXPECT(exploded[base + 0] == rNc[sf], "loop",
               "%s frame=%d subframe=%d exploded Nc got=%d want=%d",
               st->name, frame, sf, exploded[base + 0], rNc[sf]);
        EXPECT(exploded[base + 1] == rbc[sf], "loop",
               "%s frame=%d subframe=%d exploded bc got=%d want=%d",
               st->name, frame, sf, exploded[base + 1], rbc[sf]);
        EXPECT(exploded[base + 2] == rMc[sf], "loop",
               "%s frame=%d subframe=%d exploded Mc got=%d want=%d",
               st->name, frame, sf, exploded[base + 2], rMc[sf]);
        EXPECT(exploded[base + 3] == rxmaxc[sf], "loop",
               "%s frame=%d subframe=%d exploded xmaxc got=%d want=%d",
               st->name, frame, sf, exploded[base + 3], rxmaxc[sf]);
        for (k = 0; k <= 12; k++)
            EXPECT(exploded[base + 4 + k] == rxMc[13 * sf + k], "loop",
                   "%s frame=%d subframe=%d exploded xMc[%d] got=%d want=%d",
                   st->name, frame, sf, k,
                   exploded[base + 4 + k], rxMc[13 * sf + k]);
    }
    return 0;
}

#define LOOP_FRAMES 6

static int case_loop_interleaved(void)
{
    struct loop_stream a, b;
    int f;
    a.front = gsm_create(); a.coder = gsm_create(); a.packed = gsm_create();
    b.front = gsm_create(); b.coder = gsm_create(); b.packed = gsm_create();
    loop_reset(&a.ref); loop_reset(&b.ref);
    a.name = "A"; a.pcm = loop_pcm_a;
    b.name = "B"; b.pcm = loop_pcm_b;

    /* Two distinct instances advanced alternately, frame by frame. */
    for (f = 0; f < LOOP_FRAMES; f++) {
        if (loop_check_frame(&a, f)) return 1;
        if (loop_check_frame(&b, f)) return 1;
    }

    /* The periodic section really gives way to an abrupt change. */
    short periodic[160], changed[160];
    loop_pcm_a(2, periodic);
    loop_pcm_a(3, changed);
    int differing = 0;
    for (f = 0; f < 160; f++) if (periodic[f] != changed[f]) differing++;
    TASK_CHECK(differing == 160);

    gsm_destroy(a.front); gsm_destroy(a.coder); gsm_destroy(a.packed);
    gsm_destroy(b.front); gsm_destroy(b.coder); gsm_destroy(b.packed);
    return 0;
}

static int case_loop_feedback(void)
{
    /* Single stream focused on the feedback rule: each new history sample
     * must equal quantized residual plus prediction, and only 120 samples
     * are retained across every frame. */
    struct loop_stream a;
    int f, sf, k;
    a.front = gsm_create(); a.coder = gsm_create(); a.packed = gsm_create();
    loop_reset(&a.ref);
    a.name = "feedback"; a.pcm = loop_pcm_a;

    for (f = 0; f < LOOP_FRAMES; f++) {
        short s[160];
        loop_pcm_a(f, s);
        word so[160], LARc[8];
        Gsm_Preprocess(a.front, (word *)s, so);
        Gsm_LPC_Analysis(a.front, so, LARc);
        Gsm_Short_Term_Analysis_Filter(a.front, LARc, so);

        int d[160];
        for (k = 0; k < 160; k++) d[k] = so[k];

        for (sf = 0; sf < 4; sf++) {
            int Nc, bc, Mc, xmaxc, xMc[13], dpp[40], ep[40];
            int before[120];
            memcpy(before, a.ref.h, sizeof before);
            loop_subframe(&a.ref, d + 40 * sf,
                          &Nc, &bc, &Mc, &xmaxc, xMc, dpp, ep);

            /* The 40 appended samples are exactly ep + dpp. */
            for (k = 0; k < 40; k++) {
                int want = ref_add16(ep[k], dpp[k]);
                EXPECT(a.ref.h[80 + k] == want, "loop",
                       "feedback frame=%d subframe=%d reconstructed[%d]"
                       " got=%d want=%d",
                       f, sf, k, a.ref.h[80 + k], want);
            }
            /* The previous 80 samples are the aged prefix. */
            for (k = 0; k < 80; k++)
                EXPECT(a.ref.h[k] == before[40 + k], "loop",
                       "feedback frame=%d subframe=%d aged_history[%d]"
                       " got=%d want=%d",
                       f, sf, k, a.ref.h[k], before[40 + k]);
        }

        /* Drive the production coder and compare its retained history. */
        word pLARc[8], pNc[4], pbc[4], pMc[4], pxmaxc[4], pxMc[52];
        Gsm_Coder(a.coder, (word *)s, pLARc, pNc, pbc, pMc, pxmaxc, pxMc);
        for (k = 0; k < 120; k++)
            EXPECT(a.coder->dp0[k] == a.ref.h[k], "loop",
                   "feedback frame=%d coder_history[%d] got=%d want=%d",
                   f, k, a.coder->dp0[k], a.ref.h[k]);
    }
    gsm_destroy(a.front); gsm_destroy(a.coder); gsm_destroy(a.packed);
    return 0;
}

/* ====================================================================== */
/* Registration.                                                          */
/* ====================================================================== */

const struct task_case task_cases[] = {
    { "ltp",  "unique",          case_ltp_unique },
    { "ltp",  "tie",             case_ltp_tie },
    { "ltp",  "nonpos",          case_ltp_nonpos },
    { "ltp",  "zero",            case_ltp_zero },
    { "ltp",  "bc1",             case_ltp_bc1 },
    { "ltp",  "bc2",             case_ltp_bc2 },
    { "ltp",  "coverage",        case_ltp_coverage },
    { "rpe",  "grid3_endpoint",  case_rpe_grid3_endpoint },
    { "rpe",  "grid0_endpoint",  case_rpe_grid0_endpoint },
    { "rpe",  "tie_grid03",      case_rpe_tie03 },
    { "rpe",  "zero",            case_rpe_zero },
    { "rpe",  "negative_odd",    case_rpe_negative_odd },
    { "rpe",  "large",           case_rpe_large },
    { "rpe",  "tiny",            case_rpe_tiny },
    { "rpe",  "midmix",          case_rpe_midmix },
    { "rpe",  "coverage",        case_rpe_coverage },
    { "loop", "interleaved",     case_loop_interleaved },
    { "loop", "feedback",        case_loop_feedback },
};

const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
