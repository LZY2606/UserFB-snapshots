/*****************************************************************************
 * psi_lane.c: current/next generation lanes for PSI table decoders
 *----------------------------------------------------------------------------
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *----------------------------------------------------------------------------
 *
 *****************************************************************************/

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "dvbpsi.h"
#include "psi.h"
#include "psi_lane.h"

void dvbpsi_psi_epochs_init(dvbpsi_psi_epochs_t *p_epochs)
{
    if (p_epochs == NULL)
        return;
    memset(p_epochs, 0, sizeof(*p_epochs));
}

void dvbpsi_psi_epochs_clear_lane(dvbpsi_psi_epochs_t *p_epochs, int i_lane,
                                  void (*pf_delete_building)(void *))
{
    if ((p_epochs == NULL) || (i_lane < 0) || (i_lane >= DVBPSI_LANE_COUNT))
        return;

    dvbpsi_psi_epochs_reset_lane(p_epochs, i_lane, pf_delete_building);

    memset(p_epochs->delivered[i_lane], 0,
           sizeof(p_epochs->delivered[i_lane]));
    p_epochs->i_delivered_count[i_lane] = 0;
    p_epochs->i_delivered_next[i_lane] = 0;
}

void dvbpsi_psi_epochs_reset_lane(dvbpsi_psi_epochs_t *p_epochs, int i_lane,
                                  void (*pf_delete_building)(void *))
{
    dvbpsi_psi_lane_t *p_lane;

    if ((p_epochs == NULL) || (i_lane < 0) || (i_lane >= DVBPSI_LANE_COUNT))
        return;

    p_lane = &p_epochs->lanes[i_lane];

    dvbpsi_DeletePSISections(p_lane->p_sections);
    p_lane->p_sections = NULL;

    if (p_lane->p_building && pf_delete_building)
        pf_delete_building(p_lane->p_building);
    p_lane->p_building = NULL;
    p_lane->b_active = false;
    memset(&p_lane->identity, 0, sizeof(p_lane->identity));
}

void dvbpsi_psi_epochs_clear(dvbpsi_psi_epochs_t *p_epochs,
                             void (*pf_delete_building)(void *))
{
    int i;
    for (i = 0; i < DVBPSI_LANE_COUNT; i++)
        dvbpsi_psi_epochs_clear_lane(p_epochs, i, pf_delete_building);
}

int dvbpsi_psi_lane_from_section(const dvbpsi_psi_section_t *p_section)
{
    if (p_section != NULL && !p_section->b_current_next)
        return DVBPSI_LANE_NEXT;
    return DVBPSI_LANE_CURRENT;
}

bool dvbpsi_psi_identity_equal(const dvbpsi_psi_identity_t *p_a,
                               const dvbpsi_psi_identity_t *p_b)
{
    return (p_a->i_table_id  == p_b->i_table_id) &&
           (p_a->i_extension == p_b->i_extension) &&
           (p_a->i_version   == p_b->i_version) &&
           (p_a->i_last_number == p_b->i_last_number) &&
           (p_a->b_current_next == p_b->b_current_next);
}

bool dvbpsi_psi_identity_delivered(const dvbpsi_psi_epochs_t *p_epochs,
                                   int i_lane,
                                   const dvbpsi_psi_identity_t *p_identity)
{
    uint8_t i, i_count, i_start;

    if ((p_epochs == NULL) || (i_lane < 0) || (i_lane >= DVBPSI_LANE_COUNT))
        return false;

    i_count = p_epochs->i_delivered_count[i_lane];
    if (i_count > DVBPSI_DELIVERED_HISTORY)
        i_count = DVBPSI_DELIVERED_HISTORY;
    i_start = (uint8_t)(p_epochs->i_delivered_next[i_lane] +
                        DVBPSI_DELIVERED_HISTORY - i_count) %
              DVBPSI_DELIVERED_HISTORY;

    for (i = 0; i < i_count; i++)
    {
        const dvbpsi_psi_identity_t *p_stored =
            &p_epochs->delivered[i_lane][(i_start + i) % DVBPSI_DELIVERED_HISTORY];
        if (dvbpsi_psi_identity_equal(p_stored, p_identity))
            return true;
    }
    return false;
}

void dvbpsi_psi_identity_mark_delivered(dvbpsi_psi_epochs_t *p_epochs,
                                        int i_lane,
                                        const dvbpsi_psi_identity_t *p_identity)
{
    uint8_t i_next;

    if ((p_epochs == NULL) || (i_lane < 0) || (i_lane >= DVBPSI_LANE_COUNT))
        return;

    i_next = p_epochs->i_delivered_next[i_lane];
    p_epochs->delivered[i_lane][i_next] = *p_identity;
    p_epochs->i_delivered_next[i_lane] =
        (uint8_t)((i_next + 1) % DVBPSI_DELIVERED_HISTORY);
    if (p_epochs->i_delivered_count[i_lane] < DVBPSI_DELIVERED_HISTORY)
        p_epochs->i_delivered_count[i_lane]++;
}

bool dvbpsi_psi_lane_section_add(dvbpsi_psi_section_t **pp_sections,
                                 dvbpsi_psi_section_t *p_section)
{
    dvbpsi_psi_section_t *p, *p_prev = NULL;
    bool b_replace = false;

    if (pp_sections == NULL || p_section == NULL)
        return false;

    p_section->p_next = NULL;

    if (*pp_sections == NULL)
    {
        *pp_sections = p_section;
        return false;
    }

    p = *pp_sections;
    while (p != NULL)
    {
        if (p->i_number == p_section->i_number)
        {
            if (p_prev == NULL)
                *pp_sections = p_section;
            else
                p_prev->p_next = p_section;
            p_section->p_next = p->p_next;
            p->p_next = NULL;
            dvbpsi_DeletePSISections(p);
            b_replace = true;
            break;
        }
        if (p->i_number > p_section->i_number)
        {
            if (p_prev == NULL)
                *pp_sections = p_section;
            else
                p_prev->p_next = p_section;
            p_section->p_next = p;
            break;
        }
        p_prev = p;
        p = p->p_next;
    }

    if (p == NULL && p_prev != NULL)
        p_prev->p_next = p_section;

    return b_replace;
}

bool dvbpsi_psi_lane_complete(const dvbpsi_psi_lane_t *p_lane)
{
    const dvbpsi_psi_section_t *p;
    unsigned i_expected = 0;

    if (p_lane == NULL || !p_lane->b_active)
        return false;

    for (p = p_lane->p_sections; p != NULL; p = p->p_next)
    {
        if (p->i_number != i_expected)
            return false;
        if (i_expected == p_lane->identity.i_last_number)
            return true;
        i_expected++;
    }
    return false;
}
