#ifndef RESERVOIR_H
#define RESERVOIR_H

#include "types.h"

int shine_max_reservoir_bits(double *pe, shine_global_config *config);
void shine_ResvAdjust(gr_info *gi, shine_global_config *config);
void shine_ResvFrameEnd(shine_global_config *config);

/* Cross-frame bit reservoir streaming. Only used when config->ResvMax > 0.
 * Returns 0 on success, -1 on allocation failure. */
int shine_resv_init(shine_global_config *config, int max_bytes);
void shine_resv_close(shine_global_config *config);
/* Store the just coded frame (main data in config->mbs) and emit every
 * queued physical frame whose main-data bytes are all available. */
void shine_resv_store_frame(shine_global_config *config, int cap);
/* Pad the main-data stream up to the capacity of all coded frames and
 * emit every queued frame. Does not add any audio frame. */
void shine_resv_flush_frames(shine_global_config *config);

#endif
