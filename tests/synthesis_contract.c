/******************************************************************************
 *
 *  Synthesis filter regression contract.
 *
 *  This translation unit includes the production implementation directly
 *  (../src/sbc.c) and drives it through three independent groups:
 *
 *    matrix  - bit-exact check of the 2N-point cosine transform, rebuilt as
 *              a dense Q13 matrix fed by an explicit mirror projection.
 *    history - natural-time ten-tap ring model of the synthesis history,
 *              checked sample by sample against the production state machine.
 *    frames  - full sbc_encode()/sbc_decode() streams; the sub-band samples
 *              and scales observed at the synthesis boundary are rendered by
 *              the same independent model and compared on final PCM.
 *
 *  The reference model deliberately keeps ten full 2N vectors in natural
 *  time order: it never reuses the production dual-plane ring layout or the
 *  unfolded butterflies.
 *
 ******************************************************************************/

#include "check.h"

#define TASK_SYNTH_TRACE 1
#include "../src/sbc.c"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ----------------------------------------------------------------------------
 *  Independent synthesis model
 * ------------------------------------------------------------------------- */

#define M_MAX_N   8        /* max sub-bands            */
#define M_TAPS   10        /* synthesis window length  */

/* Dense cosine matrix H[k][i] = cos((i + 1/2)(k + n/2) pi / n), Q13.
 * Zeros and +/-1 positions are snapped to exact 0 / +/-8192 so the model
 * carries the same precise special values as the fixed-point library. */
static int16_t model_q13_cos(double v)
{
    double c = cos(v);

    if (fabs(c) < 0.5 / 8192.0)
        return 0;
    if (fabs(fabs(c) - 1.0) < 0.5 / 8192.0)
        return c < 0 ? -8192 : 8192;

    long q = lround(c * 8192.0);
    return (int16_t)q;
}

static void model_build_matrix(int n, int16_t h[2 * M_MAX_N][M_MAX_N])
{
    for (int k = 0; k < 2 * n; k++)
        for (int i = 0; i < n; i++)
            h[k][i] = model_q13_cos((i + 0.5) * (k + n / 2.0) * M_PI / n);
}

/* Mirror projection used by the production transform: each mirrored input
 * pair is halved towards negative infinity (arithmetic right shift). The
 * model computes this projection explicitly instead of assuming an ideal
 * floating-point DCT. */
static void model_project(const int16_t *in, int n, int16_t p[M_MAX_N])
{
    for (int i = 0; i < n / 2; i++) {
        int a = in[i];
        int b = in[n - 1 - i];

        p[i]         = (int16_t)((a + b) >> 1);
        p[n - 1 - i] = (int16_t)((a - b) >> 1);
    }
}

static int16_t model_sat16(int64_t v)
{
    if (v > INT16_MAX)
        return INT16_MAX;
    if (v < INT16_MIN)
        return INT16_MIN;
    return (int16_t)v;
}

/* One 2N-point transform.
 *
 * The projected vector p turns the dense cosine sum over s into a dense
 * matrix G over p where, for the mirror pair (i, n-1-i):
 *
 *   G[k][i]       = H[k][i] + H[k][n-1-i]
 *   G[k][n-1-i]   = H[k][i] - H[k][n-1-i]
 *
 * Every G entry is even (0 or +/-2 times a Q13 coefficient); the raw
 * half-value half[k] = sum(G[k][i] * p[i]) / 2 is the exact folded value.
 *
 * The DCT symmetries are visible directly on the dense rows:
 *   - an all-zero row (k = n/2) maps to an exact zero,
 *   - two identical rows share one rounded value,
 *   - two opposite rows round the canonical row first and negate the
 *     rounded value afterwards, matching the fixed-point rule that sign
 *     flips happen after rounding (arithmetic shifts floor towards
 *     -infinity, so round(-x) and -round(x) differ at midpoints).
 *
 * The model discovers those relations from the matrix itself rather than
 * hardcoding the production butterfly layout. Transform rounding and 16-bit
 * saturation stay separate from the later window rounding / saturation. */
