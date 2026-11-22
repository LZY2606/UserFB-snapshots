/*
 * Shared declarations for the AMR-WB front-end regression model.
 * Only the test build consumes this header.
 */

#ifndef FRONTEND_REFERENCE_H
#define FRONTEND_REFERENCE_H

#include <stdint.h>

#define FEREF_L_FRAME16K 320
#define FEREF_L_FRAME    256
#define FEREF_L_FILT16K   15
#define FEREF_L_FILT      12
#define FEREF_NSAMP      (FEREF_L_FRAME + FEREF_L_FILT)  /* 268 */
#define FEREF_NPAST      (FEREF_NHIST - FEREF_L_FILT)    /* 116 */
#define FEREF_NHIST      128
#define FEREF_NMEM        30

typedef struct FeModel FeModel;

typedef struct FeFrame {
    int16_t decim[FEREF_L_FRAME];       /* 256 real decimated points        */
    int16_t preview[FEREF_L_FILT];      /* 12 preview decimated points      */
    int16_t hp[FEREF_NSAMP];            /* 50 Hz output, real then preview  */
    int16_t preemph[FEREF_NSAMP];       /* scaled pre-emphasis output       */
    int16_t mem_decim[FEREF_NMEM];      /* long-term decimator memory       */
    int16_t mem_sig_in[6];              /* HP memory, real points only      */
    int16_t hp_mem_preview[6];          /* HP memory the *discarded* preview
                                           would leave behind (proof point)  */
    int16_t mem_preemph;                /* last real HP point (index 255)   */
    int16_t q_uncapped;                 /* shift before the Q_max clamp     */
    int16_t q_new;
    int16_t q_old;
    int16_t q_max0;
    int16_t q_max1;
    int16_t past_scaled[FEREF_NPAST];   /* previous speech interval, rescaled */
    int16_t next_old_speech[FEREF_NHIST]; /* next frame old_speech image    */

    /* Coverage counters: exact 16-bit rounding-midpoint ties observed. */
    unsigned decim_tie_pos, decim_tie_neg;  /* decimator, sum >= 0 / < 0  */
    unsigned pre_tie_pos, pre_tie_neg;      /* pre-emphasis, value sign    */
    unsigned phase_out[4];                  /* decimator outputs per phase */
    unsigned phase_tie_pos[4];              /* ties per phase, sum >= 0    */
    unsigned phase_tie_neg[4];              /* ties per phase, sum < 0     */
} FeFrame;

FeModel *fe_model_new(void);
void fe_model_reset(FeModel *m);
void fe_model_free(FeModel *m);
int fe_is_homing_frame(const int16_t in[FEREF_L_FRAME16K]);
int fe_model_frame(FeModel *m, const int16_t in[FEREF_L_FRAME16K], FeFrame *out);

#endif /* FRONTEND_REFERENCE_H */
