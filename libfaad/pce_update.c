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
   Explicit frame-boundary PCE update (NeAACDecUpdatePCE).

   A decoder that was initialized by NeAACDecInit2 with an explicit
   program configuration element can be moved to a new channel layout
   between two complete access units without reopening the handle.
   Channel elements are identified by (type, tag); identities that
   survive the update keep their synthesis history (overlap buffer and
   previous window shape) no matter where the new PCE places them.

   The accepted configurations are deliberately narrow: plain AAC-LC,
   1024 sample frames, unchanged sampling rate, front/side/back SCE and
   CPE elements only, 1..8 channels, at least one front element, no LFE,
   associated data, coupling, mixdown or extension signalling, and no
   duplicated (type, tag) pair.
*/

#include "common.h"
#include "structs.h"

#include <stdlib.h>
#include <string.h>

#include "bits.h"
#include "mp4.h"
#include "syntax.h"

#ifndef FIXED_POINT

/* number of front+side+back elements in a program_config */
static uint8_t pce_num_elements(const program_config *pce)
{
    return (uint8_t)(pce->num_front_channel_elements +
                     pce->num_side_channel_elements +
                     pce->num_back_channel_elements);
}

/* fetch the (is_cpe, tag) identity of the ordinal-th front/side/back element */
static void pce_element_identity(const program_config *pce, uint8_t ordinal,
                                 uint8_t *is_cpe, uint8_t *tag)
{
    if (ordinal < pce->num_front_channel_elements)
    {
        *is_cpe = pce->front_element_is_cpe[ordinal] & 1;
        *tag = pce->front_element_tag_select[ordinal];
    } else {
        ordinal = (uint8_t)(ordinal - pce->num_front_channel_elements);
        if (ordinal < pce->num_side_channel_elements)
        {
            *is_cpe = pce->side_element_is_cpe[ordinal] & 1;
            *tag = pce->side_element_tag_select[ordinal];
        } else {
            ordinal = (uint8_t)(ordinal - pce->num_side_channel_elements);
            *is_cpe = pce->back_element_is_cpe[ordinal] & 1;
            *tag = pce->back_element_tag_select[ordinal];
        }
    }
}

/* validate a freshly parsed PCE against the restrictions of this interface */
static int pce_update_validate(const program_config *pce,
                               const mp4AudioSpecificConfig *mp4ASC)
{
    uint8_t i, j;
    uint8_t elements = pce_num_elements(pce);

    /* at least one front element is required */
    if (pce->num_front_channel_elements == 0)
        return -1;

    /* no LFE, associated data, coupling or mixdown signalling */
    if (pce->num_lfe_channel_elements != 0)
        return -1;
    if (pce->num_assoc_data_elements != 0)
        return -1;
    if (pce->num_valid_cc_elements != 0)
        return -1;
    if (pce->mono_mixdown_present || pce->stereo_mixdown_present ||
        pce->matrix_mixdown_idx_present)
        return -1;

    /* 1..8 channels in total */
    if ((pce->channels < 1) || (pce->channels > 8))
        return -1;

    /* the PCE profile and sampling rate must match the ASC */
    if (pce->object_type + 1 != mp4ASC->objectTypeIndex)
        return -1;
    if (pce->sf_index != mp4ASC->samplingFrequencyIndex)
        return -1;

    /* no duplicated (type, tag) pair; SCE t and CPE t are two identities */
    for (i = 0; i < elements; i++)
    {
        uint8_t is_cpe_i, tag_i;
        pce_element_identity(pce, i, &is_cpe_i, &tag_i);
        for (j = (uint8_t)(i + 1); j < elements; j++)
        {
            uint8_t is_cpe_j, tag_j;
            pce_element_identity(pce, j, &is_cpe_j, &tag_j);
            if ((is_cpe_i == is_cpe_j) && (tag_i == tag_j))
                return -1;
        }
    }

    return 0;
}

#endif /* FIXED_POINT */

