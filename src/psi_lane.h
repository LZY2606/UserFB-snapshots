/*****************************************************************************
 * psi_lane.h: current/next generation lanes for PSI table decoders
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

#ifndef _DVBPSI_PSI_LANE_H_
#define _DVBPSI_PSI_LANE_H_

#include <stdbool.h>

#if defined(HAVE_INTTYPES_H)
#include <inttypes.h>
#elif defined(HAVE_STDINT_H)
#include <stdint.h>
#endif

/* Expects dvbpsi.h and psi.h to be included by the translation unit. */

#define DVBPSI_LANE_CURRENT 0  /* section with current_next_indicator == 1 */
#define DVBPSI_LANE_NEXT    1  /* section with current_next_indicator == 0 */
#define DVBPSI_LANE_COUNT   2

/*
 * Number of already delivered identities remembered per lane.  Identities are
 * tiny and versions are only five bits wide, so keeping a generous history
 * makes the rotation suppression independent of how many generations the
 * multiplex has gone through.
 */
#define DVBPSI_DELIVERED_HISTORY 32

/*!
 * \struct dvbpsi_psi_identity_s
 * \brief Identity of one generation of a table.
 *
 * A generation is identified by all the fields that may change between two
 * carousels of the same table: table_id, table_id_extension, version_number,
 * last_section_number and current_next_indicator.
 */
typedef struct dvbpsi_psi_identity_s
{
    uint8_t  i_table_id;
    uint16_t i_extension;
    uint8_t  i_version;
    uint8_t  i_last_number;
    bool     b_current_next;
} dvbpsi_psi_identity_t;

/*!
 * \struct dvbpsi_psi_lane_s
 * \brief One in-progress generation.
 */
typedef struct dvbpsi_psi_lane_s
{
    bool                    b_active;
    dvbpsi_psi_identity_t   identity;
    dvbpsi_psi_section_t   *p_sections; /* sections sorted by section_number */
    void                   *p_building; /* table under construction */
} dvbpsi_psi_lane_t;

/*!
 * \struct dvbpsi_psi_epochs_s
 * \brief Current/next collection state of a logical table decoder.
 */
typedef struct dvbpsi_psi_epochs_s
{
    dvbpsi_psi_lane_t   lanes[DVBPSI_LANE_COUNT];

    /* Already delivered identities, one history per lane. */
    dvbpsi_psi_identity_t delivered[DVBPSI_LANE_COUNT][DVBPSI_DELIVERED_HISTORY];
    uint8_t             i_delivered_count[DVBPSI_LANE_COUNT];
    uint8_t             i_delivered_next[DVBPSI_LANE_COUNT];
} dvbpsi_psi_epochs_t;

/* Initialize/reset all collection and de-duplication state. */
void dvbpsi_psi_epochs_init(dvbpsi_psi_epochs_t *p_epochs);

/*
 * Drop the half table and the delivered identities of the given lane.
 * pf_delete_building releases the table under construction (may be NULL).
 */
void dvbpsi_psi_epochs_clear_lane(dvbpsi_psi_epochs_t *p_epochs, int i_lane,
                                  void (*pf_delete_building)(void *));

/*
 * Drop only the in-progress generation of a lane: keep the identities that
 * were already delivered, so a carousel does not trigger a second callback.
 */
void dvbpsi_psi_epochs_reset_lane(dvbpsi_psi_epochs_t *p_epochs, int i_lane,
                                  void (*pf_delete_building)(void *));

/* Drop both lanes and every delivered identity. */
void dvbpsi_psi_epochs_clear(dvbpsi_psi_epochs_t *p_epochs,
                             void (*pf_delete_building)(void *));

/* Lane selected by the current_next_indicator of a section.  The two lanes
 * never exchange sections nor completion progress. */
int dvbpsi_psi_lane_from_section(const dvbpsi_psi_section_t *p_section);

bool dvbpsi_psi_identity_equal(const dvbpsi_psi_identity_t *p_a,
                               const dvbpsi_psi_identity_t *p_b);

/* Has this exact generation already been delivered on this lane? */
bool dvbpsi_psi_identity_delivered(const dvbpsi_psi_epochs_t *p_epochs,
                                   int i_lane,
                                   const dvbpsi_psi_identity_t *p_identity);

/* Remember a generation that has just been delivered. */
void dvbpsi_psi_identity_mark_delivered(dvbpsi_psi_epochs_t *p_epochs,
                                        int i_lane,
                                        const dvbpsi_psi_identity_t *p_identity);

/*
 * Insert a section in section_number order.  A section with an already
 * present number replaces the old one instead of adding completion progress.
 * Returns true when an old section was replaced.
 */
bool dvbpsi_psi_lane_section_add(dvbpsi_psi_section_t **pp_sections,
                                 dvbpsi_psi_section_t *p_section);

/* Are all of 0..last_section_number present for this lane? */
bool dvbpsi_psi_lane_complete(const dvbpsi_psi_lane_t *p_lane);

#endif
