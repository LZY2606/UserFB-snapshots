/*
 *      ancillary tail slot support (internal interface)
 *
 *      Reserved bytes live at the physical end of selected audio
 *      frames.  The quantization loop is told about the reservation
 *      before coding a frame, so that scalefactor/Huffman data never
 *      occupies these bytes, and the reservoir accounting keeps the
 *      reservations in balance with the real main_data_begin pointers.
 */

#ifndef LAME_ANCILLARY_H
#define LAME_ANCILLARY_H

#include "lame.h"

/* install/replace/clear the plan on an uninitialized encoder;
   returns 0 on success, -1 on invalid input or allocation failure */
int     ancillary_set_plan(lame_global_flags * gfp,
                           const lame_ancillary_slot *slots, size_t count);

/* release all owned plan data */
void    ancillary_free_plan(lame_internal_flags * gfc);

/* validate a non-empty plan against the configuration chosen in
   lame_init_params(); returns 0 when the plan may be used */
int     ancillary_validate_config(lame_internal_flags * gfc);

/* select the slot, if any, belonging to the frame about to be encoded */
void    ancillary_begin_frame(lame_internal_flags * gfc, unsigned long frame);

/* slot size reserved for the frame being encoded, 0 when there is none */
int     ancillary_pending_size(lame_internal_flags const * gfc);

/* one byte of the currently pending slot */
unsigned char ancillary_slot_byte(lame_internal_flags const * gfc, int index);

/* mark the pending slot as delivered */
void    ancillary_slot_consumed(lame_internal_flags * gfc);

#endif /* LAME_ANCILLARY_H */
