/*
 * AMR-WB encoder front-end contract tests.
 *
 * The tests drive the public VisualOn streaming API
 * (voAMRWB_SetInputData / voAMRWB_GetOutputData) and compare, at every
 * observation point, against the independent integer model in
 * tests/frontend_reference.c.  Whole-frame bitstreams are cross-checked
 * against E_IF_encode.
 *
 * Conventions: every case returns 0 on success and non-zero on failure.
 * TASK_CHECK (and the cmp_* wrappers) return non-zero on failure, so cases
 * propagate failures with `if (cmp_...) return 1;`.
 *
 * Four groups are registered: Polyphase, Preview, Scaling, Streaming.
 */

#include "task_support/check.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "voAMRWB.h"
#include "cmnMemory.h"
#include "enc_if.h"
#include "frontend_reference.h"

#ifdef TASK_FRONTEND_TEST
#include "frontend_observe.h"
#endif

/* amrwbenc/inc/typedefs.h maps several short identifiers onto internal
 * symbols; "Copy" collides with the memory-operator field name used here.
 * Undo only that alias locally; production translation units keep it. */
#ifdef Copy
#undef Copy
#endif

/* ------------------------------------------------------------------ */
/* Corpus                                                             */
/* ------------------------------------------------------------------ */

/*
 * A continuous, realistic frame stream.  Each frame is a genuine 320-sample
 * 16 kHz block; frames are chained so history carries over.  The pattern
 * includes full-scale positive/negative impulses at several positions,
 * impulses near the last 15 input points (the preview boundary), an
 * asymmetric low-amplitude sequence, silence and amplitude steps, and it
 * produces both rising and falling Q trajectories.
 */
#define CORPUS_FRAMES 18

static void corpus_frame(int f, int16_t *p)
{
    int i;
    memset(p, 0, FEREF_L_FRAME16K * sizeof(int16_t));
    switch (f) {
    case 0:  p[1] = 8192;                       break;  /* decim phase ties  */
    case 1:                                     break;  /* silence           */
    case 2:  p[1] = -8192;                      break;  /* decim ties, neg    */
    case 3:                                     break;  /* silence           */
    case 4:  p[0] = -2588;                      break;  /* pre tie +          */
    case 5:  p[0] = 324;                        break;  /* pre tie -          */
    case 6:                                     /* low-amplitude, asym */
        for (i = 0; i < FEREF_L_FRAME16K; ++i)
            p[i] = (int16_t)(((i % 2) ? 1 : -1) * (300 + i % 700));
        break;
    case 7:                                     /* amplitude step     */
        for (i = 0; i < FEREF_L_FRAME16K; ++i)
            p[i] = (int16_t)((i < 160) ? 20000 : -20000);
        break;
    case 8:                                     /* last-15 boundary    */
        p[FEREF_L_FRAME16K - 1] = 30000;
        p[FEREF_L_FRAME16K - 16] = -20000;
        break;
    case 9:                                     /* broadband noise     */
        for (i = 0; i < FEREF_L_FRAME16K; ++i)
            p[i] = (int16_t)((i * 7919 + 12345) % 65536 - 32768);
        break;
    case 10:                                    break;  /* silence -> Q rise  */
    case 11:                                    /* tiny tail pulses    */
        p[FEREF_L_FRAME16K - 5] = 1;
        p[FEREF_L_FRAME16K - 4] = -1;
        p[FEREF_L_FRAME16K - 3] = 2;
        p[FEREF_L_FRAME16K - 2] = -2;
        p[FEREF_L_FRAME16K - 1] = 3;
        break;
    case 12:
        for (i = 0; i < FEREF_L_FRAME16K; ++i)
            p[i] = (int16_t)((i * 113 + 7) % 4001 - 2000);
        break;
    case 13:  p[160] = -32768;                  break;  /* full-scale neg     */
    case 14:  p[FEREF_L_FRAME16K - 1] = 32767;  break;  /* last point, full   */
    case 15:                                    /* 15-boundary fill    */
        for (i = FEREF_L_FRAME16K - 15; i < FEREF_L_FRAME16K; ++i)
            p[i] = (int16_t)((i & 1) ? 12345 : -12345);
        break;
    case 16:                                    /* asymmetric ramp     */
        for (i = 0; i < FEREF_L_FRAME16K; ++i)
            p[i] = (int16_t)((i < 96) ? (i * 300) : -(i * 97));
        break;
    default:                                    /* broadband again     */
        for (i = 0; i < FEREF_L_FRAME16K; ++i)
            p[i] = (int16_t)((i * 2311 + 77) % 65536 - 32768);
        break;
    }
}

/* Modes rotate through 6.60, 8.85, 12.65 and 23.85 kbps; DTX stays off. */
static const int corpus_modes[4] = {
    VOAMRWB_MD66, VOAMRWB_MD885, VOAMRWB_MD1265, VOAMRWB_MD2385
};

/* ------------------------------------------------------------------ */
/* Production driver with observation capture                         */
/* ------------------------------------------------------------------ */

#ifdef TASK_FRONTEND_TEST

struct Observed {
    int seen[8];
    FrontendStageRecord rec[8];
};

static struct Observed g_obs;
static void *g_obs_target;   /* when set, capture only this coder handle */

static void capture_cb(void *coder, Word16 stage, const FrontendStageRecord *r)
{
    if (g_obs_target != NULL && coder != g_obs_target)
        return;
    if (stage < 1 || stage > 7)
        return;
    g_obs.seen[stage] = 1;
    g_obs.rec[stage] = *r;
}

static VO_AUDIO_CODECAPI g_api;
static VO_MEM_OPERATOR g_mem;

static void setup_api(void)
{
    g_mem.Alloc = cmnMemAlloc;
    g_mem.Copy = cmnMemCopy;
    g_mem.Free = cmnMemFree;
    g_mem.Set = cmnMemSet;
    g_mem.Check = cmnMemCheck;
    voGetAMRWBEncAPI(&g_api);
}

