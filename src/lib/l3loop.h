#ifndef L3LOOP_H
#define L3LOOP_H

#include "types.h"

/* Maximum number of bits the side information may carry for one granule and
 * channel (12 bit field). */
#define SHINE_MAX_GRANULE_BITS 4095

void shine_loop_initialise(shine_global_config *config);

void shine_iteration_loop(shine_global_config *config);

/* Quantise one complete frame from an explicit long-window spectrum array.
 *
 * freq       long-window spectra, indexed [channel][granule][line].
 * gain_shift when non zero, every global_gain is raised by 4*gain_shift so
 *            that spectra pre-scaled by 2^-gain_shift still decode at the
 *            original level. */
void shine_iteration_frame(shine_global_config *config,
                           int32_t (*freq)[MAX_GRANULES][GRANULE_SIZE],
                           int gain_shift);

/* Quantise one M/S frame using the stereo allocation.
 *
 * `empty[ch][gr]` marks a granule/channel pair whose coefficients are all
 * zero; such a pair costs nothing (part2_3_length = 0) and its half of that
 * granule's capacity is handed to the other channel.  This is how a strongly
 * correlated stereo pair makes room for the non-zero channel without ever
 * exceeding the physical frame. */
void shine_iteration_frame_ms(
    shine_global_config *config,
    int32_t (*freq)[MAX_GRANULES][GRANULE_SIZE], int gain_shift,
    const int empty[MAX_CHANNELS][MAX_GRANULES]);

#endif
