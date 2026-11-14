/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** The "appropriate copyright message" mentioned in section 2c of the GPLv2
** must read: "Code from FAAD2 is copyright (c) Nero AG, www.nero.com"
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Nero AG through Mpeg4AAClicense@nero.com.
**/

/*
   Frame-boundary program_config_element update (NeAACDecUpdatePCE).

   Allows an explicit-PCE AAC-LC raw stream to switch its channel layout
   between two complete access units without tearing the decoder down.
   Element identities (SCE/CPE type + tag) that survive the layout change
   keep their synthesis history (overlap buffer and previous window
   shape); new identities start with a clean history and dropped
   identities release their resources.
*/

#include "common.h"
#include "structs.h"

#include <stdlib.h>
#include <string.h>

#include "mp4.h"
#include "syntax.h"
#include "bits.h"

/* element slot of an identity: SCE tag t -> t, CPE tag t -> 16 + t */
#define PCE_CPE_SLOT_BASE 16

#ifndef FIXED_POINT
/* collect the (type, tag) identities of the front/side/back elements of
   a program_config; duplicate identities are rejected */
static int pce_identity_masks(const program_config *pce,
                              uint16_t *sce_mask, uint16_t *cpe_mask)
{
    uint8_t i;
    uint16_t sce = 0, cpe = 0;

    for (i = 0; i < pce->num_front_channel_elements; i++)
    {
        uint8_t tag = pce->front_element_tag_select[i];
        if (pce->front_element_is_cpe[i])
        {
            if (cpe & (1u << tag)) return -1;
            cpe |= (uint16_t)(1u << tag);
        } else {
            if (sce & (1u << tag)) return -1;
            sce |= (uint16_t)(1u << tag);
        }
    }
    for (i = 0; i < pce->num_side_channel_elements; i++)
    {
        uint8_t tag = pce->side_element_tag_select[i];
        if (pce->side_element_is_cpe[i])
        {
            if (cpe & (1u << tag)) return -1;
            cpe |= (uint16_t)(1u << tag);
        } else {
            if (sce & (1u << tag)) return -1;
            sce |= (uint16_t)(1u << tag);
        }
    }
    for (i = 0; i < pce->num_back_channel_elements; i++)
    {
        uint8_t tag = pce->back_element_tag_select[i];
        if (pce->back_element_is_cpe[i])
        {
            if (cpe & (1u << tag)) return -1;
            cpe |= (uint16_t)(1u << tag);
        } else {
            if (sce & (1u << tag)) return -1;
            sce |= (uint16_t)(1u << tag);
        }
    }

    *sce_mask = sce;
    *cpe_mask = cpe;
    return 0;
}

/* map the front/side/back SCE/CPE identities of a program_config to
   their (first) channel slot; 0xFF marks an absent identity */
static void pce_identity_slots(const program_config *pce,
                               uint8_t *sce_slot, uint8_t *cpe_slot)
{
    uint8_t i;

    for (i = 0; i < 16; i++)
    {
        sce_slot[i] = 0xFF;
        cpe_slot[i] = 0xFF;
    }
    for (i = 0; i < pce->num_front_channel_elements; i++)
    {
        uint8_t tag = pce->front_element_tag_select[i];
        if (pce->front_element_is_cpe[i])
            cpe_slot[tag] = pce->cpe_channel[tag];
        else
            sce_slot[tag] = pce->sce_channel[tag];
    }
    for (i = 0; i < pce->num_side_channel_elements; i++)
    {
        uint8_t tag = pce->side_element_tag_select[i];
        if (pce->side_element_is_cpe[i])
            cpe_slot[tag] = pce->cpe_channel[tag];
        else
            sce_slot[tag] = pce->sce_channel[tag];
    }
    for (i = 0; i < pce->num_back_channel_elements; i++)
    {
        uint8_t tag = pce->back_element_tag_select[i];
        if (pce->back_element_is_cpe[i])
            cpe_slot[tag] = pce->cpe_channel[tag];
        else
            sce_slot[tag] = pce->sce_channel[tag];
    }
}

/* parse and validate the new AudioSpecificConfig with its explicit PCE;
   on success the PCE and its identity masks are returned */