static void model_transform(int n,
    const int16_t h[2 * M_MAX_N][M_MAX_N],
    const int16_t *in, int scale, int16_t u[2 * M_MAX_N])
{
    int16_t p[M_MAX_N] = { 0 };
    model_project(in, n, p);

    int g[2 * M_MAX_N][M_MAX_N];
    int64_t half[2 * M_MAX_N];
    int shr = 12 + scale;

    for (int k = 0; k < 2 * n; k++) {
        int64_t t = 0;

        for (int i = 0; i < n / 2; i++) {
            int hp = h[k][i];
            int hm = h[k][n - 1 - i];

            g[k][i]         = hp + hm;
            g[k][n - 1 - i] = hp - hm;
            t += (int64_t)g[k][i] * p[i];
            t += (int64_t)g[k][n - 1 - i] * p[n - 1 - i];
        }

        half[k] = t / 2;
    }

    int64_t rv[2 * M_MAX_N];

    for (int k = 0; k < 2 * n; k++) {
        int zero_row = 1, equal_to = -1, opposite_to = -1;

        for (int i = 0; i < n; i++)
            zero_row &= (g[k][i] == 0);

        for (int j = 0; j < k; j++) {
            int eq = 1, opp = 1;

            for (int i = 0; i < n; i++) {
                eq  &= (g[k][i] == g[j][i]);
                opp &= (g[k][i] == -g[j][i]);
            }

            if (eq)  { equal_to = j; break; }
            if (opp) { opposite_to = j; break; }
        }

        if (zero_row)
            rv[k] = 0;
        else if (equal_to >= 0)
            rv[k] = rv[equal_to];
        else if (opposite_to >= 0)
            rv[k] = -rv[opposite_to];
        else
            rv[k] = (half[k] + (INT64_C(1) << (shr - 1))) >> shr;
    }

    /* Saturation is the last step, applied once per output coefficient so
     * sign-flipped rows saturate exactly like the production -a / SAT16
     * sequence. */
    for (int k = 0; k < 2 * n; k++)
        u[k] = model_sat16(rv[k]);
}

/* Synthesis window coefficients in natural time order (tap 0 is newest).
 * Transcribed from the format tables embedded in src/sbc.c. */
static const int16_t model_window_4[4][M_TAPS] = {
    {   0, -126,  -358, -848, -4443, -9644, 4443,  -848,  358, -126 },
    { -18, -128,  -670, -201, -6389, -9235, 2544, -1055,  100,  -90 },
    { -49,  -61,  -946,  944, -8082, -8082,  944,  -946,  -61,  -49 },
    { -90,  100, -1055, 2544, -9235, -6389, -201,  -670, -128,  -18 },
};

static const int16_t model_window_8[8][M_TAPS] = {
    {    0, -132,  -371, -848, -4456, -9631, 4456,  -848,  371, -132 },
    {  -10, -138,  -526, -580, -5438, -9528, 3486, -1004,  229, -117 },
    {  -22, -131,  -685, -192, -6395, -9224, 2561, -1063,  108,  -97 },
    {  -36, -106,  -835,  322, -7287, -8734, 1711, -1042,   12,  -75 },
    {  -54,  -59,  -960,  959, -8078, -8078,  959,  -960,  -59,  -54 },
    {  -75,   12, -1042, 1711, -8734, -7287,  322,  -835, -106,  -36 },
    {  -97,  108, -1063, 2561, -9224, -6395, -192,  -685, -131,  -22 },
    { -117,  229, -1004, 3486, -9528, -5438, -580,  -526, -138,  -10 },
};

struct synth_model {
    int n;
    int head;                        /* ring slot of the newest vector */
    int16_t h[2 * M_MAX_N][M_MAX_N];
    int16_t hist[M_TAPS][2 * M_MAX_N];  /* ring[age], age 0 = newest */
    const int16_t (*window)[M_TAPS];
};

static void model_init(struct synth_model *m, int n)
{
    memset(m, 0, sizeof(*m));
    m->n = n;
    m->head = 0;
    m->window = n == 4 ? model_window_4 : model_window_8;
    model_build_matrix(n, m->h);
}

/* Process one sub-band block.
 *
 * Natural-time reconstruction of the production dual-plane ring. The
 * production code splits every 2N vector across two parity planes (first
 * half in one, second half in the other) and a block's output reads one
 * plane. Translating slots back to block ages, that is exactly one
 * ten-tap FIR over the ten most recent vectors:
 *
 *   - even ages (0,2,4,6,8) contribute the first-half coefficient u[j],
 *   - odd  ages (1,3,5,7,9) contribute the second-half coefficient u[n+j],
 *   - each age a is weighted by the natural window tap W[j][a].
 *
 * The model keeps all ten full 2N vectors in an explicit age ring (age 0
 * is the newest block) and never shares the production plane or slot
 * layout. Window accumulation stays in 32 bit to match production;
 * |sum of taps| < 2^15 and |u| <= 2^15-1 keep every sum in range.
 * Transform rounding / saturation and window rounding / saturation stay
 * separate. */