int NeAACDecUpdatePCE(NeAACDecHandle hpDecoder,
                      const unsigned char *asc,
                      unsigned long size)
{
#ifdef FIXED_POINT
    /* this interface is limited to floating point builds */
    (void)hpDecoder;
    (void)asc;
    (void)size;
    return -1;
#else
    NeAACDecStruct *hDecoder = (NeAACDecStruct*)hpDecoder;
    mp4AudioSpecificConfig mp4ASC;
    program_config pce;
    bitfile ld;
    int8_t rc;
    uint8_t i, j;
    uint8_t old_elements, new_elements;

    real_t *new_time_out[MAX_CHANNELS];
    real_t *new_fb_intermed[MAX_CHANNELS];
    uint8_t new_window_shape_prev[MAX_CHANNELS];
    uint8_t new_element_id[MAX_CHANNELS];
    uint8_t new_element_output_channels[MAX_SYNTAX_ELEMENTS];
    uint8_t new_element_alloced[MAX_SYNTAX_ELEMENTS];
    int8_t new_ele_slot[2][16];
    uint8_t old_slot_claimed[MAX_CHANNELS];
    uint8_t old_ele_matched[MAX_SYNTAX_ELEMENTS];

    if ((hDecoder == NULL) || (asc == NULL) || (size < 2))
        return -1;

    /* only handles initialized by Init2 with an explicit PCE qualify */
    if (!hDecoder->pce_init || !hDecoder->pce_set)
        return -1;
    if (hDecoder->adts_header_present || hDecoder->adif_header_present ||
        hDecoder->latm_header_present)
        return -1;

    /* plain AAC-LC, 1024 sample frames, decoder actually initialized */
    if ((hDecoder->object_type != LC) || (hDecoder->frameLength != 1024))
        return -1;
    if (hDecoder->fb == NULL)
        return -1;
#ifdef SBR_DEC
    if ((hDecoder->sbr_present_flag == 1) || (hDecoder->forceUpSampling == 1))
        return -1;
#endif
#if (defined(PS_DEC) || defined(DRM_PS))
    if (hDecoder->ps_used_global)
        return -1;
#endif

    /* parse the complete AudioSpecificConfig; decoder state is only
       touched after every check below has passed */
    faad_initbits(&ld, asc, (uint32_t)size);
    if (ld.error != 0)
        return -1;
    rc = AudioSpecificConfigFromBitfile(&ld, &mp4ASC, &pce, (uint32_t)size, 0);
    if ((rc != 0) || (ld.error != 0))
        return -1;

    /* explicit PCE signalling only (channelConfiguration == 0) */
    if (mp4ASC.channelsConfiguration != 0)
        return -1;

    /* same object type, sampling rate and frame length as the active
       configuration */
    if (mp4ASC.objectTypeIndex != hDecoder->object_type)
        return -1;
    if (mp4ASC.frameLengthFlag != 0)
        return -1;
    if (mp4ASC.samplingFrequency != get_sample_rate(hDecoder->sf_index))
        return -1;
#ifdef SBR_DEC
    if ((mp4ASC.sbr_present_flag == 1) || (mp4ASC.ps_present_flag == 1))
        return -1;
#endif

    if (pce_update_validate(&pce, &mp4ASC) != 0)
        return -1;

    /* From here on the update cannot fail anymore.

       The old layout maps each identity to the channel slots given by
       the old PCE (in legacy mode this is the only encounter order that
       decodes at all, in identity mode it is the mapping installed by
       the previous update).  Move the per-channel synthesis history of
       every surviving identity to its new slot, leave fresh identities
       with zero history and release everything that is retired. */
    old_elements = pce_num_elements(&hDecoder->pce);
    new_elements = pce_num_elements(&pce);

    memset(new_time_out, 0, sizeof(new_time_out));
    memset(new_fb_intermed, 0, sizeof(new_fb_intermed));
    memset(new_window_shape_prev, 0, sizeof(new_window_shape_prev));
    memset(new_element_id, INVALID_ELEMENT_ID, sizeof(new_element_id));
    memset(new_element_output_channels, 0, sizeof(new_element_output_channels));
    memset(new_element_alloced, 0, sizeof(new_element_alloced));
    memset(new_ele_slot, 0xFF, sizeof(new_ele_slot));
    memset(old_slot_claimed, 0, sizeof(old_slot_claimed));
    memset(old_ele_matched, 0, sizeof(old_ele_matched));

    for (i = 0; i < new_elements; i++)
    {
        uint8_t is_cpe, tag, new_slot;

        pce_element_identity(&pce, i, &is_cpe, &tag);
        new_ele_slot[is_cpe][tag] = (int8_t)i;
        new_slot = is_cpe ? pce.cpe_channel[tag] : pce.sce_channel[tag];

        /* find the same (type, tag) identity in the old layout */
        for (j = 0; j < old_elements; j++)
        {
            uint8_t old_is_cpe, old_tag, old_slot;

            if (old_ele_matched[j])
                continue;
            pce_element_identity(&hDecoder->pce, j, &old_is_cpe, &old_tag);
            if ((old_is_cpe != is_cpe) || (old_tag != tag))
                continue;

            old_slot = old_is_cpe ? hDecoder->pce.cpe_channel[old_tag]
                                  : hDecoder->pce.sce_channel[old_tag];

            /* carry the synthesis history over to the new slot */
            new_time_out[new_slot] = hDecoder->time_out[old_slot];
            new_fb_intermed[new_slot] = hDecoder->fb_intermed[old_slot];
            new_window_shape_prev[new_slot] = hDecoder->window_shape_prev[old_slot];
            old_slot_claimed[old_slot] = 1;
            if (is_cpe)
            {
                new_time_out[new_slot+1] = hDecoder->time_out[old_slot+1];
                new_fb_intermed[new_slot+1] = hDecoder->fb_intermed[old_slot+1];
                new_window_shape_prev[new_slot+1] =
                    hDecoder->window_shape_prev[old_slot+1];
                old_slot_claimed[old_slot+1] = 1;
            }

            new_element_id[i] = hDecoder->element_id[j];
            new_element_output_channels[i] = hDecoder->element_output_channels[j];
            new_element_alloced[i] = hDecoder->element_alloced[j];
            old_ele_matched[j] = 1;
            break;
        }
    }

    /* release the resources of every retired channel slot */
    for (i = 0; i < MAX_CHANNELS; i++)
    {
        if (old_slot_claimed[i])
            continue;
        if (hDecoder->time_out[i])
            faad_free(hDecoder->time_out[i]);
        if (hDecoder->fb_intermed[i])
            faad_free(hDecoder->fb_intermed[i]);
    }

    /* commit the new layout; the caller's ASC buffer is not retained */
    memcpy(hDecoder->time_out, new_time_out, sizeof(new_time_out));
    memcpy(hDecoder->fb_intermed, new_fb_intermed, sizeof(new_fb_intermed));
    memcpy(hDecoder->window_shape_prev, new_window_shape_prev,
        sizeof(new_window_shape_prev));
    memcpy(hDecoder->element_id, new_element_id, sizeof(new_element_id));
    memcpy(hDecoder->element_output_channels, new_element_output_channels,
        sizeof(new_element_output_channels));
    memcpy(hDecoder->element_alloced, new_element_alloced,
        sizeof(new_element_alloced));
    memcpy(hDecoder->pce_ele_slot, new_ele_slot, sizeof(new_ele_slot));
    memset(hDecoder->pce_ele_seen, 0, sizeof(hDecoder->pce_ele_seen));

    memcpy(&hDecoder->pce, &pce, sizeof(program_config));
    hDecoder->pce_set = 1;
    hDecoder->pce_update = 1;

    return 0;
#endif /* FIXED_POINT */
}
