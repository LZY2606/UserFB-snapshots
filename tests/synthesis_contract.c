/******************************************************************************
 *
 *  Synthesis filter-bank regression contract.
 *
 *  This file is compiled directly by the fixed task build together with the
 *  whole production implementation (see task_support/build.sh).  It never
 *  defines main(): task_support/main.c owns the process entry and the
 *  TASK_CHECK reporting policy (CONTENT_MISMATCH / exit 42).
 *
 *  The reference model below is deliberately written in natural time order:
 *    1. an independent integer projection of the mirrored input pairs,
 *    2. a dense cosine matrix built from its mathematical definition, with
 *       coefficients quantized to the library Q13 scale,
 *    3. a plain ten-vector ring of transform values windowed with a ten
 *       tap table ordered by natural time delay.
 *  It does not copy the production dual-plane ring layout or the expanded
 *  butterfly arithmetic.
 *
 ******************************************************************************/

#include <stdint.h>

#include "check.h"

/* Observation hook used inside src/sbc.c under SBC_SYNTHESIS_CONTRACT.
 * Declared before the implementation is pulled in so the call site below
 * needs no implicit declaration. */
struct sbc;
void sbc_contract_observe(struct sbc *sbc, int mode, int nchannels,
    int nblocks, int nsubbands,
    const int16_t *sb0, const int16_t *sb1, const int *sb_scale);

/* Pull in the production implementation once: the fixed build does not link
 * src/sbc.c a second time.  The synthesis entry points and frame codec are
 * then callable directly from this translation unit. */
#include "../src/sbc.c"

#include <math.h>
#include <stdlib.h>
#include <string.h>


/* ----------------------------------------------------------------------------
 *  Independent reference model
 * ------------------------------------------------------------------------- */

#define MODEL_HIST 10
#define MODEL_MAX_N 8
#define MODEL_U_MAX (2 * MODEL_MAX_N)

/* Natural-time synthesis window, ten taps ordered by delay d = 0..9.
 * Transcribed from the format constants in src/sbc.c (first half of each
 * duplicated, transposed production row). */
static const int16_t model_window_4[4][MODEL_HIST] = {
    {    0, -126,  -358, -848, -4443, -9644, 4443,  -848,  358, -126 },
    {  -18, -128,  -670, -201, -6389, -9235, 2544, -1055,  100,  -90 },
    {  -49,  -61,  -946,  944, -8082, -8082,  944,  -946,  -61,  -49 },
    {  -90,  100, -1055, 2544, -9235, -6389, -201,  -670, -128,  -18 },
};

static const int16_t model_window_8[8][MODEL_HIST] = {
    {    0, -132,  -371, -848, -4456, -9631, 4456,  -848,  371, -132 },
    {  -10, -138,  -526, -580, -5438, -9528, 3486, -1004,  229, -117 },
    {  -22, -131,  -685, -192, -6395, -9224, 2561, -1063,  108,  -97 },
    {  -36, -106,  -835,  322, -7287, -8734, 1711, -1042,   12,  -75 },
    {  -54,  -59,  -960,  959, -8078, -8078,  959,  -960,  -59,  -54 },
    {  -75,   12, -1042, 1711, -8734, -7287,  322,  -835, -106,  -36 },
    {  -97,  108, -1063, 2561, -9224, -6395, -192,  -685, -131,  -22 },
    { -117,  229, -1004, 3486, -9528, -5438, -580,  -526, -138,  -10 },
};

struct model_synth {
    int n;
    int tick;
    /* hist[t % 10][k] holds transform component u_k of block t, already
     * rounded and saturated to 16 bits exactly as production stores it. */
    int16_t hist[MODEL_HIST][MODEL_U_MAX];
    const int16_t (*window)[MODEL_HIST];
};

static void model_init(struct model_synth *m, int n)
{
    memset(m, 0, sizeof(*m));
    m->n = n;
    m->window = n == 4 ?
        (const int16_t (*)[MODEL_HIST])model_window_4 :
        (const int16_t (*)[MODEL_HIST])model_window_8;
}