static void model_block(struct synth_model *m,
    const int16_t *in, int scale, int16_t out[M_MAX_N])
{
    int16_t u[2 * M_MAX_N];
    model_transform(m->n, m->h, in, scale, u);

    m->head = (m->head + 1) % M_TAPS;   /* advance before storing */
    for (int k = 0; k < 2 * m->n; k++)
        m->hist[m->head][k] = u[k];

    for (int j = 0; j < m->n; j++) {
        int s = 0;

        for (int age = 0; age < M_TAPS; age++) {
            int slot = m->head - age;
            if (slot < 0)
                slot += M_TAPS;

            int coeff = (age & 1) == 0 ?
                m->hist[slot][j] : m->hist[slot][m->n + j];

            s += coeff * m->window[j][age];
        }

        out[j] = model_sat16((s + (1 << 12)) >> 13);
    }
}


/* ----------------------------------------------------------------------------
 *  matrix group - dense transform vs production transformed vectors
 * ------------------------------------------------------------------------- */

/* Run one production block on a zeroed state and expose both the stored
 * transform planes and the windowed PCM. */
static void production_block(int n, const int16_t *in, int scale,
    int16_t u[2 * M_MAX_N], int16_t pcm[M_MAX_N])
{
    struct sbc_dstate state = { 0 };
    int16_t out[M_MAX_N] = { 0 };

    if (n == 4)
        sbc_synthesize_4_c(&state, in, scale, out, 1);
    else
        sbc_synthesize_8_c(&state, in, scale, out, 1);

    for (int k = 0; k < n; k++) {
        u[k]     = state.v[0][k][0];
        u[n + k] = state.v[1][k][0];
        pcm[k]   = out[k];
    }
}

static void matrix_case(int n, const int16_t *in, int scale, const char *tag)
{
    int16_t mu[2 * M_MAX_N], mp[M_MAX_N];
    int16_t pu[2 * M_MAX_N], pp[M_MAX_N];
    struct synth_model m;

    model_init(&m, n);
    model_transform(n, m.h, in, scale, mu);
    model_block(&m, in, scale, mp);
    production_block(n, in, scale, pu, pp);

    for (int k = 0; k < 2 * n; k++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "matrix %s n=%d scale=%d u[%d]",
                 tag, n, scale, k);
        TASK_CHECK(mu[k] == pu[k], msg);
    }

    for (int j = 0; j < n; j++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "matrix %s n=%d scale=%d pcm[%d]",
                 tag, n, scale, j);
        TASK_CHECK(mp[j] == pp[j], msg);
    }
}

/* Any projected vector p is realizable: for a mirror pair, in[i] = x + y
 * and in[n-1-i] = x - y give (a+b)>>1 = x and (a-b)>>1 = y. This turns a
 * single nonzero projected coefficient into a valid int16 input. */
static int project_back(const int n, const int16_t *p, int16_t *in)
{
    for (int i = 0; i < n / 2; i++) {
        long x = p[i];
        long y = p[n - 1 - i];

        if (x + y > INT16_MAX || x + y < INT16_MIN ||
            x - y > INT16_MAX || x - y < INT16_MIN)
            return 0;

        in[i]         = (int16_t)(x + y);
        in[n - 1 - i] = (int16_t)(x - y);
    }

    int16_t check[M_MAX_N];
    model_project(in, n, check);
    for (int i = 0; i < n; i++)
        if (check[i] != p[i])
            return 0;

    return 1;
}

/* Modular inverse of a modulo m (a, m coprime), via extended Euclid. */
static long mod_inverse(long a, long m)
{
    long t = 0, nt = 1, r = m, nr = a % m;

    while (nr) {
        long q = r / nr;
        long tmp = t - q * nt;
        t = nt; nt = tmp;
        tmp = r - q * nr;
        r = nr; nr = tmp;
    }

    if (t < 0)
        t += m;
    return t;
}

static int v2(long v)
{
    int n = 0;
    while ((v & 1) == 0) { v >>= 1; n++; }
    return n;
}

/* Build inputs whose raw transform value sits below, on, and above a
 * rounding midpoint.
 *
 * Production rounds the folded value half = t/2 = sum(c[i] * p[i]) with a
 * shift of shr = 12+scale, where c[i] = G[k][i]/2 is exactly a production
 * butterfly coefficient. Its rounding midpoint is the residue 2^(shr-1)
 * modulo 2^shr. For each row we pick the coefficient with the largest
 * power-of-two factor (gcd <= 2^(shr-1)): the midpoint is then reachable
 * and the modular inverse yields a projected coefficient small enough for
 * int16. +/-1 around that solution lands strictly on either side of the
 * midpoint. */
