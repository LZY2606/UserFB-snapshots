/* Copyright (c) 2026 Xiph.Org Foundation */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* NLSF contract regression tests for the SILK parameter decoder.
 *
 * Three groups:
 *  - vectors: an independent wide-integer model of the NB/MB (order 10)
 *    and WB (order 16) NLSF codebook paths, compared bit-exactly against
 *    the production decoder on paths that need no stabilization, plus
 *    property checks on legal paths that do need stabilization.
 *  - history: interpolation, NLSF2A conversion, after-loss bandwidth
 *    expansion and NLSF history handling, exercised through the
 *    controlled silk_decode_parameters() entry point with expectations
 *    from independent models (direct even/odd root polynomial
 *    multiplication for the LPC reference).
 *  - packets: real narrowband and wideband SILK streams produced by the
 *    public encoder, observed through the TASK_NLSF_TEST hooks and
 *    checked frame by frame against the same independent models.
 *
 * The models below never call silk_NLSF_unpack, silk_NLSF_decode or the
 * production fixed-point macros, and never re-use the production
 * dequantization code; format tables (codebooks, delta-min vectors, the
 * quantized cosine table) are referenced directly as format constants.
 */

#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "main.h"           /* SILK decoder internals (structs, prototypes) */
#include "tables.h"         /* NLSF codebooks, quantized cosine table       */
#include "task_nlsf_test.h" /* read-only observation hook                   */
#include "opus.h"           /* public encoder/decoder API                   */

/* Format constants restated independently of the production fixed-point
   macros; the values are part of the bitstream format. */
#define MODEL_LEVEL_ADJ_Q10 102    /* 0.1 residual level offset, Q10      */
#define MODEL_BWE_Q16       63570  /* after-loss bandwidth expansion, Q16 */
#define LPC_TOL_Q12         8      /* allowed LPC reference deviation     */
#define LPC_FIT_LIMIT       7.9    /* |a| bound proving no LPC re-fitting */

#define N_PATTERNS 12
#define MAX_REC    128
#define N_PACKETS  16
#define FRAME      960             /* 20 ms at 48 kHz */
#define DROP_PKT   7               /* packet concealed by PLC           */
#define RESET_PKT  12              /* decoder reset before this packet  */

/* ------------------------------------------------------------------ */
/* Wide-integer helpers with explicitly defined semantics              */
/* ------------------------------------------------------------------ */

/* Right shift, explicitly a floor (arithmetic) shift for negatives. */
static opus_int32 shr_floor32(opus_int32 a, int s)
{
    if (a >= 0) return a >> s;
    return (opus_int32)(-(((-(opus_int64)a) + ((opus_int64)1 << s) - 1) >> s));
}

/* 64-bit product scaled by 2^-s with a floor shift. */
static opus_int32 mul_shr_floor(opus_int32 a, opus_int32 b, int s)
{
    opus_int64 p = (opus_int64)a * (opus_int64)b;
    if (p >= 0) return (opus_int32)(p >> s);
    return (opus_int32)(-(((-p) + ((opus_int64)1 << s) - 1) >> s));
}

/* Division truncating toward zero (C99 semantics, stated explicitly). */
static opus_int32 div_trunc_zero(opus_int32 a, opus_int32 b)
{
    return a / b;
}

/* Round-to-nearest shift (add half, then floor) for the chirp recursion. */
static opus_int32 shr_round32(opus_int32 a, int s)
{
    return shr_floor32(a + ((opus_int32)1 << (s - 1)), s);
}

/* ------------------------------------------------------------------ */
/* Independent model: NLSF codebook path                               */
/* ------------------------------------------------------------------ */

/* Predictor for one dimension, read out of the packed selection bytes.
   Each byte covers two dimensions: bit 0 selects the predictor row for
   the even dimension, bit 4 for the odd dimension. */
static int model_pred_q8(const silk_NLSF_CB_struct *cb, int cb1, int dim)
{
    unsigned entry = cb->ec_sel[cb1 * (cb->order / 2) + dim / 2];
    unsigned row   = (dim & 1) ? (entry >> 4) & 1u : entry & 1u;
    return cb->pred_Q8[dim + row * (cb->order - 1)];
}

/* Full codebook path: first-stage index in idx[0], residual indices in
   idx[1..order].  Produces the pre-stabilization Q15 vector. */
