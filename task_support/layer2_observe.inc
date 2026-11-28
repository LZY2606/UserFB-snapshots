/*
    layer2_observe.inc - read-only observation point for the Layer II branch.

    This file is included by minimp3.h only when MINIMP3_LAYER2_OBSERVE is
    defined to 1.  The normal build leaves the macro at 0 and carries no extra
    state or callbacks.  Everything here is file local (static inline) so the
    production translation unit is unchanged when the hook is disabled.

    The recorder snapshots the 2 x 32 x 12 subband values of one 384 sample
    block right after L12_apply_scf_384 has run and before the synthesis
    filterbank touches them.
*/
#ifndef LAYER2_OBSERVE_INC
#define LAYER2_OBSERVE_INC

#include <string.h>

#define LAYER2_OBSERVE_MAX_RECORDS 64

typedef struct
{
    int   total_bands;
    int   stereo_bands;
    float values[2][32][12];
} layer2_obs_t;

static layer2_obs_t layer2_obs_records[LAYER2_OBSERVE_MAX_RECORDS];
static int          layer2_obs_record_count;

static inline void layer2_obs_reset(void)
{
    layer2_obs_record_count = 0;
}

static inline int layer2_obs_count(void)
{
    return layer2_obs_record_count;
}

static inline const layer2_obs_t *layer2_obs_get(int index)
{
    if (index < 0 || index >= layer2_obs_record_count)
        return 0;
    return &layer2_obs_records[index];
}

static inline void layer2_obs_record(const L12_scale_info *sci, const float *grbuf)
{
    int ch, band, moment;
    layer2_obs_t *rec;
    if (layer2_obs_record_count >= LAYER2_OBSERVE_MAX_RECORDS)
        return;
    rec = &layer2_obs_records[layer2_obs_record_count++];
    rec->total_bands  = sci->total_bands;
    rec->stereo_bands = sci->stereo_bands;
    memset(rec->values, 0, sizeof(rec->values));
    for (ch = 0; ch < 2; ch++)
    {
        for (band = 0; band < sci->total_bands; band++)
        {
            for (moment = 0; moment < 12; moment++)
                rec->values[ch][band][moment] = grbuf[ch * 576 + 18 * band + moment];
        }
    }
}

#endif /* LAYER2_OBSERVE_INC */