static VO_HANDLE open_encoder(int mode)
{
    VO_CODEC_INIT_USERDATA user;
    VO_HANDLE h = NULL;
    int fmt = VOAMRWB_RFC3267, dtx = 0;
    setup_api();
    memset(&user, 0, sizeof(user));
    user.memflag = VO_IMF_USERMEMOPERATOR;
    user.memData = &g_mem;
    if (g_api.Init(&h, VO_AUDIO_CodingAMRWB, &user) != VO_ERR_NONE)
        return NULL;
    g_api.SetParam(h, VO_PID_AMRWB_FRAMETYPE, &fmt);
    g_api.SetParam(h, VO_PID_AMRWB_DTX, &dtx);
    g_api.SetParam(h, VO_PID_AMRWB_MODE, &mode);
    frontend_set_observer(capture_cb);
    return h;
}

static void close_encoder(VO_HANDLE h)
{
    frontend_set_observer(NULL);
    if (h != NULL)
        g_api.Uninit(h);
}

/* Encode one whole frame through SetInputData/GetOutputData. */
static int encode_frame(VO_HANDLE h, int mode, const int16_t *pcm,
                        unsigned char *out, int *out_len)
{
    VO_CODECBUFFER in, od;
    VO_AUDIO_OUTPUTINFO info;
    unsigned rc;
    memset(&in, 0, sizeof(in));
    memset(&od, 0, sizeof(od));
    memset(&info, 0, sizeof(info));
    g_api.SetParam(h, VO_PID_AMRWB_MODE, &mode);
    in.Buffer = (unsigned char *)(uintptr_t)pcm;
    in.Length = FEREF_L_FRAME16K * 2;
    if (g_api.SetInputData(h, &in) != VO_ERR_NONE)
        return -1;
    od.Buffer = out;
    od.Length = 1024;
    rc = g_api.GetOutputData(h, &od, &info);
    if (rc != VO_ERR_NONE)
        return -(int)rc;
    *out_len = (int)od.Length;
    return 0;
}

#endif /* TASK_FRONTEND_TEST */

/* ------------------------------------------------------------------ */
/* Shared comparison helpers (0 = pass, non-zero = fail)              */
/* ------------------------------------------------------------------ */

/* Feeds a guaranteed failure to TASK_CHECK and yields the failing status. */
static int fe_fail_at(const char *expr, const char *file, int line)
{
    task_check(0, expr, file, line);
    return 1;
}
#define FE_FAIL(expr, file, line) fe_fail_at((expr), (file), (line))

static int cmp_samples(const FrontendStageRecord *r, const int16_t *exp,
                       int n, int offset, const char *what)
{
    int i;
    for (i = 0; i < n; ++i) {
        char expr[192];
        sprintf(expr, "%s[%d] exp=%d got=%d", what, i, (int)exp[i],
                (int)r->samples[offset + i]);
        if (!task_check(exp[i] == r->samples[offset + i], expr,
                        __FILE__, __LINE__))
            return 1;
    }
    return 0;
}

static int cmp_ints(const char *what, long exp, long got)
{
    char expr[192];
    sprintf(expr, "%s exp=%ld got=%ld", what, exp, got);
    return !task_check(exp == got, expr, __FILE__, __LINE__);
}

static int cmp_mem_decim(const FrontendStageRecord *r, const int16_t *exp)
{
    int i;
    for (i = 0; i < FEREF_NMEM; ++i) {
        char expr[192];
        sprintf(expr, "mem_decim[%d] exp=%d got=%d", i, (int)exp[i],
                (int)r->mem_decim[i]);
        if (!task_check(exp[i] == r->mem_decim[i], expr, __FILE__, __LINE__))
            return 1;
    }
    return 0;
}

static int cmp_mem_sig(const FrontendStageRecord *r, const int16_t *exp)
{
    int i;
    for (i = 0; i < 6; ++i) {
        char expr[192];
        sprintf(expr, "mem_sig_in[%d] exp=%d got=%d", i, (int)exp[i],
                (int)r->mem_sig_in[i]);
        if (!task_check(exp[i] == r->mem_sig_in[i], expr, __FILE__, __LINE__))
            return 1;
    }
    return 0;
}

static int cmp_past(const FrontendStageRecord *r, const int16_t *exp)
{
    int i;
    for (i = 0; i < FEREF_NPAST; ++i) {
        char expr[192];
        sprintf(expr, "old_speech[%d] exp=%d got=%d", i, (int)exp[i],
                (int)r->old_speech[i]);
        if (!task_check(exp[i] == r->old_speech[i], expr, __FILE__, __LINE__))
            return 1;
    }
    return 0;
}