static void model_nlsf_q15(const silk_NLSF_CB_struct *cb, const opus_int8 *idx, opus_int16 *out)
{
    opus_int32 res_q10[MAX_LPC_ORDER];
    opus_int32 acc = 0;
    const int  cb1 = idx[0];
    int i;

    /* Predictive residual dequantization, unrolled backwards from the
       last dimension so each predictor sees the decoded successor. */
    for (i = cb->order - 1; i >= 0; i--) {
        opus_int32 pred = mul_shr_floor((opus_int16)acc, model_pred_q8(cb, cb1, i), 8);
        opus_int32 cur  = idx[1 + i] * 1024;          /* residual in Q10           */
        if (cur > 0)      cur -= MODEL_LEVEL_ADJ_Q10; /* nonzero residuals stay    */
        else if (cur < 0) cur += MODEL_LEVEL_ADJ_Q10; /* clear of zero             */
        acc = pred + mul_shr_floor(cur, cb->quantStepSize_Q16, 16);
        res_q10[i] = acc;
    }

    /* Undo the inverse square-rooted weights, add first-stage centers. */
    for (i = 0; i < cb->order; i++) {
        opus_int32 v = div_trunc_zero(res_q10[i] * 16384, cb->CB1_Wght_Q9[cb1 * cb->order + i])
                     + cb->CB1_NLSF_Q8[cb1 * cb->order + i] * 128;
        if (v < 0)     v = 0;
        if (v > 32767) v = 32767;
        out[i] = (opus_int16)v;
    }
}

/* Smallest spacing margin against the delta-min vector; >= 0 means the
   vector meets every boundary and neighbor distance, so stabilization
   would leave it untouched. */
static int model_min_margin(const silk_NLSF_CB_struct *cb, const opus_int16 *nlsf)
{
    int i;
    int m = nlsf[0] - cb->deltaMin_Q15[0];
    for (i = 1; i < cb->order; i++) {
        int d = nlsf[i] - nlsf[i - 1] - cb->deltaMin_Q15[i];
        if (d < m) m = d;
    }
    {
        int d = 32768 - nlsf[cb->order - 1] - cb->deltaMin_Q15[cb->order];
        if (d < m) m = d;
    }
    return m;
}

/* ------------------------------------------------------------------ */
/* Independent model: interpolation, bandwidth expansion, LPC          */
/* ------------------------------------------------------------------ */

/* First-half-frame NLSF interpolation from the previously committed
   vector and the current vector. */
static void model_interp_q15(const opus_int16 *prev, const opus_int16 *cur,
                             int coef_q2, int order, opus_int16 *out)
{
    int i;
    for (i = 0; i < order; i++)
        out[i] = (opus_int16)(prev[i] + shr_floor32(coef_q2 * (cur[i] - prev[i]), 2));
}

/* After-loss bandwidth expansion (chirp) of an LPC set. */
static void model_bwexpand_q12(const opus_int16 *in, int order, opus_int16 *out)
{
    opus_int32 chirp = MODEL_BWE_Q16;
    const opus_int32 chirp_minus_one = MODEL_BWE_Q16 - 65536;
    int i;
    for (i = 0; i < order; i++) out[i] = in[i];
    for (i = 0; i < order - 1; i++) {
        out[i] = (opus_int16)shr_round32(chirp * out[i], 16);
        chirp += shr_round32(chirp * chirp_minus_one, 16);
    }
    out[order - 1] = (opus_int16)shr_round32(chirp * out[order - 1], 16);
}

/* LPC reference: the even-indexed roots build p(z) and the odd-indexed
   roots build q(z) by direct multiplication of the quadratic factors
   (1 - c z^-1 + z^-2) in double precision, then
   A(z) = ((1 + z^-1) p(z) + (1 - z^-1) q(z)) / 2.
   Uses the quantized cosine table but not the production ordering
   permutation or the fixed-point polynomial recurrence.
   Returns the largest |a| so callers can prove no LPC re-fitting bit. */
