#ifndef INTENSITY_H
#define INTENSITY_H

#include "types.h"

/* MPEG-1 intensity positions reuse the right channel scalefactor fields,
 * which therefore need a fixed 3 bit width for both slen groups.
 * ISO 11172-3 Table 2-B-6: scalefac_compress 13 gives slen1 == slen2 == 3. */
#define SHINE_INTENSITY_SFC 13

/* Value 7 (all ones in 3 bits) is the invalid intensity position: a band
 * carrying it stays in normal LR coding on the decoder. */
#define SHINE_INTENSITY_OFF 7

/* Public API, also declared in layer3.h. */
int shine_set_intensity(struct shine_global_flags *s, int first_sfb,
                        int position0, int position1);

/* Switch the frame header between the encoder's initial stereo mode and
 * joint stereo with intensity stereo enabled.  Called once per frame. */
void shine_intensity_set_header(shine_global_config *config);

/* Merge the L and R long-block MDCT spectra from the configured start band
 * to the top of each granule into the left channel (L+R) and clear the right
 * channel spectrum there.  No-op when intensity stereo is disabled. */
void shine_intensity_merge(shine_global_config *config);

/* Finalize one right-channel granule while intensity stereo is on: fill its
 * scalefactor slots with "off" markers below the actual intensity boundary
 * and the granule's position up to band 20, completing part2 accounting for
 * the all-zero spectrum case.  The fixed 3 bit scalefactor width and the
 * disabled right channel SCFSI are prepared inside the iteration loop. */
void shine_intensity_finish_granule(shine_global_config *config, int gr);

#endif