static int find_midpoints(int n, int scale,
    int16_t cases[][M_MAX_N], int max_cases)
{
    struct synth_model m;
    int found = 0;
    const int shr = 12 + scale;
    const long mod = 1L << shr;
    const long midpoint = mod >> 1;
    const long bound = 16000;

    model_init(&m, n);

    for (int k = 0; k < 2 * n && found < max_cases; k++) {
        int c[M_MAX_N] = { 0 };
        int any = 0;

        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n / 2; j++) {
                int hp = m.h[k][j];
                int hm = m.h[k][n - 1 - j];
                if (j == i)
                    c[i] = (hp + hm) / 2;
                if (n - 1 - j == i)
                    c[i] = (hp - hm) / 2;
            }
            any |= (c[i] != 0);
        }

        if (!any)
            continue;   /* all-zero row: nothing to round */

        /* Largest power-of-two factor below the modulus. */
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (!c[i])
                continue;
            int v = v2(labs(c[i]));
            if (v > shr - 1)
                continue;
            if (best < 0 || v > v2(labs(c[best])))
                best = i;
        }
        if (best < 0)
            continue;

        long cc = c[best];
        long gcd = 1L << v2(labs(cc));
        long a = cc / gcd;               /* odd */
        long mm = mod / gcd;
        long inv = mod_inverse(a, mm);

        if (midpoint % gcd)
            continue;

        long p_mid = ((midpoint / gcd) * inv) % mm;
        if (p_mid > mm / 2)
            p_mid -= mm;
        if (labs(p_mid) + 1 > bound)
            continue;

        /* p_mid lands exactly on the midpoint; p_mid +/- 1 lands strictly
         * on the two neighbouring reachable residues. */
        static const long steps[3] = { -1, 0, 1 };

        for (unsigned d = 0; d < 3 && found < max_cases; d++) {
            int16_t p[M_MAX_N] = { 0 };
            p[best] = (int16_t)(p_mid + steps[d]);

            long half_val = cc * p[best];
            long residue = half_val % mod;
            if (residue < 0)
                residue += mod;

            long step_res = cc % mod;
            long below = (midpoint - step_res + mod) % mod;
            long above = (midpoint + step_res) % mod;
            int ok = (d == 0 && residue == below) ||
                     (d == 1 && residue == midpoint) ||
                     (d == 2 && residue == above);
            if (!ok)
                continue;

            int16_t in[M_MAX_N];
            if (!project_back(n, p, in))
                continue;

            int duplicate = 0;
            for (int q = 0; q < found; q++) {
                int same = 1;
                for (int i = 0; i < n; i++)
                    same &= (cases[q][i] == in[i]);
                duplicate |= same;
            }
            if (duplicate)
                continue;

            memcpy(cases[found], in, sizeof(int16_t) * n);
            found++;
        }
    }

    return found;
}

