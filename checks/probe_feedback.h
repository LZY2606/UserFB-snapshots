/* Deterministic, crash-free synthetic coverage for resume tests.
   Apache-2.0. */
#ifndef CHECKS_PROBE_FEEDBACK_H
#define CHECKS_PROBE_FEEDBACK_H

#include "types.h"

/* Bytes in [PROBE_DEAD_LO, PROBE_DEAD_HI) behave as inert: mutating them
   leaves the coverage map all zero, i.e. at the base checksum. */

#define PROBE_DEAD_LO 40
#define PROBE_DEAD_HI 80

/* Fill map (map_size bytes, zeroed by the caller is not required) with a
   deterministic coverage trace for candidate cand of len bytes against the
   immutable input. The same inputs always produce the same map. */

void probe_fill_trace(u8 *map, u32 map_size, int stage, u32 byte_pos,
                      const u8 *cand, u32 len, const u8 *input);

#endif