static int cmp_old_speech(const FrontendStageRecord *r, const int16_t *exp)
{
    int i;
    for (i = 0; i < FEREF_NHIST; ++i) {
        char expr[192];
        sprintf(expr, "next old_speech[%d] exp=%d got=%d", i, (int)exp[i],
                (int)r->old_speech[i]);
        if (!task_check(exp[i] == r->old_speech[i], expr, __FILE__, __LINE__))
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Polyphase group                                                    */
/* ------------------------------------------------------------------ */

#define POLY_FRAMES 10

/*
 * The decimator's phase index is (5*j) & 3, so a run of 768 outputs visits
 * all four phases evenly.  This case asserts that the observed decimated
 * stream, its long-term memory and the 12 preview points match the model
 * for every frame of the corpus, including every phase.
 */
static int poly_shape(void)
{
#ifdef TASK_FRONTEND_TEST
    FeModel *m = fe_model_new();
    FeFrame f;
    VO_HANDLE h = open_encoder(VOAMRWB_MD66);
    unsigned char out[1024];
    int len, fr;
    unsigned phase_hits[4] = {0, 0, 0, 0};
    int rc = 0;
    if (m == NULL || h == NULL) {
        fe_model_free(m);
        close_encoder(h);
        return FE_FAIL("poly setup", __FILE__, __LINE__);
    }
    for (fr = 0; fr < POLY_FRAMES && !rc; ++fr) {
        int16_t pcm[FEREF_L_FRAME16K];
        int j;
        corpus_frame(fr, pcm);
        fe_model_frame(m, pcm, &f);
        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h, corpus_modes[fr % 4], pcm, out, &len) != 0) {
            rc = FE_FAIL("poly encode", __FILE__, __LINE__);
            break;
        }
        if (!task_check(g_obs.seen[FE_STAGE_DECIM], "decim stage seen",
                        __FILE__, __LINE__)) {
            rc = 1;
            break;
        }
        if (cmp_samples(&g_obs.rec[FE_STAGE_DECIM], f.decim,
                        FEREF_L_FRAME, 0, "decim") ||
            cmp_samples(&g_obs.rec[FE_STAGE_DECIM], f.preview,
                        FEREF_L_FILT, FEREF_L_FRAME, "preview") ||
            cmp_mem_decim(&g_obs.rec[FE_STAGE_DECIM], f.mem_decim)) {
            rc = 1;
            break;
        }
        for (j = 0; j < FEREF_L_FRAME; ++j)
            ++phase_hits[(j * 5) & 3];
    }
    close_encoder(h);
    fe_model_free(m);
    if (rc)
        return rc;
    return cmp_ints("all four phases exercised", 1,
                    phase_hits[0] && phase_hits[1] && phase_hits[2] &&
                    phase_hits[3]);
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/*
 * Impulse responses at several positions expose the per-phase tap alignment.
 * A unit impulse placed at different samples produces different phases; the
 * model reproduces the exact integer output.  A fresh encoder per impulse
 * keeps the production history pristine and comparable.
 */
static int poly_impulse_alignment(void)
{
#ifdef TASK_FRONTEND_TEST
    static const int positions[] = {0, 1, 2, 3, 4, 5, 160, 300, 304, 318, 319};
    FeModel *m = fe_model_new();
    int k, rc = 0;
    if (m == NULL)
        return FE_FAIL("impulse setup", __FILE__, __LINE__);
    for (k = 0; k < (int)(sizeof(positions) / sizeof(positions[0])) && !rc;
         ++k) {
        VO_HANDLE h = open_encoder(VOAMRWB_MD2385);
        unsigned char out[1024];
        int len, j;
        int16_t pcm[FEREF_L_FRAME16K];
        FeFrame f;
        if (h == NULL) {
            rc = FE_FAIL("impulse open", __FILE__, __LINE__);
            break;
        }
        memset(pcm, 0, sizeof(pcm));
        pcm[positions[k]] = 16384;
        fe_model_reset(m);
        fe_model_frame(m, pcm, &f);
        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h, VOAMRWB_MD2385, pcm, out, &len) < 0 ||
            !g_obs.seen[FE_STAGE_DECIM]) {
            rc = FE_FAIL("impulse encode", __FILE__, __LINE__);
            close_encoder(h);
            break;
        }
        for (j = 0; j < FEREF_L_FRAME; ++j) {
            char expr[192];
            int16_t got = g_obs.rec[FE_STAGE_DECIM].samples[j];
            sprintf(expr, "imp pos=%d decim[%d] exp=%d got=%d",
                    positions[k], j, (int)f.decim[j], (int)got);
            if (!task_check(got == f.decim[j], expr, __FILE__, __LINE__)) {
                rc = 1;
                break;
            }
        }
        close_encoder(h);
    }
    fe_model_free(m);
    return rc;
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/*
 * A single FIR function compared with its own vector proves nothing about
 * the chain.  This case runs the whole downsampling stage over the corpus and
 * asserts the model's decimated stream, memory and (via the tie count) that
 * rounding midpoints were actually exercised.
 */
static int poly_full_chain(void)
{
#ifdef TASK_FRONTEND_TEST
    FeModel *m = fe_model_new();
    FeFrame f;
    VO_HANDLE h = open_encoder(VOAMRWB_MD1265);
    unsigned char out[1024];
    int len, fr;
    unsigned ties = 0, pre_ties = 0;
    unsigned phase_tie[4] = {0, 0, 0, 0};
    int rc = 0;
    if (m == NULL || h == NULL) {
        fe_model_free(m);
        close_encoder(h);
        return FE_FAIL("chain setup", __FILE__, __LINE__);
    }
    for (fr = 0; fr < CORPUS_FRAMES && !rc; ++fr) {
        int16_t pcm[FEREF_L_FRAME16K];
        int k;
        corpus_frame(fr, pcm);
        fe_model_frame(m, pcm, &f);
        ties += f.decim_tie_pos + f.decim_tie_neg;
        pre_ties += f.pre_tie_pos + f.pre_tie_neg;
        for (k = 0; k < 4; ++k)
            phase_tie[k] += f.phase_tie_pos[k] + f.phase_tie_neg[k];
        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h, corpus_modes[fr % 4], pcm, out, &len) < 0 ||
            !g_obs.seen[FE_STAGE_DECIM] || !g_obs.seen[FE_STAGE_PREEMPH]) {
            rc = FE_FAIL("chain encode", __FILE__, __LINE__);
            break;
        }
        if (cmp_samples(&g_obs.rec[FE_STAGE_DECIM], f.decim,
                        FEREF_L_FRAME, 0, "chain decim") ||
            cmp_mem_decim(&g_obs.rec[FE_STAGE_DECIM], f.mem_decim) ||
            cmp_samples(&g_obs.rec[FE_STAGE_PREEMPH], f.preemph,
                        FEREF_NSAMP, 0, "chain preemph"))
            rc = 1;
    }
    close_encoder(h);
    fe_model_free(m);
    if (rc)
        return rc;
    /* The chain must have met rounding midpoints in every phase, and in the
     * pre-emphasis stage, otherwise those rules are only vacuously covered. */
    if (cmp_ints("decimator rounding-midpoint ties", 1, ties > 0) ||
        cmp_ints("pre-emphasis rounding-midpoint ties", 1, pre_ties > 0) ||
        cmp_ints("phase 0 rounding ties", 1, phase_tie[0] > 0) ||
        cmp_ints("phase 1 rounding ties", 1, phase_tie[1] > 0) ||
        cmp_ints("phase 2 rounding ties", 1, phase_tie[2] > 0) ||
        cmp_ints("phase 3 rounding ties", 1, phase_tie[3] > 0))
        return 1;
    return 0;
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/* ------------------------------------------------------------------ */
/* Preview group                                                      */
/* ------------------------------------------------------------------ */

