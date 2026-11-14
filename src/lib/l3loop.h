#ifndef L3LOOP_H
#define L3LOOP_H

#include "types.h"

void shine_loop_initialise(shine_global_config *config);

void shine_iteration_loop(shine_global_config *config);

/* Number of bits used by the scalefactors of one granule/channel. */
int shine_part2_length(int gr, int ch, shine_global_config *config);

#endif
