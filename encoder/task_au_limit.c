/******************************************************************************
 *                                                                            *
 * Copyright (C) 2026 The Android Open Source Project                         *
 *                                                                            *
 * Licensed under the Apache License, Version 2.0 (the "License");           *
 * you may not use this file except in compliance with the License.          *
 * You may obtain a copy of the License at                                    *
 *                                                                            *
 * http://www.apache.org/licenses/LICENSE-2.0                                 *
 *                                                                            *
 * Unless required by applicable law or agreed to in writing, software        *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and        *
 * limitations under the License.                                             *
 *                                                                            *
 *****************************************************************************/

#include "ixheaac_type_def.h"
#include "task_au_limit.h"

IA_ERRORCODE ixheaace_task_au_check_value(WORD32 max_au_bytes, WORD32 supported,
                                          WORD32 min_bytes, WORD32 max_bytes) {
  if (max_au_bytes < 0) {
    return IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_NEGATIVE;
  }

  if (max_au_bytes == 0) {
    return IA_NO_ERROR;
  }

  if (!supported) {
    return IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_UNSUPPORTED_MODE;
  }

  if (max_au_bytes < min_bytes || max_au_bytes > max_bytes) {
    return IA_EXHEAACE_CONFIG_FATAL_MAX_AU_BYTES_OUT_OF_RANGE;
  }

  return IA_NO_ERROR;
}

WORD32 ixheaace_task_au_dynamic_budget(WORD32 max_au_bytes, WORD32 prior_bits,
                                       WORD32 future_average_bits, WORD32 reserve_bits,
                                       WORD32 static_bits, WORD32 anc_bits) {
  WORD32 total_limit_bits = (max_au_bytes << 3) - prior_bits - future_average_bits - reserve_bits;
  return total_limit_bits - static_bits - anc_bits;
}

WORD32 ixheaace_task_au_limit_channel_bits(WORD32 *max_ch_dyn_bits,
                                         const FLOAT32 *ch_bit_dist,
                                         WORD32 num_channels,
                                         WORD32 hard_dynamic_bits) {
  WORD32 ch;
  WORD32 used_bits = 0;
  WORD32 changed = 0;

  if (hard_dynamic_bits < 0) {
    hard_dynamic_bits = 0;
  }

  for (ch = 0; ch < num_channels; ch++) {
    WORD32 channel_cap;

    if (ch + 1 == num_channels) {
      channel_cap = hard_dynamic_bits - used_bits;
    } else {
      channel_cap = (WORD32)(ch_bit_dist[ch] * (FLOAT32)hard_dynamic_bits);
    }

    if (channel_cap < 0) {
      channel_cap = 0;
    }

    if (max_ch_dyn_bits[ch] > channel_cap) {
      max_ch_dyn_bits[ch] = channel_cap;
      changed = 1;
    }
    used_bits += max_ch_dyn_bits[ch];
  }

  /* Flooring and per-channel clamps can otherwise leave the bookkeeping sum
     above the hard budget when the final channel was already tightly capped. */
  if (used_bits > hard_dynamic_bits && num_channels > 0) {
    WORD32 excess = used_bits - hard_dynamic_bits;
    for (ch = num_channels - 1; ch >= 0 && excess > 0; ch--) {
      WORD32 remove = excess < max_ch_dyn_bits[ch] ? excess : max_ch_dyn_bits[ch];
      max_ch_dyn_bits[ch] -= remove;
      excess -= remove;
    }
  }
  return changed;
}