/* Preview decimation must use its own copy of the *real* memory, never the
 * mutated long-term memory.  Also checks that the discarded preview HP
 * memory is not the retained one. */
static int preview_isolation(void)
{
#ifdef TASK_FRONTEND_TEST
    FeModel *m = fe_model_new();
    FeFrame f;
    VO_HANDLE h = open_encoder(VOAMRWB_MD66);
    unsigned char out[1024];
    int len, fr;
    int saw_divergence = 0, rc = 0;
    if (m == NULL || h == NULL) {
        fe_model_free(m);
        close_encoder(h);
        return FE_FAIL("preview setup", __FILE__, __LINE__);
    }
    for (fr = 0; fr < CORPUS_FRAMES && !rc; ++fr) {
        int16_t pcm[FEREF_L_FRAME16K];
        int j;
        corpus_frame(fr, pcm);
        fe_model_frame(m, pcm, &f);
        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h, corpus_modes[fr % 4], pcm, out, &len) < 0 ||
            !g_obs.seen[FE_STAGE_DECIM] || !g_obs.seen[FE_STAGE_HP]) {
            rc = FE_FAIL("preview encode", __FILE__, __LINE__);
            break;
        }
        if (cmp_samples(&g_obs.rec[FE_STAGE_DECIM], f.preview,
                        FEREF_L_FILT, FEREF_L_FRAME, "preview") ||
            cmp_samples(&g_obs.rec[FE_STAGE_HP], f.hp,
                        FEREF_NSAMP, 0, "hp") ||
            cmp_mem_sig(&g_obs.rec[FE_STAGE_HP], f.mem_sig_in)) {
            rc = 1;
            break;
        }
        for (j = 0; j < 6; ++j)
            if (f.mem_sig_in[j] != f.hp_mem_preview[j])
                saw_divergence = 1;
    }
    close_encoder(h);
    fe_model_free(m);
    if (rc)
        return rc;
    /* A mutant that committed preview HP state would otherwise pass. */
    return cmp_ints("preview HP memory diverges from real memory", 1,
                    saw_divergence);
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/* mem_preemph must hold the last real HP point, not the last preview point. */
static int preview_mem_preemph(void)
{
#ifdef TASK_FRONTEND_TEST
    FeModel *m = fe_model_new();
    FeFrame f;
    VO_HANDLE h = open_encoder(VOAMRWB_MD885);
    unsigned char out[1024];
    int len, fr;
    int differing = 0, rc = 0;
    if (m == NULL || h == NULL) {
        fe_model_free(m);
        close_encoder(h);
        return FE_FAIL("mem_preemph setup", __FILE__, __LINE__);
    }
    for (fr = 0; fr < CORPUS_FRAMES && !rc; ++fr) {
        int16_t pcm[FEREF_L_FRAME16K];
        corpus_frame(fr, pcm);
        fe_model_frame(m, pcm, &f);
        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h, corpus_modes[fr % 4], pcm, out, &len) < 0 ||
            !g_obs.seen[FE_STAGE_PREEMPH]) {
            rc = FE_FAIL("mem_preemph encode", __FILE__, __LINE__);
            break;
        }
        if (cmp_ints("mem_preemph", f.mem_preemph,
                     g_obs.rec[FE_STAGE_PREEMPH].mem_preemph)) {
            rc = 1;
            break;
        }
        if (f.mem_preemph != f.hp[FEREF_L_FRAME + FEREF_L_FILT - 1])
            differing = 1;
    }
    close_encoder(h);
    fe_model_free(m);
    if (rc)
        return rc;
    return cmp_ints("mem_preemph differs from last preview point", 1,
                    differing);
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/* The max search and Q_new must span the 256+12 points, including preview. */
static int preview_max_search(void)
{
#ifdef TASK_FRONTEND_TEST
    FeModel *m = fe_model_new();
    FeFrame f;
    VO_HANDLE h = open_encoder(VOAMRWB_MD1265);
    unsigned char out[1024];
    int len, fr;
    int saw_preview_driven = 0, rc = 0;
    if (m == NULL || h == NULL) {
        fe_model_free(m);
        close_encoder(h);
        return FE_FAIL("max setup", __FILE__, __LINE__);
    }
    for (fr = 0; fr < CORPUS_FRAMES && !rc; ++fr) {
        int16_t pcm[FEREF_L_FRAME16K];
        int i;
        corpus_frame(fr, pcm);
        fe_model_frame(m, pcm, &f);
        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h, corpus_modes[fr % 4], pcm, out, &len) < 0 ||
            !g_obs.seen[FE_STAGE_PREEMPH]) {
            rc = FE_FAIL("max encode", __FILE__, __LINE__);
            break;
        }
        if (cmp_samples(&g_obs.rec[FE_STAGE_PREEMPH], f.preemph,
                        FEREF_NSAMP, 0, "preemph") ||
            cmp_ints("q_new", f.q_new,
                     g_obs.rec[FE_STAGE_PREEMPH].q_new)) {
            rc = 1;
            break;
        }
        /* Is the preview region (indices 256..267) the global maximum? */
        {
            int32_t hmax = 0;
            for (i = 0; i < FEREF_NSAMP; ++i) {
                int32_t a = (int32_t)f.hp[i];
                if (a < 0) a = -a;
                if (a > hmax) hmax = a;
            }
            for (i = FEREF_L_FRAME; i < FEREF_NSAMP; ++i) {
                int32_t a = (int32_t)f.hp[i];
                if (a < 0) a = -a;
                if (a >= hmax && hmax > 0)
                    saw_preview_driven = 1;
            }
        }
    }
    close_encoder(h);
    fe_model_free(m);
    if (rc)
        return rc;
    return cmp_ints("preview contributes to the maximum", 1,
                    saw_preview_driven);
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/* ------------------------------------------------------------------ */
/* Scaling group                                                      */
/* ------------------------------------------------------------------ */