static int run_matrix_group(void)
{
    for (int n = 4; n <= 8; n += 4) {

        /* Every individual basis vector. */
        for (int b = 0; b < n; b++) {
            int16_t in[M_MAX_N] = { 0 };
            in[b] = 1000;
            matrix_case(n, in, 0, "basis");
        }

        /* Asymmetric combinations. */
        int16_t combo[3][M_MAX_N] = {
            { 100, -200, 300, -400, 500, -600, 700, -800 },
            { 901,  37,  -9, 111, -222, 333,  -7, 444 },
            {  -3,   5,  -7,   9, -11,  13, -15,  17 },
        };
        for (int c = 0; c < 3; c++)
            matrix_case(n, combo[c], 0, "combo");

        /* Positive and negative odd values. */
        static const int odds[] = { 1, -1, 317, -317, 1023, -2049, 16383, -16383 };
        for (unsigned o = 0; o < sizeof(odds) / sizeof(odds[0]); o++) {
            int16_t in[M_MAX_N] = { 0 };
            for (int i = 0; i < n; i++)
                in[i] = (int16_t)(odds[o] * (i + 1));
            matrix_case(n, in, 0, "odd");
        }

        /* Every supported scale on an asymmetric odd input. */
        for (int scale = -2; scale <= 14; scale++) {
            int16_t in[M_MAX_N];
            for (int i = 0; i < n; i++)
                in[i] = (int16_t)((i % 2 ? -1 : 1) * (513 + i * 257));
            matrix_case(n, in, scale, "scale");
        }

        /* Near-saturation inputs at the smallest scales. */
        static const int16_t hot[][M_MAX_N] = {
            { 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767 },
            { -32768, -32768, -32768, -32768, -32768, -32768, -32768, -32768 },
            { 32767, -32768, 32767, -32768, 32767, -32768, 32767, -32768 },
            { 32767, 1, -32768, -1, 32766, 2, -32767, -2 },
        };
        for (unsigned c = 0; c < sizeof(hot) / sizeof(hot[0]); c++)
            for (int scale = -2; scale <= 1; scale++)
                matrix_case(n, hot[c], scale, "saturate");

        /* Both sides of rounding midpoints, at several shift widths. */
        static const int mid_scales[] = { -2, 0, 4, 9, 14 };
        for (unsigned s = 0; s < sizeof(mid_scales) / sizeof(mid_scales[0]);
             s++) {
            int16_t cases[24][M_MAX_N];
            int count = find_midpoints(n, mid_scales[s], cases,
                sizeof(cases) / sizeof(cases[0]));

            /* At large shifts only rows with a high power-of-two factor
             * can reach the midpoint within int16 range; require the
             * below/on/above triple for at least one row. Every case is
             * still compared coefficient by coefficient across all 2N
             * outputs, so no transform value is exempted. */
            TASK_CHECK(count >= 3, "midpoint candidates found");

            int distinct = 0;
            int16_t seen[24][2 * M_MAX_N];
            for (int c = 0; c < count; c++) {
                struct synth_model mm;
                int16_t u[2 * M_MAX_N];
                model_init(&mm, n);
                model_transform(n, mm.h, cases[c], mid_scales[s], u);

                int is_new = 1;
                for (int q = 0; q < distinct; q++) {
                    int same = 1;
                    for (int k = 0; k < 2 * n; k++)
                        same &= (seen[q][k] == u[k]);
                    is_new &= !same;
                }
                if (is_new)
                    memcpy(seen[distinct++], u, sizeof(u));

                matrix_case(n, cases[c], mid_scales[s], "midpoint");
            }
            /* The boundary cases must change at least one rounded output,
             * otherwise the midpoint itself was never exercised. */
            TASK_CHECK(distinct >= 2, "midpoint changes rounding");
        }
    }

    return 1;
}


/* ----------------------------------------------------------------------------
 *  history group - natural time ring vs production state machine
 * ------------------------------------------------------------------------- */

#define HIST_SENTINEL ((int16_t)INT16_MIN)

static int run_history_group(void)
{
    for (int n = 4; n <= 8; n += 4) {
        for (int pitch = 1; pitch <= 3; pitch++) {

            struct sbc_dstate state = { 0 };
            struct synth_model model;
            model_init(&model, n);

            /* 40+ blocks: the ten-phase ring wraps at least twice. */
            for (int b = 0; b < 42; b++) {
                int16_t in[M_MAX_N] = { 0 };
                int16_t mout[M_MAX_N], pout[M_MAX_N];
                int16_t buf[3 * M_MAX_N];
                int scale = (b % 3) - 1;

                if (b < n) {
                    /* Pulses walking through every sub-band. */
                    in[b] = 1000;
                } else if (b < n + 10) {
                    /* Alternating signs, odd magnitudes. */
                    for (int i = 0; i < n; i++)
                        in[i] = (int16_t)((b & 1 ? -1 : 1) * (201 + i * 37));
                } else if (b < n + 22) {
                    /* Deterministic small sequence around zero. */
                    for (int i = 0; i < n; i++)
                        in[i] = (int16_t)(((b * 7 + i * 13) % 21) - 10);
                }
                /* remaining blocks are zeros: drain the ten-block tail */

                for (unsigned z = 0; z < sizeof(buf) / sizeof(buf[0]); z++)
                    buf[z] = HIST_SENTINEL;

                if (n == 4)
                    sbc_synthesize_4_c(&state, in, scale, buf, pitch);
                else
                    sbc_synthesize_8_c(&state, in, scale, buf, pitch);

                model_block(&model, in, scale, mout);

                for (int j = 0; j < n; j++)
                    pout[j] = buf[j * pitch];

                for (int j = 0; j < n; j++) {
                    char msg[96];
                    snprintf(msg, sizeof(msg),
                             "history n=%d pitch=%d block=%d pcm[%d]",
                             n, pitch, b, j);
                    TASK_CHECK(pout[j] == mout[j], msg);
                }

                /* Gap positions must never be touched. */
                for (int i = 0; i < n * pitch; i++) {
                    int valid = 0;
                    for (int j = 0; j < n; j++)
                        valid |= (i == j * pitch);

                    char msg[96];
                    snprintf(msg, sizeof(msg),
                             "history n=%d pitch=%d block=%d gap[%d]",
                             n, pitch, b, i);
                    TASK_CHECK(valid || buf[i] == HIST_SENTINEL, msg);
                }
            }

            /* After ten zero blocks the tail must be fully drained. */
            for (int j = 0; j < n; j++) {
                char msg[64];
                snprintf(msg, sizeof(msg), "history drain n=%d pcm[%d]", n, j);
                TASK_CHECK(state.v[0][j][0] == 0 && state.v[1][j][0] == 0,
                           msg);
            }
        }
    }

    return 1;
}


