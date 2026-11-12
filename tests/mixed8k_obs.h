/*
 * mixed8k_obs.h - read-only observation points for the 8 kHz mixed-block
 * contract tests.
 *
 * This header is only consumed by the test build (-DMAD_MIXED8K_TEST, see
 * support/build.sh).  The production library never includes it, allocates no
 * extra storage for it and invokes no callbacks through it.
 *
 * The structure captures the requantized spectrum before reordering and the
 * subband-ordered spectrum after reordering/alias reduction, together with
 * the layout that was actually applied to each channel.
 */

#ifndef MIXED8K_OBS_H
#define MIXED8K_OBS_H

#include "fixed.h"

struct mad_mixed8k_obs {
  int active;
  unsigned int nch;
  unsigned int samplerate;
  int mpeg25;
  int block_type[2];
  int mixed[2];
  unsigned int longlines[2];
  unsigned short global_gain[2];
  unsigned short scalefac_compress[2];
  unsigned char flags[2];
  unsigned char scalefac[2][39];
  unsigned char subblock_gain[2][3];
  mad_fixed_t xr_requant[2][576];	/* after Huffman/requantization */
  mad_fixed_t xr_reorder[2][576];	/* after reorder + alias reduction */
};

#endif
