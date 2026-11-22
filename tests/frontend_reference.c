/*
 * AMR-WB encoder front-end reference model.
 *
 * Independent, integer-exact model of the first signal-processing stage:
 *
 *   16 kHz input (320 samples, 14-bit masked)
 *     -> 4-phase / 30-tap polyphase decimation to 12.8 kHz (256 samples)
 *     -> 12 extra "preview" decimated samples built from 15 zero 16 kHz
 *        inputs after the real 256-point advance
 *     -> 50 Hz high-pass filter with high/low word splitting (268 points)
 *     -> fixed pre-emphasis, max search, Q_new and two-frame Q_max clamp
 *     -> rescaling of the previous speech interval
 *
 * The model is written from the arithmetic specification.  It does not call
 * the production routines, does not use their fixed-point macros, and does
 * not transcribe their unrolled loops.  Every 32-bit operation runs through
 * unsigned helpers so wrap-around, arithmetic right shift and rounding are
 * fully defined.  Filter coefficients are format constants (table below).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "frontend_reference.h"

#define NCOEF      15                       /* NB_COEF_DOWN              */
#define NMEM       FEREF_NMEM               /* 2 * L_FILT16k = 30        */
#define NSAMP      FEREF_NSAMP              /* 268                       */
#define NPAST      FEREF_NPAST              /* 116                       */
#define NHIST      FEREF_NHIST              /* 128                       */
#define QMASK      0xfffC
#define EHF_MASK   0x0008
#define PREEMPH    22282
#define QMU        (PREEMPH >> 1)           /* 11141, Q14                */
#define QMAX       8

/* 1/5-resolution interpolation filter, Q14, four phases x 30 taps. */
static const int16_t fir_down[4][30] = {
    {-5, 24, -50, 54, 0, -128, 294, -408, 344, 0, -647, 1505, -2379, 3034,
     13107, 3034, -2379, 1505, -647, 0, 344, -408, 294, -128, 0, 54, -50,
     24, -5, 0},
    {-6, 19, -26, 0, 77, -188, 270, -233, 0, 434, -964, 1366, -1293, 0,
     12254, 6575, -2746, 1030, 0, -507, 601, -441, 198, 0, -95, 99, -58,
     18, 0, -1},
    {-3, 9, 0, -41, 111, -170, 153, 0, -295, 649, -888, 770, 0, -1997,
     9894, 9894, -1997, 0, 770, -888, 649, -295, 0, 153, -170, 111, -41,
     0, 9, -3},
    {-1, 0, 18, -58, 99, -95, 0, 198, -441, 601, -507, 0, 1030, -2746,
     6575, 12254, 0, -1293, 1366, -964, 434, 0, -233, 270, -188, 77, 0,
     -26, 19, -6}
};

/* Second-order high-pass, Q12; a[] is stored scaled by two. */
static const int32_t hp_b0 = 4053;
static const int32_t hp_b1 = -8106;
static const int32_t hp_a1 = 16211;
static const int32_t hp_a2 = -8021;

struct FeModel {
    int16_t decim_mem[NMEM];
    int16_t hp_mem[6];
    int16_t preemph_mem;
    int16_t q_old;
    int16_t q_max0;
    int16_t q_max1;
    int16_t hist[NHIST];
};

/* ------------------------------------------------------------------ */
/* Well-defined 32-bit primitives                                     */
/* ------------------------------------------------------------------ */