/* ----------------------------------------------------------------------------
 *  frames group - real encode/decode streams rendered by the model
 * ------------------------------------------------------------------------- */

#ifdef TASK_SYNTH_TRACE
struct trace_capture {
    int active;
    int nch;
    int nblocks;
    int nsubbands;
    int scale[2];
    int16_t samples[2][SBC_MAX_SAMPLES];
};

static struct trace_capture cap;

void task_synth_trace(const struct sbc *sbc, int channel,
    int nblocks, int nsubbands, const int16_t *in, int scale)
{
    (void)sbc;
    if (!cap.active)
        return;

    cap.nblocks = nblocks;
    cap.nsubbands = nsubbands;
    cap.scale[channel] = scale;
    memcpy(cap.samples[channel], in,
        (size_t)nblocks * nsubbands * sizeof(int16_t));

    if (channel + 1 > cap.nch)
        cap.nch = channel + 1;
}
#endif

struct frame_chain {
    sbc_t enc;
    sbc_t dec;
    struct synth_model model[2];
    int nsb;
    int nch;
};

static void chain_init(struct frame_chain *c, int nsb)
{
    sbc_reset(&c->enc);
    sbc_reset(&c->dec);
    model_init(&c->model[0], nsb);
    model_init(&c->model[1], nsb);
    c->nsb = nsb;
    c->nch = 0;
}

/* One encoded frame (or a NULL-data PLC step): production PCM and model PCM
 * are rendered into separate buffers and compared sample by sample. */
static void render_frame(struct frame_chain *c, const struct sbc_frame *fr,
    const int16_t *l, int pitchl, const int16_t *r, int pitchr, int plc,
    int16_t *ol, int16_t *or_, int16_t *ml, int16_t *mr,
    unsigned char *buf, unsigned bufsize)
{
    struct sbc_frame got = { 0 };

    cap = (struct trace_capture){ 0 };
    cap.active = 1;

    if (!plc) {
        unsigned size = sbc_get_frame_size(fr);
        TASK_CHECK(size != 0 && size <= bufsize, "frame size");
        TASK_CHECK(sbc_encode(&c->enc, l, pitchl, r, pitchr, fr, buf, size)
                   == 0, "encode");
        TASK_CHECK(sbc_decode(&c->dec, buf, size, &got,
                              ol, pitchl, or_, pitchr) == 0, "decode");
    } else {
        /* PLC keeps the previous frame description: the production path
         * decides on the second channel from frame->mode, and a real
         * client reuses its last probed descriptor. */
        got = *fr;
        TASK_CHECK(sbc_decode(&c->dec, NULL, 0, &got,
                              ol, pitchl, or_, pitchr) == 0, "plc decode");

        int all_zero = 1;
        for (int z = 0; z < cap.nblocks * cap.nsubbands; z++)
            all_zero &= (cap.samples[0][z] == 0);
        TASK_CHECK(all_zero, "plc left zeroed");
        TASK_CHECK(cap.scale[0] == 0, "plc left scale");
        if (c->nch == 2) {
            int rz = 1;
            for (int z = 0; z < cap.nblocks * cap.nsubbands; z++)
                rz &= (cap.samples[1][z] == 0);
            TASK_CHECK(rz, "plc right zeroed");
            TASK_CHECK(cap.scale[1] == 0, "plc right scale");
        }
    }

    int nch = 1 + (got.mode != SBC_MODE_MONO);
    int nsb = got.nsubbands;
    int nblk = got.nblocks;

    if (!plc) {
        c->nch = nch;
        TASK_CHECK(nsb == c->nsb, "subband count stable");
    }

    TASK_CHECK(cap.nch == nch && cap.nsubbands == nsb &&
               cap.nblocks == nblk, "trace shape");

    int16_t *prod[2] = { ol, or_ };
    int16_t *mod[2]  = { ml, mr };
    int pitch[2] = { pitchl, pitchr };

    for (int ch = 0; ch < nch; ch++) {
        for (int b = 0; b < nblk; b++) {
            int16_t mout[M_MAX_N];

            model_block(&c->model[ch],
                cap.samples[ch] + b * nsb, cap.scale[ch], mout);

            for (int j = 0; j < nsb; j++)
                mod[ch][(b * nsb + j) * pitch[ch]] = mout[j];
        }

        for (int z = 0; z < nblk * nsb; z++) {
            char msg[80];
            snprintf(msg, sizeof(msg), "frame ch%d sample%d", ch, z);
            TASK_CHECK(prod[ch][z * pitch[ch]] == mod[ch][z * pitch[ch]],
                       msg);
        }
    }
}