/* Q_max two-frame constraint: Q_new is clamped by both stored maxima while
 * the stored maxima keep the uncapped shift. */
static int scaling_q_constraint(void)
{
#ifdef TASK_FRONTEND_TEST
    FeModel *m = fe_model_new();
    FeFrame f;
    VO_HANDLE h = open_encoder(VOAMRWB_MD66);
    unsigned char out[1024];
    int len, fr;
    int saw_clamp = 0, rc = 0;
    if (m == NULL || h == NULL) {
        fe_model_free(m);
        close_encoder(h);
        return FE_FAIL("q setup", __FILE__, __LINE__);
    }
    for (fr = 0; fr < CORPUS_FRAMES && !rc; ++fr) {
        int16_t pcm[FEREF_L_FRAME16K];
        corpus_frame(fr, pcm);
        fe_model_frame(m, pcm, &f);
        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h, corpus_modes[fr % 4], pcm, out, &len) < 0 ||
            !g_obs.seen[FE_STAGE_PREEMPH]) {
            rc = FE_FAIL("q encode", __FILE__, __LINE__);
            break;
        }
        if (cmp_ints("q_new", f.q_new, g_obs.rec[FE_STAGE_PREEMPH].q_new) ||
            cmp_ints("q_old", f.q_old, g_obs.rec[FE_STAGE_PREEMPH].q_old) ||
            cmp_ints("q_max0", f.q_max0, g_obs.rec[FE_STAGE_PREEMPH].q_max0) ||
            cmp_ints("q_max1", f.q_max1,
                     g_obs.rec[FE_STAGE_PREEMPH].q_max1)) {
            rc = 1;
            break;
        }
        if (f.q_new != f.q_uncapped)
            saw_clamp = 1;
    }
    close_encoder(h);
    fe_model_free(m);
    if (rc)
        return rc;
    return cmp_ints("Q clamp actually engaged", 1, saw_clamp);
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/* The previous-speech rescale (Scale_sig over the old interval) must match;
 * exp is Q_new - Q_old and can be negative as well as positive. */
static int scaling_rescale(void)
{
#ifdef TASK_FRONTEND_TEST
    FeModel *m = fe_model_new();
    FeFrame f;
    VO_HANDLE h = open_encoder(VOAMRWB_MD885);
    unsigned char out[1024];
    int len, fr;
    int saw_neg = 0, saw_pos = 0, rc = 0;
    if (m == NULL || h == NULL) {
        fe_model_free(m);
        close_encoder(h);
        return FE_FAIL("rescale setup", __FILE__, __LINE__);
    }
    for (fr = 0; fr < CORPUS_FRAMES && !rc; ++fr) {
        int16_t pcm[FEREF_L_FRAME16K];
        int exp;
        corpus_frame(fr, pcm);
        fe_model_frame(m, pcm, &f);
        exp = f.q_new - f.q_old;
        if (exp < 0) saw_neg = 1;
        if (exp > 0) saw_pos = 1;
        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h, corpus_modes[fr % 4], pcm, out, &len) < 0 ||
            !g_obs.seen[FE_STAGE_PREEMPH]) {
            rc = FE_FAIL("rescale encode", __FILE__, __LINE__);
            break;
        }
        if (cmp_past(&g_obs.rec[FE_STAGE_PREEMPH], f.past_scaled)) {
            rc = 1;
            break;
        }
    }
    close_encoder(h);
    fe_model_free(m);
    if (rc)
        return rc;
    if (cmp_ints("rescaling up seen", 1, saw_pos) ||
        cmp_ints("rescaling down seen", 1, saw_neg))
        return 1;
    return 0;
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/* Frame-end snapshot: retained memories and the next old_speech window. */
static int scaling_frame_end_state(void)
{
#ifdef TASK_FRONTEND_TEST
    FeModel *m = fe_model_new();
    FeFrame f;
    VO_HANDLE h = open_encoder(VOAMRWB_MD2385);
    unsigned char out[1024];
    int len, fr;
    int rc = 0;
    if (m == NULL || h == NULL) {
        fe_model_free(m);
        close_encoder(h);
        return FE_FAIL("end setup", __FILE__, __LINE__);
    }
    for (fr = 0; fr < CORPUS_FRAMES && !rc; ++fr) {
        int16_t pcm[FEREF_L_FRAME16K];
        corpus_frame(fr, pcm);
        fe_model_frame(m, pcm, &f);
        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h, corpus_modes[fr % 4], pcm, out, &len) < 0 ||
            !g_obs.seen[FE_STAGE_END]) {
            rc = FE_FAIL("end encode", __FILE__, __LINE__);
            break;
        }
        if (cmp_samples(&g_obs.rec[FE_STAGE_END], f.preemph,
                        FEREF_NSAMP, 0, "end preemph") ||
            cmp_mem_decim(&g_obs.rec[FE_STAGE_END], f.mem_decim) ||
            cmp_mem_sig(&g_obs.rec[FE_STAGE_END], f.mem_sig_in) ||
            cmp_ints("end mem_preemph", f.mem_preemph,
                     g_obs.rec[FE_STAGE_END].mem_preemph) ||
            cmp_ints("end q_new", f.q_new,
                     g_obs.rec[FE_STAGE_END].q_new) ||
            cmp_ints("end q_old", f.q_old,
                     g_obs.rec[FE_STAGE_END].q_old) ||
            cmp_ints("end q_max0", f.q_max0,
                     g_obs.rec[FE_STAGE_END].q_max0) ||
            cmp_ints("end q_max1", f.q_max1,
                     g_obs.rec[FE_STAGE_END].q_max1) ||
            cmp_old_speech(&g_obs.rec[FE_STAGE_END], f.next_old_speech)) {
            rc = 1;
        }
    }
    close_encoder(h);
    fe_model_free(m);
    return rc;
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/* ------------------------------------------------------------------ */
/* Streaming group                                                    */
/* ------------------------------------------------------------------ */

