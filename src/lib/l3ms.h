#ifndef L3MS_H
#define L3MS_H

#include "types.h"

/* Initialise the adaptive mid/side state of a freshly created encoder. */
void shine_ms_initialise(shine_global_config *config);

/* Select and quantise the stereo representation for the current frame.
 *
 * The long-block spectra in config->mdct_freq[2][2][576] are inspected once
 * (they are already alias-reduced by shine_mdct_sub) and both the LR and the
 * MS candidate are prepared on that same data.  On return config->l3_enc,
 * config->side_info and config->mpeg.mode/mode_ext describe the chosen
 * representation, and the bit reservoir is untouched. */
void shine_ms_frame(shine_global_config *config);

#endif
