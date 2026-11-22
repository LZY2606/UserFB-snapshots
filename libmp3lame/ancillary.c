/*
 *      ancillary tail slot support
 *
 *      A tail slot reserves the last n bytes of a physical audio frame
 *      for caller supplied data.  The reservation is made known to the
 *      quantization loop before the frame is coded: for a slotted frame
 *      the nominal main data budget and the bit reservoir maximum are
 *      reduced so that no scalefactor/Huffman bit and no back pointed
 *      main data can land in the reserved bytes, and the whole
 *      reservoir is drained, forcing the next frame's main_data_begin
 *      back to zero.
 *
 *      The bitstream writer emits the slot bytes at the physical tail
 *      position through the normal bit writer, so header insertion
 *      timing, byte alignment and the reservoir bookkeeping all stay
 *      consistent with an ordinary ancillary fill.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "lame.h"
#include "machine.h"
#include "encoder.h"
#include "util.h"
#include "lame_global_flags.h"

#include "ancillary.h"


#define ANCILLARY_MAX_SLOT_SIZE 32
/* generous upper bound on the number of plan entries; rejects wildly
 * invalid counts before dereferencing the caller supplied array */
#define ANCILLARY_MAX_SLOTS 1000000


int
ancillary_set_plan(lame_global_flags * gfp,
                   const lame_ancillary_slot *slots, size_t count)
{
    lame_internal_flags *gfc;
    anc_slot_t *new_slots = NULL;
    size_t i;

    if (!is_lame_global_flags_valid(gfp))
        return -1;

    gfc = gfp->internal_flags;
    if (gfc == NULL)
        return -1;

    /* plans can only be installed before lame_init_params() */
    if (gfc->lame_init_params_successful > 0)
        return -1;

    /* NULL/0 clears an existing plan; any other half-null input is bad */
    if (slots == NULL || count == 0) {
        if (slots != NULL || count != 0)
            return -1;
        ancillary_free_plan(gfc);
        return 0;
    }
    if (count > ANCILLARY_MAX_SLOTS)
        return -1;

    /* validate the whole plan before touching the installed one */
    {
        unsigned long prev = 0;
        int have_prev = 0;
        for (i = 0; i < count; ++i) {
            if (slots[i].data == NULL)
                return -1;
            if (slots[i].size < 1 || slots[i].size > ANCILLARY_MAX_SLOT_SIZE)
                return -1;
            if (have_prev && slots[i].audio_frame <= prev)
                return -1;
            prev = slots[i].audio_frame;
            have_prev = 1;
        }
    }

    new_slots = lame_calloc(anc_slot_t, count);
    if (new_slots == NULL)
        return -1;

    for (i = 0; i < count; ++i) {
        new_slots[i].frame = slots[i].audio_frame;
        new_slots[i].size = slots[i].size;
        new_slots[i].data = malloc(slots[i].size);
        if (new_slots[i].data == NULL) {
            size_t k;
            for (k = 0; k < i; ++k)
                free(new_slots[k].data);
            free(new_slots);
            return -1;
        }
        memcpy(new_slots[i].data, slots[i].data, slots[i].size);
    }

    /* commit: replace the previously installed plan */
    ancillary_free_plan(gfc);
    gfc->ancillary.slots = new_slots;
    gfc->ancillary.count = count;
    gfc->ancillary.cursor = 0;
    gfc->ancillary.active = 0;
    gfc->ancillary.pending_size = 0;
    return 0;
}


void
ancillary_free_plan(lame_internal_flags * gfc)
{
    ancillary_state_t *anc;
    size_t i;

    if (gfc == NULL)
        return;

    anc = &gfc->ancillary;
    if (anc->slots != NULL) {
        for (i = 0; i < anc->count; ++i)
            free(anc->slots[i].data);
        free(anc->slots);
    }
    anc->slots = NULL;
    anc->count = 0;
    anc->cursor = 0;
    anc->active = 0;
    anc->pending_size = 0;
}


int
ancillary_validate_config(lame_internal_flags * gfc)
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    static int const supported_rates[3] = { 24000, 44100, 48000 };
    int k, rate_ok, index;

    if (gfc->ancillary.slots == NULL || gfc->ancillary.count == 0) {
        gfc->ancillary.active = 0;
        return 0;
    }

    if (cfg->vbr != vbr_off)
        return -1;
    if (cfg->free_format)
        return -1;
    if (cfg->samplerate_in != cfg->samplerate_out)
        return -1;

    rate_ok = 0;
    for (k = 0; k < (int) (sizeof(supported_rates) / sizeof(supported_rates[0])); ++k) {
        if (cfg->samplerate_out == supported_rates[k])
            rate_ok = 1;
    }
    if (!rate_ok)
        return -1;

    if (cfg->mode != MONO && cfg->mode != STEREO && cfg->mode != JOINT_STEREO)
        return -1;

    if (cfg->avg_bitrate < 64 || cfg->avg_bitrate > 160)
        return -1;

    index = BitrateIndex(cfg->avg_bitrate, cfg->version, cfg->samplerate_out);
    if (index <= 0 || bitrate_table[cfg->version][index] != cfg->avg_bitrate)
        return -1;

    gfc->ancillary.active = 1;
    gfc->ancillary.cursor = 0;
    gfc->ancillary.pending_size = 0;
    return 0;
}


void
ancillary_begin_frame(lame_internal_flags * gfc, unsigned long frame)
{
    ancillary_state_t *anc = &gfc->ancillary;

    anc->pending_size = 0;
    if (!anc->active)
        return;

    /* skip plan entries whose frames were never produced */
    while (anc->cursor < anc->count && anc->slots[anc->cursor].frame < frame)
        anc->cursor++;

    if (anc->cursor < anc->count && anc->slots[anc->cursor].frame == frame)
        anc->pending_size = (int) anc->slots[anc->cursor].size;
}


int
ancillary_pending_size(lame_internal_flags const * gfc)
{
    return gfc->ancillary.pending_size;
}


unsigned char
ancillary_slot_byte(lame_internal_flags const * gfc, int index)
{
    ancillary_state_t const *anc = &gfc->ancillary;

    if (anc->cursor >= anc->count)
        return 0;
    if (index < 0 || index >= anc->pending_size)
        return 0;
    return anc->slots[anc->cursor].data[index];
}


void
ancillary_slot_consumed(lame_internal_flags * gfc)
{
    ancillary_state_t *anc = &gfc->ancillary;

    if (anc->pending_size <= 0)
        return;
    anc->pending_size = 0;
    if (anc->cursor < anc->count)
        anc->cursor++;
}