static int16_t gen_left(int f, int i)
{
    return (int16_t)(((i * 197 + f * 509) % 20001) - 10000);
}

static int16_t gen_right(int f, int i)
{
    return (int16_t)(((i * 311 + f * 677) % 24001) - 12000);
}

static int16_t gen_joint_right(int f, int i, int16_t lv)
{
    return (int16_t)((lv >> 1) + (i & 1 ? 37 : -37) + f);
}

#define FRAME_CAP 288

static void fill_mono(int f, int16_t *x, int count, int pitch)
{
    for (int i = 0; i < count; i++)
        x[i * pitch] = gen_left(f, i);
}

static void run_stream(const char *tag, const struct sbc_frame *fr,
    int frames, int pitch, int joint)
{
    int nch = 1 + (fr->mode != SBC_MODE_MONO);
    int count = fr->nblocks * fr->nsubbands;

    struct frame_chain chain;
    chain_init(&chain, fr->nsubbands);

    int16_t l[FRAME_CAP], r[FRAME_CAP];
    int16_t ol[FRAME_CAP], or_[FRAME_CAP], ml[FRAME_CAP], mr[FRAME_CAP];
    unsigned char buf[128];

    for (int f = 0; f < frames; f++) {
        fill_mono(f, l, count, pitch);
        if (nch == 2) {
            for (int i = 0; i < count; i++)
                r[i * pitch] = joint ?
                    gen_joint_right(f, i, l[i * pitch]) : gen_right(f, i);
        }

        render_frame(&chain, fr, l, pitch, nch == 2 ? r : NULL, pitch, 0,
                     ol, nch == 2 ? or_ : NULL,
                     ml, nch == 2 ? mr : NULL,
                     buf, sizeof(buf));
    }

    TASK_CHECK(frames > 0 && count > 0, tag);
}