#ifdef TASK_FRONTEND_TEST
/*
 * Observation records are indexed by the number of frames already emitted:
 * the callback for frame N fires before frame N is returned.  A record may
 * therefore only exist for an index whose 640 input bytes were complete.
 */
struct StreamObs {
    int records;                  /* callbacks seen                       */
    FrontendStageRecord rec[64];
    unsigned seen_mask[64];       /* stages observed for each record idx */
};
static struct StreamObs g_sobs;
#endif

static void stream_capture_cb(void *coder, Word16 stage,
                              const FrontendStageRecord *r)
{
#ifdef TASK_FRONTEND_TEST
    (void)coder;
    if (stage < 1 || stage > 7)
        return;
    if (g_sobs.records >= 0 && g_sobs.records < 64) {
        g_sobs.seen_mask[g_sobs.records] |= 1u << stage;
        g_sobs.rec[g_sobs.records] = *r;
    }
    /* The frame is finished when the END record fires; that is when the
     * record index advances to the next frame. */
    if (stage == FE_STAGE_END)
        ++g_sobs.records;
#endif
}

/*
 * Feed a continuous stream in byte chunks of 1, 3, 637, ...  Every chunk is
 * presented through SetInputData and fully drained before the next one is
 * offered, honouring the original borrowed-buffer lifetime.  No observation
 * record may appear while a frame is still incomplete; once complete the
 * parameter trajectory and emitted bytes must equal the whole-frame path.
 */