static int pce_update_parse(NeAACDecStruct *hDecoder,
                            const unsigned char *asc, unsigned long size,
                            program_config *pce,
                            uint16_t *sce_mask, uint16_t *cpe_mask)
{
    mp4AudioSpecificConfig mp4ASC;
    bitfile ld;
    int8_t rc;

    faad_initbits(&ld, asc, size);
    if (ld.error != 0)
        return -1;

    memset(&mp4ASC, 0, sizeof(mp4AudioSpecificConfig));

    mp4ASC.objectTypeIndex = (uint8_t)faad_getbits(&ld, 5);
    mp4ASC.samplingFrequencyIndex = (uint8_t)faad_getbits(&ld, 4);
    if (mp4ASC.samplingFrequencyIndex == 0x0f)
        mp4ASC.samplingFrequency = faad_getbits(&ld, 24);
    else
        mp4ASC.samplingFrequency = get_sample_rate(mp4ASC.samplingFrequencyIndex);
    mp4ASC.channelsConfiguration = (uint8_t)faad_getbits(&ld, 4);

    rc = GASpecificConfig(&ld, &mp4ASC, pce);
    if (rc != 0 || ld.error != 0)
    {
        faad_endbits(&ld);
        return -1;
    }

    /* the complete ASC must be consumed: anything trailing (for example
       an SBR sync extension) is outside the scope of this update */
    if (faad_get_processed_bits(&ld) != size * 8)
    {
        faad_endbits(&ld);
        return -1;
    }
    faad_endbits(&ld);

    /* same object type, sample rate and frame length as the active
       configuration, explicit PCE, no extension objects */
    if (mp4ASC.objectTypeIndex != hDecoder->object_type)
        return -1;
    if (mp4ASC.samplingFrequencyIndex != hDecoder->sf_index)
        return -1;
    if (mp4ASC.samplingFrequency != get_sample_rate(hDecoder->sf_index))
        return -1;
    if (mp4ASC.channelsConfiguration != 0)
        return -1;
    if (mp4ASC.frameLengthFlag != 0)
        return -1;
    if (mp4ASC.dependsOnCoreCoder != 0)
        return -1;
    if (mp4ASC.extensionFlag != 0)
        return -1;
    if (mp4ASC.sbr_present_flag == 1 || mp4ASC.ps_present_flag == 1)
        return -1;

    /* the PCE profile and sample rate must match the ASC */
    if (pce->object_type != 1) /* PCE profile: LC */
        return -1;
    if (pce->sf_index != mp4ASC.samplingFrequencyIndex)
        return -1;

    /* only front/side/back SCE and CPE elements, 1..8 channels in total,
       at least one front element */
    if (pce->num_front_channel_elements == 0)
        return -1;
    if (pce->num_lfe_channel_elements != 0)
        return -1;
    if (pce->num_assoc_data_elements != 0)
        return -1;
    if (pce->num_valid_cc_elements != 0)
        return -1;
    if (pce->mono_mixdown_present || pce->stereo_mixdown_present ||
        pce->matrix_mixdown_idx_present)
        return -1;
    if (pce->channels == 0 || pce->channels > 8)
        return -1;

    if (pce_identity_masks(pce, sce_mask, cpe_mask) != 0)
        return -1;

    return 0;
}

/* switch the active layout: identities that survive keep their synthesis
   history, new identities start clean, dropped identities are freed */