static int run_frames_group(void)
{
    struct sbc_frame mono4 = {
        .freq = SBC_FREQ_48K, .mode = SBC_MODE_MONO, .bam = SBC_BAM_LOUDNESS,
        .nblocks = 16, .nsubbands = 4, .bitpool = 32
    };
    struct sbc_frame mono8 = {
        .freq = SBC_FREQ_44K1, .mode = SBC_MODE_MONO, .bam = SBC_BAM_SNR,
        .nblocks = 16, .nsubbands = 8, .bitpool = 32
    };
    struct sbc_frame stereo4 = {
        .freq = SBC_FREQ_48K, .mode = SBC_MODE_STEREO, .bam = SBC_BAM_LOUDNESS,
        .nblocks = 16, .nsubbands = 4, .bitpool = 32
    };
    struct sbc_frame stereo8 = {
        .freq = SBC_FREQ_32K, .mode = SBC_MODE_STEREO, .bam = SBC_BAM_LOUDNESS,
        .nblocks = 16, .nsubbands = 8, .bitpool = 32
    };
    struct sbc_frame joint4 = {
        .freq = SBC_FREQ_48K, .mode = SBC_MODE_JOINT_STEREO,
        .bam = SBC_BAM_LOUDNESS, .nblocks = 16, .nsubbands = 4, .bitpool = 35
    };
    struct sbc_frame joint8 = {
        .freq = SBC_FREQ_48K, .mode = SBC_MODE_JOINT_STEREO,
        .bam = SBC_BAM_LOUDNESS, .nblocks = 16, .nsubbands = 8, .bitpool = 35
    };
    struct sbc_frame msbc = {
        .msbc = true, .freq = SBC_FREQ_16K, .mode = SBC_MODE_MONO,
        .bam = SBC_BAM_LOUDNESS, .nblocks = 15, .nsubbands = 8, .bitpool = 26
    };

    /* Continuous mono / stereo / joint-stereo streams. */
    run_stream("mono4", &mono4, 6, 1, 0);
    run_stream("mono8", &mono8, 6, 1, 0);
    run_stream("stereo4", &stereo4, 5, 1, 0);
    run_stream("stereo8", &stereo8, 5, 2, 0);
    run_stream("joint4", &joint4, 5, 1, 1);
    run_stream("joint8", &joint8, 5, 1, 1);

    /* mSBC: fixed 15-block frames. */
    run_stream("msbc", &msbc, 4, 1, 0);

    /* --- Two independent mono instances interleaved block by block. --- */

    struct frame_chain a, b;
    chain_init(&a, 4);
    chain_init(&b, 4);

    int16_t al[64], bl[64], aol[64], bol[64], aml[64], bml[64];
    unsigned char abuf[128], bbuf[128];

    for (int f = 0; f < 4; f++) {
        for (int i = 0; i < 64; i++) {
            al[i] = gen_left(f, i);
            bl[i] = gen_right(f + 9, i);
        }

        render_frame(&a, &mono4, al, 1, NULL, 1, 0,
                     aol, NULL, aml, NULL, abuf, sizeof(abuf));
        render_frame(&b, &mono4, bl, 1, NULL, 1, 0,
                     bol, NULL, bml, NULL, bbuf, sizeof(bbuf));
        render_frame(&a, &mono4, al, 1, NULL, 1, 0,
                     aol, NULL, aml, NULL, abuf, sizeof(abuf));
        render_frame(&b, &mono4, bl, 1, NULL, 1, 0,
                     bol, NULL, bml, NULL, bbuf, sizeof(bbuf));
    }

    /* --- Packet loss: NULL-data PLC advances against model zero input. --- */

    struct frame_chain plc_chain;
    chain_init(&plc_chain, 4);

    int16_t l[64], ol[64], or_[64], ml[64], mr[64];
    unsigned char pbuf[128];

    for (int f = 0; f < 3; f++) {
        fill_mono(f, l, 64, 1);
        render_frame(&plc_chain, &stereo4, l, 1, l, 1, 0,
                     ol, or_, ml, mr, pbuf, sizeof(pbuf));
    }

    /* Two loss steps: history tail rendered from zeros on both sides. */
    for (int f = 0; f < 2; f++)
        render_frame(&plc_chain, &stereo4, NULL, 1, NULL, 1, 1,
                     ol, or_, ml, mr, pbuf, sizeof(pbuf));

    /* Recovery on a normal encoded frame. */
    for (int f = 0; f < 2; f++) {
        fill_mono(f + 7, l, 64, 1);
        render_frame(&plc_chain, &stereo4, l, 1, l, 1, 0,
                     ol, or_, ml, mr, pbuf, sizeof(pbuf));
    }

    /* --- Reset: a reset chain must match a brand new chain. --- */

    struct frame_chain r1, r2;
    chain_init(&r1, 8);
    chain_init(&r2, 8);

    int16_t x[128], p1[128], p2[128], m1[128], m2[128];
    unsigned char rbuf[128];

    for (int f = 0; f < 3; f++) {
        fill_mono(f, x, 128, 1);
        render_frame(&r1, &mono8, x, 1, NULL, 1, 0,
                     p1, NULL, m1, NULL, rbuf, sizeof(rbuf));
    }

    /* Full reset (encoder analysis state, decoder history and model), so
     * the restarted chain is indistinguishable from a brand new one. */
    chain_init(&r1, 8);

    for (int f = 0; f < 3; f++) {
        fill_mono(f, x, 128, 1);
        render_frame(&r1, &mono8, x, 1, NULL, 1, 0,
                     p1, NULL, m1, NULL, rbuf, sizeof(rbuf));
        render_frame(&r2, &mono8, x, 1, NULL, 1, 0,
                     p2, NULL, m2, NULL, rbuf, sizeof(rbuf));

        for (int z = 0; z < 128; z++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "reset sample%d", z);
            TASK_CHECK(p1[z] == p2[z] && m1[z] == m2[z] && p1[z] == m1[z],
                       msg);
        }
    }

    return 1;
}


/* ----------------------------------------------------------------------------
 *  Entry point called by task_support/main.c
 * ------------------------------------------------------------------------- */

int task_group(const char *group)
{
    if (!strcmp(group, "matrix"))
        return run_matrix_group();
    if (!strcmp(group, "history"))
        return run_history_group();
    if (!strcmp(group, "frames"))
        return run_frames_group();

    return 0;
}