static int stream_split_equivalence(void)
{
#ifdef TASK_FRONTEND_TEST
    static const unsigned chunks[] = {1, 3, 637};
    const int nframes = 6;
    const long frame_bytes = FEREF_L_FRAME16K * 2;
    int16_t *pcm;
    unsigned char *ref, *got;
    int reflen[16], gotlen[16], ref_mode[16];
    long total, pos;
    int ci = 0, produced = 0, f, rc = 0;
    VO_HANDLE h;
    void *whole;

    pcm = (int16_t *)calloc((size_t)nframes * FEREF_L_FRAME16K,
                            sizeof(int16_t));
    ref = (unsigned char *)calloc((size_t)nframes, 1024);
    got = (unsigned char *)calloc((size_t)nframes, 1024);
    if (pcm == NULL || ref == NULL || got == NULL) {
        free(pcm); free(ref); free(got);
        return FE_FAIL("stream alloc", __FILE__, __LINE__);
    }
    for (f = 0; f < nframes; ++f) {
        corpus_frame(f, pcm + (long)f * FEREF_L_FRAME16K);
        ref_mode[f] = corpus_modes[f % 4];
    }

    setup_api();
    whole = E_IF_init();
    if (whole == NULL) {
        free(pcm); free(ref); free(got);
        return FE_FAIL("stream E_IF_init", __FILE__, __LINE__);
    }
    for (f = 0; f < nframes; ++f)
        reflen[f] = E_IF_encode(whole, ref_mode[f],
                                pcm + (long)f * FEREF_L_FRAME16K,
                                ref + (long)f * 1024, 0);
    E_IF_exit(whole);

    h = open_encoder(ref_mode[0]);
    if (h == NULL) {
        free(pcm); free(ref); free(got);
        return FE_FAIL("stream open", __FILE__, __LINE__);
    }
    memset(&g_sobs, 0, sizeof(g_sobs));
    frontend_set_observer(stream_capture_cb);
    total = (long)nframes * frame_bytes;
    pos = 0;
    while (pos < total && !rc) {
        unsigned clen = chunks[ci++ % 3];
        long frame_index;
        VO_CODECBUFFER in;
        if (pos + (long)clen > total)
            clen = (unsigned)(total - pos);
        frame_index = pos / frame_bytes;
        in.Buffer = (unsigned char *)(uintptr_t)pcm + pos;
        in.Length = clen;
        g_api.SetParam(h, VO_PID_AMRWB_MODE, &ref_mode[frame_index]);
        if (g_api.SetInputData(h, &in) != VO_ERR_NONE) {
            rc = FE_FAIL("stream set input", __FILE__, __LINE__);
            break;
        }
        for (;;) {
            VO_CODECBUFFER od;
            VO_AUDIO_OUTPUTINFO info;
            unsigned r;
            od.Buffer = got + (long)produced * 1024;
            od.Length = 1024;
            memset(&info, 0, sizeof(info));
            r = g_api.GetOutputData(h, &od, &info);
            if (r == VO_ERR_INPUT_BUFFER_SMALL)
                break;
            if (r != VO_ERR_NONE || produced >= nframes) {
                rc = FE_FAIL("stream get output", __FILE__, __LINE__);
                break;
            }
            gotlen[produced] = (int)od.Length;
            ++produced;
        }
        /* A frame is only complete when the buffered byte count is a multiple
         * of 640; otherwise no record may exist for the next frame index. */
        if (((pos + clen) % frame_bytes) != 0) {
            if (g_sobs.seen_mask[produced] != 0)
                rc = 1;
        }
        pos += clen;
    }
    frontend_set_observer(capture_cb);
    close_encoder(h);

    if (!rc && produced != nframes)
        rc = FE_FAIL("stream produced all frames", __FILE__, __LINE__);
    if (!rc) {
        for (f = 0; f < nframes; ++f) {
            if (cmp_ints("stream frame length", reflen[f], gotlen[f]) ||
                !task_check(memcmp(got + (long)f * 1024,
                                   ref + (long)f * 1024,
                                   (size_t)reflen[f]) == 0,
                            "stream frame bytes", __FILE__, __LINE__)) {
                rc = 1;
                break;
            }
        }
    }
    /* Parameter trajectory equals the model fed the same frames. */
    if (!rc) {
        FeModel *m = fe_model_new();
        FeFrame mf;
        for (f = 0; f < nframes && !rc; ++f) {
            int i;
            fe_model_frame(m, pcm + (long)f * FEREF_L_FRAME16K, &mf);
            if (!task_check((g_sobs.seen_mask[f] & (1u << FE_STAGE_DECIM)) &&
                            (g_sobs.seen_mask[f] & (1u << FE_STAGE_HP)) &&
                            (g_sobs.seen_mask[f] &
                             (1u << FE_STAGE_PREEMPH)) &&
                            (g_sobs.seen_mask[f] & (1u << FE_STAGE_END)),
                            "stream per-frame observation set",
                            __FILE__, __LINE__)) {
                rc = 1;
                break;
            }
            if (cmp_ints("stream q_new", mf.q_new, g_sobs.rec[f].q_new) ||
                cmp_ints("stream mem_preemph", mf.mem_preemph,
                         g_sobs.rec[f].mem_preemph)) {
                rc = 1;
                break;
            }
            for (i = 0; i < FEREF_NSAMP; ++i) {
                if (g_sobs.rec[f].samples[i] != mf.preemph[i]) {
                    char expr[192];
                    sprintf(expr,
                            "stream preemph frame %d [%d] exp=%d got=%d", f,
                            i, (int)mf.preemph[i],
                            (int)g_sobs.rec[f].samples[i]);
                    FE_FAIL(expr, __FILE__, __LINE__);
                    rc = 1;
                    break;
                }
            }
        }
        fe_model_free(m);
    }
    free(pcm); free(ref); free(got);
    return rc;
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/*
 * A homing frame (all 0x0008) is processed normally, then the encoder resets
 * so that the following two frames equal a freshly opened instance.
 */
static int stream_homing_reset(void)
{
#ifdef TASK_FRONTEND_TEST
    int16_t homing[FEREF_L_FRAME16K];
    int16_t a1[FEREF_L_FRAME16K], a2[FEREF_L_FRAME16K];
    unsigned char out[1024];
    unsigned char post[2][1024], fresh[2][1024];
    int len, i, homelen = -1, postlen[2], freshlen[2];
    VO_HANDLE h, h2;
    int rc = 0;

    for (i = 0; i < FEREF_L_FRAME16K; ++i)
        homing[i] = 0x0008;
    corpus_frame(12, a1);
    corpus_frame(13, a2);
    if (!task_check(fe_is_homing_frame(homing), "homing detector",
                    __FILE__, __LINE__) ||
        !task_check(!fe_is_homing_frame(a1), "non-homing detector",
                    __FILE__, __LINE__))
        return 1;

    h = open_encoder(VOAMRWB_MD1265);
    if (h == NULL)
        return FE_FAIL("homing open", __FILE__, __LINE__);
    for (i = 0; i < 3; ++i) {
        int16_t warm[FEREF_L_FRAME16K];
        corpus_frame(i, warm);
        if (encode_frame(h, corpus_modes[i % 4], warm, out, &len) < 0)
            rc = 1;
    }
    /* The homing frame is a normal frame on the front end: compare it against
     * the model before the reset the encoder performs afterwards. */
    memset(&g_obs, 0, sizeof(g_obs));
    if (encode_frame(h, VOAMRWB_MD1265, homing, out, &homelen) < 0)
        rc = 1;
    if (!rc) {
        FeModel *hm = fe_model_new();
        FeFrame hf;
        int j;
        if (hm == NULL)
            rc = 1;
        else {
            /* Replay the same three warm frames plus the homing frame. */
            for (j = 0; j < 3; ++j) {
                int16_t warm[FEREF_L_FRAME16K];
                FeFrame tmpf;
                corpus_frame(j, warm);
                fe_model_frame(hm, warm, &tmpf);
            }
            fe_model_frame(hm, homing, &hf);
            if (cmp_samples(&g_obs.rec[FE_STAGE_END], hf.preemph,
                            FEREF_NSAMP, 0, "homing preemph") ||
                cmp_ints("homing q_new", hf.q_new,
                         g_obs.rec[FE_STAGE_END].q_new) ||
                cmp_ints("homing mem_preemph", hf.mem_preemph,
                         g_obs.rec[FE_STAGE_END].mem_preemph))
                rc = 1;
            fe_model_free(hm);
        }
    }
    if (encode_frame(h, VOAMRWB_MD1265, a1, post[0], &postlen[0]) < 0)
        rc = 1;
    if (encode_frame(h, VOAMRWB_MD1265, a2, post[1], &postlen[1]) < 0)
        rc = 1;
    close_encoder(h);

    h2 = open_encoder(VOAMRWB_MD1265);
    if (h2 == NULL)
        return FE_FAIL("homing fresh open", __FILE__, __LINE__);
    if (encode_frame(h2, VOAMRWB_MD1265, a1, fresh[0], &freshlen[0]) < 0)
        rc = 1;
    if (encode_frame(h2, VOAMRWB_MD1265, a2, fresh[1], &freshlen[1]) < 0)
        rc = 1;
    close_encoder(h2);

    /* The homing frame itself is processed as a normal 12.65k frame. */
    if (!rc && cmp_ints("homing frame length", 33, homelen))
        rc = 1;
    if (!rc && (cmp_ints("post-homing length 1", freshlen[0], postlen[0]) ||
                memcmp(post[0], fresh[0], (size_t)postlen[0]) != 0)) {
        FE_FAIL("post-homing frame 1 equals fresh", __FILE__, __LINE__);
        rc = 1;
    }
    if (!rc && (cmp_ints("post-homing length 2", freshlen[1], postlen[1]) ||
                memcmp(post[1], fresh[1], (size_t)postlen[1]) != 0)) {
        FE_FAIL("post-homing frame 2 equals fresh", __FILE__, __LINE__);
        rc = 1;
    }
    return rc ? 1 : 0;
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/*
 * Two interleaved instances stay independent.  Only stream 1 is observed; on
 * any mismatch the first divergence is reported with stream, frame, stage and
 * sample coordinate.
 */
static int stream_interleaved(void)
{
#ifdef TASK_FRONTEND_TEST
    VO_HANDLE h1, h2 = NULL;
    FeModel *m1, *m2;
    FeFrame f1, f2;
    unsigned char out[1024];
    int len, fr, rc = 0;
    static const int mode1[4] = {VOAMRWB_MD66, VOAMRWB_MD885,
                                 VOAMRWB_MD1265, VOAMRWB_MD2385};
    static const int mode2[4] = {VOAMRWB_MD1265, VOAMRWB_MD66,
                                 VOAMRWB_MD2385, VOAMRWB_MD885};

    setup_api();
    h1 = open_encoder(mode1[0]);
    {
        VO_CODEC_INIT_USERDATA user;
        int fmt = VOAMRWB_RFC3267, dtx = 0;
        memset(&user, 0, sizeof(user));
        user.memflag = VO_IMF_USERMEMOPERATOR;
        user.memData = &g_mem;
        if (g_api.Init(&h2, VO_AUDIO_CodingAMRWB, &user) != VO_ERR_NONE)
            h2 = NULL;
        else {
            g_api.SetParam(h2, VO_PID_AMRWB_FRAMETYPE, &fmt);
            g_api.SetParam(h2, VO_PID_AMRWB_DTX, &dtx);
            g_api.SetParam(h2, VO_PID_AMRWB_MODE, &mode2[0]);
        }
    }
    m1 = fe_model_new();
    m2 = fe_model_new();
    if (h1 == NULL || h2 == NULL || m1 == NULL || m2 == NULL)
        rc = FE_FAIL("interleaved setup", __FILE__, __LINE__);

    g_obs_target = h1;
    frontend_set_observer(capture_cb);
    for (fr = 0; fr < CORPUS_FRAMES && !rc; ++fr) {
        int16_t pcm1[FEREF_L_FRAME16K], pcm2[FEREF_L_FRAME16K];
        int i;
        corpus_frame(fr, pcm1);
        corpus_frame(fr + 7, pcm2);
        fe_model_frame(m1, pcm1, &f1);
        fe_model_frame(m2, pcm2, &f2);

        memset(&g_obs, 0, sizeof(g_obs));
        if (encode_frame(h1, mode1[fr % 4], pcm1, out, &len) < 0 ||
            encode_frame(h2, mode2[fr % 4], pcm2, out, &len) < 0) {
            rc = FE_FAIL("interleaved encode", __FILE__, __LINE__);
            break;
        }
        if (!task_check(g_obs.seen[FE_STAGE_END],
                        "interleaved end stage", __FILE__, __LINE__)) {
            rc = 1;
            break;
        }
        for (i = 0; i < FEREF_NSAMP; ++i) {
            if (g_obs.rec[FE_STAGE_END].samples[i] != f1.preemph[i]) {
                char expr[200];
                sprintf(expr,
                        "first divergence stream=1 frame=%d stage=%d "
                        "coord=%d exp=%d got=%d", fr, FE_STAGE_END, i,
                        (int)f1.preemph[i],
                        (int)g_obs.rec[FE_STAGE_END].samples[i]);
                FE_FAIL(expr, __FILE__, __LINE__);
                rc = 1;
                break;
            }
        }
        if (!rc && g_obs.rec[FE_STAGE_END].mem_preemph != f1.mem_preemph) {
            char expr[200];
            sprintf(expr,
                    "first divergence stream=1 frame=%d stage=%d "
                    "coord=mem_preemph exp=%d got=%d", fr, FE_STAGE_END,
                    (int)f1.mem_preemph,
                    (int)g_obs.rec[FE_STAGE_END].mem_preemph);
            FE_FAIL(expr, __FILE__, __LINE__);
            rc = 1;
        }
        (void)f2;
    }
    frontend_set_observer(NULL);
    g_obs_target = NULL;
    if (h1 != NULL)
        g_api.Uninit(h1);
    if (h2 != NULL)
        g_api.Uninit(h2);
    fe_model_free(m1);
    fe_model_free(m2);
    return rc;
#else
    return FE_FAIL("TASK_FRONTEND_TEST required", __FILE__, __LINE__);
#endif
}

/* ------------------------------------------------------------------ */
/* Registry                                                           */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"Polyphase", "shape", poly_shape},
    {"Polyphase", "impulse_alignment", poly_impulse_alignment},
    {"Polyphase", "full_chain", poly_full_chain},
    {"Preview", "isolation", preview_isolation},
    {"Preview", "mem_preemph", preview_mem_preemph},
    {"Preview", "max_search", preview_max_search},
    {"Scaling", "q_constraint", scaling_q_constraint},
    {"Scaling", "rescale", scaling_rescale},
    {"Scaling", "frame_end_state", scaling_frame_end_state},
    {"Streaming", "split_equivalence", stream_split_equivalence},
    {"Streaming", "homing_reset", stream_homing_reset},
    {"Streaming", "interleaved", stream_interleaved},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