static void pce_update_commit(NeAACDecStruct *hDecoder,
                              const program_config *pce,
                              uint16_t sce_mask, uint16_t cpe_mask)
{
    real_t *old_time_out[MAX_CHANNELS];
    real_t *old_fb_intermed[MAX_CHANNELS];
    uint8_t old_shape[MAX_CHANNELS];
    uint8_t old_sce_slot[16], old_cpe_slot[16];
    uint8_t new_sce_slot[16], new_cpe_slot[16];
    uint8_t i;

    pce_identity_slots(&hDecoder->pce, old_sce_slot, old_cpe_slot);
    pce_identity_slots(pce, new_sce_slot, new_cpe_slot);

    /* detach all per-channel synthesis state */
    memcpy(old_time_out, hDecoder->time_out, sizeof(old_time_out));
    memcpy(old_fb_intermed, hDecoder->fb_intermed, sizeof(old_fb_intermed));
    memcpy(old_shape, hDecoder->window_shape_prev, sizeof(old_shape));
    memset(hDecoder->time_out, 0, sizeof(hDecoder->time_out));
    memset(hDecoder->fb_intermed, 0, sizeof(hDecoder->fb_intermed));
    memset(hDecoder->window_shape_prev, 0, sizeof(hDecoder->window_shape_prev));

    for (i = 0; i < MAX_SYNTAX_ELEMENTS; i++)
    {
        hDecoder->element_id[i] = INVALID_ELEMENT_ID;
        hDecoder->element_alloced[i] = 0;
        hDecoder->element_output_channels[i] = 0;
    }

    /* hand the history of surviving identities over to their new slots */
    for (i = 0; i < 16; i++)
    {
        uint8_t is_cpe, tag, ele, new_slot, old_slot, nch;

        for (is_cpe = 0; is_cpe < 2; is_cpe++)
        {
            if (is_cpe)
            {
                if (!(cpe_mask & (1u << i)))
                    continue;
            } else {
                if (!(sce_mask & (1u << i)))
                    continue;
            }
            tag = i;
            ele = is_cpe ? (PCE_CPE_SLOT_BASE + tag) : tag;
            new_slot = is_cpe ? new_cpe_slot[tag] : new_sce_slot[tag];
            old_slot = is_cpe ? old_cpe_slot[tag] : old_sce_slot[tag];
            nch = is_cpe ? 2 : 1;

            hDecoder->element_id[ele] = is_cpe ? ID_CPE : ID_SCE;

            if (old_slot != 0xFF && old_time_out[old_slot] != NULL)
            {
                uint8_t c;
                for (c = 0; c < nch; c++)
                {
                    hDecoder->time_out[new_slot + c] = old_time_out[old_slot + c];
                    old_time_out[old_slot + c] = NULL;
                    hDecoder->fb_intermed[new_slot + c] = old_fb_intermed[old_slot + c];
                    old_fb_intermed[old_slot + c] = NULL;
                    hDecoder->window_shape_prev[new_slot + c] = old_shape[old_slot + c];
                }
                hDecoder->element_alloced[ele] = nch;
                hDecoder->element_output_channels[ele] = nch;
            }
        }
    }

    /* everything not moved belongs to retired identities */
    for (i = 0; i < MAX_CHANNELS; i++)
    {
        if (old_time_out[i])
            faad_free(old_time_out[i]);
        if (old_fb_intermed[i])
            faad_free(old_fb_intermed[i]);
    }

    memcpy(&hDecoder->pce, pce, sizeof(program_config));
    hDecoder->pce_set = 1;
    hDecoder->pce_layout_locked = 1;
    hDecoder->pce_sce_mask = sce_mask;
    hDecoder->pce_cpe_mask = cpe_mask;
    hDecoder->pce_seen = 0;
    hDecoder->channelConfiguration = 0;
}
#endif /* FIXED_POINT */

int NeAACDecUpdatePCE(NeAACDecHandle hpDecoder,
                      const unsigned char *asc,
                      unsigned long size)
{
#ifdef FIXED_POINT
    /* the frame-boundary layout update is only available in the
       floating point build */
    (void)hpDecoder;
    (void)asc;
    (void)size;
    return -1;
#else
    NeAACDecStruct *hDecoder = (NeAACDecStruct*)hpDecoder;
    program_config pce;
    uint16_t sce_mask = 0, cpe_mask = 0;

    if ((hDecoder == NULL) || (asc == NULL) || (size < 2))
        return -1;

    /* only for handles initialised by NeAACDecInit2 with an explicit PCE;
       ADTS/ADIF/LATM and in-stream PCE conventions are not extended */
    if (hDecoder->fb == NULL)
        return -1;
    if (!hDecoder->pce_set)
        return -1;
    if (hDecoder->adts_header_present || hDecoder->adif_header_present ||
        hDecoder->latm_header_present)
        return -1;

    /* plain AAC-LC, 1024 sample frames, no SBR */
    if (hDecoder->object_type != LC)
        return -1;
    if (hDecoder->frameLength != 1024)
        return -1;
#ifdef SBR_DEC
    if ((hDecoder->sbr_present_flag == 1) || (hDecoder->forceUpSampling == 1))
        return -1;
#endif

    memset(&pce, 0, sizeof(program_config));
    if (pce_update_parse(hDecoder, asc, size, &pce, &sce_mask, &cpe_mask) != 0)
        return -1;

    pce_update_commit(hDecoder, &pce, sce_mask, cpe_mask);

    return 0;
#endif /* FIXED_POINT */
}