static int32_t wrap_add(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

static int32_t wrap_sub(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

static int32_t wrap_shl(int32_t v, int n)
{
    if (n <= 0)
        return v;
    return (int32_t)((uint32_t)v << n);
}

/* Arithmetic right shift on the two's-complement representation. */
static int32_t sar(int32_t v, int n)
{
    uint32_t u;
    if (n <= 0)
        return v;
    if (n >= 31)
        return (v < 0) ? -1 : 0;
    u = (uint32_t)v;
    if (v < 0)
        u = (u >> n) | (~(uint32_t)0u << (32 - n));
    else
        u >>= n;
    return (int32_t)u;
}

static int32_t sat_add(int32_t a, int32_t b)
{
    int64_t s = (int64_t)a + (int64_t)b;
    if (s > INT32_MAX)
        return INT32_MAX;
    if (s < INT32_MIN)
        return INT32_MIN;
    return (int32_t)s;
}

/* Saturating left shift, one bit at a time, matching the fixed-point spec. */
static int32_t l_shl(int32_t v, int n)
{
    for (; n > 0; --n) {
        if (v > 0x3fffffff)
            return INT32_MAX;
        if (v < (int32_t)0xc0000000)
            return INT32_MIN;
        v = wrap_shl(v, 1);
    }
    return v;
}

static int16_t extract_h(int32_t v)
{
    return (int16_t)(uint16_t)((uint32_t)v >> 16);
}

static int16_t round_h(int32_t v)
{
    return extract_h(wrap_add(v, 0x8000));
}

static int16_t mask14(int16_t v)
{
    return (int16_t)(uint16_t)((uint16_t)v & (uint16_t)QMASK);
}

static int32_t l_abs(int32_t v)
{
    if (v == INT32_MIN)
        return INT32_MAX;
    return v < 0 ? -v : v;
}

/* norm_s(): left shifts needed to reach the [0x4000,0x7fff] range. */
static int norm_s(int16_t v)
{
    int n = 0;
    if (v == 0)
        return 0;
    if (v == -1)
        return 15;
    if (v < 0)
        v = (int16_t)(uint16_t)~v;
    while ((uint16_t)v < 0x4000u) {
        v = (int16_t)(uint16_t)((uint16_t)v << 1);
        ++n;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Polyphase decimation                                               */
/* ------------------------------------------------------------------ */

/*
 * `base` is the start of the internal window: [memory][input samples].
 * Output j sits at input position 5*j/4; its 30 taps begin at index
 * (5*j/4)+1 of that window.  This matches the production pointer layout,
 * where Down_samp receives base+NCOEF and reads from (base+NCOEF)-NCOEF+1.
 */
static void down_samp(const int16_t *base, int nout, int16_t *out, FeFrame *tr)
{
    int j;
    for (j = 0; j < nout; ++j) {
        int pos = j * 5;             /* Q2: 5/4 per output */
        int i = pos >> 2;
        int frac = pos & 3;
        const int16_t *x = base + i + 1;
        const int16_t *y = fir_down[frac];
        int32_t sum = 0;
        int k;
        for (k = 0; k < 30; ++k)
            sum = wrap_add(sum, (int32_t)x[k] * (int32_t)y[k]);
        sum = l_shl(sum, 2);
        if (tr != NULL) {
            ++tr->phase_out[frac];
            if ((uint32_t)(sum & 0xffff) == 0x8000u) {
                if (sum >= 0) {
                    ++tr->decim_tie_pos;
                    ++tr->phase_tie_pos[frac];
                } else {
                    ++tr->decim_tie_neg;
                    ++tr->phase_tie_neg[frac];
                }
            }
        }
        out[j] = extract_h(sat_add(sum, 0x8000));
    }
}

/* ------------------------------------------------------------------ */
/* 50 Hz high-pass, high/low split recursion                          */
/* ------------------------------------------------------------------ */

static void hp50(const int16_t *in, int n, int32_t mem[6], int16_t *out)
{
    int32_t y2_hi = mem[0], y2_lo = mem[1];
    int32_t y1_hi = mem[2], y1_lo = mem[3];
    int32_t x0 = mem[4], x1 = mem[5];
    int i;

    for (i = 0; i < n; ++i) {
        int32_t x2 = x1;
        int32_t t, inner;
        x1 = x0;
        x0 = in[i];

        t = 8192;
        t = wrap_add(t, y1_lo * hp_a1);
        t = wrap_add(t, y2_lo * hp_a2);
        t = sar(t, 14);

        inner = y1_hi * hp_a1;
        inner = wrap_add(inner, y2_hi * hp_a2);
        inner = wrap_add(inner, (x0 + x2) * hp_b0);
        inner = wrap_add(inner, x1 * hp_b1);
        inner = wrap_shl(inner, 1);
        t = wrap_add(t, inner);

        t = l_shl(t, 2);             /* Q12 -> Q13 */
        y2_hi = y1_hi;
        y2_lo = y1_lo;
        y1_hi = extract_h(t);
        y1_lo = (int16_t)(uint16_t)(((uint32_t)t & 0xffffu) >> 1);
        t = l_shl(t, 1);
        out[i] = extract_h(sat_add(t, 0x8000));
    }

    mem[0] = y2_hi;
    mem[1] = y2_lo;
    mem[2] = y1_hi;
    mem[3] = y1_lo;
    mem[4] = x0;
    mem[5] = x1;
}

/* ------------------------------------------------------------------ */
/* Scaling                                                            */
/* ------------------------------------------------------------------ */

static void scale_sig(int16_t *x, int n, int exp)
{
    int i;
    if (exp > 0) {
        for (i = n - 1; i >= 0; --i) {
            int32_t t = l_shl((int32_t)x[i], 16 + exp);
            x[i] = extract_h(sat_add(t, 0x8000));
        }
    } else {
        int e = -exp;
        for (i = n - 1; i >= 0; --i) {
            int32_t t = wrap_shl((int32_t)x[i], 16);
            t = sar(t, e);
            x[i] = extract_h(wrap_add(t, 0x8000));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public model                                                       */
/* ------------------------------------------------------------------ */

FeModel *fe_model_new(void)
{
    FeModel *m = (FeModel *)calloc(1, sizeof(*m));
    if (m != NULL)
        fe_model_reset(m);
    return m;
}

void fe_model_reset(FeModel *m)
{
    memset(m, 0, sizeof(*m));
    m->q_old = 15;
    m->q_max0 = 15;
    m->q_max1 = 15;
}

void fe_model_free(FeModel *m)
{
    free(m);
}

int fe_is_homing_frame(const int16_t in[FEREF_L_FRAME16K])
{
    int i;
    for (i = 0; i < FEREF_L_FRAME16K; ++i)
        if ((int16_t)(uint16_t)((uint16_t)in[i] ^ (uint16_t)EHF_MASK) != 0)
            return 0;
    return 1;
}

int fe_model_frame(FeModel *m, const int16_t in16[FEREF_L_FRAME16K], FeFrame *out)
{
    int16_t masked[FEREF_L_FRAME16K];
    int16_t signal[NMEM + FEREF_L_FRAME16K];
    int16_t prev[NMEM + FEREF_L_FILT16K];
    int16_t hp_all[NSAMP];
    int16_t pre[NSAMP];
    int16_t scaled_past[NPAST];
    int32_t hp_mem[6];
    int32_t l_max, l_tmp;
    int16_t tmp;
    int shift, q_new, exp;
    int i;

    if (m == NULL || in16 == NULL || out == NULL)
        return -1;
    memset(out, 0, sizeof(*out));

    for (i = 0; i < FEREF_L_FRAME16K; ++i)
        masked[i] = mask14(in16[i]);

    /* ---- polyphase decimation over the 320 real inputs ---- */
    memcpy(signal, m->decim_mem, sizeof(m->decim_mem));
    memcpy(signal + NMEM, masked, sizeof(masked));
    down_samp(signal, FEREF_L_FRAME, out->decim, out);

    /* Long-term memory is the last 30 *real* input samples. */
    for (i = 0; i < NMEM; ++i)
        out->mem_decim[i] = masked[FEREF_L_FRAME16K - NMEM + i];

    /* ---- 12 preview points from 15 zero 16 kHz inputs ---- */
    memcpy(prev, out->mem_decim, sizeof(m->decim_mem));
    memset(prev + NMEM, 0, FEREF_L_FILT16K * sizeof(int16_t));
    down_samp(prev, FEREF_L_FILT, out->preview, out);

    /* ---- 50 Hz high-pass over real points, then preview ---- */
    for (i = 0; i < 6; ++i)
        hp_mem[i] = m->hp_mem[i];
    hp50(out->decim, FEREF_L_FRAME, hp_mem, hp_all);
    for (i = 0; i < 6; ++i)
        out->mem_sig_in[i] = (int16_t)hp_mem[i];
    /* Preview continues from the post-real memory but is discarded. */
    hp50(out->preview, FEREF_L_FILT, hp_mem, hp_all + FEREF_L_FRAME);
    for (i = 0; i < 6; ++i)
        out->hp_mem_preview[i] = (int16_t)hp_mem[i];
    memcpy(out->hp, hp_all, sizeof(hp_all));

    /* ---- pre-emphasis: max search over real + preview ---- */
    l_tmp = wrap_sub(wrap_shl((int32_t)hp_all[0], 15),
                     wrap_shl((int32_t)m->preemph_mem * QMU, 1));
    l_max = l_abs(l_tmp);
    for (i = 1; i < NSAMP; ++i) {
        l_tmp = wrap_sub(wrap_shl((int32_t)hp_all[i], 15),
                         wrap_shl((int32_t)hp_all[i - 1] * QMU, 1));
        l_tmp = l_abs(l_tmp);
        if (l_tmp > l_max)
            l_max = l_tmp;
    }

    tmp = extract_h(l_max);
    if (tmp == 0) {
        shift = QMAX;
    } else {
        shift = norm_s(tmp) - 1;
        if (shift < 0)
            shift = 0;
        if (shift > QMAX)
            shift = QMAX;
    }

    q_new = shift;
    if (q_new > m->q_max0)
        q_new = m->q_max0;
    if (q_new > m->q_max1)
        q_new = m->q_max1;
    exp = q_new - m->q_old;

    out->q_new = (int16_t)q_new;
    out->q_old = m->q_old;
    out->q_uncapped = (int16_t)shift;
    out->q_max0 = (int16_t)shift;       /* uncapped value stored by production */
    out->q_max1 = m->q_max0;
    m->q_old = (int16_t)q_new;
    m->q_max1 = m->q_max0;
    m->q_max0 = (int16_t)shift;

    /* mem_preemph keeps the last *real* high-pass point, not the preview. */
    tmp = hp_all[FEREF_L_FRAME - 1];
    for (i = NSAMP - 1; i > 0; --i) {
        l_tmp = wrap_sub(wrap_shl((int32_t)hp_all[i], 15),
                         wrap_shl((int32_t)hp_all[i - 1] * QMU, 1));
        l_tmp = wrap_shl(l_tmp, q_new);
        if ((uint32_t)(l_tmp & 0xffff) == 0x8000u) {
            if (l_tmp >= 0) ++out->pre_tie_pos; else ++out->pre_tie_neg;
        }
        pre[i] = round_h(l_tmp);
    }
    l_tmp = wrap_sub(wrap_shl((int32_t)hp_all[0], 15),
                     wrap_shl((int32_t)m->preemph_mem * QMU, 1));
    l_tmp = wrap_shl(l_tmp, q_new);
    if ((uint32_t)(l_tmp & 0xffff) == 0x8000u) {
        if (l_tmp >= 0) ++out->pre_tie_pos; else ++out->pre_tie_neg;
    }
    pre[0] = round_h(l_tmp);
    out->mem_preemph = tmp;
    m->preemph_mem = tmp;
    memcpy(out->preemph, pre, sizeof(pre));

    /* ---- rescale the previous speech interval ---- */
    memcpy(scaled_past, m->hist, sizeof(scaled_past));
    scale_sig(scaled_past, NPAST, exp);
    memcpy(out->past_scaled, scaled_past, sizeof(scaled_past));

    /* Next old_speech image: scaled old interval then 12 new scaled points. */
    {
        int16_t next_old[NHIST];
        for (i = 0; i < NHIST; ++i)
            next_old[i] = pre[(FEREF_NSAMP - NHIST) + i];
        memcpy(m->hist, next_old, sizeof(next_old));
        memcpy(out->next_old_speech, next_old, sizeof(next_old));
    }

    memcpy(m->decim_mem, out->mem_decim, sizeof(out->mem_decim));
    memcpy(m->hp_mem, out->mem_sig_in, sizeof(out->mem_sig_in));
    return 0;
}
