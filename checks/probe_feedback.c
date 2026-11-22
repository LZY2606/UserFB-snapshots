/* Deterministic, crash-free synthetic coverage for resume tests.
   Apache-2.0. */
#include "probe_feedback.h"
#include <string.h>

void probe_fill_trace(u8 *map, u32 map_size, int stage, u32 byte_pos,
                      const u8 *cand, u32 len, const u8 *input) {

  u32 i;
  int any_diff = 0, dead_touched = 0;
  u32 h;

  memset(map, 0, map_size);

  for (i = 0; i < len; i++) {

    if (cand[i] == input[i]) continue;

    any_diff = 1;

    if (i >= PROBE_DEAD_LO && i < PROBE_DEAD_HI) dead_touched = 1;

  }

  /* No change, or a mutation touching the inert zone: coverage stays at the
     base (zero) map. */

  if (!any_diff || dead_touched) return;

  if (stage == 0) {

    /* flip1: eight adjacent byte positions share one path, so the token
       collector sees stable multi-byte runs. */

    h = (byte_pos / 8) + 1;

  } else {

    /* Other stages: a stable mix of stage and position. */

    h = 2166136261u;
    h ^= (u32)stage;        h *= 16777619u;
    h ^= byte_pos;          h *= 16777619u;
    h ^= len;               h *= 16777619u;

  }

  map[h % map_size] = (u8)(h | 1);

}