static double ref_nlsf2a_q12(const opus_int16 *nlsf, int order, opus_int32 *a_q12)
{
    double c[MAX_LPC_ORDER];
    double p[MAX_LPC_ORDER + 1] = { 0 };
    double q[MAX_LPC_ORDER + 1] = { 0 };
    double t[MAX_LPC_ORDER + 1];
    double maxabs = 0.0;
    int np = 1, nq = 1, i, j;

    /* piecewise-linear 2*cos() from the quantized table */
    for (i = 0; i < order; i++) {
        int f_int  = nlsf[i] >> 8;
        int f_frac = nlsf[i] - (f_int << 8);
        double base  = silk_LSFCosTab_FIX_Q12[f_int];
        double delta = silk_LSFCosTab_FIX_Q12[f_int + 1] - base;
        c[i] = (base * 256.0 + delta * f_frac) / 1048576.0;
    }

    /* multiply out the even-root and odd-root quadratics directly */
    p[0] = 1.0; q[0] = 1.0;
    for (i = 0; i < order; i++) {
        double *acc = (i & 1) ? q : p;
        int    *n   = (i & 1) ? &nq : &np;
        for (j = 0; j <= *n + 1; j++) t[j] = 0.0;
        for (j = 0; j < *n; j++) {
            t[j]     += acc[j];
            t[j + 1] -= c[i] * acc[j];
            t[j + 2] += acc[j];
        }
        *n += 2;
        for (j = 0; j < *n; j++) acc[j] = t[j];
    }

    /* A(z) = ((1 + z^-1) p(z) + (1 - z^-1) q(z)) / 2, dropping a_0 = 1.
       The whitening coefficients are the negated monic taps. */
    for (i = 1; i <= order; i++) {
        double a = -((p[i] + p[i - 1]) + (q[i] - q[i - 1])) * 0.5;
        if (fabs(a) > maxabs) maxabs = fabs(a);
        a_q12[i - 1] = (opus_int32)lrint(a * 4096.0);
    }
    return maxabs;
}

/* ------------------------------------------------------------------ */
/* Comparison helpers: first difference reports packet/order/dimension */
/* ------------------------------------------------------------------ */

static int g_packet = -1;

static int check_exact(const char *what, int order, const opus_int16 *exp, const opus_int16 *act)
{
    int i;
    for (i = 0; i < order; i++) {
        if (exp[i] != act[i]) {
            fprintf(stderr, "MISMATCH %s packet=%d order=%d dim=%d expected=%d actual=%d\n",
                    what, g_packet, order, i, (int)exp[i], (int)act[i]);
            return 0;
        }
    }
    return 1;
}

