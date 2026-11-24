#ifndef IXHEAACE_TASK_AU_LIMIT_H
#define IXHEAACE_TASK_AU_LIMIT_H

#include "ixheaac_type_def.h"
#include "ixheaac_error_standards.h"
#include "ixheaace_error_codes.h"

#define IXHEAACE_TASK_AU_END_RESERVE_BITS (10)
#define IXHEAACE_TASK_AU_PCE_RESERVE_BITS (88)

IA_ERRORCODE ixheaace_task_au_check_value(WORD32 max_au_bytes, WORD32 supported,
                                          WORD32 min_bytes, WORD32 max_bytes);

WORD32 ixheaace_task_au_dynamic_budget(WORD32 max_au_bytes, WORD32 prior_bits,
                                       WORD32 future_average_bits, WORD32 reserve_bits,
                                       WORD32 static_bits, WORD32 anc_bits);

WORD32 ixheaace_task_au_limit_channel_bits(WORD32 *max_ch_dyn_bits,
                                         const FLOAT32 *ch_bit_dist,
                                         WORD32 num_channels,
                                         WORD32 hard_dynamic_bits);

WORD32 ixheaace_task_au_get_reservoir_level(pVOID pv_encoder, WORD32 element);
WORD32 ixheaace_task_au_get_reservoir_max(pVOID pv_encoder, WORD32 element);
WORD32 ixheaace_task_au_get_num_elements(pVOID pv_encoder);
WORD32 ixheaace_task_au_get_element_type(pVOID pv_encoder, WORD32 element);
WORD32 ixheaace_task_au_get_element_tag(pVOID pv_encoder, WORD32 element);

#endif