static int16_t model_sat16(int64_t v)
{
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

/* Full dense synthesis cosine matrix in Q13:
 * M(k, i) = round(8192 * cos((i + 1/2) (k + n/2) pi / n)). */
static void model_matrix(int n, int16_t m[MODEL_U_MAX][MODEL_MAX_N])
{
    for (int k = 0; k < 2 * n; k++)
        for (int i = 0; i < n; i++) {
            double angle = (i + 0.5) * (k + n / 2.0) * M_PI / n;
            m[k][i] = (int16_t)lround(8192.0 * cos(angle));
        }
}

/* Dense matrix that actually acts on the floor-halved projection p.
 *
 * The production butterfly halves every mirrored input pair toward negative
 * infinity (the projection below), then multiplies the halved sum/difference
 * pairs by Q13 cosine constants.  Algebraically each output row is therefore
 * a half-sum / half-difference combination of two columns of M.  For both
 * n = 4 and n = 8 the mirrored columns of M are exact sign opposites,
 * cos((n-(i+1/2)) theta) = +/- cos((i+1/2) theta), so these halves never
 * introduce quantization: this dense table is bit-identical to the expanded
 * butterfly while remaining an independent, definition-built construction. */
static void model_effective_matrix(int n,
    const int16_t m[MODEL_U_MAX][MODEL_MAX_N],
    int16_t hp[MODEL_U_MAX][MODEL_MAX_N])
{
    for (int k = 0; k < 2 * n; k++)
        for (int j = 0; j < n / 2; j++) {
            int mirror = n - 1 - j;
            hp[k][j] =
                (int16_t)(((int)m[k][j] + (int)m[k][mirror]) >> 1);
            hp[k][mirror] =
                (int16_t)(((int)m[k][j] - (int)m[k][mirror]) >> 1);
        }
}

/* Convenience: build the full definition matrix internally and return the
 * effective dense table consumed by model_transform / model_block. */
static void model_work_matrix(int n,
    int16_t hp[MODEL_U_MAX][MODEL_MAX_N])
{
    int16_t m[MODEL_U_MAX][MODEL_MAX_N];
    model_matrix(n, m);
    model_effective_matrix(n, m, hp);
}

/* DCT symmetry assembly.  Production rounds each unique intermediate once
 * (arithmetic shift: a negative exact half moves toward +infinity) and only
 * then applies the cosine identities h(n-k,i) = -h(k,i),
 * h(2n-k,i) = h(n+k,i).  Rounding a sign-flipped negative row directly would
 * differ by one at exact negative midpoints, so the model rounds the unique
 * intermediate rows and assembles the 2N outputs with the same identities.
 * Each entry names an intermediate (index into the source-row list) and its
 * sign, or src = -1 for the mathematical zero row. */
struct sym_entry { int src; int sign; };

static const struct sym_entry sym4[2 * 4] = {
    /* a0, a1, zero, -a1, -a0, b0, b1, b0 */
    { 0, +1 }, { 1, +1 }, { -1, 0 }, { 1, -1 },
    { 0, -1 }, { 2, +1 }, { 3, +1 }, { 2, +1 },
};
static const int sym4_rows[4] = { 0, 1, 5, 6 };

static const struct sym_entry sym8[2 * 8] = {
    /* a0 a1 a2 a3 zero -a3 -a2 -a1 | -a0 b0 b1 b2 b3 b2 b1 b0 */
    { 0, +1 }, { 1, +1 }, { 2, +1 }, { 3, +1 },
    { -1, 0 }, { 3, -1 }, { 2, -1 }, { 1, -1 },
    { 0, -1 }, { 4, +1 }, { 5, +1 }, { 6, +1 },
    { 7, +1 }, { 6, +1 }, { 5, +1 }, { 4, +1 },
};
static const int sym8_rows[8] = { 0, 1, 2, 3, 9, 10, 11, 12 };

/* Integer projection used before every transform: mirrored pairs are halved
 * with a floor-toward-negative-infinity shift, so odd inputs are not equal to
 * an ideal floating-point DCT. */
static void model_project(int n, const int16_t *in, int p[MODEL_MAX_N])
{
    for (int j = 0; j < n / 2; j++) {
        int a = in[j];
        int b = in[n - 1 - j];
        p[j] = (a + b) >> 1;
        p[n - 1 - j] = (a - b) >> 1;
    }
}

/* Transform only: projection, dense effective matrix product, rounding at
 * the block scale and 16-bit saturation.  The unique intermediate rows are
 * rounded once and the remaining outputs are assembled by the cosine
 * symmetries, matching production's round-then-negate ordering.  Rounding
 * here is kept separate from the later window rounding. */
static void model_transform(int n,
    const int16_t hp[MODEL_U_MAX][MODEL_MAX_N],
    const int16_t *in, int scale, int16_t u[MODEL_U_MAX])
{
    int p[MODEL_MAX_N];
    model_project(n, in, p);

    int shr = 12 + scale;
    int64_t add = 1LL << (shr - 1);

    const int *src_rows = n == 4 ? sym4_rows : sym8_rows;
    const struct sym_entry *sym = n == 4 ? sym4 : sym8;

    /* Intermediates are only rounded at this stage; saturation is applied
     * below after the symmetry sign flips, exactly like production. */
    int32_t mid[MODEL_MAX_N];
    for (int r = 0; r < n; r++) {
        int row = src_rows[r];
        int64_t acc = 0;
        for (int i = 0; i < n; i++)
            acc += (int64_t)hp[row][i] * p[i];

        mid[r] = (int32_t)((acc + add) >> shr);
    }

    for (int k = 0; k < 2 * n; k++) {
        if (sym[k].src < 0)
            u[k] = 0;
        else
            u[k] = model_sat16((int64_t)mid[sym[k].src] * sym[k].sign);
    }
}

/* One synthesis block in natural time order.  Each transform produces 2N
 * values; the model keeps full vectors in a plain ten-entry ring instead of
 * the production dual-plane layout.  Enumerating that layout (a stored half
 * sits at column (d - idx) mod 10 and is multiplied by window tap
 * (column + idx) mod 10 = d) gives one fixed ten-tap rule:
 *   - even delays d = 0, 2, 4, 6, 8 use the first half of the vector,
 *   - odd  delays d = 1, 3, 5, 7, 9 use the second half.
 * A regression that only windows the current block, drops one parity
 * half-plane, or reverses the tap order breaks this sum at its first
 * nonzero tap. */
static void model_block(struct model_synth *m,
    const int16_t hp[MODEL_U_MAX][MODEL_MAX_N],
    const int16_t *in, int scale, int16_t *out, int pitch)
{
    int n = m->n;
    int16_t u[MODEL_U_MAX];

    model_transform(n, hp, in, scale, u);
    memcpy(m->hist[m->tick % MODEL_HIST], u, (size_t)(2 * n) * sizeof(int16_t));

    for (int i = 0; i < n; i++) {
        int64_t s = 0;

        for (int d = 0; d < MODEL_HIST; d++) {
            int past = m->tick - d;
            if (past < 0)
                continue;

            int component = (d & 1) ? n + i : i;
            s += (int64_t)m->hist[past % MODEL_HIST][component] *
                 m->window[i][d];
        }

        out[i * pitch] = model_sat16((s + (1 << 12)) >> 13);
    }

    m->tick++;
}


/* ----------------------------------------------------------------------------
 *  Production helpers
 * ------------------------------------------------------------------------- */

static int prod_block(int n, struct sbc_dstate *st,
    const int16_t *in, int scale, int16_t *out, int pitch)
{
    int idx_before = st->idx;

    if (n == 4)
        sbc_synthesize_4_c(st, in, scale, out, pitch);
    else
        sbc_synthesize_8_c(st, in, scale, out, pitch);

    return idx_before;
}

/* Read back the 2N transform values a production call stored in its two
 * parity planes, in natural k = 0..2N-1 order.  idx_before is the state
 * index the call started from (the call advances state->idx afterwards). */
static void prod_transform(int n, const struct sbc_dstate *st, int idx_before,
    int16_t u[MODEL_U_MAX])
{
    int dct_idx = idx_before ? 10 - idx_before : 0;
    int odd = dct_idx & 1;

    for (int k = 0; k < n; k++)
        u[k] = st->v[odd][k][dct_idx];
    for (int k = 0; k < n; k++)
        u[n + k] = st->v[!odd][k][dct_idx];
}


/* ----------------------------------------------------------------------------
 *  matrix group: transform values, element for element
 * ------------------------------------------------------------------------- */

static int run_matrix(void)
{
    for (int n = 4; n <= 8; n += 4) {
        int16_t m[MODEL_U_MAX][MODEL_MAX_N];
        int16_t h[MODEL_U_MAX][MODEL_MAX_N];
        model_matrix(n, m);
        model_effective_matrix(n, m, h);

        /* Exact zeros and +/-1 positions of the mathematical matrix must
         * survive Q13 quantization. */
        for (int i = 0; i < n; i++)
            TASK_CHECK(m[n / 2][i] == 0, "matrix zero row");

        for (int k = 0; k < 2 * n; k++)
            for (int i = 0; i < n; i++) {
                double angle = (i + 0.5) * (k + n / 2.0) * M_PI / n;
                double c = cos(angle);
                if (fabs(c) < 1.0 / 16384.0)
                    TASK_CHECK(m[k][i] == 0, "quantized zero exact");
                if (fabs(c) > 1.0 - 1.0 / 16384.0)
                    TASK_CHECK(abs(m[k][i]) == 8192,
                        "quantized unit exact");
            }

        /* Every individual basis vector (even amplitude so the floor
         * projection keeps it) over the full supported scale range. */
        for (int basis = 0; basis < n; basis++) {
            int16_t in[MODEL_MAX_N] = { 0 };
            in[basis] = 2;

            for (int scale = -2; scale <= 14; scale++) {
                int16_t want[MODEL_U_MAX], got[MODEL_U_MAX];
                struct sbc_dstate st = { 0 };

                model_transform(n, h, in, scale, want);
                int idx0 = prod_block(n, &st, in, scale, got, 1);
                prod_transform(n, &st, idx0, got);

                for (int k = 0; k < 2 * n; k++)
                    TASK_CHECK(got[k] == want[k], "basis transform value");
            }
        }

        /* Asymmetric combinations with mixed sign and magnitude. */
        int16_t combos[][MODEL_MAX_N] = {
            { 3, -7, 11, -5, 13, -2, 9, -11 },
            { 101, -203, 307, -409, 511, -613, 717, -819 },
            { -1234, 567, -89, 1001, -202, 434, -777, 13 },
        };

        for (unsigned c = 0; c < sizeof(combos) / sizeof(combos[0]); c++) {
            for (int scale = -2; scale <= 14; scale++) {
                int16_t want[MODEL_U_MAX], got[MODEL_U_MAX];
                struct sbc_dstate st = { 0 };

                model_transform(n, h, combos[c], scale, want);
                int idx0 = prod_block(n, &st, combos[c], scale, got, 1);
                prod_transform(n, &st, idx0, got);

                for (int k = 0; k < 2 * n; k++)
                    TASK_CHECK(got[k] == want[k],
                        "asymmetric transform value");
            }
        }

        /* Explicit positive and negative odd inputs exercise the floor
         * projection (odd sums and differences cannot use an ideal DCT). */
        int16_t odd_pos[MODEL_MAX_N], odd_neg[MODEL_MAX_N], odd_mix[MODEL_MAX_N];
        for (int i = 0; i < n; i++) {
            odd_pos[i] = (int16_t)(2 * i + 1);
            odd_neg[i] = (int16_t)-(2 * i + 1);
            odd_mix[i] = (int16_t)((i & 1) ? -(3 * i + 1) : (3 * i + 1));
        }

        const int16_t *odd_sets[] = { odd_pos, odd_neg, odd_mix };
        for (unsigned s = 0; s < 3; s++) {
            int16_t want[MODEL_U_MAX], got[MODEL_U_MAX];
            struct sbc_dstate st = { 0 };

            model_transform(n, h, odd_sets[s], 0, want);
            int idx0 = prod_block(n, &st, odd_sets[s], 0, got, 1);
            prod_transform(n, &st, idx0, got);

            for (int k = 0; k < 2 * n; k++)
                TASK_CHECK(got[k] == want[k], "odd projection transform");
        }

        /* Near saturation: large amplitudes at the smallest scale must go
         * through 16-bit saturation identically on both sides. */
        int16_t big_sets[][MODEL_MAX_N] = {
            { 30000, 30000, 30000, 30000, 30000, 30000, 30000, 30000 },
            { -30000, -30000, -30000, -30000, -30000, -30000, -30000, -30000 },
            { 30000, -30000, 29999, -29999, 30000, -30000, 29999, -29999 },
            { 32767, -32768, 32767, -32768, 32767, -32768, 32767, -32768 },
        };
        for (unsigned c = 0; c < sizeof(big_sets) / sizeof(big_sets[0]); c++) {
            const int scales[2] = { -2, 0 };
            for (unsigned q = 0; q < 2; q++) {
                int16_t want[MODEL_U_MAX], got[MODEL_U_MAX];
                struct sbc_dstate st = { 0 };

                model_transform(n, h, big_sets[c], scales[q], want);
                int idx0 = prod_block(n, &st, big_sets[c], scales[q], got, 1);
                prod_transform(n, &st, idx0, got);

                for (int k = 0; k < 2 * n; k++)
                    TASK_CHECK(got[k] == want[k], "saturated transform");
            }
        }

        /* Rounding midpoint, approached from both sides.  Scan even
         * amplitudes for a chosen basis and retain the raw products whose
         * residues sit closest below and above the rounding half.  The scan
         * targets the unique intermediate rows that are actually rounded. */
        const int *src_rows = n == 4 ? sym4_rows : sym8_rows;
        for (int basis = 0; basis < n; basis++) {
            for (int scale = -2; scale <= 1; scale++) {
                int shr = 12 + scale;
                int mod = 1 << shr, half = 1 << (shr - 1);
                int r_pick = 0;
                while (h[src_rows[r_pick]][basis] == 0)
                    r_pick++;
                int k_pick = src_rows[r_pick];

                int best_lo_m = 0, best_hi_m = 0;
                int best_lo_d = mod, best_hi_d = mod;

                for (int m = 1; m <= 4000; m++) {
                    int16_t probe[MODEL_MAX_N] = { 0 };
                    int p[MODEL_MAX_N];
                    probe[basis] = (int16_t)(2 * m);
                    model_project(n, probe, p);

                    int64_t raw = 0;
                    for (int i = 0; i < n; i++)
                        raw += (int64_t)h[k_pick][i] * p[i];

                    int rem = (int)((raw % mod + mod) % mod);
                    if (rem < half && half - rem < best_lo_d) {
                        best_lo_d = half - rem;
                        best_lo_m = m;
                    }
                    if (rem >= half && rem - half < best_hi_d) {
                        best_hi_d = rem - half;
                        best_hi_m = m;
                    }
                }

                TASK_CHECK(best_lo_m && best_hi_m, "midpoint cases found");

                int picks[2] = { best_lo_m, best_hi_m };
                for (unsigned q = 0; q < 2; q++) {
                    int16_t in[MODEL_MAX_N] = { 0 };
                    int16_t want[MODEL_U_MAX], got[MODEL_U_MAX];
                    struct sbc_dstate st = { 0 };
                    in[basis] = (int16_t)(2 * picks[q]);

                    model_transform(n, h, in, scale, want);
                    int idx0 = prod_block(n, &st, in, scale, got, 1);
                    prod_transform(n, &st, idx0, got);

                    for (int k = 0; k < 2 * n; k++)
                        TASK_CHECK(got[k] == want[k],
                            "rounding midpoint transform");
                }
            }
        }
    }

    return 1;
}


/* ----------------------------------------------------------------------------
 *  history group: ten-tap natural-time window and parity planes
 * ------------------------------------------------------------------------- */

#define HIST_BLOCKS 40
#define HIST_TAIL 12
#define HIST_SENTINEL ((int16_t)-32123)

static void history_input(int n, int t, int16_t in[MODEL_MAX_N])
{
    memset(in, 0, (size_t)n * sizeof(int16_t));

    if (t < n) {
        /* Distinct sub-band pulses, amplitude alternates sign. */
        in[t] = (int16_t)((t & 1) ? -2400 : 2400);
    } else if (t < 2 * n) {
        /* Single-band pulses with strictly alternating sign. */
        int q = t - n;
        in[q % n] = (int16_t)((q & 1) ? -1800 : 1800);
    } else if (t < 3 * n) {
        /* Deterministic small sequence, odd values included. */
        for (int i = 0; i < n; i++)
            in[i] = (int16_t)(((t * 5 + i * 3) % 9) - 4);
    }
    /* Remaining blocks are zeros: drain the ten-tap tail. */
}

static int run_history(void)
{
    for (int n = 4; n <= 8; n += 4) {
        int16_t h[MODEL_U_MAX][MODEL_MAX_N];
        model_work_matrix(n, h);

        static const int pitches[3] = { 1, 2, 3 };

        for (unsigned pi = 0; pi < 3; pi++) {
            int pitch = pitches[pi];
            struct model_synth model;
            struct sbc_dstate prod = { 0 };
            model_init(&model, n);

            /* 40 ticks wrap the ten-phase ring four times, so all ten
             * phases and both parity planes are exercised repeatedly; the
             * last blocks are zeros that drain the ten-tap tail. */
            for (int t = 0; t < HIST_BLOCKS + HIST_TAIL; t++) {
                int16_t in[MODEL_MAX_N];
                int16_t mout[64], pout[64];

                history_input(n, t < HIST_BLOCKS ? t : HIST_BLOCKS, in);
                if (t >= HIST_BLOCKS)
                    memset(in, 0, (size_t)n * sizeof(int16_t));

                for (int i = 0; i < 64; i++)
                    mout[i] = pout[i] = HIST_SENTINEL;

                model_block(&model, h, in, 0, mout, pitch);
                prod_block(n, &prod, in, 0, pout, pitch);

                for (int i = 0; i < n; i++) {
                    TASK_CHECK(pout[i * pitch] == mout[i * pitch],
                        "history window PCM");

                    /* Gap positions must never be touched. */
                    for (int g = 1; g < pitch; g++)
                        TASK_CHECK(pout[i * pitch + g] == HIST_SENTINEL,
                            "pitch gap sentinel");
                }

                /* Once the ten-tap tail has fully drained, zero inputs must
                 * produce silence from both implementations. */
                if (t >= HIST_BLOCKS + MODEL_HIST)
                    for (int i = 0; i < n; i++)
                        TASK_CHECK(mout[i * pitch] == 0 &&
                                   pout[i * pitch] == 0,
                            "drained tail silence");
            }
        }
    }

    return 1;
}


/* ----------------------------------------------------------------------------
 *  frames group: public encode/decode path against the independent model
 * ------------------------------------------------------------------------- */

#define FRAME_CAP 4
#define FRAME_MAX_BYTES 1024

struct frame_capture {
    struct sbc *sbc;
    int pending;
    int mode, nchannels, nblocks, nsubbands;
    int16_t sb[2][SBC_MAX_SAMPLES];
    int scale[2];
};

static struct frame_capture captures[FRAME_CAP];

/* Read-only observation hook, only compiled in with SBC_SYNTHESIS_CONTRACT.
 * Normal builds contain no callback, state or allocation here. */
void sbc_contract_observe(struct sbc *sbc, int mode, int nchannels,
    int nblocks, int nsubbands,
    const int16_t *sb0, const int16_t *sb1, const int *sb_scale)
{
    for (int i = 0; i < FRAME_CAP; i++) {
        if (captures[i].sbc != sbc)
            continue;

        struct frame_capture *c = &captures[i];
        c->pending = 1;
        c->mode = mode;
        c->nchannels = nchannels;
        c->nblocks = nblocks;
        c->nsubbands = nsubbands;

        int nsamples = nblocks * nsubbands;
        memcpy(c->sb[0], sb0, (size_t)nsamples * sizeof(int16_t));
        if (sb1)
            memcpy(c->sb[1], sb1, (size_t)nsamples * sizeof(int16_t));
        c->scale[0] = sb_scale[0];
        c->scale[1] = sb_scale[1];
        return;
    }
}

static void capture_register(struct sbc *sbc)
{
    for (int i = 0; i < FRAME_CAP; i++)
        if (!captures[i].sbc) {
            captures[i].sbc = sbc;
            return;
        }
    TASK_CHECK(0, "capture slot");
}

static struct frame_capture *capture_drain(struct sbc *sbc)
{
    for (int i = 0; i < FRAME_CAP; i++)
        if (captures[i].sbc == sbc) {
            TASK_CHECK(captures[i].pending, "observation fired");
            captures[i].pending = 0;
            return &captures[i];
        }
    TASK_CHECK(0, "capture found");
    return NULL;
}

struct signal_gen {
    double phase, rate, gain;
    uint32_t lcg;
    int sign;
};

static void gen_init(struct signal_gen *g, double rate, double gain,
    uint32_t seed, int sign)
{
    g->phase = 0.0;
    g->rate = rate;
    g->gain = gain;
    g->lcg = seed ? seed : 1;
    g->sign = sign;
}

static int16_t gen_next(struct signal_gen *g)
{
    g->lcg = g->lcg * 1103515245u + 12345u;
    int noise = (int)((g->lcg >> 16) % 1201) - 600;
    double v = g->sign * sin(g->phase) * g->gain +
               sin(g->phase * 0.37 + 1.3) * (g->gain * 0.25) + noise;
    g->phase += g->rate;

    if (v > INT16_MAX) v = INT16_MAX;
    if (v < INT16_MIN) v = INT16_MIN;
    return (int16_t)v;
}

struct seq_config {
    bool msbc;
    enum sbc_freq freq;
    enum sbc_mode mode;
    int nblocks, nsubbands, bitpool, frames;
};

static void run_one_sequence(const struct seq_config *cfg,
    struct signal_gen *gen_l_a, struct signal_gen *gen_r_a,
    struct signal_gen *gen_l_b, struct signal_gen *gen_r_b)
{
    struct sbc_frame f = {
        .msbc = cfg->msbc,
        .freq = cfg->freq,
        .mode = cfg->mode,
        .bam = SBC_BAM_LOUDNESS,
        .nblocks = cfg->nblocks,
        .nsubbands = cfg->nsubbands,
        .bitpool = cfg->bitpool,
    };

    unsigned frame_size = sbc_get_frame_size(&f);
    TASK_CHECK(frame_size != 0 && frame_size <= FRAME_MAX_BYTES,
        "valid frame size");

    /* Decoder contexts are local to this sequence, so start the capture
     * table fresh each time. */
    memset(captures, 0, sizeof(captures));

    sbc_t enc_a = { 0 }, enc_b = { 0 };
    sbc_t dec_a = { 0 }, dec_b = { 0 };

    capture_register(&dec_a);
    capture_register(&dec_b);

    struct model_synth model_a[2], model_b[2];
    for (int ch = 0; ch < 2; ch++) {
        model_init(&model_a[ch], cfg->nsubbands);
        model_init(&model_b[ch], cfg->nsubbands);
    }

    int16_t h[MODEL_U_MAX][MODEL_MAX_N];
    model_work_matrix(cfg->nsubbands, h);

    int nch = 1 + (cfg->mode != SBC_MODE_MONO);
    int nsamples = cfg->nblocks * cfg->nsubbands;

    unsigned char buf_a[FRAME_MAX_BYTES], buf_b[FRAME_MAX_BYTES];
    int16_t in_l[SBC_MAX_SAMPLES], in_r[SBC_MAX_SAMPLES];
    int16_t pa[2][SBC_MAX_SAMPLES], pb[2][SBC_MAX_SAMPLES];
    int16_t ma[2][SBC_MAX_SAMPLES], mb[2][SBC_MAX_SAMPLES];

    /* Two PLC frames are inserted after the third normal frame. */
    for (int fr = 0; fr < cfg->frames; fr++) {
        for (int i = 0; i < nsamples; i++) {
            in_l[i] = gen_next(gen_l_a);
            in_r[i] = gen_next(gen_r_a);
        }

        int rc = sbc_encode(&enc_a, in_l, 1,
            nch > 1 ? in_r : in_l, 1, &f, buf_a, frame_size);
        TASK_CHECK(rc == 0, "encode instance A");

        for (int i = 0; i < nsamples; i++) {
            in_l[i] = gen_next(gen_l_b);
            in_r[i] = gen_next(gen_r_b);
        }
        rc = sbc_encode(&enc_b, in_l, 1,
            nch > 1 ? in_r : in_l, 1, &f, buf_b, frame_size);
        TASK_CHECK(rc == 0, "encode instance B");

        bool plc = (fr == 3 || fr == 4);

        /* Interleave the two decoder instances. */
        rc = sbc_decode(&dec_a, plc ? NULL : buf_a, frame_size, &f,
            pa[0], 1, nch > 1 ? pa[1] : NULL, 1);
        TASK_CHECK(rc == 0, "decode instance A");

        struct frame_capture *ca = capture_drain(&dec_a);
        TASK_CHECK(ca->nsubbands == cfg->nsubbands &&
                   ca->nblocks == cfg->nblocks &&
                   ca->nchannels == nch, "capture A shape");
        memset(ma, 0, sizeof(ma));
        for (int ch = 0; ch < nch; ch++)
            for (int blk = 0; blk < ca->nblocks; blk++)
                model_block(&model_a[ch], h,
                    ca->sb[ch] + blk * ca->nsubbands, ca->scale[ch],
                    ma[ch] + blk * ca->nsubbands, 1);

        rc = sbc_decode(&dec_b, plc ? NULL : buf_b, frame_size, &f,
            pb[0], 1, nch > 1 ? pb[1] : NULL, 1);
        TASK_CHECK(rc == 0, "decode instance B");

        struct frame_capture *cb = capture_drain(&dec_b);
        TASK_CHECK(cb->nsubbands == cfg->nsubbands &&
                   cb->nblocks == cfg->nblocks &&
                   cb->nchannels == nch, "capture B shape");
        memset(mb, 0, sizeof(mb));
        for (int ch = 0; ch < nch; ch++)
            for (int blk = 0; blk < cb->nblocks; blk++)
                model_block(&model_b[ch], h,
                    cb->sb[ch] + blk * cb->nsubbands, cb->scale[ch],
                    mb[ch] + blk * cb->nsubbands, 1);

        for (int ch = 0; ch < nch; ch++) {
            for (int i = 0; i < nsamples; i++) {
                TASK_CHECK(pa[ch][i] == ma[ch][i],
                    "frames PCM instance A");
                TASK_CHECK(pb[ch][i] == mb[ch][i],
                    "frames PCM instance B");
            }
        }

        /* PLC frames carry zero sub-band samples and zero scale: the model
         * must have advanced on the same zeros to drain the tail. */
        if (plc) {
            for (int ch = 0; ch < nch; ch++)
                for (int i = 0; i < nsamples; i++) {
                    TASK_CHECK(ca->sb[ch][i] == 0 && cb->sb[ch][i] == 0,
                        "PLC zero sub-band input");
                }
            TASK_CHECK(ca->scale[0] == 0 && cb->scale[0] == 0,
                "PLC zero scale");
        }
    }

    /* Reset one instance, then compare a fresh run against a brand new
     * instance, both still checked against the independent model. */
    sbc_reset(&dec_a);
    for (int ch = 0; ch < 2; ch++)
        model_init(&model_a[ch], cfg->nsubbands);

    sbc_t dec_c = { 0 };
    struct model_synth model_c[2];
    for (int ch = 0; ch < 2; ch++)
        model_init(&model_c[ch], cfg->nsubbands);
    capture_register(&dec_c);

    for (int fr = 0; fr < 3; fr++) {
        for (int i = 0; i < nsamples; i++) {
            in_l[i] = gen_next(gen_l_a);
            in_r[i] = gen_next(gen_r_a);
        }
        int rc = sbc_encode(&enc_a, in_l, 1,
            nch > 1 ? in_r : in_l, 1, &f, buf_a, frame_size);
        TASK_CHECK(rc == 0, "post-reset encode");

        int16_t pc[2][SBC_MAX_SAMPLES], mc[2][SBC_MAX_SAMPLES];
        memset(pc, 0, sizeof(pc));
        memset(mc, 0, sizeof(mc));
        memset(pa, 0, sizeof(pa));
        memset(ma, 0, sizeof(ma));

        rc = sbc_decode(&dec_a, buf_a, frame_size, &f,
            pa[0], 1, nch > 1 ? pa[1] : NULL, 1);
        TASK_CHECK(rc == 0, "post-reset decode A");
        rc = sbc_decode(&dec_c, buf_a, frame_size, &f,
            pc[0], 1, nch > 1 ? pc[1] : NULL, 1);
        TASK_CHECK(rc == 0, "fresh instance decode C");

        struct frame_capture *ca = capture_drain(&dec_a);
        struct frame_capture *cc = capture_drain(&dec_c);

        for (int ch = 0; ch < nch; ch++)
            for (int blk = 0; blk < ca->nblocks; blk++) {
                model_block(&model_a[ch], h,
                    ca->sb[ch] + blk * ca->nsubbands, ca->scale[ch],
                    ma[ch] + blk * ca->nsubbands, 1);
                model_block(&model_c[ch], h,
                    cc->sb[ch] + blk * cc->nsubbands, cc->scale[ch],
                    mc[ch] + blk * cc->nsubbands, 1);
            }

        for (int ch = 0; ch < nch; ch++)
            for (int i = 0; i < nsamples; i++) {
                TASK_CHECK(pa[ch][i] == pc[ch][i],
                    "reset matches fresh instance");
                TASK_CHECK(pa[ch][i] == ma[ch][i],
                    "reset instance matches model");
                TASK_CHECK(pc[ch][i] == mc[ch][i],
                    "fresh instance matches model");
            }
    }
}

static int run_frames(void)
{
    memset(captures, 0, sizeof(captures));

    static const struct seq_config configs[] = {
        { false, SBC_FREQ_48K, SBC_MODE_MONO,          16, 4, 32, 7 },
        { false, SBC_FREQ_48K, SBC_MODE_STEREO,        16, 8, 32, 7 },
        { false, SBC_FREQ_44K1, SBC_MODE_JOINT_STEREO, 16, 4, 35, 7 },
        { false, SBC_FREQ_32K, SBC_MODE_JOINT_STEREO,  16, 8, 38, 7 },
        { true,  SBC_FREQ_16K, SBC_MODE_MONO,          15, 8, 26, 7 },
    };

    for (unsigned i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        struct signal_gen gla, gra, glb, grb;
        gen_init(&gla, 0.071, 9000, 0x1234abcd, +1);
        gen_init(&gra, 0.053, 7000, 0x9e3779b9, -1);
        gen_init(&glb, 0.041, 8500, 0xdeadbeef, +1);
        gen_init(&grb, 0.029, 6500, 0x243f6a88, -1);

        run_one_sequence(&configs[i], &gla, &gra, &glb, &grb);
    }

    return 1;
}


/* ----------------------------------------------------------------------------
 *  Entry point called by task_support/main.c (no main defined here)
 * ------------------------------------------------------------------------- */

int task_group(const char *group)
{
    if (!strcmp(group, "matrix"))
        return run_matrix();
    if (!strcmp(group, "history"))
        return run_history();
    if (!strcmp(group, "frames"))
        return run_frames();

    return 0;
}