static int check_close_q12(const char *what, int order, const opus_int32 *ref,
                           const opus_int16 *act, int tol, int *max_err)
{
    int i;
    for (i = 0; i < order; i++) {
        int e = (int)(ref[i] - act[i]);
        if (e < 0) e = -e;
        if (e > *max_err) *max_err = e;
        if (e > tol) {
            fprintf(stderr, "MISMATCH %s packet=%d order=%d dim=%d expected=%d actual=%d\n",
                    what, g_packet, order, i, (int)ref[i], (int)act[i]);
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Residual index patterns swept over the first-stage entries          */
/* ------------------------------------------------------------------ */

static void fill_pattern(int pat, int order, opus_int8 *res)
{
    int i;
    for (i = 0; i < order; i++) {
        switch (pat) {
        case 0:  res[i] = 0; break;
        case 1:  res[i] = 1; break;
        case 2:  res[i] = -1; break;
        case 3:  res[i] = (opus_int8)((i & 1) ? 2 : -2); break;
        case 4:  res[i] = (opus_int8)((i & 1) ? -3 : 3); break;
        case 5:  res[i] = (opus_int8)((i * 7) % 9 - 4); break;
        case 6:  res[i] = (opus_int8)(4 - (i * 5) % 9); break;
        default: res[i] = (opus_int8)(((i * 13 + pat * 7) % 9) - 4); break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* vectors group                                                       */
/* ------------------------------------------------------------------ */

static int vectors_exact(const silk_NLSF_CB_struct *cb, const char *tag)
{
    opus_int8  idx[MAX_LPC_ORDER + 1];
    opus_int16 expect[MAX_LPC_ORDER], actual[MAX_LPC_ORDER];
    int cb1, pat;
    int compared = 0, compared_nonzero = 0, min_margin = 32767;

    for (cb1 = 0; cb1 < cb->nVectors; cb1++) {
        for (pat = 0; pat < N_PATTERNS; pat++) {
            int margin;
            idx[0] = (opus_int8)cb1;
            fill_pattern(pat, cb->order, &idx[1]);
            model_nlsf_q15(cb, idx, expect);
            margin = model_min_margin(cb, expect);
            if (margin < 0) continue;  /* would be stabilized: not an exact path */
            /* the path clears every delta-min boundary on its own */
            TASK_CHECK(margin >= 0);
            silk_NLSF_decode(actual, idx, cb);
            TASK_CHECK(check_exact(tag, cb->order, expect, actual));
            compared++;
            if (pat != 0) compared_nonzero++;
            if (margin < min_margin) min_margin = margin;
        }
    }
    TASK_CHECK(compared >= 16);
    TASK_CHECK(compared_nonzero > 0);  /* nonzero residuals really exercised */
    TASK_CHECK(min_margin >= 0);
    printf("INFO %s exact_paths=%d nonzero=%d min_margin=%d\n",
           tag, compared, compared_nonzero, min_margin);
    return 0;
}

static int vectors_stabilized(const silk_NLSF_CB_struct *cb, const char *tag)
{
    opus_int8  idx[MAX_LPC_ORDER + 1];
    opus_int16 raw[MAX_LPC_ORDER], out[MAX_LPC_ORDER], again[MAX_LPC_ORDER];
    int cb1, pat, i;
    int found = 0;

    for (cb1 = 0; cb1 < cb->nVectors && found < 4; cb1++) {
        for (pat = 0; pat < N_PATTERNS && found < 4; pat++) {
            int margin, in_range;
            idx[0] = (opus_int8)cb1;
            fill_pattern(pat, cb->order, &idx[1]);
            model_nlsf_q15(cb, idx, raw);
            margin = model_min_margin(cb, raw);
            if (margin >= 0) continue;
            /* legal index path whose raw vector violates the spacing:
               the unstabilized vector must not be the final answer */
            TASK_CHECK(margin < 0);
            silk_NLSF_decode(out, idx, cb);
            /* boundary and neighbor spacing restored */
            TASK_CHECK(model_min_margin(cb, out) >= 0);
            /* every value finite and inside the Q15 contract range */
            in_range = 1;
            for (i = 0; i < cb->order; i++)
                if (out[i] < 0 || out[i] > 32767) in_range = 0;
            TASK_CHECK(in_range);
            /* stabilizing the result again changes nothing */
            memcpy(again, out, (size_t)cb->order * sizeof(opus_int16));
            silk_NLSF_stabilize(again, cb->deltaMin_Q15, cb->order);
            TASK_CHECK(memcmp(again, out, (size_t)cb->order * sizeof(opus_int16)) == 0);
            found++;
        }
    }
    TASK_CHECK(found > 0);
    printf("INFO %s stabilized_paths=%d\n", tag, found);
    return 0;
}

static int vectors_order10_exact(void)      { return vectors_exact(&silk_NLSF_CB_NB_MB, "nb_mb"); }
static int vectors_order16_exact(void)      { return vectors_exact(&silk_NLSF_CB_WB, "wb"); }
static int vectors_order10_stabilized(void) { return vectors_stabilized(&silk_NLSF_CB_NB_MB, "nb_mb"); }
static int vectors_order16_stabilized(void) { return vectors_stabilized(&silk_NLSF_CB_WB, "wb"); }

/* ------------------------------------------------------------------ */
/* Observation hook recording                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int        order, fs_kHz, lossCnt, first_reset, interp, packet;
    opus_int8  idx[MAX_LPC_ORDER + 1];
    opus_int16 prev[MAX_LPC_ORDER];        /* history offered at entry      */
    opus_int16 cur[MAX_LPC_ORDER];         /* current NLSF, post-decode     */
    opus_int16 lpc_pre[2][MAX_LPC_ORDER];  /* LPC sets before after-loss BWE */
    opus_int16 lpc_post[2][MAX_LPC_ORDER]; /* LPC sets after after-loss BWE  */
    opus_int16 saved[MAX_LPC_ORDER];       /* history stored for next frame  */
    int        stage;
} frame_rec;

static frame_rec g_rec[MAX_REC];
static int       g_nrec;
static int       g_rec_on;

static void obs_hook(int event, const void *dec, const void *ctrl, const opus_int16 *nlsf)
{
    const silk_decoder_state   *st = (const silk_decoder_state *)dec;
    const silk_decoder_control *ct = (const silk_decoder_control *)ctrl;
    frame_rec *r;
    if (!g_rec_on || g_nrec >= MAX_REC) return;
    r = &g_rec[g_nrec];
    if (event == TASK_NLSF_EVENT_NLSF) {
        memset(r, 0, sizeof(*r));
        r->order       = st->LPC_order;
        r->fs_kHz      = st->fs_kHz;
        r->lossCnt     = st->lossCnt;
        r->first_reset = st->first_frame_after_reset;
        r->interp      = st->indices.NLSFInterpCoef_Q2;
        r->packet      = g_packet;
        memcpy(r->idx,  st->indices.NLSFIndices, (size_t)st->LPC_order + 1);
        memcpy(r->prev, st->prevNLSF_Q15, st->LPC_order * sizeof(opus_int16));
        memcpy(r->cur,  nlsf, st->LPC_order * sizeof(opus_int16));
        r->stage = 1;
    } else if (event == TASK_NLSF_EVENT_LPC && r->stage == 1) {
        memcpy(r->lpc_pre, ct->PredCoef_Q12, sizeof(r->lpc_pre));
        memcpy(r->saved, st->prevNLSF_Q15, st->LPC_order * sizeof(opus_int16));
        r->stage = 2;
    } else if (event == TASK_NLSF_EVENT_LPC_FINAL && r->stage == 2) {
        memcpy(r->lpc_post, ct->PredCoef_Q12, sizeof(r->lpc_post));
        r->stage = 3;
        g_nrec++;
    }
}

static void rec_start(void)
{
    silk_task_nlsf_hook = obs_hook;
    g_rec_on = 1;
    g_nrec   = 0;
}

static void rec_stop(void)
{
    g_rec_on = 0;
}

/* ------------------------------------------------------------------ */
/* Shared per-frame verification against the independent models        */
/* ------------------------------------------------------------------ */

typedef struct {
    int max_lpc_err;
    int interp_seen;
    int stab_seen;
} verify_ctx;

static int verify_frame(const frame_rec *r, const frame_rec *prevrec, verify_ctx *ctx)
{
    const silk_NLSF_CB_struct *cb = (r->order == 16) ? &silk_NLSF_CB_WB : &silk_NLSF_CB_NB_MB;
    opus_int16 expect[MAX_LPC_ORDER], nlsf0[MAX_LPC_ORDER], bwe[2][MAX_LPC_ORDER];
    opus_int32 ref0[MAX_LPC_ORDER], ref1[MAX_LPC_ORDER];
    double maxabs;

    g_packet = r->packet;

    /* codebook path: exact Q15, with the production stabilizer applied
       to the model output on the rare paths that need it */
    model_nlsf_q15(cb, r->idx, expect);
    if (model_min_margin(cb, expect) < 0) {
        silk_NLSF_stabilize(expect, cb->deltaMin_Q15, cb->order);
        ctx->stab_seen++;
    }
    TASK_CHECK(check_exact("nlsf", r->order, expect, r->cur));

    /* the decoder must offer the previously committed vector as history */
    if (prevrec && !r->first_reset) {
        TASK_CHECK(check_exact("history", r->order, prevrec->cur, r->prev));
    }

    /* interpolation and LPC conversion from our own history model */
    maxabs = ref_nlsf2a_q12(r->cur, r->order, ref1);
    TASK_CHECK(maxabs < LPC_FIT_LIMIT);
    if (r->interp < 4) {
        TASK_CHECK(prevrec != 0 && !r->first_reset);
        ctx->interp_seen = 1;
        model_interp_q15(prevrec->cur, r->cur, r->interp, r->order, nlsf0);
        maxabs = ref_nlsf2a_q12(nlsf0, r->order, ref0);
        TASK_CHECK(maxabs < LPC_FIT_LIMIT);
    } else {
        memcpy(ref0, ref1, sizeof(ref0));
    }
    TASK_CHECK(check_close_q12("lpc0", r->order, ref0, r->lpc_pre[0], LPC_TOL_Q12, &ctx->max_lpc_err));
    TASK_CHECK(check_close_q12("lpc1", r->order, ref1, r->lpc_pre[1], LPC_TOL_Q12, &ctx->max_lpc_err));

    /* after a loss both LPC sets get the bandwidth expansion; without a
       loss the final sets are the pre-expansion sets */
    if (r->lossCnt > 0) {
        model_bwexpand_q12(r->lpc_pre[0], r->order, bwe[0]);
        model_bwexpand_q12(r->lpc_pre[1], r->order, bwe[1]);
        TASK_CHECK(check_exact("bwe0", r->order, bwe[0], r->lpc_post[0]));
        TASK_CHECK(check_exact("bwe1", r->order, bwe[1], r->lpc_post[1]));
    } else {
        TASK_CHECK(memcmp(r->lpc_pre, r->lpc_post, sizeof(r->lpc_pre)) == 0);
    }

    /* the history stored for the next frame is the current NLSF vector,
       never anything derived from the expanded LPC coefficients */
    TASK_CHECK(check_exact("saved", r->order, r->cur, r->saved));
    return 0;
}

/* ------------------------------------------------------------------ */
/* history group: controlled parameter entry point                     */
/* ------------------------------------------------------------------ */

static void controlled_call(const silk_NLSF_CB_struct *cb, const opus_int8 *idx,
                            const opus_int16 *prev, int interp, int first_reset, int loss,
                            silk_decoder_state *st, silk_decoder_control *ctrl)
{
    memset(st, 0, sizeof(*st));
    memset(ctrl, 0, sizeof(*ctrl));
    st->psNLSF_CB  = cb;
    st->LPC_order  = cb->order;
    st->nb_subfr   = MAX_NB_SUBFR;
    st->fs_kHz     = cb->order == 16 ? 16 : 8;
    st->indices.signalType = TYPE_UNVOICED;
    st->indices.NLSFInterpCoef_Q2 = (opus_int8)interp;
    memcpy(st->indices.NLSFIndices, idx, (size_t)cb->order + 1);
    if (prev) memcpy(st->prevNLSF_Q15, prev, cb->order * sizeof(opus_int16));
    st->first_frame_after_reset = first_reset;
    st->lossCnt = loss;
    silk_decode_parameters(st, ctrl, CODE_INDEPENDENTLY);
}

/* Pick two distinct first-stage entries whose zero-residual model
   vectors clear the delta-min spacing with slack to spare. */
static int pick_stable_pair(const silk_NLSF_CB_struct *cb, opus_int8 idxA[MAX_LPC_ORDER + 1],
                            opus_int8 idxB[MAX_LPC_ORDER + 1], opus_int16 curA[MAX_LPC_ORDER],
                            opus_int16 curB[MAX_LPC_ORDER])
{
    int cb1, found = 0;
    for (cb1 = 0; cb1 < cb->nVectors && found < 2; cb1++) {
        opus_int8 idx[MAX_LPC_ORDER + 1];
        opus_int16 cur[MAX_LPC_ORDER];
        memset(idx, 0, sizeof(idx));
        idx[0] = (opus_int8)cb1;
        model_nlsf_q15(cb, idx, cur);
        if (model_min_margin(cb, cur) < 200) continue;
        if (found == 0) {
            memcpy(idxA, idx, sizeof(idx));
            memcpy(curA, cur, sizeof(cur));
        } else {
            memcpy(idxB, idx, sizeof(idx));
            memcpy(curB, cur, sizeof(cur));
        }
        found++;
    }
    return found == 2;
}

static int history_controlled(int use_wb)
{
    const silk_NLSF_CB_struct *cb = use_wb ? &silk_NLSF_CB_WB : &silk_NLSF_CB_NB_MB;
    const int order = cb->order;
    silk_decoder_state st;
    silk_decoder_control ctrl;
    opus_int8  idxA[MAX_LPC_ORDER + 1], idxB[MAX_LPC_ORDER + 1];
    opus_int16 curA[MAX_LPC_ORDER], curB[MAX_LPC_ORDER];
    opus_int16 nlsf0[MAX_LPC_ORDER], bwe[2][MAX_LPC_ORDER];
    opus_int32 ref0[MAX_LPC_ORDER], ref1[MAX_LPC_ORDER];
    frame_rec *r;
    double maxabs;
    int max_err = 0;

    TASK_CHECK(pick_stable_pair(cb, idxA, idxB, curA, curB));
    rec_start();

    /* -- interpolation with nonzero factor, no loss ------------------ */
    g_packet = 0;
    controlled_call(cb, idxB, curA, 2, 0, 0, &st, &ctrl);
    TASK_CHECK(g_nrec == 1);
    r = &g_rec[0];
    TASK_CHECK(r->interp == 2);
    TASK_CHECK(check_exact("nlsf", order, curB, r->cur));
    TASK_CHECK(check_exact("history", order, curA, r->prev));
    model_interp_q15(curA, curB, 2, order, nlsf0);
    maxabs = ref_nlsf2a_q12(nlsf0, order, ref0);
    TASK_CHECK(maxabs < LPC_FIT_LIMIT);
    maxabs = ref_nlsf2a_q12(curB, order, ref1);
    TASK_CHECK(maxabs < LPC_FIT_LIMIT);
    TASK_CHECK(check_close_q12("lpc0", order, ref0, r->lpc_pre[0], LPC_TOL_Q12, &max_err));
    TASK_CHECK(check_close_q12("lpc1", order, ref1, r->lpc_pre[1], LPC_TOL_Q12, &max_err));
    TASK_CHECK(memcmp(r->lpc_pre, r->lpc_post, sizeof(r->lpc_pre)) == 0);
    TASK_CHECK(check_exact("saved", order, curB, r->saved));

    /* -- first frame after a reset: current vector only -------------- */
    g_packet = 1;
    controlled_call(cb, idxA, curB, 1, 1, 0, &st, &ctrl);
    TASK_CHECK(g_nrec == 2);
    r = &g_rec[1];
    TASK_CHECK(r->interp == 4);   /* interpolation disabled by the reset */
    TASK_CHECK(memcmp(r->lpc_pre[0], r->lpc_pre[1], (size_t)order * sizeof(opus_int16)) == 0);
    TASK_CHECK(check_exact("saved", order, curA, r->saved));

    /* -- next frame: interpolation with the committed history resumes  */
    st.first_frame_after_reset = 0;
    st.indices.NLSFInterpCoef_Q2 = 3;
    memcpy(st.indices.NLSFIndices, idxB, (size_t)order + 1);
    g_packet = 2;
    silk_decode_parameters(&st, &ctrl, CODE_INDEPENDENTLY);
    TASK_CHECK(g_nrec == 3);
    r = &g_rec[2];
    TASK_CHECK(r->interp == 3);
    TASK_CHECK(check_exact("history", order, curA, r->prev));
    model_interp_q15(curA, curB, 3, order, nlsf0);
    maxabs = ref_nlsf2a_q12(nlsf0, order, ref0);
    TASK_CHECK(maxabs < LPC_FIT_LIMIT);
    TASK_CHECK(check_close_q12("lpc0", order, ref0, r->lpc_pre[0], LPC_TOL_Q12, &max_err));
    TASK_CHECK(check_exact("saved", order, curB, r->saved));

    /* -- after a loss: bandwidth expansion on both LPC sets ----------- */
    g_packet = 3;
    controlled_call(cb, idxB, curA, 2, 0, 1, &st, &ctrl);
    TASK_CHECK(g_nrec == 4);
    r = &g_rec[3];
    TASK_CHECK(r->lossCnt == 1);
    model_bwexpand_q12(r->lpc_pre[0], order, bwe[0]);
    model_bwexpand_q12(r->lpc_pre[1], order, bwe[1]);
    TASK_CHECK(check_exact("bwe0", order, bwe[0], r->lpc_post[0]));
    TASK_CHECK(check_exact("bwe1", order, bwe[1], r->lpc_post[1]));
    /* the expansion really changed both sets */
    TASK_CHECK(memcmp(r->lpc_pre, r->lpc_post, sizeof(r->lpc_pre)) != 0);
    /* but the expanded coefficients are not stored as NLSF history */
    TASK_CHECK(check_exact("saved", order, curB, r->saved));

    rec_stop();
    printf("INFO history_order%d max_lpc_err=%d\n", order, max_err);
    return 0;
}

static int history_order10(void) { return history_controlled(0); }
static int history_order16(void) { return history_controlled(1); }

/* ------------------------------------------------------------------ */
/* packets group: real NB/WB SILK streams through the public API       */
/* ------------------------------------------------------------------ */

#define MODEL_PI 3.14159265358979323846

static int packets_stream(int use_wb)
{
    OpusEncoder *enc = 0;
    OpusDecoder *dec = 0;
    unsigned char packet[512];
    opus_int16 pcm[FRAME];
    float out[FRAME];
    verify_ctx ctx;
    double ph1 = 0.0, ph2 = 0.0, ph3 = 0.0;
    int err = 0, pkt, n, ret, i, checked = 0;
    const int expect_order = use_wb ? 16 : 10;
    const int expect_fs    = use_wb ? 16 : 8;

    memset(&ctx, 0, sizeof(ctx));

    enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_SILK, &err);
    TASK_CHECK(enc != 0 && err == OPUS_OK);
    TASK_CHECK(opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) == OPUS_OK);
    TASK_CHECK(opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(
        use_wb ? OPUS_BANDWIDTH_WIDEBAND : OPUS_BANDWIDTH_NARROWBAND)) == OPUS_OK);
    TASK_CHECK(opus_encoder_ctl(enc, OPUS_SET_BITRATE(use_wb ? 16000 : 12000)) == OPUS_OK);
    dec = opus_decoder_create(48000, 1, &err);
    TASK_CHECK(dec != 0 && err == OPUS_OK);

    rec_start();
    for (pkt = 0; pkt < N_PACKETS; pkt++) {
        double energy = 0.0;
        /* continuous low-level voiced-like signal: slow vibrato harmonic stack */
        for (i = 0; i < FRAME; i++) {
            double env = 0.75 + 0.25 * sin(2.0 * MODEL_PI * 2.0 * (pkt * FRAME + i) / 48000.0);
            double v   = env * (0.5 * sin(ph1) + 0.3 * sin(ph2) + 0.2 * sin(ph3));
            pcm[i] = (opus_int16)(1200.0 * v);
            ph1 += 2.0 * MODEL_PI * 130.0 / 48000.0;
            ph2 += 2.0 * MODEL_PI * 260.0 / 48000.0;
            ph3 += 2.0 * MODEL_PI * 391.0 / 48000.0;
        }
        n = opus_encode(enc, pcm, FRAME, packet, (opus_int32)sizeof(packet));
        TASK_CHECK(n > 0);

        g_packet = pkt;
        if (pkt == DROP_PKT) {
            /* packet lost in transit: concealment */
            ret = opus_decode_float(dec, 0, 0, out, FRAME, 0);
        } else {
            if (pkt == RESET_PKT) {
                /* reset the decoder, then re-warm with live packets */
                TASK_CHECK(opus_decoder_ctl(dec, OPUS_RESET_STATE) == OPUS_OK);
            }
            ret = opus_decode_float(dec, packet, n, out, FRAME, 0);
        }
        TASK_CHECK(ret == FRAME);
        for (i = 0; i < FRAME; i++) {
            TASK_CHECK(isfinite(out[i]));
            energy += (double)out[i] * out[i];
        }
        TASK_CHECK(energy > 0.0);

        /* verify every frame observed during this packet */
        while (checked < g_nrec) {
            const frame_rec *rr = &g_rec[checked];
            TASK_CHECK(rr->order == expect_order);
            TASK_CHECK(rr->fs_kHz == expect_fs);
            if (pkt == RESET_PKT) TASK_CHECK(rr->first_reset == 1);
            if (pkt == DROP_PKT + 1) TASK_CHECK(rr->lossCnt > 0);
            if (verify_frame(rr, checked ? &g_rec[checked - 1] : 0, &ctx)) return 1;
            checked++;
        }
    }
    rec_stop();

    TASK_CHECK(checked >= 12);      /* at least twelve real 20 ms packets */
    TASK_CHECK(ctx.interp_seen);    /* a non-first frame really interpolated */
    printf("INFO %s frames=%d stabilized=%d max_lpc_err=%d\n",
           use_wb ? "wb" : "nb", checked, ctx.stab_seen, ctx.max_lpc_err);

    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
    return 0;
}

static int packets_nb(void) { return packets_stream(0); }
static int packets_wb(void) { return packets_stream(1); }

/* ------------------------------------------------------------------ */
/* Case registry                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "vectors", "order10_exact",      vectors_order10_exact },
    { "vectors", "order16_exact",      vectors_order16_exact },
    { "vectors", "order10_stabilized", vectors_order10_stabilized },
    { "vectors", "order16_stabilized", vectors_order16_stabilized },
    { "history", "controlled_order10", history_order10 },
    { "history", "controlled_order16", history_order16 },
    { "packets", "nb_stream",          packets_nb },
    { "packets", "wb_stream",          packets_wb },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
