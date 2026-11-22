/*
 * Read-only front-end observation interface used only by the regression
 * build (-DTASK_FRONTEND_TEST).  In a normal build this header is never
 * included, so no observer, callback or extra state exists in production.
 */

#ifndef __FRONTEND_OBSERVE_H__
#define __FRONTEND_OBSERVE_H__

#include "typedef.h"

#define FE_STAGE_DECIM    1
#define FE_STAGE_HP       2
#define FE_STAGE_PREEMPH  3
#define FE_STAGE_END      4

#define FE_OBS_SAMPLES    268   /* L_FRAME + L_FILT                        */
#define FE_OBS_DECIM_MEM   30   /* 2 * L_FILT16k                          */
#define FE_OBS_SIG_MEM      6
#define FE_OBS_OLD        128   /* L_TOTAL - L_FRAME                      */
#define FE_OBS_PAST       116   /* L_TOTAL - L_FRAME - L_FILT             */

typedef struct
{
    Word16 samples[FE_OBS_SAMPLES];
    Word16 mem_decim[FE_OBS_DECIM_MEM];
    Word16 mem_sig_in[FE_OBS_SIG_MEM];
    Word16 mem_preemph;
    Word16 q_new;
    Word16 q_old;
    Word16 q_max0;
    Word16 q_max1;
    Word16 old_speech[FE_OBS_OLD];
    Word16 past_scaled[FE_OBS_PAST];
} FrontendStageRecord;

typedef void (*FrontendObserver)(void *coder, Word16 stage,
                                 const FrontendStageRecord *record);

void frontend_set_observer(FrontendObserver observer);

#endif /* __FRONTEND_OBSERVE_H__ */
